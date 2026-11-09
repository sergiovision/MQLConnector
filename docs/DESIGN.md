# MQLConnector design

This document explains how the library is built and why. For the wire format, see
[PROTOCOL.md](PROTOCOL.md).

## Goals, in priority order

1. **Correctness under concurrency and at the ABI boundary.** The library is loaded into a host
   process (the MetaTrader 5 terminal) that it does not control. A crash or a hang there loses money.
2. **Backward compatibility.** The 7 legacy exports and the bytes they put on the wire are frozen.
   Golden-frame tests enforce this.
3. **Latency.** The request → reply path should cost a few tens of microseconds plus the network.
4. **Simplicity.** A small number of threads, one lock per component, no lock-free code until a
   benchmark shows the lock matters.

## Layers

```
exports.cpp        C ABI: UTF-16 <-> UTF-8, catch-all, thread_local result buffers, module pin
  Profile          fincore (log-path handshake, GetLevels/GetSignalCache) | generic (no-ops)
  Session          ref-counted; owns Config, Link{transport, framer, codec}, MailboxRegistry, Logger
    Codec          JsonCodec: route + correlation + Id + Sym from one read_members pass
    MailboxRegistry  route -> Mailbox (bounded deque + condition variable)
    Transport      TcpTransport: reader thread, writer thread, reconnect with backoff
      Framer       DelimiterFramer | LengthPrefixFramer
      Socket       socket_winsock.cpp | socket_posix.cpp
```

Everything below `exports.cpp` is platform-neutral C++20 apart from the socket files. The session,
transport and core tests therefore run on Windows and Linux (in CI) and on macOS.

## Threads

| Thread | Owner | Does |
|---|---|---|
| Caller threads (one per chart in MT5) | MT5 | `InitLib`, `DoSendMessage` (blocks on its mailbox), `DoPostMessage` (enqueues), `DoListenMessage` (non-blocking pop) |
| Reader | `TcpTransport` (`std::jthread`) | Connect / reconnect with backoff, `recv`, `Framer::feed`, `Session::on_frame` → codec → mailbox |
| Writer | `TcpTransport` (`std::jthread`) | Drains the outgoing queue and sends everything queued so far in **one** `send` (batching) |
| Logger | `Logger` (`std::jthread`) | Formats and appends log lines. Callers only push to a bounded queue and are never blocked |

## Concurrency invariants

These are stated in the code next to each member they protect.

- **One lock per component.** The mailbox registry's map lock and a mailbox's lock are never held
  together. The map lock is held only for the lookup. The caller then holds a `shared_ptr` to the
  mailbox, so an unsubscribe cannot free it under a waiter.
- **No lock is held across a socket call or a callback.** The reader calls `on_frame` without holding
  any transport lock, and the writer swaps the queue out under its lock and sends after releasing it.
- **Every wait can be interrupted.** All waits are `condition_variable_any::wait_until(lock,
  stop_token, deadline, pred)`. Socket waits are woken by `shutdown`, and connects are sliced into
  50 ms steps. `DestroyLib` therefore returns within a bounded time even while a `DoSendMessage` is
  waiting.
- **Configuration and the transport are shared through `shared_ptr<Link>`.** A request copies the
  pointer once, so a concurrent `DestroyLib` cannot free the transport mid-request.
- **`notify_one` vs `notify_all`.** In legacy mode the next frame on a route may be taken by any
  waiter, so `notify_one` is enough. In correlated mode, waiters on one mailbox wait for *different*
  values, so a push uses `notify_all`.

## The hot path

**Incoming frame** (reader thread):

1. `recv` into a reusable buffer.
2. `Framer::feed`: one virtual call per read. The delimiter search is `std::string_view::find`
   (`memchr`). A frame that sits entirely inside the read is passed on as a view, with no copy.
3. `JsonCodec::inspect`: one validating pass of `read_members` that returns slices for the route,
   correlation, `Id` and `Sym` members. Nested objects and arrays are skipped with a 4096-bit stack:
   no recursion, no DOM, no allocation.
4. `MailboxRegistry::deliver`: lock the map, look up the mailbox, unlock. Lock the mailbox, copy the
   frame into the deque (the one allocation per frame: the caller needs to own its string), unlock,
   and notify.

**Outgoing request** (caller thread):

1. UTF-16 → UTF-8 (results go back the other way into a `thread_local` buffer MT5 copies from).
2. If the frame contains the delimiter byte, `Codec::protect` rewrites it: inside a JSON string it
   becomes `\u00XX`, and outside strings (only possible when the delimiter is JSON whitespace) it
   becomes a space. Otherwise the frame is passed through unchanged, with no copy.
3. Register the correlated wait (if any) *before* sending, so a fast reply cannot be lost.
4. Enqueue the frame for the writer and wait on the mailbox.

## Lifetime and the Windows loader

- **No `DllMain`.** Nothing runs under the loader lock. Threads start on the first `InitLib` and stop
  on the last `DestroyLib`.
- **The process-wide session is never destroyed** (`NoDestructor`). Joining threads from a DLL's
  static destructors would happen under the loader lock and can deadlock.
- **The module is pinned** (`GET_MODULE_HANDLE_EX_FLAG_PIN`) on the first `InitLib`. Live EAs never
  call `DestroyLib`, and MT5 must not unmap code that a background thread is still running.
- **`DisableThreadLibraryCalls` is not called.** With the static CRT, the CRT needs thread-detach
  notifications to free per-thread data.
- **No exception crosses `extern "C"`.** Every export runs inside `guarded(...)`, which logs the error
  and returns the neutral value (`0` or `""`).

## Ownership rules

- RAII everywhere. No raw `new`/`delete`, and every socket is a move-only handle.
- Every piece of shared state has one documented owner and one lock, noted at the member.
- Strings stay `string_view`s until a copy is needed: the mailbox entry and the `thread_local` result
  buffer that MT5 copies from.

## Testing strategy

| Level | Suite | Checks |
|---|---|---|
| Unit | `framer_test`, `frame_decoder_test`, `signal_json_test`, `codec_test`, `utf_test`, `config_test` | Parsers and framers on adversarial input: split reads, garbage, depth limits, size limits |
| Concurrency | `mailbox_test` | Timeout accuracy, overflow, 8 correlated waiters on one mailbox |
| Integration | `transport_test`, `session_test`, `phase1_session_test` | A real TCP loopback server: reconnects, broadcasts, cross-delivery, both framings |
| Compatibility | `fincore_compat_test` | Byte-exact frames and returned strings for every FinCore call shape |
| ABI | `dll_exports_test` (Windows) | `LoadLibraryW` + `GetProcAddress` of all 8 exports |
| Packaging | `.github/scripts/check-dll.ps1` | Exactly 8 exports; no CRT or .NET imports |

## Extending

- **A new wire encoding** (for example, Fast Binary Encoding): implement `Codec` and choose it from
  `Protocol`. With `Framing:"length"` the frame layout already matches FBE's (a u32 LE size that
  includes itself).
- **Several connections per process:** `Session` is not a singleton, only `process_session()` is.
  A handle-based API can create more sessions without touching the existing exports.
