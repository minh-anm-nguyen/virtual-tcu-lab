# Implementation Plan: Thread-Safe Event Bus

**Branch**: `001-thread-safe-event-bus` | **Date**: 2026-11-04 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/001-thread-safe-event-bus/spec.md`

## Summary

Build `tcu::eventbus`, an in-process C++17 event bus: many publisher threads, a bounded FIFO queue,
one dispatcher worker, RAII subscriptions, two backpressure policies (`Reject`, `BlockUntil`), two
shutdown modes (`Drain`, `Discard`), and metrics. It is the foundation that projects 05, 10, 13, 16,
18 and 30 consume (spec section 15).

The spec already fixes the design: public API (section 5), concurrency invariants (section 6), state
machine (section 7), the 15 unit tests (section 9.1), acceptance criteria (section 10), repository
layout (section 12), behaviour contracts B01-B10 (section 18.2) and metric definitions (section 19).
This plan does not redesign any of it; it closes the decisions the spec explicitly defers (sections
14.1, 17.2, 21) so implementation has no remaining ambiguity. Those closures are in
[research.md](./research.md).

## Technical Context

**Language/Version**: C++17, no compiler extensions in library code.

**Primary Dependencies**: standard library only in the core library (NFR-01). GoogleTest/GoogleMock
for tests, fetched by CMake FetchContent.

**Storage**: N/A - in-process, no persistence (section 20.1).

**Testing**: GoogleTest + CTest. Deterministic synchronization via `std::promise`/`std::future` and a
condition-variable test gate; no sleep-based timing (section 9.1). Sanitizer jobs:
`-fsanitize=address,undefined` and a separate `-fsanitize=thread` build, never combined.

**Target Platform**: Linux/Ubuntu x86-64, GCC 11+ or Clang 14+ (NFR-01). Developed on Windows via
WSL2 Ubuntu - see research.md R-00, the one decision that needs operator confirmation.

**Project Type**: Static library + demo executable + test targets, inside the Virtual TCU Lab
monorepo.

**Performance Goals**: none asserted. NFR-06 forbids inventing benchmark numbers; the deliverable is
a reproducible benchmark harness that records p50/p95/p99 latency and throughput together with
hardware, OS, compiler and build configuration.

**Constraints**: bounded memory - queue capacity fixed at construction, `capacity == 0` rejected. No
callback invoked while holding any internal lock (section 6). No deadlock in supported usage. No
use-after-free when a `Subscription` outlives its `EventBus`.

**Scale/Scope**: one `EventBus` instance per process, one dispatcher worker; 8 concurrent publisher
threads and 8000 events exercised by the stress test (B09). Roughly 1500-2000 lines of library plus
test code.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Gate | Status |
|---|---|---|
| I. Spec Fidelity and Scope Discipline | Deliver only "Pham vi v1"; nothing from "Ngoai pham vi v1" (IPC, durable queue, exactly-once, event priority, thread pool, zero-copy, auto retry) | PASS - research.md R-03/R-05/R-06 defer them explicitly. `PublishStatus` keeps exactly the five values of FR-06; no `NotRunning` added (R-02). |
| II. Library-First Reuse | One CMake target with a public header surface; no duplication | PASS - single `tcu_eventbus` target. First project, nothing earlier to reuse. |
| III. Test-First | Every FR has a unit test; every BDD scenario has an integration test; every acceptance criterion observable | PASS - traceability matrix below; each task writes its test first. |
| IV. Simulation-Only | No real hardware; time and faults injectable; deterministic | PASS - in-process only; a slow consumer is simulated by a condition-variable test gate, not by sleeping. |
| V. Safe Observability | Structured metrics and logs; no secrets; failures isolated and counted | PASS - log fields fixed to `event_id`, `event_type`, `publisher`, `subscriber`, `queue_depth`, `publish_status`, `callback_status` (section 20); payload contents never logged; callback exceptions caught and counted. |

**Post-Phase 1 re-check**: still PASS. Phase 1 added no dependency, no abstraction and no API beyond
spec section 5, so no new gate is engaged and Complexity Tracking stays empty.

### Requirement to test traceability

| Requirement | Covered by |
|---|---|
| FR-01 concurrent publish | UT-03, B09 |
| FR-02 subscribe by type, many subscribers | UT-01, UT-02, B01 |
| FR-03 async dispatch off publisher thread | UT-01 with thread-id assertion, B01 |
| FR-04 bounded queue | UT-04, B03 |
| FR-05 Reject / BlockUntil, no silent drop | UT-04, UT-05, B03, B04 |
| FR-06 five publish statuses | UT-04, UT-05, UT-14, UT-15, B03, B04, B10 |
| FR-07 safe unsubscribe | UT-08, UT-09, B05 |
| FR-08 Drain / Discard | UT-10, UT-11, UT-12, B07, B08 |
| FR-09 metrics | UT-07, B06, B08, B09 plus the conservation assertions of section 19 |
| FR-10 callback error isolation | UT-07, B06 |
| NFR-02 no data race | TSan stress job on B09 |
| NFR-03 bounded memory | UT-04 plus queue-depth and high-watermark assertions |
| NFR-04 clear ownership, no UAF | UT-09, UT-13, ASan job |
| NFR-05 deterministic tests | all tests - test gate, no sleeps |
| NFR-06 measured benchmark | benchmark harness, results recorded with environment |

## Project Structure

### Documentation (this feature)

```text
specs/001-thread-safe-event-bus/
  plan.md               # This file
  research.md           # Phase 0: closes the spec's open decisions
  data-model.md         # Phase 1: entities, states, invariants
  quickstart.md         # Phase 1: build, test, demo validation
  contracts/
    event_bus_api.md    # Public API contract and preconditions
    metrics.md          # Metric definitions and conservation equations
  tasks.md              # Phase 2 output (/speckit-tasks - NOT created here)
```

### Source Code (repository root)

Layout follows spec section 12, rooted at the monorepo root:

```text
CMakeLists.txt                             # root: C++17, options, FetchContent GoogleTest, CTest
README.md                                  # quickstart, trade-offs, benchmark method

modules/core/event_bus/
  CMakeLists.txt                           # target tcu_eventbus
  include/tcu/eventbus/event.hpp           # EventType, payload variant, Event
  include/tcu/eventbus/event_bus.hpp       # EventBus, Subscription, config, statuses
  include/tcu/eventbus/metrics.hpp         # MetricsSnapshot
  src/bounded_queue.hpp                    # internal: BoundedQueue, not installed
  src/bounded_queue.cpp
  src/subscription_registry.hpp            # internal: registry plus admission control
  src/subscription_registry.cpp
  src/event_bus.cpp                        # lifecycle, publish, dispatcher loop

apps/event_bus_demo/
  main.cpp                                 # section 20 demo: capacity=3, OFF-ON-OFF, Drain then Discard

tests/unit/event_bus/
  support/test_gate.hpp                    # condition-variable gate; blocks a callback on command
  event_bus_test.cpp                       # UT-01,02,03,13,14 and B01,B02
  queue_test.cpp                           # UT-04,05,06 and B03,B04,B10
  subscription_test.cpp                    # UT-08,09 and B05
  callback_test.cpp                        # UT-07,15 and B06
  shutdown_test.cpp                        # UT-10,11,12 and B07,B08
  concurrency_test.cpp                     # B09 stress, sanitizer target

benchmarks/event_bus_bench/
  main.cpp                                 # throughput and latency percentiles, prints environment
```

**Structure Decision**: spec section 12's layout is adopted unchanged, with three deltas, each
justified:

1. The `docs/01_...md` entry of section 12 is **not** created. The design document already lives at
   `Virtual_TCU_Lab_30_System_Designs/01_Thread_Safe_Event_Bus_System_Design.md` and is copied into
   `specs/001-thread-safe-event-bus/spec.md`. A third copy would drift (Principle II).
2. `bounded_queue.hpp` and `subscription_registry.hpp` are **private** headers under `src/`, not
   under `include/`. They are implementation detail; publishing them would widen the contract that
   projects 05-30 depend on.
3. `benchmarks/` and `tests/unit/event_bus/support/` are added because the acceptance criteria
   require a reproducible benchmark (section 10, NFR-06) and deterministic tests without sleeps
   (section 9.1) - neither is achievable with the section 12 tree alone.

The root `CMakeLists.txt` is created by this project and will be extended, not replaced, by projects
02-30: each adds one `add_subdirectory` line.

## Complexity Tracking

No Constitution Check violations. Nothing to justify.
