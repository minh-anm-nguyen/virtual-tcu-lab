---

description: "Task list for Timer & Task Scheduler (project 03)"
---

# Tasks: Timer & Task Scheduler

**Input**: Design documents from `/specs/003-timer-task-scheduler/`

**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/, quickstart.md — all present.

**Tests**: REQUIRED, not optional. Constitution Principle III is non-negotiable: every FR gets a unit
test naming it, every behavioural contract B-01..B-20 gets a test, and the test is written and seen
to FAIL before the implementation that satisfies it.

**Platform**: unlike project 02, every command here runs on **both** Linux/WSL and Windows (research
R-00) — the library uses only the standard library. The one exception is
`tests/integration/scheduler/ipc_deadline_test.cpp`, which links the Linux-only `tcu_ipc`. Sanitizer
runs are Linux/WSL only.

**Organization**: phases follow the ticket dependency chain of spec section 18,
`01 -> 02 -> 03 -> 04/05 -> 06 -> 07 -> 08 -> 09 -> 10 -> 12 -> 11/13 -> 14`.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: can run in parallel — different files, no dependency on an incomplete task
- **[Story]**: US1..US7, mapped to the backlog groups below
- Every description carries its exact file path

## Story-to-ticket mapping

| Story | Delivers | Backlog tickets (spec section 18) | Tests delivered |
|---|---|---|---|
| US1 (P1) | A one-shot timer fires once, no earlier than its deadline, on a worker thread, with bounded memory | TCU-SCH-01..06 | UT-01..04, UT-11, UT-12, B-01..04, B-11, B-15; H1-H3, Q1, Q3, Q4, S1-S3, T1, T2 |
| US2 (P2) | Both periodic modes with their documented overload policy, and error isolation | TCU-SCH-07 | UT-08, UT-09, UT-10, B-08, B-09, B-10; T4, T6, T7 |
| US3 (P3) | Cancellation defined in all three races plus the id arithmetic | TCU-SCH-08 | UT-05..07, UT-13, B-05..07, B-16; T3, T5 |
| US4 (P4) | Shutdown Drain/Discard and real backpressure | TCU-SCH-09 | IT-01..03, B-12..14; Q2, S6 |
| US5 (P5) | Structured logs and the full metric set with conservation | TCU-SCH-10 | M1-M6, FR-10, FR-12's log half |
| US6 (P6) | Both demos and the two cross-project integrations | TCU-SCH-12 | IT-04, IT-05, B-19, FR-12 |
| US7 (P7) | Stress, fault matrix, sanitizers and the 10k benchmark | TCU-SCH-11, TCU-SCH-13 | ST-01..03, PT-01, B-17, B-20 |

**Deviation from the template, stated deliberately**: these seven stories are **layers, not
independent features**, exactly as spec section 18's dependency chain declares. US2's rearm needs
US1's coordinator; US3's `RunningCancelRequested` must suppress a rearm that only exists after US2;
US4's drain policy needs US3's cancellation to cancel future timers with. So the template's "all
stories can run in parallel once Foundational is done" does **not** apply. Each story's checkpoint is
a working increment, but the order is fixed. Parallelism exists *within* phases, marked `[P]`.

**Metrics note**: the atomic counters are declared in Foundational (T013) and each story increments
the ones it owns, because UT-11 in US1 asserts `rejected_capacity_total` and cannot wait for US5.
US5 completes the set, adds the structured logging and asserts the M1-M6 conservation equations. This
splits ticket TCU-SCH-10 across the stories; that is intentional, not an oversight.

**Demo note**: the two demo binaries are built in US6 rather than incrementally, because — unlike
project 02's integration tests — nothing in this project forks a binary. Every test links the library
directly, so the demos are pure spec section 20 deliverables.

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: directory structure and build wiring, so a test can be compiled before any library code
exists

- [X] T001 Create the `modules/scheduler/` tree — `include/tcu/scheduler/`, `src/` — and write `modules/scheduler/CMakeLists.txt` declaring a static library target `tcu_scheduler` with `target_include_directories(tcu_scheduler PUBLIC include)` and `PRIVATE src`, `target_compile_features(... PUBLIC cxx_std_17)`, `-Wall -Wextra` on GCC/Clang, and `find_package(Threads REQUIRED)` linked `PUBLIC`
- [X] T002 Append the project 03 block to the root `CMakeLists.txt` **outside** the existing `if(CMAKE_SYSTEM_NAME STREQUAL "Linux")` guard (research R-00): `add_subdirectory(modules/scheduler)` plus the two app directories, and inside the existing `TCU_BUILD_TESTS` block the three test directories. Do not restructure the project 01 or 02 lines
- [X] T003 [P] Create `tests/unit/scheduler/CMakeLists.txt` building one `scheduler_unit_tests` target linked against `tcu_scheduler` and `gtest_main`, with `target_include_directories(... PRIVATE ${CMAKE_SOURCE_DIR}/modules/scheduler/src ${CMAKE_CURRENT_SOURCE_DIR})` so private headers are testable, registered with `gtest_discover_tests(... PROPERTIES LABELS "scheduler-unit")`
- [X] T004 [P] Create `tests/integration/scheduler/CMakeLists.txt` building `scheduler_integration_tests` with label `scheduler-integration`, linking `tcu_scheduler` and `tcu_eventbus`, and adding `ipc_deadline_test.cpp` plus `tcu_ipc` to the target **only** when `CMAKE_SYSTEM_NAME STREQUAL "Linux"`
- [X] T005 [P] Create `tests/stress/scheduler/CMakeLists.txt` building `scheduler_stress_tests` with label `scheduler-stress`
- [X] T006 [P] Create `benchmarks/scheduler_bench/CMakeLists.txt` building the `scheduler_bench` executable, and add `add_subdirectory(benchmarks/scheduler_bench)` to the root `CMakeLists.txt` inside the existing `if(TCU_BUILD_BENCHMARKS)` block
- [X] T007 [P] Create `tests/unit/scheduler/support/test_gate.hpp`: a condition-variable rendezvous with `void open()`, `void waitOpen()`, `bool waitOpenFor(duration)` and `bool opened() const`, so a test can block a callback and release it deterministically. Model it on `tests/unit/event_bus/support/test_gate.hpp`; reuse that file as-is if its interface already suffices (Principle II — check before writing)
- [X] T008 [P] Create `tests/unit/scheduler/support/counting_clock.hpp`: a `FakeClockAdapter` subclass that counts `waitUntil` entries and returns the count, so invariant S5 ("the coordinator never spins") is assertable

**Checkpoint**: `cmake -S . -B build -G Ninja && cmake --build build` configures and builds the
project 01 and 02 targets plus three empty project 03 test targets, on Linux and on Windows.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: the public types, the injectable clock and the counter storage that every story needs

- [X] T009 Create `modules/scheduler/include/tcu/scheduler/scheduler_types.hpp` with exactly the declarations of contracts/scheduler_api.md section 1: `using Clock = std::chrono::steady_clock`, `using TimerId = std::uint64_t` (**0 is never issued and means "no timer"**), `using Callback = std::function<void()>`, `enum class PeriodicMode { FixedDelay, FixedRateSkipMissed }`, `enum class StopMode { Drain, Discard }`, `enum class Error { None, InvalidArgument, InvalidState, CapacityExceeded }`, `enum class CancelResult { Cancelled, RunningCancelRequested, AlreadyFinished, NotFound }`, `struct Config { std::size_t workerThreads{2}; std::size_t maxTimers{10000}; std::size_t maxReadyTasks{1024}; }` and `struct ScheduleResult { TimerId id{0}; Error error{Error::None}; explicit operator bool() const; }`. Add no further enum value
- [X] T010 [P] Create `modules/scheduler/include/tcu/scheduler/scheduler_metrics.hpp`: `struct Histogram { std::array<std::uint64_t, 9> buckets{}; std::uint64_t count{0}; std::uint64_t sum_ms{0}; }` with the upper bounds `{1, 5, 10, 25, 50, 100, 250, 1000, +inf}` documented in a comment, and `struct SchedulerMetrics` holding the 14 plain-value fields of data-model.md section 1.5 in that order
- [X] T011 [P] Write `tests/unit/scheduler/clock_adapter_test.cpp` covering UT-14 and S5, and see it FAIL: `FakeClock_NowStartsAtEpochAndAdvances`, `FakeClock_AdvanceToNeverGoesBackwards`, `FakeClock_AdvanceWakesCoordinator` (a thread blocked in `waitUntil` returns after `advance`), `SteadyClock_WaitUntilReturnsAtDeadline`
- [X] T012 Create `modules/scheduler/include/tcu/scheduler/clock_adapter.hpp` and `modules/scheduler/src/clock_adapter.cpp` implementing the five-rule contract of contracts/scheduler_api.md section 3: abstract `ClockAdapter` with `now()` and `waitUntil(std::unique_lock<std::mutex>&, std::condition_variable&, Clock::time_point)`; `SteadyClockAdapter` delegating to `Clock::now()` and `cv.wait_until`; `FakeClockAdapter` with `now()`, `advance(duration)`, `advanceTo(time_point)` that records the mutex and cv on first `waitUntil`, waits with no timeout, and on advance locks that mutex, moves `now_` forward (never backwards) and calls `notify_all`. Make T011 pass
- [X] T013 Create `modules/scheduler/src/metrics.hpp` and `src/metrics.cpp`: an internal `Metrics` struct of `std::atomic<std::uint64_t>` counters matching every `SchedulerMetrics` field, three atomic histogram bucket arrays plus count and sum, `void recordLateness(Clock::duration)` clamping at 0, `void recordDuration(Clock::duration)`, and `SchedulerMetrics snapshot() const`. The three rejection reasons are three separate counters, never a map (research R-15)
- [X] T014 [P] Create `modules/scheduler/src/timer_record.hpp`: `enum class TimerState { Scheduled, Queued, Running, CancelRequested }`, `enum class TimerKind { OneShot, Periodic }`, and `struct TimerRecord` with the ten fields of data-model.md section 1.2 — `id`, `state`, `generation`, `deadline`, `callback`, `kind`, `mode`, `interval` (**`> 0` only when `kind == Periodic`, zero otherwise**), `phase`, `periodIndex` (**0 before the first fire**)
- [X] T015 Create `modules/scheduler/include/tcu/scheduler/timer_scheduler.hpp` declaring `class TimerScheduler final` with both constructors (`explicit TimerScheduler(Config)` and `TimerScheduler(Config, std::shared_ptr<ClockAdapter>)` — research R-05), the destructor, deleted copy, and the seven public methods of contracts/scheduler_api.md section 2, with `struct Impl; std::unique_ptr<Impl> impl_;`

**Checkpoint**: the public headers compile standalone, `clock_adapter_test.cpp` passes, and nothing
else is implemented yet.

---

## Phase 3: User Story 1 — One-shot timers on a bounded scheduler (Priority: P1) 🎯 MVP

**Goal**: `start()`, `scheduleAfter`/`scheduleAt`, a coordinator that sleeps on the earliest deadline,
workers that run the callback with no lock held, bounded registry and ready queue, and `stop()` that
joins. This is the smallest thing worth having: a working one-shot timer.

**Independent Test**: `ctest --test-dir build -L scheduler-unit` — UT-01..04, UT-11, UT-12 pass, and
`Lock_CallbackCanScheduleAndCancel` completes rather than deadlocking.

### Tests for User Story 1 ⚠️

> Write these FIRST and see each FAIL before the implementation tasks below.

- [X] T016 [P] [US1] Write `tests/unit/scheduler/lifecycle_test.cpp`: `Lifecycle_StartOnce` (UT-01, B-01), `Lifecycle_StartTwiceIsInvalidState` (UT-02, B-02 — assert no extra thread by asserting the second `start()` returns `Error::InvalidState`), `Lifecycle_StartWithZeroWorkersIsInvalidArgument`, `Lifecycle_ScheduleAfterStop` (UT-12, B-15), `Lifecycle_StopIdempotent`, `Lifecycle_StopOnCreatedIsNoOp`, `Lifecycle_StartAfterStopIsInvalidState` (single-use, spec section 8.1), `Lifecycle_StopJoinsEveryThread` (S2), `Lifecycle_NoCallbackAfterStop` (S6), `Lifecycle_DestructorStopsRunningScheduler`
- [X] T017 [P] [US1] Write `tests/unit/scheduler/validation_test.cpp` covering all five input rules of spec section 9.1: `Validation_EmptyCallbackIsInvalidArgument`, `Validation_NegativeDelayIsInvalidArgument` (UT-04, B-04), `Validation_ZeroIntervalIsInvalidArgument`, `Validation_NegativeIntervalIsInvalidArgument`, `Validation_ZeroConfigFieldsAreInvalidArgument` (each of the three fields separately), and `Validation_EmptyCallbackWinsOverStoppedState` — asserting the check order of contracts/scheduler_api.md section 2.2 so exactly one `rejected_*` counter moves (M6)
- [X] T018 [P] [US1] Write `tests/unit/scheduler/one_shot_test.cpp` on the fake clock: `ZeroDelayIsImmediate` (UT-03, B-03), `OneShot_StartsExactlyOnce` (T1), `OneShot_NotBeforeDeadline` (T2 — advance to one tick before the deadline and assert the callback has not run), `ScheduleAt_FutureDeadline`, `ScheduleAt_PastDeadlineIsImmediate` (FR-02), `OneShot_EqualDeadlinesRunInIdOrder` (H1's tiebreak), `OneShot_RecordReleasedAfterCompletion` (research R-03 accounting, via `timers_active`)
- [X] T019 [P] [US1] Write `tests/unit/scheduler/deadline_queue_test.cpp` against the private header: `DeadlineQueue_EarliestWins` (H1), `DeadlineQueue_TieBreaksOnId`, `DeadlineQueue_StaleEntryIgnored` (H2 — generation mismatch and missing record), `DeadlineQueue_CompactionBoundsSize` (H3 — after 10,000 push/invalidate cycles with 100 live entries assert `size() <= 2*100 + 64`)
- [X] T020 [P] [US1] Write `tests/unit/scheduler/ready_queue_test.cpp`: `ReadyQueue_Fifo` (Q4), `ReadyQueue_NeverExceedsCapacity` (Q1), `ReadyQueue_TryPushReportsFullWithoutGrowing`
- [X] T021 [P] [US1] Write `tests/unit/scheduler/capacity_test.cpp`: `Capacity_RejectsBeyondMaxTimers` (UT-11, B-11, Q3 — with `maxTimers == 4`, the fifth `scheduleAfter` returns `CapacityExceeded`, `id == 0`, `timers_active` stays 4 and `rejected_capacity_total` is 1), `Capacity_SlotReleasedAfterCallbackCompletes`, `Capacity_PeriodicHoldsOneSlotAcrossManyFires`
- [X] T022 [P] [US1] Write `tests/unit/scheduler/lock_test.cpp`: `Lock_CallbackCanScheduleAndCancel` (FR-11, S1, flow 12.15 — a callback calls `scheduleAfter` and `cancel` and the test completes within a deadline rather than deadlocking), `Lock_CallbackCanReadMetrics`, `Lock_StopFromCallbackDoesNotDeadlock` (research R-12 — `stop()` inside a callback returns immediately and the scheduler is still `Running`)

### Implementation for User Story 1

- [X] T023 [P] [US1] Implement `modules/scheduler/src/deadline_queue.hpp` and `src/deadline_queue.cpp`: `struct HeapEntry { Clock::time_point deadline; TimerId id; std::uint64_t generation; }`, a `std::priority_queue`-equivalent ordered by `(deadline, id)` ascending, `push`, `peek`, `pop`, `markStale()` and `compactIfNeeded(registry)` rebuilding when `staleEstimate_ * 2 > size()` **and** `size() > 64` (research R-11; both are internal constants, not `Config` fields). Make T019 pass
- [X] T024 [P] [US1] Implement `modules/scheduler/src/ready_queue.hpp` and `src/ready_queue.cpp`: `struct ReadyJob { std::shared_ptr<TimerRecord> record; std::uint64_t generation; Callback callback; Clock::time_point deadline; }` and a bounded FIFO `std::deque` with `size()`, `full()`, `push()`, `pop()` and `clear()`, capacity fixed at construction from `maxReadyTasks`. Make T020 pass
- [X] T025 [US1] Implement `TimerScheduler::Impl` in `modules/scheduler/src/timer_scheduler.cpp`: the `std::unordered_map<TimerId, std::shared_ptr<TimerRecord>>` registry, `nextId_` starting at 1, the single `std::mutex mu_`, `coord_cv_`, `work_cv_`, the scheduler state enum, the `Metrics` member, the clock `shared_ptr`, and both constructors (a null clock becomes a `SteadyClockAdapter`)
- [X] T026 [US1] Implement `TimerScheduler::start()`: validate the three `Config` fields, reject a non-`Created` state with `InvalidState`, create one coordinator thread plus `workerThreads` workers, record their `std::thread::id`s for the R-12 check, and set state `Running`. Make the start/stop-state cases of T016 pass
- [X] T027 [US1] Implement the coordinator loop: under `mu_`, prune stale heap entries, `compactIfNeeded`, and either wait on `coord_cv_` with no deadline when the heap is empty, or `clock_->waitUntil(lk, coord_cv_, head.deadline)` when the head is in the future, or dispatch when `clock_->now() >= head.deadline` — popping the entry, setting `Queued`, pushing a `ReadyJob` and notifying `work_cv_`. Never invoke a callback here
- [X] T028 [US1] Implement the worker loop: wait on `work_cv_` for a job or for stopping; pop under `mu_`; claim `Queued -> Running`, sample lateness and increment `callbacks_started_total`; **unlock**; invoke the callback; re-lock and record the outcome and duration. Make T022's `Lock_CallbackCanScheduleAndCancel` pass
- [X] T029 [US1] Implement `scheduleAfter`, `scheduleAt` and the shared registration path: the check order of contracts/scheduler_api.md section 2.2 (empty callback → negative delay → state → capacity), one `clock_->now()` reading per call, a new record at `nextId_++`, a heap push, `timers_scheduled_total++`, and `coord_cv_.notify_one()`. On rejection allocate no record and increment exactly one `rejected_*` counter. Make T017, T018 and T021 pass
- [X] T030 [US1] Implement `scheduleEvery`'s registration only (validation, `kind = Periodic`, `interval`, `phase = now + interval`, `periodIndex = 0`); leave the rearm to US2. Make `Capacity_PeriodicHoldsOneSlotAcrossManyFires` compile and the validation cases of T017 pass
- [X] T031 [US1] Implement `stop(StopMode)` for the `Discard` path plus the destructor: the R-12 self-call check (log-and-return comes in US5; for now return without changing state), `Created -> Stopped` with nothing to join, cancel every record not `Running`, `ready_.clear()`, notify both cvs, join the coordinator and every worker, `Stopped`, and make a repeated `stop()` a no-op. The destructor calls `stop(Discard)` when still `Running`. Make the rest of T016 pass
- [X] T032 [US1] Implement `metrics()` returning `impl_->metrics_.snapshot()` with `timers_active` and `ready_queue_depth` read as the live registry and queue sizes, callable in every state including `Created` and `Stopped`

**Checkpoint**: a one-shot timer fires exactly once, no earlier than its deadline, on a worker thread,
with the registry and ready queue both bounded, and `stop()` joins every thread. US1 is a usable
scheduler for any caller that only needs timeouts.

---

## Phase 4: User Story 2 — Periodic modes and error isolation (Priority: P2)

**Goal**: `FixedDelay` and `FixedRateSkipMissed` with the exact arithmetic of research R-14, no
self-overlap, no catch-up burst, and a throwing callback that cannot kill a worker.

**Independent Test**: `ctest -L scheduler-unit -R 'Periodic|FixedDelay|FixedRate|Callback'` — the two
worked examples of contracts/timing_semantics.md sections 3 and 4 reproduce tick for tick on the fake
clock.

### Tests for User Story 2 ⚠️

- [X] T033 [P] [US2] Write `tests/unit/scheduler/periodic_test.cpp` for fixed-delay: `FixedDelay_NextIsCompletionPlusInterval` (UT-08, B-08 — reproduce the table of contracts/timing_semantics.md section 3 exactly: interval 100, a 250-long callback, starts at 100 then 450), `FixedDelay_NoOverlap` (T4), `FixedDelay_NeverSkips` (`periods_skipped_total` stays 0), `FixedDelay_FiresRepeatedlyUntilCancelled`
- [X] T034 [P] [US2] Extend `tests/unit/scheduler/periodic_test.cpp` for fixed-rate: `FixedRate_DeadlinesAlignToPhase`, `FixedRate_SkipsMissedTicks` (UT-09, B-09 — reproduce the five-row table of contracts/timing_semantics.md section 4: `periodIndex` 0→1→3→4 and `periods_skipped_total == 1`), `FixedRate_NoBurst` (T7 — at least `interval` of clock time between consecutive starts), `FixedRate_CompletionExactlyOnBoundaryWaitsForNextSlot` (the "strictly after" rule), `FixedRate_NoOverlap` (T4)
- [X] T035 [P] [US2] Write `tests/unit/scheduler/callback_error_test.cpp`: `Callback_ThrowIsCountedAndWorkerSurvives` (UT-10, B-10 — `callbacks_failed_total == 1`, and a second timer scheduled afterwards still runs), `Callback_ThrowDisablesPeriodic` (T6, research R-09 — the periodic record is gone and no further callback occurs), `Callback_ThrowOnOneShotOnlyCounts`, `Callback_NonStdExceptionIsAlsoCaught` (throw an `int`)

### Implementation for User Story 2

- [X] T036 [US2] Implement the fixed-delay rearm in the worker's outcome path: on normal completion of a `Periodic`/`FixedDelay` record, set `deadline = completion_time + interval`, bump `generation`, state back to `Scheduled`, heap push, notify `coord_cv_`. Make T033 pass
- [X] T037 [US2] Implement the fixed-rate rearm with the integer arithmetic of research R-14 — `k = max(periodIndex + 1, elapsed / interval + 1)` where `elapsed = completion_time - phase`, then `periods_skipped_total += k - periodIndex - 1`, `periodIndex = k`, `deadline = phase + k * interval`. Use `Clock::duration` ticks only, no floating point. Make T034 pass
- [X] T038 [US2] Implement the worker's exception boundary: `catch (...)` around the callback, `callbacks_failed_total++`, record the duration, and remove the record instead of rearming — for both kinds (research R-09). No exception may cross the `std::thread` boundary. Make T035 pass

**Checkpoint**: both periodic modes behave as contracts/timing_semantics.md documents, neither
overlaps itself, neither bursts, and a throwing callback is counted and isolated.

---

## Phase 5: User Story 3 — Cancellation in every race (Priority: P3)

**Goal**: `cancel()` returns a defined result for `Scheduled`, `Queued`, `Running`, an issued-but-gone
id and a never-issued id, is idempotent, and suppresses a periodic rearm.

**Independent Test**: `ctest -L scheduler-unit -R Cancel` — UT-05, UT-06, UT-07 and UT-13 pass, and
`cancel(0)` / `cancel(UINT64_MAX)` return `NotFound` without crashing.

### Tests for User Story 3 ⚠️

- [X] T039 [P] [US3] Write `tests/unit/scheduler/cancel_test.cpp` for the three live races: `Cancel_Scheduled` (UT-05, B-05 — returns `Cancelled`, the callback never runs, `timers_active` drops and `timers_cancelled_total` is 1), `Cancel_Queued` (UT-06, B-06 — with workers held by a `TestGate`, cancel a job already in the ready queue, then release: the callback does not run and `callbacks_skipped_cancelled_total` is 1), `Cancel_Running` (UT-07, B-07 — returns `RunningCancelRequested`, the callback is not interrupted and runs to completion)
- [X] T040 [P] [US3] Extend `tests/unit/scheduler/cancel_test.cpp` with the id semantics of research R-07: `Cancel_IdSemantics` (UT-13, B-16 — a second `cancel` of a cancelled id returns `AlreadyFinished`; a cancel of a completed one-shot returns `AlreadyFinished`; a cancel while `CancelRequested` returns `AlreadyFinished`), `Cancel_ZeroAndMaxAreNotFound`, `Cancel_IdsAreStrictlyIncreasingAndNeverReused` (T5), `Cancel_RunningPeriodicIsNotRearmed` (flow 12.5), `Cancel_FromInsideOwnCallbackStopsPeriodic`

### Implementation for User Story 3

- [X] T041 [US3] Implement `cancel(TimerId)` as the full five-row resolution table of
  contracts/scheduler_api.md section 2.5, under `mu_`: look the id up; for `Scheduled`/`Queued` bump
  `generation`, erase the record, `timers_cancelled_total++`, mark the heap entry stale and return
  `Cancelled`; for `Running` set `CancelRequested` and return `RunningCancelRequested`; for
  `CancelRequested` return `AlreadyFinished`; for an absent id return `AlreadyFinished` when
  `1 <= id < nextId_` and `NotFound` otherwise. Never block on a callback. Make T039's first two cases
  and all of T040 pass
- [X] T042 [US3] Implement the worker-side cancellation checks: on popping a job, compare
  `job.generation` with `record->generation` and skip the callback when stale, incrementing
  `callbacks_skipped_cancelled_total`; after a callback returns in state `CancelRequested`, remove the
  record instead of rearming. Make the rest of T039 pass

**Checkpoint**: every cancellation race of spec section 8.2 has a defined, tested outcome, and no
cancelled timer can start.

---

## Phase 6: User Story 4 — Shutdown policies and backpressure (Priority: P4)

**Goal**: `stop(Drain)`'s bounded drain, `stop(Discard)`'s cancellation, and a full ready queue that
parks a due one-shot instead of dropping it.

**Independent Test**: `ctest -L scheduler-integration -R 'Backpressure|StopDrain|StopDiscard'` —
IT-01, IT-02 and IT-03 pass on the real clock.

### Tests for User Story 4 ⚠️

- [X] T043 [P] [US4] Write `tests/integration/scheduler/backpressure_test.cpp`: `Backpressure_OneShotNotLost` (IT-01, B-12, Q2 — with `workerThreads == 1` and `maxReadyTasks == 2`, hold the worker on a `TestGate`, make 10 one-shots due, assert `ready_queue_depth` never exceeds 2, then release and assert all 10 callbacks ran exactly once), `Backpressure_QueueDepthNeverExceedsCapacity` (Q1), `Backpressure_FixedRateSkipsWhenQueueFull` (research R-10 — `periods_skipped_total` grows while one-shots do not), `Backpressure_RegistryFillsThenScheduleIsRejected` (the R-10 chain to `CapacityExceeded`), `Backpressure_CoordinatorDoesNotSpin` (S5, via `counting_clock.hpp`)
- [X] T044 [P] [US4] Write `tests/integration/scheduler/shutdown_test.cpp`: `StopDrain_RunsDueCancelsFuture` (IT-02, B-13 — one timer already due and one due far later; after `stop(Drain)` the first ran and the second is counted in `timers_cancelled_total`), `StopDrain_WaitsForInFlightCallback`, `StopDrain_PeriodicNotRearmedDuringDrain`, `StopDrain_TerminatesWithAOneMillisecondPeriodicTimer` (the reason research R-13 reads the clock once), `StopDiscard_DropsPendingFinishesInFlight` (IT-03, B-14), `StopDiscard_ClearedJobsCountedAsCancelled`, `Stop_RepeatedIsNoOp` (B-15), `Stop_QueueEmptyAfterReturn` (M5)

### Implementation for User Story 4

- [X] T045 [US4] Implement the ready-queue-full branch of the coordinator per research R-10: when `ready_.full()`, leave the due timer `Scheduled` with its **deadline unchanged** so it stays at the heap head, and wait on `coord_cv_`; have every worker notify `coord_cv_` after popping a job. For a `FixedRateSkipMissed` record instead skip the tick, increment `periods_skipped_total` and compute the next aligned deadline. Never block forever and never poll. Make T043 pass
- [X] T046 [US4] Implement `stop(StopMode::Drain)` per research R-13: state `Running -> StoppingDrain`, one `clock_->now()` reading `t`, cancel every `Scheduled` record with `deadline > t` and count it, keep the rest, suppress every rearm for the remainder of the drain, let the coordinator and workers finish the due and queued set honouring `maxReadyTasks`, then join and set `Stopped`. Make T044 pass
- [X] T047 [US4] Make a concurrent `stop()` call wait for the in-progress stop rather than racing it, and make `schedule*` return `InvalidState` with `rejected_invalid_state_total++` in both stopping states

**Checkpoint**: both shutdown policies terminate and are observably different, and sustained overload
costs lateness rather than correctness.

---

## Phase 7: User Story 5 — Structured logs and metric conservation (Priority: P5)

**Goal**: the eight-field log line with its closed event vocabulary, and the full metric set with the
M1-M6 equations holding.

**Independent Test**: `ctest -L scheduler-unit -R 'Metrics|Log'` — M1..M6 hold after a mixed workload
and the log renderer has no parameter a payload could travel through.

### Tests for User Story 5 ⚠️

- [X] T048 [P] [US5] Write `tests/unit/scheduler/log_test.cpp`: `Log_FieldOrderAndCount` (the eight fields of contracts/metrics.md section 3, in order, single-space separated), `Log_AbsentOptionalsRenderAsDash`, `Log_TimestampIsIso8601UtcMillis`, `Log_EventVocabularyIsClosed` (only `start`, `schedule`, `cancel`, `backpressure`, `callback`, `stop`), `Log_RendersEachSpecSection15ExampleLine`, and `Log_HasNoPayloadParameter` — a static assertion over the `format` signature, so a payload has no path into a line (Principle V)
- [X] T049 [P] [US5] Write `tests/unit/scheduler/metrics_test.cpp`: `Metrics_StartedEqualsCompletedPlusFailed` (M1), `Metrics_ConservationAfterMixedOutcomes` (M2 — run completions, cancellations, queued-cancellations, throws and leftovers in one scheduler, counting timers not invocations), `Metrics_LatenessSampledPerStart` (M3), `Metrics_DurationSampledPerFinish` (M4), `Metrics_QueueEmptyAfterStop` (M5), `Metrics_RejectionReasonsDisjoint` (M6), `Metrics_ActiveGaugeTracksRegistry` (S4), `Metrics_LatenessClampedAtZero`, `Metrics_HistogramBucketBoundaries` (a 1 ms sample lands in bucket 0, a 1001 ms sample in bucket 8)

### Implementation for User Story 5

- [X] T050 [US5] Create `modules/scheduler/src/log.hpp` and `src/log.cpp`: `enum class LogLevel { Debug, Info, Warn, Error }`, a `LogFields` struct holding `level`, `service` (fixed `scheduler`), `std::optional<TimerId> timer_id`, `mode`, `event`, `reason` and `std::optional<std::uint64_t> value`, a `std::string format(const LogFields&)` exposed for the test, and `void emit(const LogFields&)` writing one newline-terminated line to stderr. No payload or capture parameter exists. Make T048 pass
- [X] T051 [US5] Wire the log call sites of contracts/metrics.md section 3: `INFO start` on a successful `start()`, `DEBUG schedule` on each accepted `schedule*`, `INFO cancel` on `Cancelled` and `RunningCancelRequested`, `WARN backpressure` **once per transition into the full state** (not per blocked dispatch), `ERROR callback reason=exception`, `INFO stop` at begin and end, and `ERROR stop reason=called_from_scheduler_thread` for the research R-12 misuse — replacing T031's silent return
- [X] T052 [US5] Complete every remaining counter increment and histogram sample so the M1-M6 equations hold, and add the `shortcut:` comments the plan's ledger lists at `log.cpp`, `metrics.hpp`, `deadline_queue.cpp` and `timer_scheduler.cpp`'s `stop()`. Make T049 pass

**Checkpoint**: every state transition spec section 15 names is observable in logs or metrics, and
"nothing was lost" is arithmetic rather than review.

---

## Phase 8: User Story 6 — Demos and cross-project integration (Priority: P6)

**Goal**: the two binaries and the two integration points of spec sections 16 and 20, including the
single-completion rule that `cancel()` alone cannot provide.

**Independent Test**: `./scripts/run_scheduler_demo.sh` reproduces the illustrative log of spec
section 20, and `ctest -L scheduler-integration -R 'EventBus|IpcDeadline'` passes.

### Tests for User Story 6 ⚠️

- [X] T053 [P] [US6] Write `tests/integration/scheduler/event_bus_test.cpp`: `EventBusHeartbeat_SubscriberReceivesTicks` (IT-05, FR-12 — a `FixedRateSkipMissed` timer publishes onto a project 01 `EventBus` and a subscriber receives the expected number of events in order), `EventBusHeartbeat_PublishBackpressureIsHandledNotIgnored` (the callback checks the publish result, spec section 16), `EventBusHeartbeat_PublishDoesNotRunOnCoordinatorThread` (a thread-id assertion)
- [X] T054 [P] [US6] Write `tests/integration/scheduler/ipc_deadline_test.cpp`, guarded to Linux: `IpcDeadline_SingleCompletion` (IT-04, B-19, flow 12.13 — a request guarded by a scheduler timeout against a deliberately slow `tcu_ipc` handler; the request state is an `std::atomic` flipped with `compare_exchange_strong`, and the test asserts exactly one of `Replied`/`TimedOut` per round over 200 rounds with the timeout equal to the handler delay), `IpcDeadline_CancelAloneIsNotSufficient` (the losing side observes it lost)

### Implementation for User Story 6

- [X] T055 [P] [US6] Create `apps/telemetry_timer_demo/main.cpp` and its `CMakeLists.txt`: flags `--interval-ms`, `--samples`, `--callback-ms`, `--mode fixed-rate|fixed-delay`, `--stop drain|discard`; a periodic timer publishing a simulated sample onto a project 01 `EventBus` with a subscriber printing `telemetry.sample tick=N vehicle_speed=0`; prints the final `SchedulerMetrics` snapshot
- [X] T056 [P] [US6] Create `apps/modem_timeout_demo/main.cpp` and its `CMakeLists.txt`: flags `--timeout-ms`, `--reply-ms`, `--repeat`; a simulated AT request whose reply and whose scheduler timeout race on one `std::atomic<RequestState>` via `compare_exchange_strong`, printing the `modem.request` / `modem.reply` / `modem.timeout` / `modem.late_reply id=N ignored=true` lines of spec section 20, and asserting that reply and timeout counts sum to `--repeat`
- [X] T057 [US6] Create `scripts/run_scheduler_demo.sh` running the seven-step sequence of spec section 20 across both binaries, including the slow-callback case and both stop modes, with `set -euo pipefail` and a non-zero exit when any step fails

**Checkpoint**: both headline TCU use cases run end to end without hardware, and the timeout/reply
race is demonstrably decided exactly once.

---

## Phase 9: User Story 7 — Stress, faults, sanitizers and benchmark (Priority: P7)

**Goal**: the fault matrix of spec section 14, the 10k-timer case, clean sanitizer runs and the
section 19.4 report.

**Independent Test**: `ctest -L scheduler-stress` passes in its 5 s default budget, and both
sanitizer builds report nothing.

### Tests for User Story 7 ⚠️

- [X] T058 [P] [US7] Write `tests/stress/scheduler/concurrency_test.cpp`: `Stress_ConcurrentScheduleCancel` (ST-01, B-17 — 8 threads scheduling and cancelling for `TCU_SCHEDULER_STRESS_SECONDS` (default 5), asserting every scheduled timer reaches exactly one terminal outcome and M1/M2 hold at the end), `Stress_RepeatedFreshInstances` (ST-02 — 200 construct/start/schedule/stop cycles leave no lingering thread, S2), `Stress_StopRacesSchedule` (spec section 14 row 5 — a barrier-synchronised `stop()` against `schedule*`; every call is either accepted before the transition or rejected, and no callback runs after `stop()` returns), `Stress_ConcurrentCancelSameId` (one transition wins, the others return a stable status)
- [X] T059 [P] [US7] Write `tests/stress/scheduler/churn_test.cpp`: `Stress_TimerChurnBoundsHeap` (ST-03, spec section 14 row 3, H3 — rapid create/cancel cycles leave no stale callback and the heap stays `O(live)`), `Stress_BlockingCallbackDoesNotUnboundQueue` (spec section 14 row 1 — a callback held on a `TestGate`; the queue grows only to capacity, and the test releases the gate **before** joining, per research R-04), `Stress_OwnerDestroyedCallbackUsesWeakPtr` (spec section 14 row 7 — a callback capturing a `std::weak_ptr` never dereferences a dead object)
- [X] T060 [P] [US7] Write `tests/stress/scheduler/scale_test.cpp`: `Stress_TenThousandTimersBounded` (PT-01, B-20 — register 10,000 timers with randomised deadlines, then jump the fake clock so all become due at once; assert no loss, `ready_queue_depth <= maxReadyTasks`, `timers_active <= maxTimers` and M1 at the end). Assert **no** latency threshold (research R-18)

### Implementation for User Story 7

- [X] T061 [US7] Fix every defect the stress and fault suites expose, and record in `specs/003-timer-task-scheduler/plan.md` any that changed the design rather than only the code, as project 02 did
- [X] T062 [US7] Run the ASan + UBSan build of quickstart.md section 1 over all three labels and fix every finding
- [X] T063 [US7] Run the TSan build (clang++, `TCU_SANITIZER_SETARCH=ON`) over all three labels and fix every finding. Never combine it with ASan
- [X] T064 [US7] Create `benchmarks/scheduler_bench/main.cpp`: flags `--timers`, `--duration-s`, `--workers`, `--max-ready`; registers the requested timers on the real clock, derives p50/p95/p99 lateness from the histogram buckets, and prints the spec section 19.4 table — host, cores, OS/kernel, compiler and flags, timer count, callback rate, worker count, queue limits, percentiles and skip counts. Report, never gate

**Checkpoint**: the concurrency contracts hold under load and under both sanitizers, and PT-01 is
measured and reported rather than asserted.

---

## Phase 10: Polish and Cross-Cutting Concerns

- [X] T065 [P] Extend the root `README.md` with a project 03 section: what `tcu_scheduler` is, the build and test commands, both demo commands, and a one-line pointer to `specs/003-timer-task-scheduler/`
- [X] T066 [P] Re-read `specs/003-timer-task-scheduler/plan.md` against the finished code and update the Constitution Check with a post-implementation re-check, listing every public member added beyond the Phase 1 list and why, as project 02's plan does
- [X] T067 [P] Confirm the Deferred shortcuts table of `plan.md` matches the `shortcut:` comments actually present in `modules/scheduler/src/`, and add any that appeared during implementation
- [X] T068 Walk `specs/003-timer-task-scheduler/quickstart.md` end to end on a clean checkout — build, three labels, both sanitizer builds, both demos, the script, the benchmark — and fix any command that does not work as written
- [X] T069 Verify every box of spec section 21's Definition of Done, and report explicitly any criterion not met rather than closing the project (Constitution, Development Workflow, gate 3)

---

## Dependencies and Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: no dependencies; T001 and T002 are sequential (T002 references the target T001 declares), T003-T008 are parallel after them
- **Foundational (Phase 2)**: depends on Setup; blocks every story. T009 blocks T010-T015
- **US1 (Phase 3)**: depends on Foundational. **Blocks every later story**
- **US2 (Phase 4)**: depends on US1's coordinator and worker loop (T027, T028)
- **US3 (Phase 5)**: depends on US2 — `Cancel_RunningPeriodicIsNotRearmed` needs a rearm to suppress
- **US4 (Phase 6)**: depends on US3 — the drain cancels future timers with the same code path
- **US5 (Phase 7)**: depends on US4 — the `backpressure` log line and M2's cancellation terms need both
- **US6 (Phase 8)**: depends on US5 for the metrics each demo prints; the two integration tests need only US4
- **US7 (Phase 9)**: depends on US1-US5; the sanitizer tasks should run after US6 so the demos are covered too
- **Polish (Phase 10)**: depends on everything

### Within Each Story

- Tests are written and seen to FAIL before the implementation tasks beneath them
- Private components (`deadline_queue`, `ready_queue`) before the scheduler that composes them
- The coordinator before the worker loop; both before `schedule*`
- Logging and metrics call sites after the behaviour they describe exists

### Parallel Opportunities

- T003-T008 after T002
- T010, T011, T014 after T009
- Every `[P]` test task inside a story's test block — they are separate files
- T023 and T024 (different files, no shared state)
- T055 and T056 (two independent demo binaries)
- T058, T059, T060 (three independent stress files)
- T065, T066, T067 (three independent documents)

---

## Parallel Example: User Story 1 tests

```bash
# Seven independent test files; write them together, then watch all seven fail:
Task: "Write tests/unit/scheduler/lifecycle_test.cpp"       # T016
Task: "Write tests/unit/scheduler/validation_test.cpp"      # T017
Task: "Write tests/unit/scheduler/one_shot_test.cpp"        # T018
Task: "Write tests/unit/scheduler/deadline_queue_test.cpp"  # T019
Task: "Write tests/unit/scheduler/ready_queue_test.cpp"     # T020
Task: "Write tests/unit/scheduler/capacity_test.cpp"        # T021
Task: "Write tests/unit/scheduler/lock_test.cpp"            # T022

cmake --build build && ctest --test-dir build -L scheduler-unit   # expect failures, not errors
```

---

## Implementation Strategy

### MVP First (User Story 1 only)

1. Phase 1 Setup
2. Phase 2 Foundational
3. Phase 3 US1
4. **STOP and VALIDATE**: `ctest -L scheduler-unit` green; a one-shot timer fires once, bounded and
   joinable
5. At this point projects 10, 13 and 15 could already use the scheduler for plain timeouts

### Incremental Delivery

1. Setup + Foundational → the public headers compile and the clock is injectable
2. US1 → one-shot timers (MVP)
3. US2 → periodic sampling, both modes
4. US3 → safe cancellation, which is what makes timeouts usable in a race
5. US4 → predictable shutdown and overload behaviour
6. US5 → operability
7. US6 → the two spec section 20 demos
8. US7 → proof under load and under sanitizers

### Order discipline

The dependency chain above is not advisory. Implementing US3 before US2 produces a cancellation path
with no rearm to suppress, and US4 before US3 produces a drain that cannot cancel. Follow the phase
order; parallelise only inside a phase.

---

## Notes

- `[P]` tasks touch different files and depend on no incomplete task
- Every deadline assertion uses `FakeClockAdapter`; no unit test may use a sleep as a
  synchronisation device (research R-17)
- `FakeClockAdapter::advance` is called from the test thread only, never from inside a callback
- Commit after each task or logical group, with the project number in the message (Constitution,
  Development Workflow, step 4)
- Stop at any checkpoint to validate the increment
- Avoid: widening `Config` beyond spec section 9's three fields, adding an `Error` or `CancelResult`
  value, and any feature from spec sections 1-2's out-of-scope and non-goal lists
