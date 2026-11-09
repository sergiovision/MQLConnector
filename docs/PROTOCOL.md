# MQLConnector wire protocol

This document is for anyone writing a server that MQLConnector talks to. The reference
implementations are [`examples/python/server.py`](../examples/python/server.py) and
[`examples/csharp/`](../examples/csharp).

## Transport

One TCP connection per MetaTrader 5 terminal process. The client connects to `Host:Port`, sets
`TCP_NODELAY` and reconnects with backoff if the connection drops. Both directions carry a stream of
**frames**. Each frame holds one UTF-8 JSON object.

## Framing (InitLib `Framing`)

### `delimiter` (default)

```
+---------------------------+----+
| UTF-8 JSON object         | d  |      d = Delimiter, default 0x1A (26); any byte 1..31
+---------------------------+----+
```

- A frame never contains its delimiter. Before sending, the client rewrites any raw `d` in a JSON
  string as `\u00XX` and any raw `d` outside strings (possible only when `d` is `\t`, `\n` or `\r`) as
  a space. Your server must do the same.
- A frame longer than 16 MiB is dropped up to the next delimiter, and the stream continues.
- `Delimiter:10` gives newline-delimited JSON. Pretty-printed requests are still sent safely, as
  described above.

### `length`

```
+--------+--------+--------+--------+---------------------------+
|  u32 little-endian total length   | payload (UTF-8 JSON)      |
|  (counts these 4 bytes too)       | total - 4 bytes           |
+--------+--------+--------+--------+---------------------------+
```

- This is the frame layout of Fast Binary Encoding's protocol. Example: a 10-byte payload is prefixed
  with `0E 00 00 00`.
- A length below 4, or a payload above 16 MiB, cannot be recovered from in-band. The client closes the
  connection and reconnects.

## Routing (InitLib `RouteKey`, default `ChartId`)

Every incoming frame is delivered to the mailbox of the int64 value of its top-level `RouteKey` member
(a number, or a string that holds a number). A frame without it goes to route 0. Frames for routes
that no `InitLib` or `DoSendMessage` subscribed are dropped.

**Server rule:** copy the request's route member into the reply.

## Correlation (InitLib `CorrelationKey`, off by default)

When `CorrelationKey` is set (for example to `ReqId`) and a `DoSendMessage` request has that member,
the call returns only the first frame on its route whose member is equal. Two values are equal when
both are numbers with the same value, or both are strings with the same decoded text. The client
leaves other frames (broadcasts, replies to other threads) queued in order for `DoListenMessage`.
Without the key, or when the request lacks the member, `DoSendMessage` returns the next frame on its
route.

**Server rule:** echo the correlation member unchanged in the reply.

Mailboxes are bounded. When one is full, the oldest frame is dropped. If unsolicited frames pile up
and nobody drains them with `DoListenMessage`, a correlated reply can be the frame that is dropped.

## Profiles (InitLib `Profile`)

| Profile | Behaviour |
|---|---|
| `fincore` (default) | On the first connect, sends `{"ChartId":…,"Id":1026,…}` and uses the reply's `Data` as the log path. `GetLevels` (Id 1023) and `GetSignalCache` (Id 1033) send FinCore requests. |
| `generic` | No handshake: `InitLib` returns as soon as TCP connects. `GetLevels` and `GetSignalCache` return `""`. |

A generic server should use `Profile:"generic"`. A server that sees id 1026 may simply reply with any
`Data` string, or ignore it (the client then times out once and logs to `LogPath`).

## Server checklist

1. Accept TCP and read frames with the same `Framing` / `Delimiter` as the client.
2. Parse each frame as one JSON object.
3. Reply on the same connection with a JSON object that carries the request's route member and, if
   used, its correlation member.
4. Push unsolicited frames at any time with a route member. MQL reads them with `DoListenMessage`.
5. Keep frames at or below 16 MiB, and never put a raw delimiter byte inside a frame.
6. One connection is shared by every chart in a terminal, so reply per frame, not per connection.
