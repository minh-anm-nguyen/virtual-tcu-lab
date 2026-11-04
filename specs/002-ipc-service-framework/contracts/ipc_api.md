# Contract: `tcu::ipc` public API

Preconditions, postconditions and thread-safety for every public entry point. Derived from spec
section 6 (API), section 7 (concurrency, ownership, lock discipline), section 8 (operational
behavior) and section 19 (review checklist). The wire-level half of the contract is in
[wire_protocol.md](./wire_protocol.md).

Public headers: `tcu/ipc/ipc_types.hpp`, `tcu/ipc/frame_codec.hpp`, `tcu/ipc/ipc_client.hpp`,
`tcu/ipc/ipc_server.hpp`. Everything under `modules/ipc/src/` is private and not installed.

**Global rules, true for every function below:**

- No public function invokes a user callback or a handler while holding an internal lock
  (spec section 7, "Lock discipline").
- No public function blocks on I/O. The only blocking calls are `stop()` and the destructors, which
  join threads.
- A callback or handler never runs on an I/O thread (spec section 4.1).
- No function throws for a remote or peer condition; those are reported through `IpcError`.
  `std::bad_alloc` and exceptions from user code are the only exceptions that can escape, and user
  exceptions are caught at the executor boundary.

---

## `IpcClient::IpcClient(ClientOptions options)`

**Preconditions**: none. Invalid option values are not rejected here.

**Postconditions**: state is `Stopped`; no thread, no FD, no socket exists yet.

**Thread safety**: not applicable - construction is single-threaded by definition.

---

## `bool IpcClient::start()`

**Preconditions**: state is `Stopped`. Calling it in any other state returns `false` and changes
nothing.

**Postconditions on `true`**: the I/O thread and the executor thread are running, the state is
`Connecting`, and the self-pipe FD pair exists. `true` does **not** mean connected - spec section
8.1: "`start()` tra ve sau khi khoi dong worker, con trang thai `Connected` chi co khi handshake xac
thuc protocol thanh cong".

**Returns `false`** when an option violates invariant P1 (`request_timeout <= 0`,
`max_pending_requests == 0`, `max_tx_messages == 0`, `max_tx_bytes < 65540`), or when a thread or
the self-pipe cannot be created. The reason is logged once; the object stays usable in `Stopped`.

**Thread safety**: safe to call from any one thread. Concurrent `start()` calls are serialised and
all but one return `false`.

---

## `std::future<IpcResponse> IpcClient::request(std::string method, std::string payload_json, std::chrono::milliseconds timeout)`

**Preconditions**: `method` is non-empty, at most 255 bytes, matches `[A-Za-z0-9_.-]+`, and does not
start with `ipc.`. `payload_json.size() + method.size() + 14 <= 65536`. `timeout > 0`; a
non-positive value is replaced by `ClientOptions::request_timeout`.

**Postconditions**: a future is returned, **always**, and is resolved exactly once (invariant S1).
The call does not block on the network. The deadline starts now, before the frame is queued
(invariant S2, spec section 7.2).

| Condition at call time | Future resolves with |
|---|---|
| state `Connected`, caps free | the peer's `Response` or `Error`, or `Timeout` |
| state `Connecting` or `Backoff` | `NotConnected` - v1 does not queue across a disconnect |
| state `Stopping` or `Stopped` | `ShuttingDown` (B-17) |
| `pending_requests` at `max_pending_requests` | `QueueFull` (invariant S3) |
| TX queue at `max_tx_messages` or `max_tx_bytes` | `QueueFull` (B-10) |
| precondition on `method`/size violated | `ProtocolError` locally, `detail="invalid request"` |

**Never**: returns an invalid future, blocks longer than one mutex acquisition, or retries. An RPC is
sent at most once per call - spec section 8.4 and B-20 forbid automatic replay.

**Thread safety**: callable concurrently from any number of threads.

---

## `void IpcClient::subscribe(std::string topic, std::function<void(std::string)> callback)`

**Preconditions**: `topic` satisfies the same name rules as `method` and does not start with `ipc.`.
`callback` is non-empty. Subscribing to a topic already subscribed replaces the callback.

**Postconditions**: the record exists for the lifetime of the client (research R-07). If the state is
`Connected`, `ipc.subscribe` is sent; the record becomes `active` only when its ACK arrives, and is
re-sent automatically after every reconnect.

**Callback contract**: invoked on the executor thread, never on the I/O thread, never with a lock
held, once per received notification for that topic. An exception thrown by the callback is caught,
counted and logged; it does not reach the I/O loop and does not close the connection.

**Thread safety**: callable from any thread, including from inside another callback.

---

## `void IpcClient::stop()`

**Preconditions**: none. Callable in any state, from any thread, any number of times.

**Postconditions** (spec section 8.9, research R-09):

1. State is `Stopped`.
2. Every pending request has been resolved exactly once with `Cancelled`.
3. The I/O and executor threads are joined; the socket, the self-pipe and every session FD are
   closed exactly once.
4. `tx_queue_bytes` is 0 (invariant M3).

**Bounded**: returns within the executor's current task plus one `poll()` wake. The self-pipe makes
the wake immediate rather than waiting out the 1000 ms clamp (research R-04).

**Idempotent**: the second and later calls return without touching an FD (B-15). Concurrent with a
reconnect attempt it does not deadlock (B-16).

**Must not** be called from the executor thread (that would self-join). A handler or callback that
wants to stop the client schedules the call on another thread.

---

## `ClientState IpcClient::state() const` and `IpcMetricsSnapshot IpcClient::metrics() const`

**Preconditions**: none. **Postconditions**: a value copy; no lock is held on return. The snapshot is
not an atomic view across all counters - each counter is read atomically, the set is not. The
conservation equations of [metrics.md](./metrics.md) therefore hold only when no request is
outstanding.

---

## `IpcClient::~IpcClient()`

Calls `stop()`, then releases. A pending future held by a caller stays valid - it was already
resolved by `stop()`. Destroying the client while a callback is running is safe: the destructor joins
the executor, so the callback has returned before any member is destroyed.

---

## `IpcServer::IpcServer(ServerOptions options)` / `IpcServer::IpcServer(std::string socket_path)`

**Preconditions**: none. The path overload exists to keep spec section 6's snippet valid and fills
the remaining options with their defaults.

**Postconditions**: no socket, no thread, no directory is created yet. The filesystem is untouched
until `start()`.

---

## `bool IpcServer::registerHandler(std::string method, Handler handler)`

**Preconditions**: state is `Stopped` - handlers are registered before `start()`. `method` satisfies
the name rules and does **not** begin with `ipc.` (reserved, research R-06). `handler` is non-empty.

**Postconditions on `true`**: the method is routable. **Returns `false`** for a reserved prefix, a
duplicate method, an invalid name, an empty handler, or a call after `start()`. Nothing is
registered partially (invariant R1).

**Handler contract**: runs on the executor thread with a copy of the payload; returns an
`IpcResponse`. Returning `error != None` produces an `Error` frame with the mapped code. Throwing
produces `INTERNAL_ERROR` and the server stays alive (B-11). A handler must not call
`IpcServer::stop()` directly.

---

## `bool IpcServer::start()`

**Preconditions**: state is `Stopped`.

**Postconditions on `true`**: the runtime directory exists with mode `0700` owned by the effective
UID, the socket is bound and listening, and the I/O and executor threads are running.

**Returns `false`**, with nothing left behind, when (research R-10):

- the runtime directory exists but is not owned by the effective UID, or has any group/other
  permission bit set - the server does not "fix" it;
- `bind()` fails `EADDRINUSE` and a probe `connect()` to the path succeeds, meaning a live server
  owns it;
- `bind()` fails for any other reason;
- an option violates invariant P1.

A stale socket - `EADDRINUSE` plus a probe that fails `ECONNREFUSED` - is unlinked and `bind()` is
retried exactly once.

---

## `void IpcServer::publishNotification(std::string topic, std::string payload_json)`

**Preconditions**: `topic` satisfies the name rules and does not begin with `ipc.`.
`payload_json.size() + topic.size() + 14 <= 65536`; an oversize payload is dropped, counted and
logged rather than truncated.

**Postconditions**: the frame is enqueued for every session currently subscribed to `topic`. Sessions
whose TX caps are full have it dropped and counted (research R-08). No return value - delivery is
best-effort by design (spec section 8.7).

**Non-blocking**: never waits for a socket to become writable, and never blocks on a slow consumer.

**Thread safety**: callable from any thread, including from inside a handler.

---

## `void IpcServer::stop()`

**Preconditions**: none; callable in any state, from any thread, repeatedly.

**Postconditions**:

1. New connections are no longer accepted and new requests are refused `SHUTTING_DOWN`.
2. Every session is closed exactly once; `connections_active` is 0 (invariant M4).
3. Both threads are joined.
4. The socket file is unlinked **only if this server created it** - a server that failed to bind
   never unlinks a path it does not own.

**Idempotent and bounded**, under the same rules as `IpcClient::stop()`.

---

## `FrameCodec` (public, for tests and for a peer implementation)

`encode(const FrameHeader&, std::string_view name, std::string_view payload) -> std::string` and
`tryDecode(std::string_view buffer, Frame& out, std::size_t& consumed) -> DecodeResult`. Both are
pure functions with no I/O, no allocation beyond the output, and no global state; they are safe to
call concurrently on distinct buffers. The full decoder contract is section 4 of
[wire_protocol.md](./wire_protocol.md).

---

## Explicitly not in this contract (v1)

From spec section 3.3, section 6's API notes and section 17.1:

- No `shared_future`, no cancellation token, no typed `Result<T>` - section 6 marks those as
  production improvements, and `std::future` is what the snippet declares.
- No synchronous `requestSync()` helper - a caller that wants blocking calls `.get()`.
- No request queueing across a disconnect, no automatic RPC replay, no idempotency key.
- No `unsubscribe()` on the client beyond replacing the callback; `ipc.unsubscribe` exists on the
  wire for a future client API but is not exposed as a public method in v1.
- No server-side per-UID method ACLs beyond the connection-level UID allowlist.
- No connection pooling, no multiplexing of several servers behind one client object.
