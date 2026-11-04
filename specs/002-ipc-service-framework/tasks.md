---

description: "Task list for IPC Service Framework (project 02)"
---

# Tasks: IPC Service Framework

**Input**: Design documents from `/specs/002-ipc-service-framework/`

**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/, quickstart.md - all present.

**Tests**: REQUIRED, not optional. Constitution Principle III is non-negotiable: every FR gets a unit
test naming it, every BDD scenario B-01..B-20 gets a test, and the test is written and seen to FAIL
before the implementation that satisfies it.

**Platform**: every build and test command runs inside WSL2 Ubuntu under `/mnt/d/TCU` (research
R-00). Project 02 does not compile on Windows. Socket paths must be under `/tmp`, never on `/mnt`
(`bind()` fails `EOPNOTSUPP`).

**Organization**: phases follow the ticket dependency chain of spec section 14,
`001 -> 002 -> 003 -> 004/005 -> 006/007 -> 008/009 -> 010/011 -> 012`.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: can run in parallel - different files, no dependency on an incomplete task
- **[Story]**: US1..US6, mapped to the backlog groups below
- Every description carries its exact file path

## Story-to-ticket mapping

| Story | Delivers | Backlog tickets (spec section 14) | Tests delivered |
|---|---|---|---|
| US1 (P1) | Framing primitives: a frame survives any read/write split | IPC-001, IPC-002, IPC-003 | `FrameCodec_*` (8 cases), W1-W7, B-04, B-05, B-06 |
| US2 (P2) | Request/response between two processes | IPC-004, IPC-005 | integration 1, B-01, B-02, B-03, B-11, B-18, B-19 |
| US3 (P3) | Deadlines and backpressure | IPC-006, IPC-007 | B-07, B-10, B-12, B-13, B-15, B-17 |
| US4 (P4) | Reconnect and notifications - the spec section 1 demo goal | IPC-008, IPC-009 | integration 2, 3, 4, 5, B-08, B-09, B-16, B-20 |
| US5 (P5) | Peer authorization and observability | IPC-010, IPC-011 | integration 6, B-14, FR-12, E1-E5 |
| US6 (P6) | Fault injection and stress suite | IPC-012 | fault matrix rows, sanitizer jobs |

**Deviation from the template, stated deliberately**: these six stories are **layers, not independent
features**, exactly as spec section 14's dependency chain declares. US2 cannot be tested without the
codec US1 builds; US3's timeout needs US2's request path; US4's reconnect needs US3's pending-map
cleanup to prove B-08. So the template's "all stories can run in parallel once Foundational is done"
does **not** apply. Each story's checkpoint is a working increment, but the order is fixed.
Parallelism exists *within* phases, marked `[P]`.

**Metrics note**: the atomic counters are declared in Foundational (T009) and each story increments
the counters it owns, because B-10 and B-12 in US3 both assert counter values and cannot wait for
US5. US5 completes the set, adds the structured logging and asserts the conservation equations
E1-E5. This splits ticket IPC-011 across US3 and US5; that is intentional, not an oversight.

**Demo apps note**: `apps/vehicle_service` and `apps/tcu_client` are built incrementally rather than
in one task, because the integration tests fork the real server binary. US2 creates the minimal pair,
US4 adds the ignition notification and the project 01 event bus, US5 adds `--allow-uid` and
`--slow-ms`. Ticket IPC-004's "Multi-client demo" acceptance is therefore met at the US4 checkpoint.

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: directory structure and build wiring, so a test can be compiled before any library code
exists

- [X] T001 Create the `modules/ipc/` tree - `include/tcu/ipc/`, `src/` - and write `modules/ipc/CMakeLists.txt` declaring a static library target `tcu_ipc` with `target_include_directories(tcu_ipc PUBLIC include)`, C++17, no extensions, and `-Wall -Wextra` on GCC/Clang
- [X] T002 Append one Linux-guarded block to the root `CMakeLists.txt` after the project 01 lines: `if(CMAKE_SYSTEM_NAME STREQUAL "Linux")` containing `add_subdirectory(modules/ipc)`, the two app directories and, inside the existing `TCU_BUILD_TESTS` block, the three test directories; `else()` print `message(STATUS "Project 02 (IPC) skipped: Linux only")`. Do not restructure the existing project 01 lines
- [X] T003 [P] Create `tests/unit/ipc/CMakeLists.txt` building one `ipc_unit_tests` target linked against `tcu_ipc` and `gtest_main`, registered with `gtest_discover_tests(... PROPERTIES LABELS "ipc-unit")`
- [X] T004 [P] Create `tests/integration/ipc/CMakeLists.txt` building `ipc_integration_tests` with label `ipc-integration`, and add a dependency on the `vehicle_service` target so the binary exists before the tests run
- [X] T005 [P] Create `tests/fault_injection/ipc/CMakeLists.txt` building `ipc_fault_tests` with label `ipc-fault`
- [X] T006 [P] Create `tests/unit/ipc/support/temp_socket_dir.hpp`: an RAII helper that makes a unique directory under `/tmp` with mode `0700`, exposes `path()` and `socket_path()`, and removes the tree in its destructor. It must refuse to operate on a path under `/mnt` (research R-00, note 2)
- [X] T007 [P] Create `tests/unit/ipc/support/raw_peer.hpp`: a raw `AF_UNIX` `SOCK_STREAM` peer owning its FD, with `connect(path)`, `writeBytes(span)`, `writeByteByByte(span)`, `readAvailable(timeout)` and `close()`, so a test can emit chosen byte sequences (research R-13)

**Checkpoint**: `cmake -S . -B build -G Ninja && cmake --build build` configures and builds the
project 01 targets plus three empty project 02 test targets.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: the types, the RAII socket, the executor and the logger that every story needs

**CRITICAL**: no user story work can begin until this phase is complete

- [X] T008 Create `modules/ipc/include/tcu/ipc/ipc_types.hpp` with the public value types of data-model.md section 3: `enum class IpcError` holding **exactly** the ten values `None, NotConnected, Timeout, Cancelled, QueueFull, PermissionDenied, ProtocolError, PeerClosed, RemoteError, ShuttingDown`; `enum class ClientState { Stopped, Connecting, Connected, Backoff, Stopping }`; `struct IpcResponse`; `struct ClientOptions` with defaults `socket_path="/tmp/virtual-tcu/vehicle.sock"`, `request_timeout=2000ms`, `max_pending_requests=128`, `max_tx_messages=256`, `max_tx_bytes=1048576`, `reconnect_enabled=true`; `struct ServerOptions` with `allowed_uids` empty meaning owner-only and `max_sessions=32`; `struct IpcMetricsSnapshot` with the twelve fields of data-model.md section 9. Add no enumerator beyond those listed
- [X] T009 [P] Create `modules/ipc/src/metrics.hpp`: an internal `Metrics` struct of `std::atomic<std::uint64_t>` counters mirroring `IpcMetricsSnapshot`, a `LatencyHistogram` with the fixed upper bounds `{1,2,5,10,25,50,100,250,500,1000,2000,+inf}` ms, and `snapshot()`. Per-counter atomic reads only - the snapshot is explicitly not an atomic view across counters (contracts/ipc_api.md)
- [X] T010 [P] Write `tests/unit/ipc/log_test.cpp` first: `Log_FieldsAndNoPayload` asserts a formatted line carries exactly the nine keys `timestamp, level, service, connection_id, request_id, method, event, reason, duration_ms` in that order, that an absent value is rendered `-`, and that no formatting overload accepts a payload argument. Watch it fail to compile
- [X] T011 Create `modules/ipc/src/log.hpp` and `src/log.cpp` satisfying T010: one line per call, `key=value` pairs, ISO 8601 UTC milliseconds timestamp, and the closed `event` vocabulary of contracts/metrics.md. There is deliberately no payload parameter, so a payload cannot leak through a path that does not exist
- [X] T012 [P] Write `tests/unit/ipc/executor_test.cpp` first: tasks run on a thread that is not the caller's; `stop()` drains queued tasks then joins; a task that throws is caught, counted and does not kill the worker; posting after `stop()` is refused rather than silently dropped. Watch them fail
- [X] T013 Create `modules/ipc/src/executor.hpp` and `src/executor.cpp` satisfying T012: one worker thread, a bounded queue of `std::function<void()>`, drain-on-stop, exceptions caught at the boundary (research R-05). Private header - it must not appear under `include/`
- [X] T014 [P] Write `tests/unit/ipc/unix_socket_test.cpp` first: construction owns no FD; `bind()` on a path under `/mnt` fails and reports the errno rather than aborting; moving a socket transfers ownership so the destructor closes exactly once; a closed socket is not closed twice. Watch them fail
- [X] T015 Create `modules/ipc/src/unix_socket.hpp` and `src/unix_socket.cpp` satisfying T014: RAII over one FD, move-only, `connect/bind/listen/accept/close`, non-blocking mode, `MSG_NOSIGNAL` on every `send` so `SIGPIPE` cannot terminate the process (spec section 8.3), and `EINTR` retried while `EAGAIN` is returned to the caller

**Checkpoint**: foundation ready - `ctest -L ipc-unit` runs and passes the log, executor and socket
suites.

---

## Phase 3: User Story 1 - Framing primitives (Priority: P1) MVP

**Goal**: a frame encodes and decodes correctly no matter how the bytes are split, and a hostile
length can never cause an allocation.

**Independent Test**: `ctest -L ipc-unit -R FrameCodec` passes, including the byte-at-a-time and
two-frames-in-one-buffer cases, with no socket and no thread involved.

### Tests for User Story 1 (write first, watch them fail)

- [X] T016 [P] [US1] Write `FrameCodec_RejectOversizeBeforeAlloc` in `tests/unit/ipc/frame_codec_test.cpp`: a 4-byte prefix declaring 10 MiB is rejected with `TooLarge` and the decoder performs no allocation sized by that value (invariant W1, B-06). Assert the buffer is left untouched
- [X] T017 [P] [US1] Write `FrameCodec_ReadHeaderByteByByte` in the same file: feeding a valid frame one byte at a time returns `Incomplete` for every byte but the last and then decodes identically to feeding it whole; every `Incomplete` leaves the buffer byte-identical (invariant W6, B-04)
- [X] T018 [P] [US1] Write `FrameCodec_TwoFramesOneBuffer`: two concatenated frames in one buffer decode in order, each consuming exactly `4 + body_length` bytes, and a third call returns `Incomplete` (B-05)
- [X] T019 [P] [US1] Write `FrameCodec_RejectInvalidNameLength`: `name_length` larger than `body_length - 14` is `ProtocolError`, and a frame whose `14 + name_length + payload_size` does not equal `body_length` is `ProtocolError` (invariants W2, W3)
- [X] T020 [P] [US1] Write `FrameCodec_RejectUnsupportedVersion`: `version != 1` yields `UnsupportedVersion`; `flags != 0` and a `type` outside `1..4` yield `ProtocolError` (invariant W4)
- [X] T021 [P] [US1] Write `FrameCodec_RequestIdRules`: `request_id == 0` is valid only for `Notification`, and nonzero is required for `Request`, `Response` and `Error` (invariant W5)
- [X] T022 [P] [US1] Write `FrameCodec_RejectInvalidName`: a name containing a NUL, a character outside `[A-Za-z0-9_.-]`, or of length 0, is `ProtocolError` before the name is used as a map key (invariant W7)
- [X] T023 [P] [US1] Write `FrameCodec_RoundTrip`: `encode` then `tryDecode` returns the original header fields, name and payload for an empty payload, a 1-byte payload and a payload at the 64 KiB limit

### Implementation for User Story 1

- [X] T024 [US1] Create `modules/ipc/include/tcu/ipc/frame_codec.hpp`: `enum class FrameType : std::uint8_t { Request = 1, Response = 2, Notification = 3, Error = 4 }`, `struct FrameHeader`, `struct Frame`, `enum class DecodeResult { Ok, Incomplete, TooLarge, UnsupportedVersion, ProtocolError }`, the constants `kProtocolVersion = 1`, `kHeaderPrefix = 4`, `kFixedBodyFields = 14`, `kFrameOverhead = 18`, `kMaxBodyLength = 65536`, `kMaxFrameBytes = 65540`, and the two free functions `encode` and `tryDecode(buffer, out, consumed)`
- [X] T025 [US1] Implement `modules/ipc/src/frame_codec.cpp` satisfying T016-T023: field-by-field big-endian encode/decode with no packed struct and no `reinterpret_cast` over the buffer, bound checks in the order the contract specifies (prefix, then size bound, then completeness, then internal consistency, then name validity)
- [X] T026 [US1] Add `tests/unit/ipc/frame_codec_test.cpp` to `tests/unit/ipc/CMakeLists.txt` and confirm `ctest -L ipc-unit -R FrameCodec` is green with 8 cases

**Checkpoint**: the wire format of contracts/wire_protocol.md is implemented and proven against
hostile input, with no I/O in the picture. IPC-003 complete.

---

## Phase 4: User Story 2 - Request/response between two processes (Priority: P2)

**Goal**: `tcu_client` calls `vehicle.getStatus()` on `vehicle_service` over a real Unix socket and
gets the matching answer, with the handshake enforced.

**Independent Test**: `ctest -L ipc-integration -R RequestResponse` forks the real server binary and
asserts the response payload; `ctest -L ipc-unit -R 'Client_|Server_|Handler_'` covers the rest.

### Tests for User Story 2 (write first, watch them fail)

- [X] T027 [P] [US2] Write `Client_ConnectedOnlyAfterHelloAck` in `tests/unit/ipc/client_request_test.cpp`: against a `RawPeer` that accepts the connection but never answers `ipc.hello`, the client stays `Connecting` and never reports `Connected` (invariant C1, B-01)
- [X] T028 [P] [US2] Write `Client_VersionMismatchIsTerminal` in `tests/unit/ipc/client_lifecycle_test.cpp`: a peer answering `ipc.hello` with an `Error` frame carrying `UNSUPPORTED_VERSION` drives the client to `Stopped` and it makes no further `connect()` attempt - assert `connect_attempts_total` stops increasing (invariant C2, B-19)
- [X] T029 [P] [US2] Write `Client_ResponseOutOfOrderMatchesFuture` in `client_request_test.cpp`: two inflight requests answered in reverse order resolve their own futures with their own payloads (B-03)
- [X] T030 [P] [US2] Write `Server_RejectUnauthorizedMethod` in `tests/unit/ipc/server_router_test.cpp`: `registerHandler` returns false for a name beginning `ipc.`, for a duplicate, for an empty handler and for a call after `start()`; an unregistered method on the wire is answered `UNKNOWN_METHOD` with the session left open (invariant R1)
- [X] T031 [P] [US2] Write `Server_HandlerExceptionMappedToError` in `server_router_test.cpp`: a handler that throws produces an `Error` frame with code `INTERNAL_ERROR`, the server stays alive and serves the next request on the same session (B-11)
- [X] T032 [P] [US2] Write `Server_HandlerRunsOffIoThread` in `server_router_test.cpp`: the handler records `std::this_thread::get_id()` and it differs from the I/O thread's id (invariant R2, spec section 4.1)
- [X] T033 [P] [US2] Write `Handler_MalformedPayloadIsInvalidArgument` in `server_router_test.cpp`: a handler whose payload scanner rejects the input returns `INVALID_ARGUMENT`, the session stays open and the I/O loop keeps running (B-18)
- [X] T034 [P] [US2] Write `Server_UnauthenticatedBeforeHello` in `server_router_test.cpp`: a `RawPeer` that sends `vehicle.getStatus` as its first frame receives `UNAUTHENTICATED` and the session is closed (invariant S13)
- [X] T035 [P] [US2] Write `tests/integration/ipc/helpers/server_process.hpp`: fork/exec the `vehicle_service` binary with a `--socket` under a `TempSocketDir`, wait for the socket to accept a connection with a bounded deadline rather than a sleep, expose `kill(SIGKILL)`, `restart()` and `pid()`, and reap the child in the destructor
- [X] T036 [P] [US2] Write `tests/integration/ipc/request_response_test.cpp` (integration 1): launch the server, connect a client, `request("vehicle.getStatus", "{}", 2000ms)`, assert `error == None` and that the payload contains the three status fields (B-02)

### Implementation for User Story 2

- [X] T037 [US2] Create `modules/ipc/src/request_router.hpp` and `src/request_router.cpp`: method-to-handler map, the reserved `ipc.` namespace check, and the lookup order "control methods first, then handlers" of contracts/wire_protocol.md section 3
- [X] T038 [US2] Create `modules/ipc/src/client_session.hpp` and `src/client_session.cpp`: the `ClientSession` struct of data-model.md section 6, RX buffer accumulation feeding `tryDecode` in a loop, TX queue with `tx_offset` so a partial `write()` removes exactly the bytes sent, and `hello_done` gating
- [X] T039 [US2] Create `modules/ipc/include/tcu/ipc/ipc_server.hpp` declaring `IpcServer` per contracts/ipc_api.md: both constructors, `registerHandler`, `start`, `publishNotification`, `stop`, `metrics`, deleted copy, destructor calling `stop()`
- [X] T040 [US2] Implement the `IpcServer` `poll()` loop in `modules/ipc/src/ipc_server.cpp`: listening FD plus one `pollfd` per session, `POLLOUT` requested only while a TX queue is non-empty (research R-02), accept up to `max_sessions`, decode-dispatch-respond with the handler posted to the executor, and the `ipc.hello` handshake answering `{"version":1,"capabilities":[]}`
- [X] T041 [US2] Create `modules/ipc/include/tcu/ipc/ipc_client.hpp` declaring `IpcClient` per contracts/ipc_api.md: `start`, `request`, `subscribe`, `stop`, `state`, `metrics`, deleted copy, destructor calling `stop()`
- [X] T042 [US2] Implement the `IpcClient` `poll()` loop and request path in `modules/ipc/src/ipc_client.cpp`: the `socketpair` self-pipe, connect, send `ipc.hello`, reach `Connected` only on its ACK, the mutex-guarded `pending_requests` map of research R-03, and the two-step resolution - erase under the lock, `set_value()` after releasing it (invariants S1, S6)
- [X] T043 [US2] Create `apps/vehicle_service/main.cpp` with a `--socket` flag and one handler for `vehicle.getStatus` returning `{"ignition":true,"speed_kph":0,"battery_pct":79}`, plus `apps/vehicle_service/CMakeLists.txt` linking `tcu_ipc`. Compose the JSON as a literal; do not add a JSON library (research R-01)
- [X] T044 [US2] Create `apps/tcu_client/main.cpp` with `--socket` and `--scenario status`, printing the `[client]` lines of spec section 16.1, plus `apps/tcu_client/CMakeLists.txt`
- [X] T045 [US2] Add a depth-limited payload reader to `apps/` (not to the library) with a depth cap of 8, used by the handler to reject malformed input with `INVALID_ARGUMENT`, satisfying T033 and spec section 5.1's "gioi han JSON depth o application layer"
- [X] T046 [US2] Wire the new test files into their CMakeLists and confirm `ctest -L ipc-unit` and `ctest -L ipc-integration` are green

**Checkpoint**: two processes exchange request/response over a Unix socket - acceptance criterion 2
of spec section 13. IPC-001, IPC-002, IPC-004, IPC-005 complete.

---

## Phase 5: User Story 3 - Deadlines and backpressure (Priority: P3)

**Goal**: nothing waits forever and nothing grows without bound.

**Independent Test**: `ctest -L ipc-unit -R 'Timeout|QueueFull|SlowConsumer|LateResponse|Stop'`
passes, and a slow consumer's queue is observed to stop growing.

### Tests for User Story 3 (write first, watch them fail)

- [X] T047 [P] [US3] Write `Client_TimeoutCompletesExactlyOnce` in `tests/unit/ipc/client_timeout_test.cpp`: against a peer that never answers, the future resolves `Timeout` once, the pending map is empty afterwards, `request_timeouts_total == 1`, and a second resolution is impossible (invariants S1, S2, B-07)
- [X] T048 [P] [US3] Write `Client_TimeoutCountsQueueTime` in the same file: a request whose deadline elapses while still queued resolves `Timeout` and is never sent - assert `requests_sent_total` did not increase (invariant S2, equation E2)
- [X] T049 [P] [US3] Write `Client_LateResponseIgnored` in the same file: a response arriving after its request timed out is dropped, `late_responses_total == 1`, and no other pending request is touched (invariant S4, B-12)
- [X] T050 [P] [US3] Write `Client_QueueFullReturnsError` in `client_request_test.cpp`: with `max_tx_messages` lowered, the request that breaches the cap resolves `QueueFull` promptly and the call does not block (invariant S3, B-10)
- [X] T051 [P] [US3] Write `Client_PendingCapReturnsQueueFull` in the same file: with `max_pending_requests` lowered, the request past the cap resolves `QueueFull` without being registered
- [X] T052 [P] [US3] Write `Session_SlowConsumerDoesNotGrowMemory` in `tests/unit/ipc/session_test.cpp`: a peer that never reads while the server publishes repeatedly keeps `tx_bytes <= max_tx_bytes` and `tx_queue.size() <= max_tx_messages`, drops the excess, increments `queue_rejects_total`, logs `tx_queue_full` once per transition, and does not close the session (invariant S11, research R-08)
- [X] T053 [P] [US3] Write `Session_OneSlowPeerDoesNotStarveAnother` in `session_test.cpp`: with two sessions, one not reading, the other still completes requests (B-13)
- [X] T054 [P] [US3] Write `Client_StopIdempotent` and `Server_StopIdempotent` in `client_lifecycle_test.cpp`: `stop()` twice does not crash and closes no FD twice - compare the `/proc/self/fd` count before and after (B-15)
- [X] T055 [P] [US3] Write `Client_RequestAfterStopIsShuttingDown` in `client_lifecycle_test.cpp`: a request issued after `stop()` resolves `ShuttingDown` (B-17)
- [X] T056 [P] [US3] Write `Client_StopIsBounded` in `client_lifecycle_test.cpp`: `stop()` with requests inflight returns within 3 s and resolves each pending request exactly once with `Cancelled` (spec section 3.2, research R-09)
- [X] T057 [P] [US3] Write `Session_ProtocolErrorClosesSession` in `session_test.cpp`: a `RawPeer` sending a corrupt header has its session closed, `protocol_errors_total` increments, and other sessions are unaffected

### Implementation for User Story 3

- [X] T058 [US3] Add the deadline sweep to `modules/ipc/src/ipc_client.cpp`: `poll()`'s timeout is the time to the earliest deadline clamped to `[0 ms, 1000 ms]`, expired entries are swept after every wake, and the deadline is set at `request()` time, before queueing (research R-04)
- [X] T059 [US3] Enforce the pending cap and the two TX caps on the client, resolving `QueueFull` without blocking, satisfying T050 and T051
- [X] T060 [US3] Enforce the per-session TX caps on the server in `src/client_session.cpp`, dropping and counting the breaching frame and logging `tx_queue_full` once per transition into the full state, satisfying T052. Add the `shortcut:` comment research R-08 requires, naming closing-a-slow-session as the upgrade path
- [X] T061 [US3] Implement `stop()` on both sides per contracts/ipc_api.md: `std::atomic` state plus `std::once_flag` around the join-and-close block, self-pipe wake, executor drained then joined, every pending request resolved exactly once with `Cancelled`, and no self-join, satisfying T054-T056
- [X] T062 [US3] Add the late-response and generation checks: a response whose `request_id` is not pending in the current generation increments `late_responses_total` and is dropped; a frame from an older generation is dropped as a stale session (research R-12, satisfying T049)
- [X] T063 [US3] Confirm `ctest -L ipc-unit` is green and that no test uses a bare `sleep` as a synchronisation device (Principle IV)

**Checkpoint**: acceptance criteria 6 and 7 of spec section 13 hold - timeouts resolve exactly once
and queues are bounded. IPC-006, IPC-007 complete.

---

## Phase 6: User Story 4 - Reconnect and notifications (Priority: P4)

**Goal**: the demo goal of spec section 1 - kill the server, and the client reconnects,
re-subscribes and keeps receiving notifications without crashing and without replaying an RPC.

**Independent Test**: `ctest -L ipc-integration -R 'Reconnect|Notification|MultiClient'` passes with
the server killed by `SIGKILL` mid-test.

### Tests for User Story 4 (write first, watch them fail)

- [X] T064 [P] [US4] Write `tests/integration/ipc/notification_test.cpp` (integration 2): two clients subscribe to `vehicle.ignitionChanged`, the server publishes, both receive it, and a third client that did not subscribe does not (FR-05, invariant B1)
- [X] T065 [P] [US4] Write `tests/integration/ipc/reconnect_test.cpp` case 1 (integration 3): kill the server with a request inflight; the pending future resolves `PeerClosed` exactly once, the client enters `Backoff` and reconnects after the server restarts (B-08)
- [X] T066 [P] [US4] Write `reconnect_test.cpp` case 2 (integration 4): after a restart the client re-sends `ipc.subscribe`, the record becomes active only on its ACK, and a notification published afterwards arrives (B-09, invariant B2, research R-07)
- [X] T067 [P] [US4] Write `Client_NeverReplaysRequest` in `client_lifecycle_test.cpp`: a request inflight when the peer dies is resolved `PeerClosed` and is **not** re-sent after the reconnect - assert the server's received-request count across the restart (B-20, spec section 8.4)
- [X] T068 [P] [US4] Write `Client_BackoffScheduleDoubles` in `client_lifecycle_test.cpp`: with no server listening, successive attempt gaps follow 100, 200, 400, 800, 1600, 3200 ms and then cap at 5000 ms, and reset to 100 ms after a successful handshake (spec section 3.2)
- [X] T069 [P] [US4] Write `Client_StopRacesReconnect` in `client_lifecycle_test.cpp`: `stop()` called from another thread while the client is in `Backoff`/`Connecting` terminates within 3 s with no deadlock; this is the TSan target for B-16
- [X] T070 [P] [US4] Write `Client_ResubscribeAfterReconnect` in `client_request_test.cpp` against a `RawPeer`: after a forced disconnect the client sends `ipc.subscribe` for every registered topic before delivering any notification
- [X] T071 [P] [US4] Write `tests/integration/ipc/multi_client_test.cpp` (integration 5): 8 clients each pipeline 10 requests; every future resolves with its own correct answer and equation E1 holds per client (FR-10)
- [X] T072 [P] [US4] Write `Server_NotificationOnlyToSubscribers` in `server_router_test.cpp`: `publishNotification` enqueues to subscribed sessions only, and a topic beginning `ipc.` is rejected

### Implementation for User Story 4

- [X] T073 [US4] Implement the reconnect state machine in `modules/ipc/src/ipc_client.cpp` per data-model.md section 4: the five states, the backoff schedule with its 5 s cap, `generation` incremented on every entry into `Connected`, and `reconnect_total` counted only from the second successful handshake onwards
- [X] T074 [US4] Implement client subscriptions in `src/ipc_client.cpp`: the `SubscriptionRecord` list of data-model.md section 8, `ipc.subscribe` on `subscribe()` when connected, automatic re-send on reconnect, `active` set only on ACK, and notification dispatch to the executor (invariant B3)
- [X] T075 [US4] Implement server-side subscriptions and `publishNotification` in `src/ipc_server.cpp`: per-session `topics` set, `ipc.subscribe`/`ipc.unsubscribe` control handlers, enqueue-to-subscribers, and subscriptions dropped when a session closes (invariant S15)
- [X] T076 [US4] Extend `apps/vehicle_service/main.cpp`: create a `tcu::eventbus::EventBus`, subscribe to project 01's existing `IgnitionChanged` payload, forward it to `publishNotification("vehicle.ignitionChanged", ...)`, and toggle ignition on a timer so the demo has something to publish. Link `tcu_eventbus` in the **app** target only - the library must not link it (research R-05, Principle II)
- [X] T077 [US4] Extend `apps/tcu_client/main.cpp` with `--scenario status-and-subscribe`, printing the seven `[client]` lines of spec section 16.1 in order, including `state=Backoff reason=PeerClosed`, `reconnect_count` and `subscription=... outcome=restored`
- [X] T078 [US4] Confirm `ctest -L ipc-integration` is green, including a `SIGKILL` of the server mid-request

**Checkpoint**: acceptance criterion 5 of spec section 13 holds, and the section 16 demo sequence
works end to end. IPC-008, IPC-009 complete.

---

## Phase 7: User Story 5 - Peer authorization and observability (Priority: P5)

**Goal**: only an allowed peer connects, and every acceptance criterion is observable in metrics and
logs.

**Independent Test**: `ctest -L ipc-unit -R 'Security|Metrics|Log'` plus
`ctest -L ipc-integration -R Lifecycle` passes; the conservation equations hold.

### Tests for User Story 5 (write first, watch them fail)

- [X] T079 [P] [US5] Write `Server_RejectsUnauthorizedUid` in `tests/unit/ipc/server_security_test.cpp`: with `allowed_uids = {0}` and the test running as a non-root user, the connection is closed before any frame is read, one `rejected_peer` line is logged carrying UID, PID and GID, and no handler is dispatched (B-14, FR-09)
- [X] T080 [P] [US5] Write `Server_RejectsDirectoryWithLoosePermissions` in `server_security_test.cpp`: a runtime directory that exists with mode `0755`, or owned by another UID, makes `start()` return `false` without altering the directory (research R-10)
- [X] T081 [P] [US5] Write `Server_RefusesLiveSocketAndUnlinksStaleOne` in `server_security_test.cpp`: a socket path still served by a live listener makes `start()` fail, while a stale socket file with nobody listening is unlinked and bound successfully on one retry
- [X] T082 [P] [US5] Write `Metrics_ConservationAfterMixedOutcomes` in `tests/unit/ipc/metrics_test.cpp`: after a run mixing successes, timeouts, queue rejects and a `stop()`, equation E1 holds exactly and `tx_queue_bytes == 0` (equations E1, E3)
- [X] T083 [P] [US5] Write `Metrics_QueueRejectIsNeverSent` and `Metrics_LateResponseCounted` in `metrics_test.cpp` (equations E2, E5)
- [X] T084 [P] [US5] Write `tests/integration/ipc/server_lifecycle_test.cpp` (integration 6): close the server while one client is stopping; neither process hangs, `connections_active` reaches 0 and the socket file is removed (equation E4)
- [X] T085 [P] [US5] Extend `log_test.cpp` with `Log_RejectedPeerIsTheOnlyPlaceWithPeerIds`: assert no other `event` value formats a UID or PID

### Implementation for User Story 5

- [X] T086 [US5] Implement the runtime directory and socket lifecycle in `modules/ipc/src/ipc_server.cpp` per research R-10: create `/tmp/virtual-tcu` mode `0700` owned by the effective UID, refuse a directory with any group/other bit set or a different owner, `bind()` first and on `EADDRINUSE` probe with `connect()` so only `ECONNREFUSED` authorises `unlink()` plus one retry, and unlink on `stop()` only if this server created the file
- [X] T087 [US5] Implement the peer check on accept: `getsockopt(SO_PEERCRED)`, the UID allowlist defaulting to `{geteuid()}` when `allowed_uids` is empty, and close-before-read on rejection with the single audit log line, satisfying T079
- [X] T088 [US5] Complete the counter set on both sides and add the structured log calls for every `event` value of contracts/metrics.md, satisfying T082-T085. No log call takes a payload argument
- [X] T089 [US5] Implement the latency histogram observation from request creation to resolution, recorded only for `None` and `RemoteError` outcomes (contracts/metrics.md)
- [X] T090 [US5] Extend `apps/vehicle_service/main.cpp` with `--allow-uid` (repeatable) and `--slow-ms` for the delayed-handler fault case, and `apps/tcu_client/main.cpp` with `--timeout-ms` and `--scenario slow-reader`

**Checkpoint**: acceptance criteria 9 and 10 of spec section 13 hold. IPC-010, IPC-011 complete.

---

## Phase 8: User Story 6 - Fault injection and stress suite (Priority: P6)

**Goal**: every row of the spec section 10 fault matrix is exercised automatically.

**Independent Test**: `ctest -L ipc-fault` passes, and the ASan and TSan builds are clean.

- [X] T091 [P] [US6] Write `tests/fault_injection/ipc/partial_frame_test.cpp` covering four matrix rows with `RawPeer`: a frame written one byte at a time, two frames in one write, a prefix declaring 10 MiB, and a corrupted `version`/`type` byte. Assert the server's reaction for each - decode, decode both, reject before allocation, close the session
- [X] T092 [P] [US6] Write `tests/fault_injection/ipc/rapid_restart_test.cpp`: 20 kill/restart cycles with a client running throughout; assert the client reconnects every time, the backoff never exceeds 5 s, and the `/proc/self/fd` count at teardown equals the count at setup (acceptance criterion 8, fault rows "rapid restart" and "process kill")
- [X] T093 [P] [US6] Write `tests/fault_injection/ipc/stress_test.cpp`: 8 clients times 10 requests/s for `TCU_IPC_STRESS_SECONDS` seconds, default 5; assert no deadlock, equation E1 per client, `tx_queue_bytes == 0` at the end and an unchanged FD count (research R-14, fault row "mass concurrency")
- [X] T094 [P] [US6] Add a fault case for the delayed handler: `--slow-ms 3000` against a 2000 ms deadline must produce `Timeout` on the client and a counted, discarded late response, with the server's I/O loop still serving a second client throughout (fault row "server delayed response")
- [X] T095 [P] [US6] Add a fault case for client shutdown mid-write: `stop()` while a TX queue is non-empty terminates within 3 s and releases the buffers (`tx_queue_bytes == 0`, fault row "client shutdown mid-write")
- [X] T096 [US6] Run the ASan/UBSan build per quickstart.md section 4 and fix every report; record the command used
- [X] T097 [US6] Run the TSan build with `clang++` and `-DTCU_SANITIZER_SETARCH=ON` per quickstart.md section 4 and fix every report. Do not add a suppressions file - project 01's research explains why a suppression here would mask a real race

**Checkpoint**: the fault matrix and section 12.3 are satisfied. IPC-012 complete.

---

## Phase 9: Polish and Cross-Cutting Concerns

- [X] T098 [P] Write `scripts/run_ipc_demo.sh`: start `vehicle_service`, run `tcu_client --scenario status-and-subscribe`, `SIGKILL` the server, restart it, and show the reconnect and restored subscription. Must be idempotent, must clean up its background processes on exit, and must use a `/tmp` socket path
- [X] T099 [P] Extend the root `README.md` with the project 02 section: architecture summary, protocol version and the link to contracts/wire_protocol.md, the run script, and the limitations list from spec section 3.3 (acceptance criterion 10)
- [X] T100 Walk quickstart.md end to end on a clean WSL2 checkout and fix any command that does not work as written
- [X] T101 Re-check the Constitution gates of plan.md against the delivered code, and confirm Complexity Tracking is still empty. If any gate now fails, record it rather than quietly fixing the table
- [X] T102 Verify the acceptance checklist of spec section 13 item by item and report any unmet criterion explicitly rather than closing the project (constitution, Development Workflow gate 3)
- [X] T103 Harvest the `shortcut:` comments added during implementation into the project 02 debt notes, and confirm each one names its limit and the condition for upgrading

---

## Dependencies and Execution Order

### Phase dependencies

- **Setup (Phase 1)**: no dependencies
- **Foundational (Phase 2)**: needs Setup; blocks every story
- **US1 (Phase 3)**: needs Foundational
- **US2 (Phase 4)**: needs US1 - the loop cannot frame anything without the codec
- **US3 (Phase 5)**: needs US2 - a deadline needs a request to expire
- **US4 (Phase 6)**: needs US3 - B-08 asserts pending requests are cleaned up on peer loss
- **US5 (Phase 7)**: needs US4 - E1 is asserted across reconnects, and `rejected_peer` needs accept
- **US6 (Phase 8)**: needs US5 - the fault suite asserts counters US5 completes
- **Polish (Phase 9)**: needs US6

### Why the stories are not parallel

Spec section 14 states the chain itself: `001 -> 002 -> 003 -> 004/005 -> 006/007 -> 008/009 ->
010/011 -> 012`. Each story is a layer of one transport, not a separate feature, so the template's
parallel-stories strategy does not apply. What *is* parallel is marked `[P]` inside each phase: the
test-writing tasks almost always are, because each touches a distinct test case.

### Within each story

- Tests are written and seen to FAIL before the implementation task that satisfies them
- Types before the components that use them
- Codec before the loops; loops before the apps; apps before the integration tests that fork them

### Parallel opportunities

- T003-T007 (Setup, five distinct files)
- T009/T010/T012/T014 (Foundational test-first tasks, distinct files)
- T016-T023 (all eight US1 codec cases, one file but independent cases - write them in one pass)
- T027-T036 (US2 tests, distinct files and cases)
- T047-T057 (US3 tests), T064-T072 (US4 tests), T079-T085 (US5 tests), T091-T095 (US6 cases)

---

## Parallel Example: User Story 1 tests

```bash
# All eight US1 codec cases are independent and can be written in one pass:
Task: "FrameCodec_RejectOversizeBeforeAlloc in tests/unit/ipc/frame_codec_test.cpp"
Task: "FrameCodec_ReadHeaderByteByByte in tests/unit/ipc/frame_codec_test.cpp"
Task: "FrameCodec_TwoFramesOneBuffer in tests/unit/ipc/frame_codec_test.cpp"
Task: "FrameCodec_RejectInvalidNameLength in tests/unit/ipc/frame_codec_test.cpp"
Task: "FrameCodec_RejectUnsupportedVersion in tests/unit/ipc/frame_codec_test.cpp"
Task: "FrameCodec_RequestIdRules in tests/unit/ipc/frame_codec_test.cpp"
Task: "FrameCodec_RejectInvalidName in tests/unit/ipc/frame_codec_test.cpp"
Task: "FrameCodec_RoundTrip in tests/unit/ipc/frame_codec_test.cpp"
```

---

## Implementation Strategy

### MVP first

1. Phase 1 Setup, Phase 2 Foundational
2. Phase 3 US1 - the codec, provable with no socket in the picture
3. **STOP and VALIDATE**: `ctest -L ipc-unit -R FrameCodec` green, including byte-at-a-time
4. Phase 4 US2 - two processes talking
5. **STOP and VALIDATE**: `ctest -L ipc-integration -R RequestResponse` green. This is the first
   demonstrable increment and satisfies acceptance criteria 1-3 of spec section 13

### Incremental delivery

1. Setup + Foundational -> a test can compile
2. US1 -> framing proven against hostile input
3. US2 -> request/response across a process boundary (demo-able)
4. US3 -> nothing hangs, nothing grows
5. US4 -> the headline demo: kill, reconnect, re-subscribe
6. US5 -> only allowed peers, and everything observable
7. US6 -> the fault matrix runs in CI

### Suggested commits

One commit per story checkpoint, each naming the tickets it closes, per the constitution's "Commit
per project with the project number in the message" - for example
`project 02: framing primitives (IPC-001..003)`.

---

## Notes

- `[P]` means different files or independent cases, with no dependency on an incomplete task
- Every command runs in WSL2; a socket path under `/mnt` will fail `EOPNOTSUPP`
- Verify each test fails before implementing it - a test that passes on an empty implementation is
  testing nothing
- Never combine ASan and TSan in one binary (spec section 12.3)
- A shortcut taken deliberately gets a `shortcut: <limit>, <when to upgrade>` comment
- Stop at any checkpoint; each one is a working increment
