# MQLConnector

A native x64 Windows DLL that connects MetaTrader 5 programs (Expert Advisors, indicators, tester
agents) to the [FinCore](https://github.com/sergiovision) server. It speaks FinCore's TCP protocol
(one UTF-8 JSON `SignalInfo` per frame, frames ended by byte `0x1A`), routes replies to the chart that
asked, and keeps one shared connection per terminal process.

It replaces a .NET Framework assembly with the same seven exports. Same wire format, same semantics,
no server changes. The differences:

- **Latency.** A reply wakes the waiting call through a condition variable. The .NET connector slept
  `WaitOp` (300 ms live, 50 ms in the tester) before it first looked. `TCP_NODELAY` is on.
- **Deployment.** One file: static CRT, only system DLLs (`KERNEL32`, `WS2_32`). Nothing to copy next to
  `terminal64.exe` for the strategy tester's agents.
- **Robustness.** No exception crosses the C ABI. `DoPostMessage` never blocks MQL on the socket. The
  logger never blocks its callers.

## Why C++

C++ is where my professional career started, about twenty years ago, and it is still my favourite
language. I still use it wherever speed and memory matter most: when a hot path decides how fast a
whole application feels, I write that part as a small native library in C++, the way I wrote this one.
The applications that load such a library get much faster and use less memory without being
rewritten. Here a request that used to wait at least 50 ms in the strategy tester (300 ms on live
charts) now completes in about 65 µs, and the old runtime dependencies are gone.

AI tools make this kind of optimisation faster to do, but they do not replace knowing how C++ works.
Getting the most out of it still depends on that knowledge:

- **Ownership and lifetimes.** RAII throughout and no raw `new`/`delete`; strings and buffers are
  views until a copy is really needed.
- **The memory model.** Every piece of state shared between threads has one documented owner and
  one lock. Threads stop through `std::stop_token`, and each blocking call can be interrupted, so
  shutdown time is bounded.
- **The ABI and the loader.** No exception crosses `extern "C"`. Nothing runs under the loader lock.
  The module is pinned while its threads run, and the static CRT keeps it a single file.
- **Allocation-conscious parsing.** The JSON reader validates a frame and skips nested data of any
  depth without recursion or heap allocation. The frame decoder hands out views into the read
  buffer when a frame arrives in one piece.
- **Measuring rather than guessing.** Latency claims come from the benchmark below, and behaviour is
  covered by unit and integration tests that also run on macOS.

## The C ABI

```mql5
#import "MQLConnector.dll"
int    InitLib(string json);                     // 1 connected, 0 not (yet)
void   DestroyLib(long chartId);
string DoSendMessage(string json);               // send, wait up to WaitTimeout for the chart's reply
void   DoPostMessage(string json);               // send, don't wait
string DoListenMessage(long key);                // next queued frame for a chart, or ""
string GetLevels(string sym, long chartId);      // SIGNAL_LEVELS4SYMBOL -> reply Data
string GetSignalCache(string key, string sym, long chartId);
#import
```

[`mql/Include/MQLConnector.mqh`](mql/Include/MQLConnector.mqh) holds exactly this import;
[`mql/Scripts/ConnectorBench.mq5`](mql/Scripts/ConnectorBench.mq5) is a complete example. The `mql/`
tree mirrors a terminal's `MQL5\` folder: copy `Include` and `Scripts` there and the DLL to
`MQL5\Libraries\`, and enable *Allow DLL imports*.

| Export | Semantics |
|---|---|
| `InitLib` | Process-wide, ref-counted session. The first call parses the parameters (`Host`, `Port`, `Account`, `Protocol`, `ChartId`, `WaitOp`, `WaitTimeout`; strings or numbers; defaults `127.0.0.1`, `2022`, `1000000`, `tcp`, `0`, `300`, `3000`; `<= 0` means default). It connects (waiting up to WaitTimeout), asks FinCore for its main log path (`SIGNAL_GETMAINLOGPATH`) and logs there, falling back to `%TEMP%\MQLConnector.log`. Every call adds a reference and subscribes its `ChartId`. |
| `DestroyLib` | Unsubscribes the chart and drops a reference; the last one stops the connection (bounded). |
| `DoSendMessage` | Subscribes the message's `ChartId`, sends, blocks on that chart's mailbox up to WaitTimeout, returns the first frame queued there (verbatim) or `""`. While disconnected it first waits up to WaitTimeout for the reconnect. |
| `DoPostMessage` | Queues the frame for the writer thread and returns. Dropped (and logged) while disconnected. |
| `DoListenMessage` | Non-blocking dequeue from the mailbox keyed by `key`. |
| `GetLevels` | `{Id:1023, ChartId, Sym}` round trip. Returns `Data`: a string unescaped, object/array as raw JSON, null as `""`. Before `InitLib` it returns `Failed to initialize MQLConnector.dll`. |
| `GetSignalCache` | `{Id:1033, ChartId, Sym, Key}` round trip, returns the whole reply. |

Returned strings live in a per-thread, per-function buffer until the next call of that function on that
thread. MT5 copies them immediately.

## Design

```
include/mqlconnector/mqlconnector.h   C ABI declarations
src/exports.cpp, MQLConnector.def   ABI shims: UTF-16 <-> UTF-8, catch-all, module pin
src/connector/session.*               ref-counted session: config, transport, mailboxes, logger
src/net/transport.h                   transport interface (frames in/out, connected or not)
src/net/tcp_transport.cpp             reconnect loop, reader thread, batching writer thread
src/net/socket.h                      RAII sockets; socket_winsock.cpp / socket_posix.cpp
src/core/frame_decoder.h              streaming 0x1A splitter, 16 MiB cap, resync after overflow
src/core/signal_json.*                validating top-level JSON reader + escaping writer
src/core/utf.h                        UTF-16 <-> UTF-8 with surrogates, U+FFFD for bad input
src/core/mailbox.*                    bounded per-chart queues (mutex + condition_variable)
src/core/config.*                     InitLib parameter parsing
src/core/logger.*                     async bounded logger, log4net line format, shared appends
tests/                                GoogleTest suites (core, transport, session, DLL exports)
bench/, tools/echo_server/            round-trip benchmark and a loopback stand-in server
mql/                                  MQL5 import header and benchmark script
```

Everything below `exports.cpp` is platform-neutral C++20 apart from the socket layer, so the core,
transport and session tests also run on macOS and Linux.

**Threads.** MQL calls arrive on each chart's thread. The transport owns two threads. The reader
connects, reads, splits frames and delivers each one to the mailbox of its `ChartId` (dropping it if no
chart subscribed). The writer drains the outgoing queue and sends everything queued so far in one
write. The logger has one writer thread. Shared state is guarded by one mutex per component and is
documented at the member. Waits use `std::condition_variable(_any)` with stop tokens. Every blocking
call can be interrupted (socket shutdown, a connect sliced into 50 ms steps), so stopping is bounded.

**JSON.** The connector routes messages; it never builds a DOM. `read_members` validates the whole
document and returns source slices for the few top-level members it needs. Nested `Data` of any shape
is skipped without recursion or allocation: a bit stack tracks open brackets, up to 4096 levels deep.
The writer escapes every control character, including `0x1A`, so a frame can never contain its own
delimiter.

**Lifetime.** The process-wide session is constructed on first use and never destroyed
(`NoDestructor`). Joining threads from a DLL's static destructors would happen under the loader lock.
The first `InitLib` pins the module (`GET_MODULE_HANDLE_EX_FLAG_PIN`): live EAs never call
`DestroyLib`, and MT5 must not unmap code a background thread is running. There is no `DllMain`. With
the static CRT, `DisableThreadLibraryCalls` must not be called: the CRT needs thread-detach
notifications to free per-thread data.

## Building

Requirements: CMake ≥ 3.25, Ninja, and a C++20 compiler. On Windows that is MSVC v143 (VS 2022 Build
Tools), run from an x64 developer prompt. On macOS it is Xcode 16.3+ or Homebrew LLVM 18+, for
`std::jthread`.

```bat
build.bat                                   :: configure + build + test (+ copy to ..\MQL\Libraries if present)
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release
```

Run the manual commands from a "x64 Native Tools" prompt with Visual Studio's own `cmake` first on
`PATH` (`build.bat` arranges that). A MinGW/MSYS2 `cmake` has no Windows CA store, so the test-only
GoogleTest download fails with "certificate signer not trusted"; the tests fall back to Git for
Windows' `ca-bundle.crt` when it exists, or pass `-DCMAKE_TLS_CAINFO=<bundle>` or
`-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<unpacked googletest>`. After a failed download, delete
`build\windows-x64-release` before configuring again.

```sh
cmake --preset macos-dev && cmake --build --preset macos-dev && ctest --preset macos-dev
```

Release builds use `/W4 /WX /permissive- /O2 /GL /LTCG /MT`. The output name is the CMake variable
`MQLC_OUTPUT_NAME`. GoogleTest is fetched for the tests only.

Verify a build:

```bat
dumpbin /exports   build\windows-x64-release\MQLConnector.dll   :: exactly the seven names
dumpbin /dependents build\windows-x64-release\MQLConnector.dll  :: system DLLs only
```

## Tests

`mqlc_tests` (all platforms) covers:

- **Frame decoder:** split and coalesced reads, UTF-8 cut across reads, oversized frames dropped with
  recovery.
- **JSON:** escapes, nested `Data`, numbers vs strings, missing keys, garbage, the depth limit.
- **UTF-16:** surrogates and invalid input.
- **InitLib config:** strings vs numbers.
- **Mailboxes:** timeout accuracy, routing, unsubscribed drops, overflow.
- **Logger:** format and rolling.
- **Transport and session against an in-process loopback server:** round trip; broadcast routed only to
  the subscribed chart; 8 charts on 8 threads with no cross-delivery; timeout returns `""` within
  WaitTimeout ± 20 ms; server restart, then auto-reconnect; `DestroyLib` ref counting;
  `DoPostMessage` never blocking while the server stops reading.

`mqlc_dll_tests` (Windows) loads the built DLL with `LoadLibraryW` and calls every export through
`GetProcAddress`.

## Benchmarks

`mqlc_bench` measures session round trips against an in-process echo server. `echo_server` plus
`mql/Scripts/ConnectorBench.mq5` measures `DoSendMessage` as MQL sees it.

```bat
build\windows-x64-release\mqlc_bench.exe --n 10000
build\windows-x64-release\echo_server.exe --port 2023   :: then run ConnectorBench in MT5
```

`mqlc_bench`, Release build, Windows 11 x64, loopback, request -> reply:

| Payload | Round trips | p50 | p99 | p99.9 | Throughput |
|---|---|---|---|---|---|
| 64 B | 10 000 | 65 µs | 260 µs | 736 µs | 12 600 /s |
| 4 KiB | 2 000 | 90 µs | 286 µs | 779 µs | 9 500 /s |

For comparison, the .NET connector this DLL replaces sent, slept `WaitOp` and only then checked for a
reply, so its round trip could not be shorter than one `WaitOp` plus a Windows timer tick: at least
50–62 ms in the strategy tester (`WaitOp` 50) and 300–312 ms on live charts (`WaitOp` 300). That floor
follows from its code; it was not measured with this benchmark.

## Roadmap

- MT5-side benchmark numbers (`ConnectorBench.mq5`) in the table above.
- A binary encoding of the frames as an alternative to JSON.

## License

MIT. See [LICENSE](LICENSE).
