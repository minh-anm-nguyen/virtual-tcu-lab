# Contract: metrics, logs and observable acceptance

Spec section 11 lists the metrics and the log fields and states one conservation rule in prose. This
file turns both into checkable equations and names the test that asserts each one, so the acceptance
criteria of section 13 are observable rather than asserted.

Metric names for an observability system are the `ipc_`-prefixed names of section 11. In C++ they are
the fields of `IpcMetricsSnapshot`; the mapping is one to one and is listed below.

---

## Counters

| Spec name | Snapshot field | Incremented when | Never |
|---|---|---|---|
| `ipc_connect_attempts_total` | `connect_attempts_total` | each `connect()` syscall is issued, success or failure | not incremented for a retry timer that has not fired |
| `ipc_reconnect_total` | `reconnect_total` | a HELLO_ACK completes on a connection that is not the first | incremented for the first connect |
| `ipc_requests_sent_total` | `requests_sent_total` | a request frame's **last** byte reaches the socket | incremented for a partially written frame |
| `ipc_requests_completed_total` | `requests_completed_total` | a `Response` or `Error` frame resolves a pending request | incremented for a late or unmatched response |
| `ipc_request_timeouts_total` | `request_timeouts_total` | a deadline sweep resolves a request `Timeout` | incremented twice for one request |
| `ipc_queue_rejects_total` | `queue_rejects_total` | a frame is refused or dropped by a TX cap, on either side | incremented when the frame is merely delayed |
| `ipc_protocol_errors_total` | `protocol_errors_total` | the decoder returns `ProtocolError` or `TooLarge` | incremented for an application-level `INVALID_ARGUMENT` |
| `ipc_late_responses_total` | `late_responses_total` | a response arrives whose `request_id` is not pending, in the current generation | incremented for a frame from an older generation (that is a stale session, not a late response) |

Two counters are not in section 11's table but are required by its own conservation sentence
("tong `completed + timeouts + failures` chi khop `created`"):

| Snapshot field | Incremented when |
|---|---|
| `requests_created_total` | `request()` registers a pending entry, **or** resolves immediately with `QueueFull`/`NotConnected`/`ShuttingDown` - every call counts exactly once |
| `requests_failed_total` | a request is resolved with anything other than `None`, `RemoteError` or `Timeout`: `Cancelled`, `PeerClosed`, `QueueFull`, `NotConnected`, `ShuttingDown`, `ProtocolError`, `PermissionDenied` |

A `RemoteError` outcome counts as **completed**: the peer answered, and the answer was an error. That
is the distinction section 11 draws by listing `requests_completed_total` as "Response matched hop
le".

## Gauges

| Spec name | Snapshot field | Meaning |
|---|---|---|
| `ipc_connections_active` | `connections_active` | server: live sessions. client: 0 or 1 |
| `ipc_tx_queue_bytes` | `tx_queue_bytes` | sum of unsent bytes across all TX queues of this object |

## Histogram

`ipc_request_latency_ms` / `request_latency_ms`: fixed buckets, in milliseconds, with upper bounds
`{1, 2, 5, 10, 25, 50, 100, 250, 500, 1000, 2000, +inf}`. Observed from request **creation** to
resolution, measured with `steady_clock`, and recorded only for requests that resolve `None` or
`RemoteError` - a timeout's latency is its deadline by definition and would flatten the tail.

The buckets are fixed rather than configurable because nothing in the spec asks for a target
latency (research R-17, question 7); they exist to make the demo observable, not to assert a budget.

---

## Conservation equations

Let a quiescent client mean: `stop()` has returned, or no request is outstanding and no frame is in
flight. Then:

```text
E1  requests_created_total
      == requests_completed_total + request_timeouts_total + requests_failed_total

E2  requests_sent_total <= requests_created_total
      (a request refused by a cap, or cancelled while queued, is never sent)

E3  tx_queue_bytes == 0

E4  connections_active == 0                        (after a server stop)

E5  late_responses_total + requests_completed_total
      <= number of Response/Error frames decoded   (each frame does one or the other, or is stale)
```

E1 is the equation section 11 states in prose and is the one that makes "resolve exactly once"
(invariant S1) observable: if any request were resolved twice, a terminal counter would be one too
high; if any were never resolved, the sum would be short.

E3 is how a leaked TX buffer is caught - a dropped frame decrements `tx_queue_bytes`, a forgotten one
does not.

---

## Tests that assert these

| Equation / rule | Test |
|---|---|
| E1, after a mixed run of successes, timeouts and queue rejects | `tests/unit/ipc/metrics_test.cpp` - `Metrics_ConservationAfterMixedOutcomes` |
| E1 under concurrency | `tests/fault_injection/ipc/stress_test.cpp` - 8 clients, asserted at the end |
| E2 | `Metrics_QueueRejectIsNeverSent` |
| E3 | asserted in the teardown of every integration test fixture |
| E4 | `tests/integration/ipc/server_lifecycle_test.cpp` |
| E5 | `Client_LateResponseIgnored` plus `Metrics_LateResponseCounted` |
| timeouts counted once | `Client_TimeoutCompletesExactlyOnce` |
| protocol errors counted, not swallowed | `FrameCodec_*` suite plus `Session_ProtocolErrorClosesSession` |
| no FD leak across kill/restart cycles | `tests/fault_injection/ipc/rapid_restart_test.cpp` compares the `/proc/self/fd` count before and after |

The FD-count check is the acceptance criterion "khong FD leaks ro rang trong test" (section 13) made
executable: it counts entries in `/proc/self/fd` at fixture setup and at teardown and requires
equality, which catches a double-open as well as a missing close.

---

## Structured log fields

Exactly the fields of spec section 11, in this order, as `key=value` pairs on one line:

```text
timestamp=2026-11-04T09:12:33.481Z level=info service=vehicle_service connection_id=7
  request_id=42 method=vehicle.getStatus event=request_completed reason=- duration_ms=3
```

(one physical line; wrapped here only to fit the page)

| Field | Type | Absent as |
|---|---|---|
| `timestamp` | ISO 8601 UTC, milliseconds | never absent |
| `level` | `debug`/`info`/`warn`/`error` | never absent |
| `service` | the process name | never absent |
| `connection_id` | session id, or `-` on the client before connect | `-` |
| `request_id` | the correlation id, or `-` for a notification | `-` |
| `method` | method or topic name, bounded to 255 bytes by invariant W7 | `-` |
| `event` | a fixed vocabulary, below | never absent |
| `reason` | an error code or a short cause | `-` |
| `duration_ms` | integer, for events that measure a span | `-` |

**`event` vocabulary** (closed, so logs are greppable): `listening`, `accepted`, `rejected_peer`,
`handshake_ok`, `handshake_failed`, `request_received`, `request_completed`, `request_failed`,
`request_timeout`, `late_response`, `notification_sent`, `notification_dropped`, `tx_queue_full`,
`protocol_error`, `peer_closed`, `state_changed`, `reconnecting`, `subscription_active`,
`subscription_restored`, `stopping`, `stopped`.

### What must never appear in a log line

Principle V, made specific:

- No `payload` field, and no payload bytes inside `reason` or `message`.
- No token, password, key or certificate - the protocol has none, so any such string in a log is a
  defect by construction.
- No peer UID or PID except in the single `rejected_peer` line, where they are the audit record
  that B-14 requires.

A log line is written by the component that owns the state it reports; the I/O thread never formats
a payload, so a payload cannot leak through a log path that does not exist.
