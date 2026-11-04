---

description: "Task list for Thread-Safe Event Bus (project 01)"
---

# Tasks: Thread-Safe Event Bus

**Input**: Design documents from `/specs/001-thread-safe-event-bus/`

**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/, quickstart.md - all present.

**Tests**: REQUIRED, not optional. Constitution Principle III is non-negotiable: every FR gets a unit
test, every BDD scenario B01-B10 gets an integration test, and the test is written and seen to FAIL
before the implementation that satisfies it.

**Organization**: phases follow the five milestones of spec section 13; the backlog tickets
TCU-EB-001..010 of section 11 are named on the tasks that deliver them.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: can run in parallel - different files, no dependency on an incomplete task
- **[Story]**: US1..US5, mapped to milestones M1..M5 below
- Every description carries its exact file path

## Story-to-milestone mapping

| Story | Milestone (spec section 13) | Backlog tickets | Tests delivered |
|---|---|---|---|
| US1 (P1) | M1 Compile and basic delivery | TCU-EB-002, delivery half of 005 | UT-01, UT-02, UT-03, UT-13, UT-14, B01, B02 |
| US2 (P2) | M2 Boundedness | TCU-EB-003 | UT-04, UT-05, UT-06, B03, B04, B10 |
| US3 (P3) | M3 Lifecycle correctness | TCU-EB-004, TCU-EB-006 | UT-08, UT-09, UT-10, UT-11, UT-12, B05, B07, B08 |
| US4 (P4) | M4 Reliability | TCU-EB-007, TCU-EB-009 | UT-07, UT-15, B06, B09 |
| US5 (P5) | M5 TCU demo | TCU-EB-008, TCU-EB-010 | quickstart.md steps 2 and 4 |

**Deviation from the template, stated deliberately**: these five stories are **layers, not independent
features**. US2 cannot be tested without the queue US1 builds, US3 needs US2's blocking publisher to
prove B10, and so on. Spec section 13 orders them for exactly this reason. So the template's usual
"all stories can run in parallel once Foundational is done" does **not** apply here; each story's
checkpoint is a working increment, but the order is fixed. Parallelism exists *within* phases, marked
`[P]`.

**Metrics note**: `MetricsSnapshot` and its atomic counters are declared in Foundational (T012), and
each story increments the counters it owns, because B07/B08 in US3 assert `discarded_on_stop_total`
and cannot wait for M4. US4 completes the set (`callback_failures_total`), adds the structured
logging, and asserts the conservation equations across both stop modes. This splits ticket
TCU-EB-007 across US3 and US4; that is intentional, not an oversight.

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: a configurable, buildable monorepo skeleton. Ticket TCU-EB-001.

Per research R-00, every command in this phase runs in WSL2 Ubuntu 22.04+ from `/mnt/d/TCU`.

- [X] T001 Create root `CMakeLists.txt` with `cmake_minimum_required(VERSION 3.20)`, `project(virtual_tcu_lab CXX)`, `set(CMAKE_CXX_STANDARD 17)`, `set(CMAKE_CXX_STANDARD_REQUIRED ON)`, `set(CMAKE_CXX_EXTENSIONS OFF)` (constitution: no compiler extensions), `option(TCU_BUILD_TESTS "Build tests" ON)`, `option(TCU_BUILD_BENCHMARKS "Build benchmarks" ON)`, `enable_testing()`, and `add_subdirectory(modules/core/event_bus)`
- [X] T002 [P] Add `.gitignore` at repository root ignoring `build/`, `build-*/`, `.cache/`, `compile_commands.json`
- [X] T003 [P] Create `modules/core/event_bus/CMakeLists.txt` declaring `add_library(tcu_eventbus STATIC)` with `target_include_directories(tcu_eventbus PUBLIC include)` and `target_link_libraries(tcu_eventbus PUBLIC Threads::Threads)` via `find_package(Threads REQUIRED)`; source files are added by later tasks
- [X] T004 Add GoogleTest to root `CMakeLists.txt` under `if(TCU_BUILD_TESTS)`: `FetchContent_Declare(googletest GIT_REPOSITORY https://github.com/google/googletest.git GIT_TAG v1.14.0)` pinned exactly per research R-14, `set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)`, `FetchContent_MakeAvailable(googletest)`, then `add_subdirectory(tests/unit/event_bus)`
- [X] T005 Verify the skeleton configures and builds: `cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug && cmake --build build/debug -j`. Expected at this point: build succeeds and `ctest --test-dir build/debug -N` lists 0 tests. Record the CMake and compiler versions found

**Checkpoint**: an empty but correctly configured C++17 monorepo that builds in WSL2.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: the type system, the public header surface, and the test harness every story needs.
Ticket TCU-EB-002.

**CRITICAL**: no story work begins until this phase is complete.

- [X] T006 Create `tests/unit/event_bus/CMakeLists.txt` that will add one `add_executable` per test file plus `gtest_discover_tests(... PROPERTIES TIMEOUT 30)`. The 30-second per-test timeout is mandatory, not cosmetic: a `Drain` stop waiting on a stuck handler hangs forever (contract G17), and quickstart.md Troubleshooting names this as the expected failure mode
- [X] T007 [P] Create `tests/unit/event_bus/support/test_gate.hpp`: a condition-variable gate with `wait_until_opened()`, `open()` and `wait_until_entered()`, used to hold a callback inside the dispatcher deterministically. Spec section 9.1 forbids sleep-based synchronization; no test in this project may call `std::this_thread::sleep_for` to create ordering
- [X] T008 [P] Create `modules/core/event_bus/include/tcu/eventbus/event.hpp` with the three payload structs of data-model.md section 1 - `IgnitionChanged{bool on}`, `VehicleSpeedUpdated{double kmh}`, `NetworkStatusChanged{bool online}` - the alias `using EventPayload = std::variant<IgnitionChanged, VehicleSpeedUpdated, NetworkStatusChanged>`, `using EventType = std::uint32_t`, `constexpr EventType event_type_of<T>()` returning T's index in `EventPayload`, and `struct Event { std::uint64_t id; EventType type; std::chrono::steady_clock::time_point created_at; EventPayload payload; }`. Payloads own all their data and hold no raw pointer or reference (invariant P1)
- [X] T009 [P] Create `modules/core/event_bus/include/tcu/eventbus/metrics.hpp` with `MetricsSnapshot` carrying the nine fields of data-model.md section 9, exactly these names: `accepted_total`, `rejected_queue_full_total`, `publish_timeout_total`, `dispatched_total`, `callback_invocations_total`, `callback_failures_total`, `discarded_on_stop_total` (all `std::uint64_t`), `queue_depth`, `queue_high_watermark` (both `std::size_t`)
- [X] T010 Create `modules/core/event_bus/include/tcu/eventbus/event_bus.hpp` declaring, with no implementation yet: `enum class PublishStatus { Accepted, QueueFull, TimedOut, Stopped, InvalidArgument }` - exactly these five values, FR-06, and research R-02 declined a sixth; `enum class FullQueuePolicy { Reject, BlockUntil }`; `enum class StopMode { Drain, Discard }`; `struct BusConfig { std::size_t queue_capacity = 1024; }`; `struct PublishOptions { FullQueuePolicy on_full = FullQueuePolicy::Reject; std::chrono::milliseconds timeout{0}; }`; `using Handler = std::function<void(const Event&)>`; move-only `class Subscription`; and `class EventBus` with the seven members of contracts/event_bus_api.md. Copy and assignment deleted on both classes
- [X] T011 [P] Add compile-time checks in `tests/unit/event_bus/event_bus_test.cpp` asserting invariant T1: `static_assert(event_type_of<IgnitionChanged>() == 0)`, likewise for the other two alternatives, and that `event_type_of<T>()` equals the variant index. These must compile and pass before any runtime test is written
- [X] T012 Add the internal counter block to `modules/core/event_bus/src/event_bus.cpp`: one `std::atomic<std::uint64_t>` per total and one `std::atomic<std::size_t>` per gauge, plus `EventBus::metrics()` assembling a `MetricsSnapshot` by value. Per-counter reads are consistent; the snapshot is not atomic across fields (invariant M2). Stories increment the counters they own
- [X] T013 Write the failing test for the capacity precondition in `tests/unit/event_bus/event_bus_test.cpp`: constructing `EventBus{BusConfig{0}}` throws `std::invalid_argument` (data-model.md section 4, spec section 17.2). Run it and confirm it fails
- [X] T014 Implement the `EventBus` constructor in `modules/core/event_bus/src/event_bus.cpp` to validate `queue_capacity > 0`, throw `std::invalid_argument` on zero, and leave the bus in state `Created` with no worker thread. T013 now passes

**Checkpoint**: the public headers compile, the type/payload mapping is proven at compile time, and
the first precondition test is green.

---

## Phase 3: User Story 1 - M1 Compile and basic delivery (Priority: P1) 🎯 MVP

**Goal**: one worker dispatches published events to registered handlers, off the publisher's thread,
in registration order. Ticket TCU-EB-002 plus the delivery half of TCU-EB-005.

**Independent Test**: with capacity large enough that backpressure never triggers, publish events and
assert each subscriber receives each matching event exactly once, on a thread that is not the
publisher's, in registration order; then `stop(Drain)` and assert `accepted_total == dispatched_total`.

### Tests for User Story 1 (write first, watch them fail)

- [X] T015 [P] [US1] UT-01 in `tests/unit/event_bus/event_bus_test.cpp`: one subscriber, one event - the handler receives the identical event (same `id`, `type`, payload value), and `std::this_thread::get_id()` inside the handler differs from the publisher's (FR-03, guarantee G4). Synchronize with `std::promise`/`std::future`, never a sleep
- [X] T016 [P] [US1] UT-02 in `tests/unit/event_bus/event_bus_test.cpp`: three subscribers on one type each receive the event exactly once, and the recorded invocation order equals registration order (guarantee G9, invariant U4, research R-04)
- [X] T017 [P] [US1] UT-03 in `tests/unit/event_bus/event_bus_test.cpp`: four publisher threads publish concurrently with capacity high enough to avoid rejection; after `stop(Drain)` the number of `Accepted` returns equals `dispatched_total` and equals the number of handler invocations (FR-01)
- [X] T018 [P] [US1] UT-13 in `tests/unit/event_bus/event_bus_test.cpp`: payload ownership - publish an event built from a local object, let that object go out of scope, and assert the handler still observes the correct value. No dangling reference (invariant P1, NFR-04)
- [X] T019 [P] [US1] UT-14 in `tests/unit/event_bus/event_bus_test.cpp`: `publish()` returns `PublishStatus::Stopped` **both** before `start()` and after `stop()` (research R-02 - `Stopped`, not a new `NotRunning` value)
- [X] T020 [P] [US1] B01 in `tests/unit/event_bus/event_bus_test.cpp`: bus `Running`, queue has room, Telemetry subscribed to `VehicleSpeedUpdated`; publish `kmh = 42.5`; `publish` returns `Accepted`, Telemetry gets exactly one callback carrying 42.5, and after `stop(Drain)` that callback has completed
- [X] T021 [P] [US1] B02 in `tests/unit/event_bus/event_bus_test.cpp`: publish a type nobody subscribed to; no callback runs, `dispatched_total` still increments by one (contracts/metrics.md), and ASan reports no leak
- [X] T022 [US1] Add `event_bus_test.cpp` to `tests/unit/event_bus/CMakeLists.txt`, build, and confirm every test above FAILS for the right reason - missing implementation, not a compile error in the test itself

### Implementation for User Story 1

- [X] T023 [US1] Create `modules/core/event_bus/src/bounded_queue.hpp` and `.cpp` with the fields of data-model.md section 8: `std::deque<Event> items`, fixed `capacity`, `not_empty` and `not_full` condition variables, `closed` flag, one mutex. For US1 implement only the room-available `push` and a blocking `pop`; capacity enforcement is US2's job. Keep it a private header under `src/`, never under `include/` (plan.md Structure Decision 2)
- [X] T024 [US1] Create `modules/core/event_bus/src/subscription_registry.hpp` and `.cpp` holding `EventType -> vector<shared_ptr<SubscriptionState>>` ordered by ascending registration `id`, with `add()` and `snapshot(type)`. `SubscriptionState` carries `active`, `in_flight`, `owner`, `type`, `handler`, `id` per data-model.md section 7
- [X] T025 [US1] Implement `EventBus::subscribe()` in `modules/core/event_bus/src/event_bus.cpp`: reject an empty `Handler` with `std::invalid_argument`, reject the `Stopping`/`Stopped` states with `std::logic_error` (spec section 17.2), assign the next registration `id`, and return a `Subscription` holding a `std::weak_ptr<SubscriptionState>` - weak, so a handle outliving the bus is inert rather than dangling (invariant U3)
- [X] T026 [US1] Implement `EventBus::start()` in `modules/core/event_bus/src/event_bus.cpp`: valid only from `Created`, spawn exactly one worker, move to `Running`, and throw `std::logic_error` from any other state without creating a second worker (research R-08, invariant S2)
- [X] T027 [US1] Implement `EventBus::publish()` in `modules/core/event_bus/src/event_bus.cpp` for the happy path: validate `event.type == event.payload.index()` and return `InvalidArgument` on mismatch (invariant E2), return `Stopped` when the state is not `Running`, otherwise assign a strictly increasing `id`, enqueue under the lock, increment `accepted_total`, update `queue_depth` and `queue_high_watermark`, notify `not_empty`, and return `Accepted`. An `id` is consumed only by accepted events (invariant E3)
- [X] T028 [US1] Implement the dispatcher loop in `modules/core/event_bus/src/event_bus.cpp`: dequeue under the lock, notify `not_full`, **release the lock**, snapshot the subscribers for that type, invoke each handler in ascending registration `id`, then increment `dispatched_total` once for the event and `callback_invocations_total` once per handler call. No handler is ever invoked while the queue or registry lock is held (invariant Q4, guarantee G11)
- [X] T029 [US1] Implement a minimal `EventBus::stop(StopMode)` and `~EventBus()` sufficient for US1: move `Running -> Stopping` under the lock, close the queue, wake the worker, drain the backlog, join once, move to `Stopped`. Full `Drain`/`Discard` semantics and the concurrent-caller contract are US3's job
- [X] T030 [US1] Run the US1 tests until green: `ctest --test-dir build/debug -R 'EventBus|B01|B02' --output-on-failure`

**Checkpoint**: M1 reached - events flow from publisher to subscribers on a separate thread, in
registration order, with correct ownership. This is the MVP.

---

## Phase 4: User Story 2 - M2 Boundedness (Priority: P2)

**Goal**: the queue honours its capacity and both backpressure policies return the documented status.
Ticket TCU-EB-003.

**Independent Test**: with `capacity = 1` or `2` and a handler held open by the test gate, fill the
queue and assert `Reject` yields `QueueFull`, `BlockUntil` yields `Accepted` or `TimedOut` by its
deadline, and queue depth never exceeds capacity.

### Tests for User Story 2 (write first, watch them fail)

- [X] T031 [P] [US2] UT-04 in `tests/unit/event_bus/queue_test.cpp`: `capacity = 1`, handler held by the test gate, queue full; a further `publish` with `Reject` returns `QueueFull`, `rejected_queue_full_total` increments by one, `publish_timeout_total` does **not** move, and `queue_depth` never exceeds capacity (contracts/metrics.md)
- [X] T032 [P] [US2] UT-05 in `tests/unit/event_bus/queue_test.cpp`: queue full, `publish` with `BlockUntil` and a 100 ms deadline and no slot freed returns `TimedOut`; `publish_timeout_total` increments and `rejected_queue_full_total` does **not**. Assert the elapsed time is at least the deadline using `steady_clock`, and never use a sleep to create the condition
- [X] T033 [P] [US2] UT-06 in `tests/unit/event_bus/queue_test.cpp`: a publisher waiting in `BlockUntil` is woken by `stop()` and returns `Stopped` - not `TimedOut` and not `Accepted` (guarantee G16)
- [X] T034 [P] [US2] B03 in `tests/unit/event_bus/queue_test.cpp`: test-gate-blocked worker with a full queue - a `Reject` publish returns `QueueFull`, the event is never delivered, and `queue_high_watermark <= capacity` (invariant M1)
- [X] T035 [P] [US2] B04 in `tests/unit/event_bus/queue_test.cpp`: busy worker, full queue, `BlockUntil` with a 100 ms deadline returns `TimedOut` and the event is not enqueued
- [X] T036 [P] [US2] B10 in `tests/unit/event_bus/queue_test.cpp`: one publisher blocked in `BlockUntil` on a full queue; another thread calls `stop(Discard)`; the publisher wakes and returns `Stopped` and does not hang. The test's own timeout guard must be shorter than the CTest timeout so a hang fails loudly
- [X] T037 [P] [US2] Zero-timeout poll test in `tests/unit/event_bus/queue_test.cpp` for research R-11: `BlockUntil` with `timeout = 0` returns `Accepted` when a slot is free, and `TimedOut` - never `QueueFull` - when the queue is full
- [X] T038 [P] [US2] Negative-timeout test in `tests/unit/event_bus/queue_test.cpp`: `BlockUntil` with a negative `timeout` returns `InvalidArgument` (data-model.md section 4)
- [X] T039 [US2] Add `queue_test.cpp` to `tests/unit/event_bus/CMakeLists.txt`, build, and confirm all of T031-T038 fail for the right reason

### Implementation for User Story 2

- [X] T040 [US2] Enforce capacity in `modules/core/event_bus/src/bounded_queue.cpp`: `try_push` returns false when `items.size() == capacity`, maintaining `0 <= size <= capacity` at all times (invariant Q1)
- [X] T041 [US2] Implement the `BlockUntil` wait in `modules/core/event_bus/src/bounded_queue.cpp` using `std::chrono::steady_clock`, an **absolute** deadline and a predicate loop, so a spurious wakeup cannot shorten the timeout (invariant Q3). `timeout = 0` degenerates to a single predicate check (R-11)
- [X] T042 [US2] Wire the policies into `EventBus::publish()` in `modules/core/event_bus/src/event_bus.cpp`: `Reject` on a full queue returns `QueueFull` and increments `rejected_queue_full_total`; `BlockUntil` waits and returns `Accepted`, `TimedOut` (incrementing `publish_timeout_total`) or `Stopped`; a negative timeout returns `InvalidArgument`. A publish returning `Stopped` or `InvalidArgument` increments nothing (contracts/metrics.md)
- [X] T043 [US2] Make the queue's `closed` flag wake every blocked publisher so each returns `Stopped` (guarantee G16), and update `queue_depth`/`queue_high_watermark` on every push and pop
- [X] T044 [US2] Run the US2 tests until green: `ctest --test-dir build/debug -R 'Queue|B03|B04|B10' --output-on-failure`

**Checkpoint**: M2 reached - memory is bounded, backpressure is explicit, and no accepted event is
ever silently dropped.

---

## Phase 5: User Story 3 - M3 Lifecycle correctness (Priority: P3)

**Goal**: unsubscribe is safe against a running callback, and both stop modes honour their contracts
with no deadlock and no double join. Tickets TCU-EB-004 and TCU-EB-006.

**Independent Test**: unsubscribe while a handler is mid-flight and assert no new invocation starts,
the in-flight one completes, and nothing crashes under ASan; then run `Drain` and `Discard` against a
known backlog and assert the two conservation equations of contracts/metrics.md.

### Tests for User Story 3 (write first, watch them fail)

- [X] T045 [P] [US3] UT-08 in `tests/unit/event_bus/subscription_test.cpp`: unsubscribe before publishing - the handler is never invoked (FR-07)
- [X] T046 [P] [US3] UT-09 in `tests/unit/event_bus/subscription_test.cpp`: unsubscribe while the handler is held open by the test gate - no use-after-free, no deadlock, the in-flight invocation completes, no new one starts. Add the self-unsubscribe case: a handler calling `unsubscribe()` on its own subscription must not deadlock on the per-subscription mutex (research R-13 caveat). Run this test under both ASan and TSan
- [X] T047 [P] [US3] UT-10 in `tests/unit/event_bus/shutdown_test.cpp`: `Drain` with a backlog - every accepted event is dispatched before `stop()` returns, and afterwards `accepted_total == dispatched_total` with `discarded_on_stop_total == 0`
- [X] T048 [P] [US3] UT-11 in `tests/unit/event_bus/shutdown_test.cpp`: `Discard` with a backlog - queued events are dropped without invoking their handlers, `stop()` does not wait for the backlog, and `accepted_total == dispatched_total + discarded_on_stop_total`
- [X] T049 [P] [US3] UT-12 in `tests/unit/event_bus/shutdown_test.cpp`: `stop()` twice, and several threads calling `stop()` concurrently - no crash, no double join, and every caller returns only once the state is `Stopped` (research R-09, invariant S2)
- [X] T050 [P] [US3] B05 in `tests/unit/event_bus/subscription_test.cpp`: handler A possibly running; once `unsubscribe(A)` returns, no new A invocation begins, an already-started one returns normally, and there is no crash or deadlock
- [X] T051 [P] [US3] B07 in `tests/unit/event_bus/shutdown_test.cpp`: E1, E2, E3 all `Accepted`, then `stop(Drain)` - all three are dispatched before `stop()` returns, including an event with no subscriber, which is processed as a no-op
- [X] T052 [P] [US3] B08 in `tests/unit/event_bus/shutdown_test.cpp`: E1 mid-dispatch, E2 and E3 queued, then `stop(Discard)` - E1 completes, E2 and E3 invoke no handler, and `discarded_on_stop_total` increments by exactly 2
- [X] T053 [P] [US3] Subscription-outlives-bus test in `tests/unit/event_bus/subscription_test.cpp`: destroy the `EventBus` while a `Subscription` handle is still alive, then call `unsubscribe()` on the handle - a no-op, not a use-after-free (invariant U3, guarantee G15). Must be clean under ASan
- [X] T054 [P] [US3] Subscribe-after-shutdown test in `tests/unit/event_bus/subscription_test.cpp`: `subscribe()` during `Stopping` or after `Stopped` throws `std::logic_error` (spec section 17.2)
- [X] T055 [US3] Add `subscription_test.cpp` and `shutdown_test.cpp` to `tests/unit/event_bus/CMakeLists.txt`, build, and confirm T045-T054 fail for the right reason

### Implementation for User Story 3

- [X] T056 [US3] Implement the admission-control mechanism of research R-13 in `modules/core/event_bus/src/subscription_registry.cpp`: per-subscription `std::atomic<bool> active`, a per-subscription `in_flight` mutex, and an `owner` thread id. The dispatcher snapshots under the registry lock, releases it, then per entry takes that subscription's own mutex, re-checks `active`, records `owner`, and invokes. Snapshotting alone is explicitly not sufficient (spec section 17.5)
- [X] T057 [US3] Implement `Subscription::unsubscribe()` in `modules/core/event_bus/src/event_bus.cpp`: clear `active`, then acquire the same per-subscription mutex to wait out an in-flight invocation - while holding neither the registry nor the queue lock. If `owner` is the calling thread, clear `active` and return **without** acquiring, so self-unsubscribe from inside the handler cannot self-deadlock (R-13 caveat). Idempotent; a no-op once the bus is gone (invariants U2, U3)
- [X] T058 [US3] Implement `Subscription` move-construction, move-assignment and destructor in `modules/core/event_bus/src/event_bus.cpp`: move-only, and destruction or move-assignment unsubscribes the previous registration (invariant U1)
- [X] T059 [US3] Implement full `EventBus::stop(StopMode)` in `modules/core/event_bus/src/event_bus.cpp`: the `Running -> Stopping` transition happens under the lock and is the single linearization point after which no publish is accepted (invariant S1); `Drain` dispatches the whole backlog FIFO then joins; `Discard` clears the queue, counts each dropped event in `discarded_on_stop_total`, lets the running handler finish, then joins
- [X] T060 [US3] Implement the concurrent-`stop()` contract in `modules/core/event_bus/src/event_bus.cpp`: the first caller to reach `Stopping` performs the shutdown and joins; other callers block until the state is `Stopped` and then return; a call on an already-`Stopped` bus returns at once; the worker is joined exactly once (research R-09)
- [X] T061 [US3] Implement `~EventBus()` in `modules/core/event_bus/src/event_bus.cpp` to stop the bus if it is still running, and document caller obligation C3 as a Doxygen comment on the class: no other thread may call a method on the bus while it is being destroyed
- [X] T062 [US3] Run the US3 tests until green: `ctest --test-dir build/debug -R 'Subscription|Shutdown|B05|B07|B08' --output-on-failure`

**Checkpoint**: M3 reached - lifecycle and subscription contracts hold; both stop modes satisfy their
conservation equation.

---

## Phase 6: User Story 4 - M4 Reliability (Priority: P4)

**Goal**: a failing or re-entrant handler cannot take down the worker; metrics and logging are
complete; sanitizers and the concurrency stress test are clean. Tickets TCU-EB-007 and TCU-EB-009.

**Independent Test**: make one handler throw and assert the next handler still runs and
`callback_failures_total` increments by exactly one; then run the 8-thread 8000-event stress test
under TSan and ASan with zero reports.

### Tests for User Story 4 (write first, watch them fail)

- [X] T063 [P] [US4] UT-07 in `tests/unit/event_bus/callback_test.cpp`: a handler throws `std::runtime_error`; it is caught, `callback_failures_total` increments by one, the worker keeps running, and later events are still dispatched (FR-10)
- [X] T064 [P] [US4] UT-15 in `tests/unit/event_bus/callback_test.cpp`: re-entrant publish from inside a handler - `Reject` returns `Accepted` or `QueueFull` and the new event is processed after the current one; `BlockUntil` from the dispatcher thread returns `InvalidArgument` immediately and never deadlocks (research R-10, guarantee G5)
- [X] T065 [P] [US4] B06 in `tests/unit/event_bus/callback_test.cpp`: handler A throws and handler B is subscribed to the same type; B still receives the callback and `callback_failures_total` increments by exactly 1
- [X] T066 [P] [US4] B09 in `tests/unit/event_bus/concurrency_test.cpp`: 8 publisher threads, 1000 events each with a per-thread sequence id; after `stop(Drain)`, `accepted_total == dispatched_total`, no data race, and FIFO holds by enqueue order - assert on the monotonic `Event::id` (invariant E3), **not** on wall-clock ordering between publishers (invariant Q2)
- [X] T067 [P] [US4] Conservation-equation test in `tests/unit/event_bus/concurrency_test.cpp`: run the same stress load twice, once ending in `Drain` and once in `Discard`, and assert the matching equation from contracts/metrics.md each time. Also assert `callback_failures_total <= callback_invocations_total`, and that `callback_invocations_total` is **not** assumed equal to `dispatched_total`
- [X] T068 [US4] Add `callback_test.cpp` and `concurrency_test.cpp` to `tests/unit/event_bus/CMakeLists.txt`, build, and confirm T063-T067 fail for the right reason

### Implementation for User Story 4

- [X] T069 [US4] Wrap each handler invocation in the dispatcher in `modules/core/event_bus/src/event_bus.cpp` in `try`/`catch(...)`: increment `callback_failures_total`, log once at warning level, and continue with the next subscriber. One failing subscriber must not affect the others (spec section 17.6). `dispatched_total` still increments for an event whose handler threw (research R-12)
- [X] T070 [US4] Add the dispatcher-thread guard in `modules/core/event_bus/src/event_bus.cpp`: store the worker's `std::thread::id` at `start()` and return `InvalidArgument` when `publish()` is called with `BlockUntil` from that thread (research R-10). A thread-id comparison, not a heuristic
- [X] T071 [US4] Complete the counter set in `modules/core/event_bus/src/event_bus.cpp` so all nine fields of contracts/metrics.md are maintained with the exact semantics given there - in particular `dispatched_total` counts completed dispatch rounds, including events with no subscriber, and never counts callback invocations
- [X] T072 [US4] Add structured logging to `modules/core/event_bus/src/event_bus.cpp` using exactly the keys listed in contracts/metrics.md: `bus_state`, `capacity`, `mode`, `event_id`, `event_type`, `publisher`, `subscriber`, `queue_depth`, `publish_status`, `callback_status`, `accepted`, `dispatched`, `discarded`, `callback_failures`. Never log payload contents, keys, tokens or credentials (constitution Principle V); logging must not recurse through the bus and must not block the dispatcher for long (spec section 8)
- [X] T073 [US4] Run the ASan/UBSan build per quickstart.md step 3 and fix every report: `cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"`, build, then `ctest --test-dir build/asan --output-on-failure`
- [X] T074 [US4] Wire `CMAKE_CROSSCOMPILING_EMULATOR` to `setarch -R` for the TSan build (research R-00 addendum: TSan aborts with `unexpected memory mapping` on this kernel without it), then run the separate TSan build per quickstart.md step 3 and fix every report: `cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g"`, build, then `ctest --test-dir build/tsan --output-on-failure`. Never combine TSan with ASan (spec section 9.2). A TSan warning is a failure, not noise
- [X] T075 [US4] Add a CI workflow at `.github/workflows/ci.yml` running three jobs on `ubuntu-latest` - plain Debug, ASan/UBSan, TSan - each doing configure, build and `ctest --output-on-failure` (spec section 10 acceptance criterion)

**Checkpoint**: M4 reached - the bus survives handler failure, metrics are trustworthy, and both
sanitizers are clean.

---

## Phase 7: User Story 5 - M5 TCU demo (Priority: P5)

**Goal**: a runnable demo showing the bus inside a miniature TCU, plus a reproducible benchmark.
Tickets TCU-EB-008 and TCU-EB-010.

**Independent Test**: run the demo and check its output against the seven expectations of
quickstart.md step 2; run the benchmark and check it prints percentiles together with its environment.

### Implementation for User Story 5

- [X] T076 [US5] Create `apps/event_bus_demo/main.cpp` per spec section 20: a bus with `capacity = 3`; `PowerManager` then `Telemetry` subscribe to `IgnitionChanged` in that order; a `vehicle_simulator` publishes `OFF -> ON -> OFF` with sequence ids; log one line per publish and per callback using only the approved keys
- [X] T077 [US5] Extend `apps/event_bus_demo/main.cpp` with the backpressure phase: a handler held slow by a gate fills the queue, then demonstrate `Reject` returning `QueueFull` and `BlockUntil` returning `TimedOut`, printing each status
- [X] T078 [US5] Extend `apps/event_bus_demo/main.cpp` to run the whole scenario twice, once ending in `stop(Drain)` and once in `stop(Discard)`, printing the summary line each time so `accepted == dispatched` and `accepted == dispatched + discarded` are visible to the eye
- [X] T079 [P] [US5] Add `apps/event_bus_demo/CMakeLists.txt` linking `tcu_eventbus`, and `add_subdirectory(apps/event_bus_demo)` to the root `CMakeLists.txt`
- [X] T080 [US5] Create `benchmarks/event_bus_bench/main.cpp` measuring throughput and p50/p95/p99 enqueue-to-dispatch latency from `Event::created_at`, with the publisher-thread count and queue capacity configurable by argument
- [X] T081 [US5] Make `benchmarks/event_bus_bench/main.cpp` print its environment alongside the numbers: CPU model, core count, OS and kernel version, compiler and version, build type, queue capacity. NFR-06 forbids invented figures, so a result without its environment is not a result
- [X] T082 [P] [US5] Add `benchmarks/event_bus_bench/CMakeLists.txt` and guard `add_subdirectory(benchmarks/event_bus_bench)` in the root `CMakeLists.txt` behind `if(TCU_BUILD_BENCHMARKS)`
- [X] T083 [US5] Add a demo smoke test to `tests/unit/event_bus/` that runs the demo scenario in-process and asserts the metric summary of both stop modes, so the demo cannot rot silently

**Checkpoint**: M5 reached - the bus is demonstrably usable by TCU modules and has measured numbers.

---

## Phase 8: Polish and Cross-Cutting Concerns

- [X] T084 [P] Add Doxygen comments to the three public headers under `modules/core/event_bus/include/tcu/eventbus/` documenting ownership, the state machine, the callback and thread rules, and caller obligations C1, C2 and C3 from contracts/event_bus_api.md. This is an acceptance criterion in spec section 10, not decoration
- [X] T085 Write `README.md` at the repository root: the WSL2 prerequisites and build commands of quickstart.md, the five design trade-offs of spec section 14, the v1 non-guarantees of spec section 20.1, and the benchmark output from T081 pasted verbatim with its environment block
- [X] T086 [P] Verify the test inventory: `ctest --test-dir build/debug -N` lists all of UT-01..UT-15 and B01..B10 by name. Any missing entry is an unmet acceptance criterion in spec section 10, which requires at least the 15 tests of section 9.1
- [X] T087 Walk the acceptance checklist in quickstart.md section 5 and tick every line, or state explicitly which one is unmet and why. Constitution Development Workflow gate 3: report unmet criteria rather than closing the project
- [X] T088 Review `modules/core/event_bus/src/` against the review rules of spec section 21 and the constitution: nothing from "Ngoai pham vi v1" was built, no secret reaches a log, no lock is held across a callback, and no new third-party dependency was added
- [X] T089 Commit with the project number in the message, per the constitution Development Workflow

---

## Dependencies and Execution Order

### Phase dependencies

- **Phase 1 Setup**: no dependencies
- **Phase 2 Foundational**: needs Phase 1; blocks every story
- **Phase 3 US1**: needs Phase 2
- **Phase 4 US2**: needs US1 - capacity enforcement extends the queue US1 created
- **Phase 5 US3**: needs US2 - B10 requires a publisher blocked in `BlockUntil`
- **Phase 6 US4**: needs US3 - the conservation equations require both stop modes to be correct
- **Phase 7 US5**: needs US4 - the demo prints the metrics US4 completes
- **Phase 8 Polish**: needs every story intended for this release

### Why the stories are not parallel

The template's default is independent stories worked in parallel. That does not hold here, and this
plan does not pretend it does: spec section 13 defines M1..M5 as a deliberate ordering, each
milestone extending the previous one's code. Running them in parallel would mean two people editing
`event_bus.cpp` and `bounded_queue.cpp` at once for no gain.

### Within each story

- Tests are written first and must be seen to FAIL before implementation (constitution Principle III)
- Private headers before the `.cpp` that implements them
- Queue and registry before the dispatcher that drives them
- A story is finished only when its own `ctest` selection is green

### Parallel opportunities

- T002, T003 in Setup
- T007, T008, T009 in Foundational - three separate files
- All test-writing tasks within one story: T015-T021, T031-T038, T045-T054, T063-T067 - each writes
  distinct test cases and can be split across people
- T079, T082 in US5
- T084, T086 in Polish

---

## Parallel Example: User Story 1 tests

```bash
# All seven US1 test tasks touch distinct test cases and can be written concurrently:
Task: "UT-01 single subscriber receives the event off the publisher thread"
Task: "UT-02 three subscribers, registration order"
Task: "UT-03 four concurrent publishers, accepted == dispatched"
Task: "UT-13 payload ownership outlives its producer"
Task: "UT-14 publish before start and after stop both return Stopped"
Task: "B01 normal delivery of VehicleSpeedUpdated 42.5"
Task: "B02 publish with no subscriber still increments dispatched_total"
```

---

## Implementation Strategy

### MVP first

1. Phase 1 Setup
2. Phase 2 Foundational
3. Phase 3 US1
4. **STOP and VALIDATE**: events are delivered off the publisher thread, in registration order, with
   no leak. That proves the design, though it does not yet unblock project 05, which needs the
   bounded queue of US2 and the shutdown contract of US3.

### Incremental delivery

US1 proves delivery, US2 adds bounded memory, US3 adds a safe lifecycle, US4 makes it trustworthy,
US5 makes it demonstrable. Stop after any checkpoint and the result still builds and passes its own
tests. Only after US3 is the library honestly usable by project 05 or later: an event bus whose
shutdown is unproven is not a dependency anyone should take.

### Suggested commits

One commit per task, or per test-plus-implementation pair. One commit per checkpoint at minimum, so a
failed sanitizer run can be bisected.

---

## Notes

- `[P]` means different files and no dependency on an incomplete task
- No test may use `std::this_thread::sleep_for` to create ordering - use `support/test_gate.hpp`
  (spec section 9.1)
- Every test needs its own timeout guard; the CTest 30-second timeout is the backstop, not the plan
- Verify each test fails before implementing it; a test that passes before the code exists is testing
  nothing
- Nothing from spec "Ngoai pham vi v1" may be implemented, however convenient it looks
  (constitution Principle I)
