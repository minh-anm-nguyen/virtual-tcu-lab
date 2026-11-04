# Contract: IPC transport protocol v1

Authoritative byte-level contract for `libtcu_ipc`. Derived from spec section 5; the decisions that
section leaves open are closed by research R-06 (control methods) and R-11 (error codes). Any change
to this file is a protocol change and requires a `version` bump, not a patch.

`version = 1` for every frame in this contract. A peer that sends any other value is answered
`UNSUPPORTED_VERSION` and disconnected.

---

## 1. Frame layout

All multi-byte integers are big-endian (network byte order).

```text
 0        4    5    6        8                16        18
 +--------+----+----+--------+----------------+--------+---------+---------+
 | body_  | ve | ty | flags  |   request_id   | name_  |  name   | payload |
 | length | rs | pe | uint16 |     uint64     | length | N bytes | M bytes |
 | uint32 | u8 | u8 |        |                | uint16 |         |         |
 +--------+----+----+--------+----------------+--------+---------+---------+
 |<-- not counted ->|<------------- body_length = 14 + N + M ------------->|
```

- `body_length` counts every byte after itself, i.e. from `version` to the end of `payload`.
- `kMaxBodyLength = 65536`. A prefix above it is a hard error (see section 4).
- `kMinBodyLength = 15`: the 14 fixed body bytes plus at least one name byte. A name of length 0 is
  invalid for every frame type.

## 2. Frame types

| `type` | Name | `request_id` | `name` holds | `payload` |
|---:|---|---|---|---|
| 1 | Request | nonzero | method | request arguments |
| 2 | Response | nonzero, echoes the request | method, echoed | result |
| 3 | Notification | **0** | topic | event data |
| 4 | Error | nonzero, echoes the request | method, echoed | error document (section 5) |

An `Error` frame always echoes the `request_id` of the request it rejects. A protocol violation that
cannot be attributed to a request (a bad prefix, a bad `version`, a bad `type`) is **not** reported
with an `Error` frame - the session is closed, because the sender has already proven it cannot be
parsed reliably.

## 3. Reserved control methods

The `ipc.` method and topic prefix is reserved for the transport. `registerHandler()` rejects it.

| Method | Caller | Request payload | Response payload |
|---|---|---|---|
| `ipc.hello` | client | `{"version":1,"client_id":"<id>"}` | `{"version":1,"capabilities":[]}` |
| `ipc.subscribe` | client | `{"topic":"<topic>"}` | `{"topic":"<topic>","active":true}` |
| `ipc.unsubscribe` | client | `{"topic":"<topic>"}` | `{"topic":"<topic>","active":false}` |

**Handshake ordering** (spec section 8.1, research R-06):

1. The client's first frame on a new connection MUST be `ipc.hello`.
2. Until its response arrives, the client sends nothing else; the server answers any other method
   with `UNAUTHENTICATED` and closes the session.
3. The client enters `Connected` only on a successful `ipc.hello` response whose `version` is 1.
4. An `UNSUPPORTED_VERSION` reply is terminal: the client stops and does not retry (a fixed
   configuration error must not spin).

`capabilities` is an empty array in v1. It exists so a v2 server can advertise without a new frame
type; a v1 client ignores its contents.

## 4. Decoder contract

`FrameCodec::tryDecode` is the only place that turns bytes into a frame. Its contract:

| Input state | Result | Buffer effect |
|---|---|---|
| fewer than 4 bytes | `Incomplete` | none |
| prefix read, `body_length < 15` or `> 65536` | `TooLarge` / `ProtocolError` | none - caller closes the session |
| prefix valid, fewer than `4 + body_length` bytes present | `Incomplete` | none |
| full frame present, all invariants hold | `Ok` plus frame | exactly `4 + body_length` bytes consumed |
| full frame present, an invariant fails | `ProtocolError` | none - caller closes the session |

Hard rules, each with a test:

- **No allocation sized by an unvalidated length.** The `body_length` bound is checked against the
  constant before any `reserve`/`resize`. (`FrameCodec_RejectOversizeBeforeAlloc`)
- **Partial input is never destructive.** `Incomplete` leaves the buffer byte-identical, so feeding
  one byte at a time decodes the same frame as feeding it whole.
  (`FrameCodec_ReadHeaderByteByByte`)
- **Multiple frames in one read are decoded in order**, by calling `tryDecode` until it returns
  `Incomplete`. (`FrameCodec_TwoFramesOneBuffer`)
- **Internal consistency is checked**, not assumed: `14 + name_length + payload_size` must equal
  `body_length`. (`FrameCodec_RejectInvalidNameLength`)
- **`name` is validated before use as a map key**: `[A-Za-z0-9_.-]{1,255}`, no NUL.
- **`request_id == 0` iff Notification.**

## 5. Error document

The payload of an `Error` frame:

```json
{"code":"INVALID_ARGUMENT","message":"bad request"}
```

`code` is from the closed set below. `message` is a short, operator-facing string; it MUST NOT echo
request payload bytes, a credential or a token (Principle V).

| `code` | Sent when | Session after |
|---|---|---|
| `INVALID_ARGUMENT` | a handler rejects its arguments | stays open |
| `UNKNOWN_METHOD` | no handler registered for the method | stays open |
| `PERMISSION_DENIED` | the method is not allowed for this peer | stays open |
| `UNAUTHENTICATED` | a non-`ipc.hello` method before the handshake | closed |
| `UNSUPPORTED_VERSION` | `version != 1` in a decodable frame or in `ipc.hello` | closed |
| `FRAME_TOO_LARGE` | `body_length > 65536` | closed |
| `PROTOCOL_ERROR` | any other invariant violation | closed |
| `INTERNAL_ERROR` | a handler threw, or an unexpected local failure | stays open |
| `SHUTTING_DOWN` | the server is in `Stopping` | closed after the frame |

Where the table says "closed", the server closes the session after attempting to write the frame; it
never waits for the write to drain before closing.

## 6. Client-side mapping

A received frame becomes an `IpcResponse` for the matching pending request:

| Received | `IpcResponse::error` | `payload_json` | `detail` |
|---|---|---|---|
| Response | `None` | frame payload | empty |
| Error | `RemoteError` | empty | the wire `code` |

Locally generated outcomes never involve a frame:

| Situation | `error` | `detail` |
|---|---|---|
| no connection at `request()` time | `NotConnected` | `"not connected"` |
| deadline elapsed | `Timeout` | `"deadline exceeded"` |
| TX queue at a cap | `QueueFull` | `"tx queue full"` |
| peer closed with the request inflight | `PeerClosed` | `"peer closed"` |
| `stop()` while inflight | `Cancelled` | `"cancelled by stop"` |
| `request()` after `stop()` | `ShuttingDown` | `"client stopped"` |
| local decode failure on the session | `ProtocolError` | `"protocol error"` |
| socket directory or peer check failed | `PermissionDenied` | `"permission denied"` |

`IpcError::RemoteError` is the only value that carries a server-chosen string, which is why the
public enum of spec section 6 needs no new enumerator for `UNKNOWN_METHOD` or `UNAUTHENTICATED`.

## 7. Notification delivery

- A notification is sent only to sessions whose `topics` set contains the topic, i.e. only after a
  successful `ipc.subscribe` (spec section 8.7).
- Frame order **within one session** is the enqueue order. No ordering is promised across sessions.
- Delivery is best-effort, at-most-once, per session: a notification that does not fit the session's
  TX caps is dropped and counted (`ipc_queue_rejects_total`, research R-08), and notifications
  produced while a client is disconnected are lost (spec section 8.4).

## 8. Explicitly not in this contract (v1)

Taken from spec section 3.3 and section 17.1, listed here so no implementation task adds them:

- No multi-host transport, no shared memory, no POSIX message queues.
- No `flags` bits - any nonzero value is a protocol error, which is what reserves them for v2.
- No compression, no encryption, no authentication token (the trust boundary is filesystem
  permissions plus `SO_PEERCRED`).
- No exactly-once delivery, no durable replay, no automatic RPC retry.
- No schema for `payload`: the library does not parse it (research R-01).
- No `SOCK_SEQPACKET` variant.
