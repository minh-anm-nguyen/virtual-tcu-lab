# Implementation Plan: Timer & Task Scheduler

**Branch**: `003-timer-task-scheduler` | **Date**: 2026-11-04 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/003-timer-task-scheduler/spec.md`

## Summary

Build `tcu::scheduler`, a C++17 in-process timer and task scheduler: one coordinator thread sleeping
on `steady_clock::wait_until` over a min-heap of deadlines, a bounded FIFO ready queue, a fixed
worker pool that executes callbacks with no scheduler lock held, one-shot and two explicit periodic
modes, cancellation that is defined in every race, bounded memory with real backpressure, metrics and
structured logs, and an injectable clock so deadline tests are deterministic. Two demos prove the
spec's headline cases: a telemetry timer publishing onto project 01's Event Bus, and a modem AT
timeout racing a reply where exactly one side completes the request.

It is the deadline primitive projects 10, 13 and 15 consume — the spec's section 16 names them.

The spec already fixes the design: architecture and components (section 7), both state machines
(section 8), the illustrative API (section 9), the threading and locking rules (section 10), the
periodic semantics (section 11), 15 end-to-end flows (section 12), 20 behavioural contracts
(section 13), the fault matrix (section 14), the metrics and log lines (section 15), the integration
points (section 16), the folder layout (section 17), a 14-ticket backlog (section 18), the testing
strategy (section 19), the demo workflow (section 20) and the Definition of Done (section 21). This
plan does not redesign any of it. It closes the seven ADR candidates of section 22 plus eleven
mechanisms the spec states as a goal without naming an implementation; those closures are
[research.md](./research.md) R-00..R-18.

## Technical Context

**Language/Version**: C++17, no compiler extensions in library code.

**Primary Dependencies**: the standard library only — `<thread>`, `<mutex>`,
`<condition_variable>`, `<chrono>`, `<queue>`, `<unordered_map>`, `<functional>`, `<atomic>`. **No
POSIX call anywhere**, which is why this project is not Linux-gated (R-00). GoogleTest/GoogleMock for
tests, reused from the root `CMakeLists.txt` pin at `v1.14.0`. `tcu_eventbus` (project 01) is linked
by one demo and one integration test; `tcu_ipc` (project 02) by one Linux-guarded integration test.
Neither is linked by the library.

**Storage**: N/A. Nothing is persisted; durability across process restart is an explicit non-goal
(spec section 2).

**Testing**: GoogleTest + CTest, three labels — `scheduler-unit`, `scheduler-integration`,
`scheduler-stress`. The unit suite drives a `FakeClockAdapter` so every deadline assertion is
deterministic and the whole suite runs in under a second; a `TestGate` (condition variable +
predicate) replaces every "wait for the callback" sleep. The integration suite uses the real
`steady_clock` and asserts ordering and counts, never elapsed time. Stress duration is
`TCU_SCHEDULER_STRESS_SECONDS`, default 5 (R-17). Sanitizers: `-fsanitize=address,undefined`, and a
separate `-fsanitize=thread` build that must use `clang++` with `TCU_SANITIZER_SETARCH=ON`; never
combined.

**Target Platform**: Linux/Ubuntu 22.04+ x86-64 (GCC 11+ or Clang 14+) **and** Windows/MSVC 19.3x.
Both are first-class for build and test; sanitizer jobs are Linux-only as a toolchain limit, so
spec section 21's sanitizer criterion is evaluated on Linux/WSL (R-00).

**Project Type**: Static library + two demo executables + three test suites + one benchmark, inside
the Virtual TCU Lab monorepo.

**Performance Goals**: measured, never asserted. Spec section 6 labels p99 scheduling-to-start
lateness < 50 ms a "benchmark, not guarantee" and section 19.4 forbids claiming real-time behaviour
from a workstation run. The histogram exists for observability; `benchmarks/scheduler_bench` reports
p50/p95/p99 and no CTest case fails on a latency threshold (R-18).

**Constraints**: bounded memory — `maxTimers` (default 10,000) registered timers and `maxReadyTasks`
(default 1,024) ready jobs, both hard caps; heap size held at `O(live timers)` by threshold
compaction (R-11). One mutex guards registry, heap, timer states and ready queue; two condition
variables; **no user callback is ever invoked with that mutex held** (FR-11, S1). No busy-wait: the
coordinator blocks in `waitUntil`. No detached thread; every thread joins. No one-shot callback runs
twice. `stop()` has no timeout and no force-kill. A throwing callback cannot terminate a worker.
`steady_clock` only — wall-clock, timezone and NTP changes cannot move a deadline.

**Scale/Scope**: 10,000 registered timers in the stress and benchmark cases, 100-1,000 callbacks/s,
2 worker threads by default, 8 concurrent schedule/cancel threads in ST-01. Roughly 2,000-2,500 lines
of library plus test code.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Gate | Status |
|---|---|---|
| I. Spec Fidelity and Scope Discipline | Deliver FR-01..FR-12; nothing from section 2's non-goals (hard real-time, OS real-time priorities, durable jobs, exactly-once across crashes, interrupt-safe scheduling, distributed task queue) and nothing from section 1's out-of-scope list (cross-process persistence, cron/calendar, execution while suspended) | PASS — R-01..R-07 answer all seven of section 22's ADR candidates with an explicit v1 boundary; [contracts/scheduler_api.md](./contracts/scheduler_api.md) section 5 ends with a seven-item not-in-v1 list. `Error` and `CancelResult` keep exactly the values of section 9. The one API addition is the `ClockAdapter` constructor, which section 6's testability goal requires (R-05). |
| II. Library-First Reuse | One CMake target with a public header surface; consume earlier projects rather than copy them | PASS — single `tcu_scheduler` target. GoogleTest is reused at project 01's pin, not re-declared. Project 01's `EventBus` is genuinely reused in `telemetry_timer_demo` and IT-05, project 02's `IpcClient` in IT-04; no code from either is copied. The two near-misses (project 02's private `log.hpp` and `metrics.hpp`) are recorded with reasons in R-15 and R-16: both are private headers under `src/`, so reusing them would mean promoting project 02's internals to a public surface for projects 04-30. The real fix — one shared logging target — is flagged for project 04, which the design list schedules next. |
| III. Test-First | Every FR has a unit test naming it; every BDD scenario has a test; every acceptance criterion is observable | PASS — the two traceability tables below cover FR-01..FR-12 and B-01..B-20 with the spec's own UT/IT/ST/PT ids. Each task writes its test first. The M1-M6 conservation equations of [contracts/metrics.md](./contracts/metrics.md) make "no timer was lost" and "no callback went uncounted" arithmetic rather than review. |
| IV. Simulation-Only | No real hardware; time and faults injectable; deterministic | PASS — nothing but threads and memory. Time is injectable by design (`ClockAdapter`), which is the whole reason the 14 unit deadline cases need no sleeps. Faults are injected by a test-controlled gate, a throwing callback, a fake-clock jump and a churn loop — never by timing luck. Stress duration is a variable with a 5 s default so CI finishes. |
| V. Safe Observability | Structured logs and metrics where the spec asks; no secrets; failures isolated and counted | PASS — log fields fixed to the eight keys of [contracts/metrics.md](./contracts/metrics.md) section 3 with a closed six-word `event` vocabulary, and there is **no payload or capture field**, so callback state has no path into a log line. Every callback exception is caught at the worker boundary, counted in `callbacks_failed_total`, logged with `reason=exception`, and a periodic timer that threw is disabled rather than silently retried (R-09) — isolated and counted, never swallowed. |

**Post-implementation re-check (T066, 2026-11-04)**: all five gates still PASS. Six things changed
during implementation and are recorded rather than quietly absorbed:

1. **Principle I** — one public member was added beyond the Phase 1 list:
   `ClockAdapter::attach(std::mutex&, std::condition_variable&)`, a virtual with a do-nothing
   default. Without it a fake clock has to discover the coordinator's mutex from its first
   `waitUntil`, and an `advance()` racing that first registration can move time without notifying
   anyone — a lost wake-up that hangs the test suite. `start()` now attaches before any coordinator
   thread exists. `Histogram::kBucketCount` and `kUpperBoundsMs` are also public, because the
   benchmark derives percentiles from the buckets and spec section 19.4 asks for exactly that.
   Nothing from section 1 or 2's out-of-scope lists was built.
2. **Principle III** — the test-first order held for every piece of code, but not story by story:
   the unit and integration tests for US1..US5 were all written, and all seen to fail, **before**
   `timer_scheduler.cpp` existed, and the implementation then landed in one pass. Writing it in five
   passes would have meant rewriting the coordinator and worker loops four times for no behavioural
   difference. Two defects below were found by tests written before the code they broke.
3. **The design document's fixed-rate example was wrong**, and the tests found it. With a 100 ms
   interval and a 250 ms callback starting at t=100, the slots at 200 **and** 300 are both missed, so
   `periods_skipped_total` is 2, not 1. The arithmetic of research R-14 is now stated in terms of the
   cadence index of the current deadline (index 0 is the phase), and the worked table in
   contracts/timing_semantics.md section 4 was corrected to match. The code implements the corrected
   formula; `FixedRate_SkipsMissedTicks` asserts the whole sequence.
4. **Conservation equation M2 double-counted a cancellation.** As first written it added
   `callbacks_skipped_cancelled_total` as its own term, but a timer cancelled while queued is already
   counted once in `timers_cancelled_total`; the skip counter only says where the cancellation
   landed. The equation is now
   `timers_scheduled_total == finished_one_shots + disabled_periodics + timers_cancelled_total + timers_active`,
   and `Metrics_ConservationAfterMixedOutcomes` checks it over eleven timers with six different
   outcomes.
5. **Logging needed a level threshold after all.** Spec section 15 makes the per-schedule line DEBUG,
   and with no filter a 10,000-timer run emits 10,000 lines before it starts measuring anything.
   `emit` now drops anything below a minimum level read once from `TCU_SCHEDULER_LOG_LEVEL`
   (default `info`), which keeps every INFO/WARN/ERROR transition observable and makes the DEBUG
   line opt-in. The shortcut ledger below was updated instead of leaving the old claim standing.
6. **Principle II, acted on rather than noted.** Project 01's `TestGate` already did what the
   scheduler tests needed, so it moved from `tests/unit/event_bus/support/` to `tests/support/` and
   both suites now include it through a `tests/` include path — the three existing `#include
   "support/test_gate.hpp"` lines in project 01 are unchanged. It gained an entered-count overload,
   additive, because a scheduler runs several workers. Project 02's `TempSocketDir` is reused as-is
   by IT-04 rather than copied.

7. **ThreadSanitizer found a real use-after-free that review did not.** `attach()` (item 1) hands the
   clock a pointer to the coordinator's mutex and condition variable, and an injected clock is a
   `shared_ptr` that normally outlives the scheduler. A test that advanced the clock after the
   scheduler was destroyed therefore locked a freed mutex and broadcast on a freed condition
   variable — `Lifecycle.DestructorStopsRunningScheduler` failed under TSan while passing everywhere
   else. The fix is the missing half of the pair: `ClockAdapter::detach()`, called once every thread
   has joined, with `FakeClockAdapter` clearing both pointers.
   `Lifecycle.ClockOutlivesSchedulerSafely` now pins it on both the explicit-stop and the
   destructor-only path. This is exactly the class of defect spec section 19.3 asks TSan for: the
   shared state lived inside a type that looked single-owner.

8. **ThreadSanitizer also found a flaky assertion in a test.**
   `CallbackError.ThrowIsCountedAndWorkerSurvives` read `callbacks_completed_total` immediately after
   the `Recorder` saw the second callback, but a recorder entry proves only that the callback
   *started*: the worker increments the counter after it returns. The window is invisible in a plain
   build and wide enough to fail under TSan. The test now waits for the outcome with
   `waitForFinished`. A sweep of the whole suite for the same shape - a counter assertion within
   three lines of a `waitFor` with no `waitForFinished`, `waitForActive` or `stop()` in between -
   found no other instance.

9. **One more test-setup defect, found by re-running rather than by review.** `IpcClient::start()`
   connects asynchronously, so IT-04's first request could come back `NotConnected` when the
   handshake lost the race - it passed for several runs and then failed. The four IPC tests now wait
   for `ClientState::Connected` before issuing a request. This is a test bug, not a scheduler bug,
   and it is recorded because "it passed yesterday" is exactly how such a test survives review.

**Post-Phase 1 re-check**: still PASS. Phase 1 added no third-party dependency, no abstraction with
a single implementation, and no public API beyond spec section 9 plus `ClockAdapter`,
`SchedulerMetrics` and `metrics()` — each required by section 6 (injectable clock, testability) and
section 15 (metrics must be exposed). Complexity Tracking stays empty.

### Requirement to test traceability

| Requirement | Covered by |
|---|---|
| FR-01 one-shot delay | `OneShot_StartsExactlyOnce` (UT-03), `OneShot_NotBeforeDeadline`, `ZeroDelayIsImmediate` (UT-03) |
| FR-02 absolute deadline | `ScheduleAt_FutureDeadline`, `ScheduleAt_PastDeadlineIsImmediate` |
| FR-03 periodic fixed-delay | `FixedDelay_NextIsCompletionPlusInterval` (UT-08), `FixedDelay_NoOverlap` |
| FR-04 periodic fixed-rate | `FixedRate_DeadlinesAlignToPhase`, `FixedRate_SkipsMissedTicks` (UT-09), `FixedRate_NoBurst` |
| FR-05 cancellation | `Cancel_Scheduled` (UT-05), `Cancel_Queued` (UT-06), `Cancel_Running` (UT-07) |
| FR-06 idempotent cancellation | `Cancel_IdSemantics` (UT-13), `Cancel_ZeroAndMaxAreNotFound` |
| FR-07 bounded capacity | `Capacity_RejectsBeyondMaxTimers` (UT-11), `ReadyQueue_NeverExceedsCapacity`, `Backpressure_OneShotNotLost` (IT-01) |
| FR-08 lifecycle | `Lifecycle_StartOnce` (UT-01), `Lifecycle_StartTwiceIsInvalidState` (UT-02), `Lifecycle_ScheduleAfterStop` (UT-12), `Lifecycle_StopIdempotent`, `StopDrain_*` (IT-02), `StopDiscard_*` (IT-03) |
| FR-09 per-task error isolation | `Callback_ThrowIsCountedAndWorkerSurvives` (UT-10), `Callback_ThrowDisablesPeriodic` |
| FR-10 metrics | `Metrics_StartedEqualsCompletedPlusFailed`, `Metrics_ConservationAfterMixedOutcomes`, `Metrics_RejectionReasonsDisjoint`, `Metrics_ActiveGaugeTracksRegistry` |
| FR-11 no callback lock nesting | `Lock_CallbackCanScheduleAndCancel` — a nested lock would deadlock the test (S1) |
| FR-12 integration | `EventBusHeartbeat_SubscriberReceivesTicks` (IT-05), `IpcDeadline_SingleCompletion` (IT-04) |
| NFR safety (no race, no UAF, no double execution, no deadlock, no busy loop) | ASan/UBSan job, TSan job (clang + setarch), `Coordinator_NoWakeupWithoutDueTimerOrNotify` (S5), ST-01 |
| NFR bounded memory | `Capacity_RejectsBeyondMaxTimers`, `DeadlineQueue_CompactionBoundsSize`, ST-03 churn, PT-01 |
| NFR recovery (exception cannot kill a worker) | UT-10, then the suite continues to pass on the same scheduler instance |
| NFR shutdown terminates | `Lifecycle_StopJoinsEveryThread` (S2), IT-02, IT-03, ST-02 repeated fresh instances |
| NFR testability (injected clock) | `FakeClock_AdvanceWakesCoordinator` (UT-14) and every other unit deadline case |
| NFR latency (measured, not asserted) | `benchmarks/scheduler_bench` PT-01 — reported, never gated (R-18) |

### BDD scenario traceability (spec section 13)

| ID | Spec test id | Covered by | Kind |
|---|---|---|---|
| B-01 | UT-01 | `Lifecycle_StartOnce` | unit |
| B-02 | UT-02 | `Lifecycle_StartTwiceIsInvalidState` | unit |
| B-03 | UT-03 | `ZeroDelayIsImmediate`, `OneShot_StartsExactlyOnce` | unit |
| B-04 | UT-04 | `Validation_NegativeDelayIsInvalidArgument` | unit |
| B-05 | UT-05 | `Cancel_Scheduled` | unit |
| B-06 | UT-06 | `Cancel_Queued` | unit |
| B-07 | UT-07 | `Cancel_Running` | unit |
| B-08 | UT-08 | `FixedDelay_NextIsCompletionPlusInterval` | unit |
| B-09 | UT-09 | `FixedRate_SkipsMissedTicks` | unit |
| B-10 | UT-10 | `Callback_ThrowIsCountedAndWorkerSurvives` | unit |
| B-11 | UT-11 | `Capacity_RejectsBeyondMaxTimers` | unit |
| B-12 | IT-01 | `Backpressure_OneShotNotLost` | integration |
| B-13 | IT-02 | `StopDrain_RunsDueCancelsFuture` | integration |
| B-14 | IT-03 | `StopDiscard_DropsPendingFinishesInFlight` | integration |
| B-15 | UT-12 | `Lifecycle_ScheduleAfterStop` | unit |
| B-16 | UT-13 | `Cancel_IdSemantics` | unit |
| B-17 | ST-01 | `Stress_ConcurrentScheduleCancel` | stress + TSan |
| B-18 | UT-14 | `FakeClock_AdvanceWakesCoordinator` | unit |
| B-19 | IT-04 | `IpcDeadline_SingleCompletion` | integration (Linux only) |
| B-20 | PT-01 | `Stress_TenThousandTimersBounded` + `scheduler_bench` | stress + benchmark |

B-17 is listed as a sanitizer target as well because its assertion is partly the *absence* of a
race, which only TSan can observe; the test itself asserts that every scheduled timer reaches exactly
one terminal outcome.

Three cases exist beyond the spec's 20 contracts, each because a Phase 1 invariant would otherwise be
unobservable: `Lock_CallbackCanScheduleAndCancel` (S1/FR-11, flow 12.15),
`Coordinator_NoWakeupWithoutDueTimerOrNotify` (S5, the "no busy-waiting" goal of section 2) and
`EventBusHeartbeat_SubscriberReceivesTicks` (IT-05, FR-12's Event Bus half, which section 13's matrix
covers only for IPC).

## Project Structure

### Documentation (this feature)

```text
specs/003-timer-task-scheduler/
  plan.md                      # This file
  research.md                  # Phase 0: closes the spec's open decisions (R-00..R-18)
  data-model.md                # Phase 1: entities, states, invariants S*/T*/H*/Q*/M*
  quickstart.md                # Phase 1: build, test, both demos, benchmark, acceptance checklist
  contracts/
    scheduler_api.md           # Public API pre/postconditions, thread safety, not-in-v1
    timing_semantics.md        # One-shot, both periodic modes, backpressure, shutdown, determinism
    metrics.md                 # Metric definitions, conservation equations, log fields
  tasks.md                     # Phase 2 output (/speckit-tasks - NOT created here)
```

### Source Code (repository root)

Spec section 17's layout, re-rooted into the monorepo:

```text
CMakeLists.txt                                 # root: project 03 block, no platform guard
README.md                                      # extended with the scheduler section

modules/scheduler/
  CMakeLists.txt                               # target tcu_scheduler
  include/tcu/scheduler/scheduler_types.hpp    # Clock, TimerId, Callback, enums, Config, ScheduleResult
  include/tcu/scheduler/clock_adapter.hpp      # ClockAdapter, SteadyClockAdapter, FakeClockAdapter
  include/tcu/scheduler/scheduler_metrics.hpp  # Histogram, SchedulerMetrics
  include/tcu/scheduler/timer_scheduler.hpp    # TimerScheduler
  src/clock_adapter.cpp
  src/log.hpp                                  # internal: eight fixed fields, closed event set
  src/log.cpp
  src/metrics.hpp                              # internal: atomics behind the snapshot
  src/metrics.cpp
  src/timer_record.hpp                         # internal: TimerRecord, TimerState, TimerKind
  src/deadline_queue.hpp                       # internal: min-heap + generation staleness + compaction
  src/deadline_queue.cpp
  src/ready_queue.hpp                          # internal: bounded FIFO of ReadyJob
  src/ready_queue.cpp
  src/timer_scheduler.cpp                      # Impl: registry, coordinator loop, worker loop, lifecycle

apps/telemetry_timer_demo/main.cpp             # periodic sampling -> project 01 EventBus
apps/modem_timeout_demo/main.cpp               # AT timeout racing a reply, atomic single completion

tests/unit/scheduler/
  support/test_gate.hpp                        # CV + predicate rendezvous (modelled on project 01's)
  support/counting_clock.hpp                   # FakeClockAdapter wrapper that counts waits (S5)
  lifecycle_test.cpp                           # UT-01, UT-02, UT-12, stop idempotence, S2, S6
  validation_test.cpp                          # UT-04, section 9.1's five input rules
  one_shot_test.cpp                            # UT-03, FR-02, T1, T2
  periodic_test.cpp                            # UT-08, UT-09, T4, T7, R-14's arithmetic
  cancel_test.cpp                              # UT-05, UT-06, UT-07, UT-13, T3, T5
  capacity_test.cpp                            # UT-11, Q1, Q3
  deadline_queue_test.cpp                      # H1, H2, H3
  ready_queue_test.cpp                         # Q1, Q4
  callback_error_test.cpp                      # UT-10, T6
  clock_adapter_test.cpp                       # UT-14, S5
  lock_test.cpp                                # FR-11, S1, flow 12.15
  metrics_test.cpp                             # M1..M6
  log_test.cpp                                 # field set, closed event set, no payload path

tests/integration/scheduler/
  backpressure_test.cpp                        # IT-01, Q2
  shutdown_test.cpp                            # IT-02, IT-03
  event_bus_test.cpp                           # IT-05, FR-12 (links tcu_eventbus)
  ipc_deadline_test.cpp                        # IT-04, flow 12.13 (links tcu_ipc, Linux only)

tests/stress/scheduler/
  concurrency_test.cpp                         # ST-01 (B-17), ST-02 repeated fresh instances
  churn_test.cpp                               # ST-03, section 14's timer-churn row, H3
  scale_test.cpp                               # PT-01 (B-20), 10k timers, fake-clock burst

benchmarks/scheduler_bench/main.cpp            # PT-01 measurement and the section 19.4 report

scripts/run_scheduler_demo.sh                  # the section 20 sequence, both demos
```

**Structure Decision**: spec section 17's tree is adopted with five deltas, each justified:

1. **Re-rooted into the monorepo.** `03_timer_task_scheduler/` becomes `modules/scheduler/` plus
   `apps/`, `tests/*/scheduler/` and `benchmarks/`, matching what projects 01 and 02 already did.
   Principle II: one repo, one target per project, tests beside their siblings.
2. **`docs/system_design.md` and `docs/state_transitions.md` are not created.** The design document
   already exists twice — as
   `Virtual_TCU_Lab_30_System_Designs/03_Timer_Task_Scheduler_System_Design.md` and as this feature's
   `spec.md`. A third copy would drift. The state transitions `docs/state_transitions.md` would hold
   are in [data-model.md](./data-model.md) section 2, in table form, with the test that asserts each
   one. Same delta projects 01 and 02 recorded.
3. **`examples/` becomes `apps/`**, because that is where this repo's runnable demos live and where
   the root `CMakeLists.txt` already looks.
4. **`scripts/run_tests.sh` and `scripts/run_sanitizers.sh` are not created.** Both would wrap three
   `cmake`/`ctest` invocations that [quickstart.md](./quickstart.md) section 1 already spells out, and
   the repo has no equivalent for projects 01 or 02. `scripts/run_scheduler_demo.sh` *is* created,
   because the section 20 demo sequence is seven steps across two binaries — that one earns a script.
5. **Six private headers are added under `src/`** beyond section 17's four `.cpp` files
   (`log`, `metrics`, `timer_record`, `deadline_queue`, `ready_queue`), while `clock_adapter.hpp`
   goes in `include/` because tests inject it. Section 7.1 names `TimerRegistry`, `DeadlineQueue`,
   `ReadyQueue`, `WorkerPool`, `ClockAdapter` and `Metrics` as components, and section 15 requires
   structured logging; the four listed `.cpp` files cannot hold them. The private ones live under
   `src/`, not `include/`, so the public surface projects 04-30 depend on stays the four headers of
   section 17 — and `WorkerPool` deliberately gets no file of its own: it is a
   `std::vector<std::thread>` and one loop function inside `timer_scheduler.cpp`, because a class
   whose entire state is a thread vector is a wrapper nobody asked for.

The root `CMakeLists.txt` is extended, not restructured. Project 03's block goes **outside** the
existing `if(CMAKE_SYSTEM_NAME STREQUAL "Linux")` guard (R-00), with only
`tests/integration/scheduler/ipc_deadline_test.cpp` guarded, since that single file links the
Linux-only `tcu_ipc`.

## Complexity Tracking

No Constitution Check violations. Nothing to justify.

The three decisions that came closest to needing an entry, and why they do not:

| Candidate | Why it is not a violation |
|---|---|
| A public `FakeClockAdapter` shipped in `include/` | It looks like test scaffolding in a production header. But spec section 6 makes "deterministic deadline tests using an injected clock" a requirement, and projects 10, 13 and 15 all have timeouts to test. Keeping it private would make each of them write it again — the duplication Principle II exists to prevent. |
| A private `log.hpp` and `metrics.hpp` that resemble project 02's | Principle II forbids re-implementing what an earlier project *provides*. Project 02 provides neither: both headers live under `modules/ipc/src/`, are unreachable from outside that target, and hard-code IPC's field and counter names. Reuse would mean promoting project 02's internals to a public surface for projects 04-30 — a larger change than the one it avoids. The shared-logging fix belongs to project 04 (R-16). |
| One mutex covering registry, heap, states and ready queue | It is coarser than the two-mutex design section 10 mentions first. Section 10 explicitly permits "a single scheduling mutex plus condition variables", and the single mutex is what makes `scheduleAfter()` from inside a callback deadlock-free with no lock-order rule to get wrong (R-08). Contention is not a concern at 100-1,000 callbacks/s. |

## Definition of Done (spec section 21, verified T069)

Verified 2026-11-04 on WSL2 Ubuntu 22.04, GCC 11.4 / Clang 14, plus a native Windows g++ 16.1
compile. Test counts are per binary: 87 unit, 20 integration, 9 stress = **116**.

| Criterion | Status | Evidence |
|---|---|---|
| Implements FR-01..FR-12, or documents deviations | MET | Requirement traceability table above; every FR names at least one test. The deviations are the four rows at the end of research.md |
| All 20 behavioural contracts covered by tests | MET | BDD traceability table above maps B-01..B-20 to named unit, integration or stress cases, all green |
| No callbacks run under internal scheduler locks | MET | `Lock_CallbackCanScheduleAndCancel`, `Lock_CallbackCanReadMetrics`, `Lock_StopFromCallbackDoesNotDeadlock` - a nested lock would hang them, and the periodic tests depend on a callback advancing the clock |
| Cancellation and shutdown concurrency contracts documented and tested | MET | contracts/scheduler_api.md 2.5-2.6 and research R-07/R-13; `Cancel.*` (9 cases), `StopDrain.*`/`StopDiscard.*` (8), `Stress.ConcurrentCancelSameId`, `Stress.StopRacesSchedule` |
| Timer and ready queues bounded; heap invalidation compaction tested | MET | `Capacity.*`, `ReadyQueue.*`, `DeadlineQueue.CompactionBoundsSize` (asserts `size() <= 2*live + 64` after 10,000 churn cycles), `Churn.BlockingCallbackDoesNotUnboundQueue` |
| No data race or memory error in supported sanitizer builds | MET | ASan+UBSan 117/117 and TSan 116/116 clean. TSan found two real defects first - the `attach`/`detach` use-after-free (item 7) and a flaky test (item 8) - and passes after both fixes |
| Metrics reflect attempts, start/completion and missed periods | MET | `Metrics.*` covering M1-M6, `MetricsStorage.*` for the histogram and the lateness clamp, `Periodic.FixedRate_SkipsMissedTicks` for the skip count |
| API and examples compile under C++17 and work without hardware | MET | Both demos run end to end; `scripts/run_scheduler_demo.sh` exits 0 on all nine steps. Nothing opens a device; the only socket is in the Linux-gated IT-04. All six library TUs also compile warning-free with Windows g++ 16.1 `-Wall -Wextra`, and a real timer fires there |
| Limitations clearly documented | MET | contracts/timing_semantics.md section 1 ("Not guaranteed"), contracts/scheduler_api.md section 5 (not-in-v1), the README section "What scheduler v1 does not promise", and the shortcut ledger below |

Measured, not asserted (research R-18): `scheduler_bench --timers 10000 --duration-s 5` on that host,
debug build, 2 workers - about 5,900 callback starts per second, mean start lateness 1.8 ms, p50
under 1 ms, p95 under 10 ms, p99 under 25 ms, zero skipped periods. No CTest case gates on any of it.

One cosmetic artifact: `ctest --test-dir build-wsl -L scheduler` prints 117 rather than 116 because
that build directory still carries a duplicate registration of
`Metrics.ConservationAfterMixedOutcomes` from an earlier discovery pass. The binaries contain 116
tests; a fresh configure clears the stale entry.

## Deferred shortcuts

Every deliberate shortcut carries a `shortcut: <limit>, <when to upgrade>` comment at the code that
takes it. Confirmed against the source after implementation (T067); no further shortcut appeared:

| Where | Limit accepted | Upgrade when |
|---|---|---|
| `modules/scheduler/src/timer_scheduler.cpp`, `stop()` | `stop()` from inside a callback is a logged no-op; the request is dropped rather than queued | the spec adds the asynchronous `requestStop()` that section 9 anticipates (R-12) |
| `modules/scheduler/src/log.cpp` | the level threshold is one environment variable (`TCU_SCHEDULER_LOG_LEVEL`, default `info`) and the sink is always stderr | project 04 (Configuration & Logging Service) owns both (R-16) |
| `modules/scheduler/src/metrics.hpp` | nine fixed histogram buckets, no configurable boundaries, percentiles computed outside the library | a consumer needs a different resolution than spec section 19.4's p50/p95/p99 (R-15) |
| `modules/scheduler/src/deadline_queue.cpp` | compaction thresholds (50% stale, 64 entries) are internal constants, not config | a workload shows the 50% ratio is wrong; `Config` stays at section 9's three fields until then (R-11) |

All four sit inside the envelope spec sections 1 and 2 draw, so none is a Constitution Check
violation — they are recorded here so a later project finds them without reading the source.
