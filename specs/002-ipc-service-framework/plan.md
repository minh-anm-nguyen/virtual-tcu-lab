# Implementation Plan: IPC Service Framework

**Branch**: `002-ipc-service-framework` | **Date**: 2026-11-04 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/002-ipc-service-framework/spec.md`

## Summary

Build `tcu::ipc`, a C++17 Unix-domain-socket IPC library: length-prefixed framing, request/response
correlation with deadlines, server-to-client notifications, bounded TX queues, reconnect with
backoff, peer authorization via `SO_PEERCRED`, metrics and structured logs. Two demo processes
(`vehicle_service`, `tcu_client`) prove the section 1 goal: a client survives the server being
killed, reconnects, re-subscribes and keeps receiving notifications. It is the process-boundary
transport that projects 13, 15 and 16 consume (spec section 18).

The spec already fixes the design: wire format (section 5.1), public API (section 6), concurrency
model and both state machines (section 7), operational behaviour (section 8), 20 behavioural
contracts (section 9), the fault matrix (section 10), metrics (section 11), the 15 unit tests and 6
integration tests (section 12), acceptance criteria (section 13), the 12-ticket backlog (section 14)
and the repository layout (section 15). This plan does not redesign any of it; it closes the
decisions the spec explicitly defers (sections 4.2, 6, 7, 8.6, 8.9, 17.1) so implementation has no
remaining ambiguity. Those closures are in [research.md](./research.md).

## Technical Context

**Language/Version**: C++17, no compiler extensions in library code.

**Primary Dependencies**: standard library plus POSIX (`sys/socket.h`, `sys/un.h`, `poll.h`,
`unistd.h`). **No JSON library** - the transport treats a payload as opaque bytes (research R-01).
GoogleTest/GoogleMock for tests, reused from the root `CMakeLists.txt` pin at `v1.14.0` (R-16).
`tcu_eventbus` (project 01) is linked by the demo app only, never by the library (R-05).

**Storage**: N/A - no persistence. Delivery is explicitly non-durable (spec section 8.7).

**Testing**: GoogleTest + CTest, three labels - `ipc-unit`, `ipc-integration`, `ipc-fault`.
Integration tests fork a real server process over a real socket in a per-test `/tmp` directory;
fault injection drives a raw `AF_UNIX` FD from C++ rather than Python (R-13). Sanitizer jobs:
`-fsanitize=address,undefined`, and a separate `-fsanitize=thread` build that must use `clang++`
with `TCU_SANITIZER_SETARCH=ON`; never combined.

**Target Platform**: Linux/Ubuntu 22.04+ x86-64, GCC 11+ or Clang 14+. Developed from WSL2 Ubuntu
under `/mnt/d/TCU`. **Windows is not a target for this project at all** (R-00) - the project 02
targets are guarded by a Linux check so the project 01 build keeps configuring on Windows. The socket
path must be on the Linux filesystem: `bind()` on a DrvFs path fails `EOPNOTSUPP`, measured
2026-11-04.

**Project Type**: Static library + two demo executables + three test suites, inside the Virtual TCU
Lab monorepo.

**Performance Goals**: none asserted. Spec section 3.2 labels its own numbers "muc tieu kiem thu de
xuat, khong phai requirement cua OEM", and section 17.1 question 7 defers real targets. No benchmark
harness is in scope for project 02; the latency histogram exists for observability, not as a budget
(contracts/metrics.md).

**Constraints**: bounded memory - TX queues capped at 256 messages / 1 MiB per connection, RX buffer
capped at two maximum frames, pending requests capped at 128. Max payload 64 KiB. Default request
timeout 2000 ms on `steady_clock`. Reconnect backoff 100 ms doubling to a 5 s cap. No callback or
handler on an I/O thread; no lock held across a callback, a `promise.set_value()` or a syscall. No
busy-spin; `stop()` bounded. No double-close, no use-after-free, no data race. No automatic replay of
a side-effecting RPC.

**Scale/Scope**: 1 server, 8 concurrent clients in the integration suite and 8-32 in the fault
suite, `max_sessions` 32. Roughly 2500-3000 lines of library plus test code.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Gate | Status |
|---|---|---|
| I. Spec Fidelity and Scope Discipline | Deliver only section 3.1's FRs; nothing from section 3.3's non-goals (multi-host IPC, shared memory, POSIX mqueue, exactly-once, durable replay, encryption, broker scale, Windows, auto-retry of side-effecting RPC) and nothing from FR-13 (D-Bus adapter, P2) | PASS - research R-17 answers all seven of section 17.1's production questions with "out of v1"; contracts/wire_protocol.md section 8 and contracts/ipc_api.md both end with an explicit not-in-v1 list. `IpcError` keeps exactly the ten values of section 6; `UNKNOWN_METHOD` and `UNAUTHENTICATED` are wire codes only (R-11). |
| II. Library-First Reuse | One CMake target with a public header surface; consume earlier projects rather than copy them | PASS - single `tcu_ipc` target. GoogleTest is reused at project 01's pin instead of re-declared (R-16). Project 01's `tcu_eventbus` was evaluated for the callback executor and rejected with a recorded reason (R-05), and is genuinely reused in the demo, where `apps/vehicle_service` publishes project 01's existing `IgnitionChanged` payload onto an `EventBus` and forwards it to `publishNotification` - the `WORK <--> EB` edge of spec section 4.1. No event-bus code is copied. |
| III. Test-First | Every FR has a unit test naming it; every BDD scenario has a test; every acceptance criterion is observable | PASS - traceability matrices below cover FR-01..FR-12 and B-01..B-20. Each task writes its test first. The conservation equations of contracts/metrics.md make "resolve exactly once" and "no FD leak" machine-checkable rather than reviewed. |
| IV. Simulation-Only | No real hardware; time and faults injectable; deterministic | PASS - loopback Unix sockets only. Faults are injected by a raw-FD peer, a `--slow-ms` handler flag and a `slow-reader` scenario, not by timing luck. The stress duration is a variable with a 5 s default so the suite finishes (R-14). The one unavoidable sleep-like wait is a condition-variable wait with a deadline, never a bare `sleep` as a synchronisation device. |
| V. Safe Observability | Structured logs and metrics where the spec asks; no secrets; failures isolated and counted | PASS - log fields fixed to section 11's nine keys with a closed `event` vocabulary, and there is no payload field, so a payload cannot leak through a path that does not exist. Peer UID/PID appear only in the `rejected_peer` audit line B-14 requires. Handler and callback exceptions are caught at the executor boundary, counted and logged, never swallowed silently (B-11, B-18). |

**Post-Phase 1 re-check**: still PASS. Phase 1 added no third-party dependency, no abstraction with a
single implementation, and no public API beyond spec section 6 plus `ServerOptions`, `ClientState`,
`metrics()` and `FrameCodec` - each of which the spec's own sections 3.2, 7.1, 11 and 5.1 require to
be reachable. Complexity Tracking stays empty.

**Post-implementation re-check (T101, 2026-11-04)**: all five gates still PASS. Three things
changed during implementation and are recorded rather than quietly absorbed:

1. **Principle I** - two public members were added beyond the Phase 1 list: `IpcServer::sessionCount()`
   and `tcu::ipc::isValidName()`. The first makes B-13's "the other client is still served"
   assertable without a log scrape; the second is needed by the router to validate a name before
   using it as a map key (invariant W7), and the tests use it too. Both are observations, not
   behaviour. No feature from section 3.3 or FR-13 was built.
2. **Principle III** - unchanged in substance, but the executor's stop semantics gained a
   `DrainPolicy`, so `Executor` has two stop tests instead of one. The test-first order held for
   every task; the two defects below were found by tests that were written before the code they
   broke.
3. **Principle IV** - three tests originally waited for a late response with a fixed
   `sleep_for`, which is a timing assumption rather than an event. They now poll the counter
   against a deadline. No test uses a bare sleep as a synchronisation device; the remaining sleeps
   exist only where time genuinely has to pass (a deadline expiring, a handler being slow on
   purpose).

### Requirement to test traceability

| Requirement | Covered by |
|---|---|
| FR-01 server bind/listen/accept | `Server_StartBindsAndListens`, integration 1 |
| FR-02 client connect/disconnect/reconnect with backoff | `Client_BackoffScheduleDoubles`, integration 3, 4 |
| FR-03 length-prefix framing, partial I/O | `FrameCodec_ReadHeaderByteByByte`, `FrameCodec_TwoFramesOneBuffer`, fault "partial frame" |
| FR-04 correlation ID and deadline | `Client_ResponseOutOfOrderMatchesFuture`, `Client_TimeoutCompletesExactlyOnce`, integration 5 |
| FR-05 one-way notification | integration 2, `Server_NotificationOnlyToSubscribers` |
| FR-06 max frame size, validation, standard error codes | `FrameCodec_RejectOversizeBeforeAlloc`, `FrameCodec_RejectInvalidNameLength`, `FrameCodec_RejectUnsupportedVersion`, `Server_HandlerExceptionMappedToError` |
| FR-07 bounded queue and backpressure | `Client_QueueFullReturnsError`, `Session_SlowConsumerDoesNotGrowMemory` |
| FR-08 graceful shutdown, join, close | `Client_StopIdempotent`, `Server_StopClosesEverySession`, integration 6, FD-count check |
| FR-09 socket permissions and peer credentials | `Server_RejectsDirectoryWithLoosePermissions`, `Server_RejectsUnauthorizedUid`, fault "path permission denied" |
| FR-10 multiple clients, per-connection isolation | integration 2, 5, `Session_SlowConsumerDoesNotGrowMemory` (B-13) |
| FR-11 subscribe and re-subscribe after reconnect | integration 4, `Client_ResubscribeAfterReconnect` |
| FR-12 instrumentation, metrics, structured logs | `Metrics_ConservationAfterMixedOutcomes`, `Metrics_LateResponseCounted`, `Log_FieldsAndNoPayload` |
| FR-13 D-Bus adapter (P2) | **not implemented** - section 3.1 marks it "Khong nam trong MVP" |
| NFR 8 concurrent clients | integration 5, stress test |
| NFR no busy-spin, bounded shutdown | `Client_StopIsBounded`, `poll()` timeout assertions |
| NFR no double-close / UAF / data race | ASan job, TSan job (clang + setarch), FD-count check |
| NFR handler never blocks the I/O loop | `Server_HandlerRunsOffIoThread` (thread-id assertion), fault "server delayed response" |

### BDD scenario traceability (spec section 9)

| ID | Covered by | Kind |
|---|---|---|
| B-01 | `Client_ConnectedOnlyAfterHelloAck` | unit + integration 1 |
| B-02 | integration 1 | integration |
| B-03 | `Client_ResponseOutOfOrderMatchesFuture` | unit |
| B-04 | `FrameCodec_ReadHeaderByteByByte` | unit |
| B-05 | `FrameCodec_TwoFramesOneBuffer` | unit |
| B-06 | `FrameCodec_RejectOversizeBeforeAlloc`, fault "oversized frame" | unit + fault |
| B-07 | `Client_TimeoutCompletesExactlyOnce` | unit |
| B-08 | integration 3 | integration |
| B-09 | integration 4 | integration |
| B-10 | `Client_QueueFullReturnsError` | unit |
| B-11 | `Server_HandlerExceptionMappedToError` | unit |
| B-12 | `Client_LateResponseIgnored`, `Metrics_LateResponseCounted` | unit |
| B-13 | `Session_SlowConsumerDoesNotGrowMemory`, fault "slow client" | unit + fault |
| B-14 | `Server_RejectsUnauthorizedUid` | unit |
| B-15 | `Client_StopIdempotent`, `Server_StopIdempotent` | unit |
| B-16 | `Client_StopRacesReconnect` (TSan target) | unit + sanitizer |
| B-17 | `Client_RequestAfterStopIsShuttingDown` | unit |
| B-18 | `Handler_MalformedPayloadIsInvalidArgument` | unit |
| B-19 | `Client_VersionMismatchIsTerminal` | unit |
| B-20 | `Client_NeverReplaysRequest` | unit |

B-16 is listed as a sanitizer target because its assertion is the absence of a race, which only TSan
can observe; the test itself asserts bounded termination.

## Project Structure

### Documentation (this feature)

```text
specs/002-ipc-service-framework/
  plan.md                    # This file
  research.md                # Phase 0: closes the spec's open decisions (R-00..R-17)
  data-model.md              # Phase 1: entities, states, invariants W*/C*/S*/P*/M*
  quickstart.md              # Phase 1: build, test, demo and fault validation
  contracts/
    wire_protocol.md         # Byte layout, control methods, decoder contract, error codes
    ipc_api.md               # Public API pre/postconditions and thread safety
    metrics.md               # Metric definitions, conservation equations, log fields
  tasks.md                   # Phase 2 output (/speckit-tasks - NOT created here)
```

### Source Code (repository root)

Layout follows spec section 15, rooted at the monorepo root:

```text
CMakeLists.txt                              # root: one Linux-guarded block for project 02
README.md                                   # extended with the IPC section (acceptance criterion)

modules/ipc/
  CMakeLists.txt                            # target tcu_ipc
  include/tcu/ipc/ipc_types.hpp             # IpcError, IpcResponse, ClientOptions, ServerOptions,
                                            #   ClientState, FrameType, IpcMetricsSnapshot
  include/tcu/ipc/frame_codec.hpp           # FrameHeader, Frame, DecodeResult, encode/tryDecode
  include/tcu/ipc/ipc_client.hpp            # IpcClient
  include/tcu/ipc/ipc_server.hpp            # IpcServer
  src/frame_codec.cpp
  src/unix_socket.hpp                       # internal: RAII FD, connect/bind/listen/accept
  src/unix_socket.cpp
  src/executor.hpp                          # internal: one worker, bounded task queue (R-05)
  src/executor.cpp
  src/log.hpp                               # internal: the nine fixed fields, one line
  src/log.cpp
  src/metrics.hpp                           # internal: atomic counters behind the snapshot
  src/client_session.hpp                    # internal: server-side session state
  src/client_session.cpp
  src/request_router.hpp                    # internal: method -> handler, reserved ipc.* names
  src/request_router.cpp
  src/ipc_client.cpp                        # poll loop, pending map, deadlines, reconnect
  src/ipc_server.cpp                        # poll loop, accept, peer check, notifications

apps/vehicle_service/main.cpp               # handlers + project 01 EventBus -> notification
apps/tcu_client/main.cpp                    # scenarios: status, status-and-subscribe, slow-reader

tests/unit/ipc/
  support/raw_peer.hpp                      # raw AF_UNIX peer, byte-level writes (R-13)
  support/temp_socket_dir.hpp               # per-test /tmp directory, 0700, removed on teardown
  frame_codec_test.cpp                      # FrameCodec_* (B-04,05,06), W1..W7
  client_request_test.cpp                   # Client_* correlation, QueueFull (B-02,03,10)
  client_timeout_test.cpp                   # timeout and late response (B-07,12)
  client_lifecycle_test.cpp                 # stop, version mismatch, no replay (B-15,16,17,19,20)
  server_router_test.cpp                    # handler exception, unknown/reserved method (B-11,18)
  server_security_test.cpp                  # UID allowlist, directory permissions (B-14)
  session_test.cpp                          # TX caps, slow consumer (B-13)
  metrics_test.cpp                          # conservation equations E1, E2, E5
  log_test.cpp                              # field set, no payload leak

tests/integration/ipc/
  helpers/server_process.hpp                # fork/exec the demo server, wait for readiness, kill
  request_response_test.cpp                 # integration 1
  notification_test.cpp                     # integration 2
  reconnect_test.cpp                        # integration 3, 4
  multi_client_test.cpp                     # integration 5
  server_lifecycle_test.cpp                 # integration 6, E3, E4

tests/fault_injection/ipc/
  partial_frame_test.cpp                    # byte-at-a-time, two-in-one, oversize, corrupt header
  rapid_restart_test.cpp                    # kill/restart loop, FD-count equality
  stress_test.cpp                           # 8 clients x 10 req/s, TCU_IPC_STRESS_SECONDS

scripts/run_ipc_demo.sh                     # the section 16 demo, including kill and restart
```

**Structure Decision**: spec section 15's layout is adopted, with four deltas, each justified:

1. The `docs/02_...md` entry of section 15 is **not** created. The design document already lives at
   `Virtual_TCU_Lab_30_System_Designs/02_IPC_Service_Framework_System_Design.md` and is copied into
   `specs/002-ipc-service-framework/spec.md`. A third copy would drift (Principle II). This is the
   same delta project 01 recorded.
2. `configs/ipc_demo.json` is **not** created. Section 16's own target commands pass everything as
   CLI flags, no acceptance criterion mentions a config file, and parsing one would re-introduce the
   JSON dependency that research R-01 removed. Full reasoning in R-15.
3. Section 15 lists only the six `src/*.cpp` files of the headline components. Six further private
   files are added under `src/` - `unix_socket.hpp`, `executor.hpp/.cpp`, `log.hpp/.cpp`,
   `metrics.hpp` - because section 4.1 names `Executor` and `Metrics` as components and section 11
   requires structured logging, none of which the six listed files can hold. They are **private**
   headers under `src/`, not under `include/`, so the public contract that projects 13-30 depend on
   stays the four headers of section 15.
4. `tests/unit/ipc/support/` and `tests/integration/ipc/helpers/` are added. Section 12.2's
   integration tests require launching a server subprocess and the fault matrix requires a raw
   byte-level peer; neither is achievable with the flat test tree of section 15 alone.

The root `CMakeLists.txt` is extended, not restructured: its project 01 comment already says
"Projects 02-30 each append one add_subdirectory line here". Project 02 appends a single
`if(CMAKE_SYSTEM_NAME STREQUAL "Linux")` block containing the module, the two apps and the three
test directories, so a Windows configure still builds project 01.

## Complexity Tracking

No Constitution Check violations. Nothing to justify.

The two decisions that came closest to needing an entry, and why they do not:

| Candidate | Why it is not a violation |
|---|---|
| A private `Executor` instead of reusing project 01's `EventBus` (R-05) | Principle II forbids re-implementing what an earlier project provides. `EventBus` does not provide task execution: its payload is a closed `std::variant` fixed by project 01's spec and its subscribers are keyed by `EventType`, so using it would mean widening project 01's public API for projects 05-30 in order to pass a `std::function`. The private queue reuses nothing because there is nothing to reuse. |
| Two metrics beyond spec section 11's table (`requests_created_total`, `requests_failed_total`) | Section 11's own conservation sentence names both terms; without them the stated rule cannot be checked. They add no behaviour and no public API beyond two snapshot fields. |

## Deferred shortcuts (T103 ledger)

Every deliberate shortcut carries a `shortcut: <limit>, <when to upgrade>` comment at the code that
takes it. The full set after implementation:

| Where | Limit accepted | Upgrade when |
|---|---|---|
| `modules/ipc/src/client_session.cpp` | A session parked at its TX cap is counted and logged, never closed, so a peer that stops reading forever keeps a session and loses notifications | A spec revision asks for a slow-consumer disconnect policy; it needs a second timer, a config knob and a new error code (research R-08) |
| `modules/ipc/src/ipc_server.cpp` | The `poll()` array is rebuilt on every wake, O(n) in the session count | A deployment needs more than roughly 100 concurrent sessions; switch to `epoll` with a persistent FD registry (research R-02) |

Both are inside the envelope spec section 3.3 draws (no broker-scale client counts, no durable
delivery), so neither is a Constitution Check violation - they are recorded here so a later project
finds them without reading the whole source.

### Changes the fault suite forced

Two defects were found by tests rather than by review, and are recorded because they changed the
design rather than only the code:

1. **The server executor needed more than one thread.** With one, a handler sleeping 3 s made an
   unrelated client's 2 s request time out - a slow handler starved every other session. Spec
   section 7 already specifies "worker executor N threads" for the server; research R-05's "one
   thread is enough for the demo" was right for the client and wrong for the server. The server now
   runs four handler threads; the client keeps one, so notification order per session stays
   observable (spec section 8.7).
2. **`Executor::stop()` had to stop draining by default.** Draining 100 queued 1.5 s handlers took
   150 s, against the 3 s shutdown budget of spec section 3.2. `stop()` now discards tasks that have
   not started, counts them, and takes a `DrainPolicy` for the caller that wants the old behaviour.
3. **The self-pipe write end could not be a `UnixSocket`.** ThreadSanitizer reported a data race on
   `UnixSocket::last_errno_`: `wake()` is called from any thread, so `subscribe()` on a user thread
   and `sendSubscribe()` on the I/O thread both wrote that field for the same object. The write end
   is now a bare `std::atomic<int>` descriptor written with `::send` and an ignored result - the
   wake byte's errno carries no information, because a full pipe already means a wake is pending.
   `stop()` clears the atomic before closing, so a racing `wake()` sees `-1` rather than a closed
   descriptor. Found only under TSan; the review checklist of spec section 19 would not have caught
   it, because the shared state was inside a type that looked single-owner.
