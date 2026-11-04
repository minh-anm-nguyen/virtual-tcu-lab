# Phase 1 Data Model: IPC Service Framework

Entities, states and invariants derived from spec sections 5 (wire format), 6 (public API), 7
(concurrency and state machines) and 11 (metrics). Nothing here adds a field the spec does not
require. Invariant IDs (`W*`, `C*`, `S*`, `P*`, `M*`) are referenced by the tasks and by the tests.

---

## 1. Frame header

The on-wire layout of spec section 5.1, byte for byte. Big-endian for every multi-byte field.

| Offset | Size | Field | Type | v1 constraint |
|---:|---:|---|---|---|
| 0 | 4 | `body_length` | `uint32` | `14 <= body_length <= 65536` |
| 4 | 1 | `version` | `uint8` | `== 1` |
| 5 | 1 | `type` | `uint8` | `1..4` |
| 6 | 2 | `flags` | `uint16` | `== 0` |
| 8 | 8 | `request_id` | `uint64` | nonzero for Request/Response/Error; `0` for Notification |
| 16 | 2 | `name_length` | `uint16` | `name_length <= body_length - 14` |
| 18 | N | `name` | bytes | method or topic, UTF-8, `1 <= N` |
| 18+N | M | `payload` | bytes | opaque to the library (R-01) |

In C++ this is **not** a packed struct; it is encoded and decoded field by field so no endianness or
alignment assumption leaks in:

```cpp
struct FrameHeader {
    std::uint32_t body_length{0};
    std::uint8_t  version{kProtocolVersion};  // 1
    FrameType     type{FrameType::Request};
    std::uint16_t flags{0};
    std::uint64_t request_id{0};
    std::uint16_t name_length{0};
};
```

**Derived constants**: `kHeaderPrefix = 4`, `kFixedBodyFields = 14`, `kFrameOverhead = 18`,
`kMaxBodyLength = 65536`, `kMaxFrameBytes = kHeaderPrefix + kMaxBodyLength = 65540`.

### Invariants

| ID | Invariant | Enforced where |
|---|---|---|
| W1 | `body_length` is validated **before** any allocation sized by it | `FrameCodec::tryDecode` reads the 4-byte prefix, compares to `kMaxBodyLength`, and fails without reserving |
| W2 | `body_length == kFixedBodyFields + name_length + payload_size` exactly; a mismatch is `PROTOCOL_ERROR` | decoder, after the body is complete |
| W3 | `name_length` never exceeds the remaining body; no `substr` is computed from an unchecked length | decoder |
| W4 | `version != 1` is `UNSUPPORTED_VERSION`; `flags != 0` and `type` outside `1..4` are `PROTOCOL_ERROR` | decoder |
| W5 | `request_id == 0` iff `type == Notification` | decoder, and encoder by construction |
| W6 | A decode either consumes a whole frame or consumes nothing and reports "need more bytes" | `tryDecode` returns `Incomplete` without mutating the buffer offset |
| W7 | `name` matches `[A-Za-z0-9_.-]{1,255}` and contains no NUL | decoder, before the name reaches any map lookup |

W6 is what B-04 and B-05 test; W1 is what B-06 tests.

---

## 2. Enumerations

### `FrameType` (wire, `uint8`)

`Request = 1`, `Response = 2`, `Notification = 3`, `Error = 4`. Closed set - section 5.1.

### `IpcError` (public, section 6)

Exactly the ten values the spec declares, unchanged:

`None`, `NotConnected`, `Timeout`, `Cancelled`, `QueueFull`, `PermissionDenied`, `ProtocolError`,
`PeerClosed`, `RemoteError`, `ShuttingDown`.

### Wire error codes (payload of an `Error` frame, R-11)

`INVALID_ARGUMENT`, `UNKNOWN_METHOD`, `PERMISSION_DENIED`, `UNAUTHENTICATED`,
`UNSUPPORTED_VERSION`, `FRAME_TOO_LARGE`, `PROTOCOL_ERROR`, `INTERNAL_ERROR`, `SHUTTING_DOWN`.
These are strings on the wire and are mapped to `IpcError::RemoteError` + `detail` on the client.

---

## 3. Public value types

```cpp
struct IpcResponse {
    IpcError    error{IpcError::None};
    std::string payload_json;   // opaque bytes; empty unless error == None
    std::string detail;         // wire error code, or a local reason; never a payload excerpt
};

struct ClientOptions {
    std::string               socket_path{"/tmp/virtual-tcu/vehicle.sock"};
    std::chrono::milliseconds request_timeout{2000};
    std::size_t               max_pending_requests{128};
    std::size_t               max_tx_messages{256};
    std::size_t               max_tx_bytes{1024 * 1024};
    bool                      reconnect_enabled{true};
    std::string               client_id{};        // sent in ipc.hello; defaults to pid
};

struct ServerOptions {
    std::string             socket_path{"/tmp/virtual-tcu/vehicle.sock"};
    std::vector<uid_t>      allowed_uids{};       // empty => {geteuid()} (R-10)
    std::size_t             max_tx_messages{256};
    std::size_t             max_tx_bytes{1024 * 1024};
    std::size_t             max_sessions{32};
};
```

`ClientOptions` is spec section 6 verbatim plus `client_id`, which section 8.1's HELLO already
requires on the wire. `ServerOptions` replaces section 6's bare `std::string socket_path`
constructor argument; the constructor taking a path is kept as a convenience overload so the spec's
API snippet stays valid.

### Invariants

| ID | Invariant |
|---|---|
| P1 | `request_timeout > 0`; `max_pending_requests > 0`; `max_tx_messages > 0`; `max_tx_bytes >= kMaxFrameBytes`. A violation makes `start()` return `false` and log the reason, never UB. |
| P2 | `IpcResponse::detail` never contains payload bytes (Principle V). |
| P3 | `allowed_uids` empty means "owner only", not "anyone". |

---

## 4. Client state machine

States of spec section 7.1: `Stopped`, `Connecting`, `Connected`, `Backoff`, `Stopping`. One
`std::atomic<ClientState>` owned by `IpcClient`, written only by the I/O thread except for the
`Stopping` transition, which `stop()` performs from any thread.

| From | Event | To |
|---|---|---|
| `Stopped` | `start()` | `Connecting` |
| `Connecting` | connect + HELLO_ACK ok | `Connected` |
| `Connecting` | connect error, or HELLO timeout | `Backoff` |
| `Connecting` | `UNSUPPORTED_VERSION` | `Stopping` then `Stopped` (terminal, R-06) |
| `Connected` | EOF, `EPIPE`, `ECONNRESET`, protocol violation | `Backoff` |
| `Backoff` | retry timer elapsed | `Connecting` |
| any | `stop()` | `Stopping` then `Stopped` |

**Backoff schedule** (section 3.2): 100, 200, 400, 800, 1600, 3200, then capped at 5000 ms. Reset to
100 ms on a successful HELLO_ACK. No jitter in v1 - section 3.2 marks it "trong ban nang cao".

### Invariants

| ID | Invariant | Tested by |
|---|---|---|
| C1 | `Connected` is reached only after HELLO_ACK, never after a bare `connect()` | B-01 |
| C2 | A version mismatch does not re-enter `Connecting` | B-19 |
| C3 | Every transition out of `Connected`/`Connecting` resolves all pending requests of that generation exactly once | B-08, B-15 |
| C4 | `generation` increments on every entry into `Connected`; frames tagged with an older generation are dropped | R-12, B-12 |
| C5 | `stop()` from any state terminates in bounded time and is idempotent | B-15, B-16 |

---

## 5. Request state machine

States of spec section 7.2: `Created`, `Queued`, `Sent`, `Completed`, `TimedOut`, `Failed`.
Represented not as an enum but by *where the entry lives*: an entry in `pending_requests` with
`sent == false` is `Queued`, one with the frame fully written is `Sent`, and the terminal states are
the act of erasing the entry and resolving its promise.

```cpp
struct PendingRequest {
    std::uint64_t                         request_id{0};
    std::uint64_t                         generation{0};
    std::string                           method;
    std::chrono::steady_clock::time_point deadline{};
    std::promise<IpcResponse>             promise;
    bool                                  sent{false};
};
```

### Invariants

| ID | Invariant | Tested by |
|---|---|---|
| S1 | A `PendingRequest` is resolved exactly once; the resolver is the thread that erased it from the map (R-03) | UT `Client_TimeoutCompletesExactlyOnce` |
| S2 | The deadline runs from request creation, including queue time | UT `Client_TimeoutCompletesExactlyOnce`, B-07 |
| S3 | `pending_requests.size() <= max_pending_requests`; over the cap, `request()` resolves `QueueFull` immediately | B-10 |
| S4 | A response whose `request_id` is absent from the map is dropped and counted, never matched to another entry | B-12 |
| S5 | `request_id` is unique among live entries and is never reused while pending (R-12) | UT `Client_RequestIdUniqueWhileInFlight` |
| S6 | No promise is resolved while any internal mutex is held | TSan job, review checklist |

---

## 6. Session (server side)

```cpp
struct ClientSession {
    std::uint64_t           id{0};            // monotonic, never reused
    UnixSocket              socket;           // sole owner of the FD (RAII)
    uid_t                   peer_uid{0};
    pid_t                   peer_pid{0};
    bool                    hello_done{false};
    std::string             client_id;
    std::vector<char>       rx_buffer;        // bounded by kMaxFrameBytes * 2
    std::deque<std::string> tx_queue;         // encoded frames
    std::size_t             tx_offset{0};     // bytes of tx_queue.front() already written
    std::size_t             tx_bytes{0};      // sum of unsent bytes, for the cap
    bool                    tx_full_logged{false};
    std::set<std::string>   topics;           // subscriptions, per session (R-07)
};
```

### Invariants

| ID | Invariant | Tested by |
|---|---|---|
| S10 | Exactly one `UnixSocket` owns a given FD; closing happens once, in its destructor | ASan plus FD-count assertions |
| S11 | `tx_bytes <= max_tx_bytes` and `tx_queue.size() <= max_tx_messages` at all times; the frame that would breach either is dropped and counted (R-08) | UT `Session_SlowConsumerDoesNotGrowMemory`, B-13 |
| S12 | `rx_buffer` never grows past two maximum frames; a frame that cannot fit is `FRAME_TOO_LARGE` and closes the session | B-06 |
| S13 | A session with `hello_done == false` may invoke only `ipc.hello` | B-01, B-19 |
| S14 | Session state is touched only by the I/O thread; handlers receive copies and return values | TSan job |
| S15 | Closing a session drops its subscriptions and affects no other session | B-13 |

---

## 7. Request router (server side)

```cpp
struct RequestRouter {
    std::unordered_map<std::string, IpcServer::Handler> handlers;  // method -> handler
};
```

Lookup order for an incoming `Request`: reserved `ipc.*` control methods first, then `handlers`. A
miss is `UNKNOWN_METHOD`. A handler that throws is caught at the executor boundary and becomes
`INTERNAL_ERROR` (B-11); the exception never crosses the I/O thread.

### Invariants

| ID | Invariant | Tested by |
|---|---|---|
| R1 | `registerHandler` rejects a name in the reserved `ipc.` namespace and rejects a duplicate | UT `Server_RejectUnauthorizedMethod` |
| R2 | A handler runs on the executor, never on the I/O thread | UT with thread-id assertion |
| R3 | Every `Request` produces exactly one `Response` or one `Error` frame, or the session is closed | B-02, B-11, B-18 |

---

## 8. Subscription (client side)

```cpp
struct SubscriptionRecord {       // client-side record, not a handle type
    std::string                      topic;
    std::function<void(std::string)> callback;
    bool                             active{false};   // true only after ACK (R-07)
};
```

`subscribe()` stores the record and, if `Connected`, sends `ipc.subscribe`. On reconnect every
record is re-sent and `active` is cleared until its ACK arrives.

### Invariants

| ID | Invariant | Tested by |
|---|---|---|
| B1 | A notification is delivered to a callback only while that topic's record exists | B-09 |
| B2 | `active` is false between a disconnect and the re-subscribe ACK; notifications in that window are lost, by design (section 8.4) | B-09 |
| B3 | Callbacks run on the executor, never on the I/O thread | section 6 API comment, UT thread-id assertion |

---

## 9. Metrics

One `IpcMetricsSnapshot` per `IpcClient` and per `IpcServer`, mirroring spec section 11 exactly.
Counters are `std::atomic<std::uint64_t>` internally and a plain struct when snapshotted.

```cpp
struct IpcMetricsSnapshot {
    std::uint64_t connections_active{0};      // gauge
    std::uint64_t connect_attempts_total{0};
    std::uint64_t reconnect_total{0};
    std::uint64_t requests_created_total{0};
    std::uint64_t requests_sent_total{0};
    std::uint64_t requests_completed_total{0};
    std::uint64_t requests_failed_total{0};
    std::uint64_t request_timeouts_total{0};
    std::uint64_t queue_rejects_total{0};
    std::uint64_t protocol_errors_total{0};
    std::uint64_t late_responses_total{0};
    std::uint64_t tx_queue_bytes{0};          // gauge
    LatencyHistogram request_latency_ms{};    // fixed buckets, see contracts/metrics.md
};
```

`requests_created_total` and `requests_failed_total` are not in spec section 11's table; they are the
two terms section 11's own conservation sentence needs ("tong `completed + timeouts + failures` chi
khop `created`"), so the table's metrics cannot be checked without them.

### Invariants

| ID | Invariant |
|---|---|
| M1 | `requests_completed_total + request_timeouts_total + requests_failed_total == requests_created_total` once no request is outstanding (section 11's conservation rule). |
| M2 | Each request contributes to exactly one terminal counter - a direct consequence of S1. |
| M3 | `tx_queue_bytes` returns to 0 after a clean `stop()`. |
| M4 | `connections_active` returns to 0 after a clean server `stop()`. |
| M5 | No metric label or log field carries payload bytes, a token or a credential (Principle V). |

The exact conservation equations and the tests that assert them are in
[contracts/metrics.md](./contracts/metrics.md).

---

## 10. Structured log fields

Fixed set, from spec section 11: `timestamp`, `level`, `service`, `connection_id`, `request_id`,
`method`, `event`, `reason`, `duration_ms`. A log line is `key=value` pairs, one line, no payload
field. `method` is bounded to 255 bytes by W7, so no truncation logic is needed.
