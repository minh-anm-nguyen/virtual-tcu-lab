# Phase 0 Research: Thread-Safe Event Bus

The spec leaves a fixed, enumerated set of decisions open: section 14.1 ("Open questions truoc khi
code production"), section 17.2 (behaviour of `publish()` before `start()`), and the review
checklist in section 21. Nothing here invents new scope; each entry closes one of those questions so
no `NEEDS CLARIFICATION` remains before Phase 1.

---

## R-00 Build and test platform

**Decision**: Linux/Ubuntu 22.04 or later is the build and test target. On this Windows machine,
development runs inside WSL2 Ubuntu; the repository stays on the Windows filesystem but is built
from the WSL2 side under `/mnt/d/TCU` (or a clone inside the WSL2 filesystem if build times matter).

**Rationale**: NFR-01 states Linux/Ubuntu. The acceptance criterion "CI chay unit tests + sanitizer
jobs" requires `-fsanitize=address,undefined` and `-fsanitize=thread`; MSVC provides ASan only and
has no ThreadSanitizer. Projects 26 (systemd daemon) and 27 (Yocto/QEMU) are Linux-only anyway, so a
Linux toolchain is needed before project 26 regardless.

**Alternatives considered**:
- *Native Windows + MSVC*: drops TSan, so NFR-02 ("no data race") would rest on review instead of a
  tool. Rejected - a concurrency library whose race detector is unavailable is the one case where
  the tooling is not optional.
- *Docker Desktop container*: works, but adds a daemon and image layer to manage for no gain over
  WSL2 on a single-developer machine. Rejected as unnecessary.
- *Dual build, MSVC for day-to-day plus WSL2 for sanitizers*: doubles the CMake surface from project
  01 onward. Rejected now; revisit only if WSL2 proves too slow.

**Addendum, measured 2026-11-04**: the WSL2 Ubuntu-22.04 instance has cmake 3.30.5 and g++ 11.4.0,
both above the required minimums, and `/mnt/d/TCU` builds fine. But a ThreadSanitizer binary aborts
at startup with `FATAL: ThreadSanitizer: unexpected memory mapping`, the known clash between TSan's
fixed shadow mapping and the high-entropy ASLR of recent kernels. Two workarounds exist:
`sudo sysctl -w vm.mmap_rnd_bits=28` (whole instance, not persistent across restarts), or running
each binary under `setarch -R`, which was verified to work here. The second is preferable because it
needs no privileges and can be expressed in CMake as `CMAKE_CROSSCOMPILING_EMULATOR`, so CTest and
`gtest_discover_tests` both pick it up. T074 must wire this up; without it the sanitizer acceptance
criterion of spec section 10 cannot be met on this machine.

**Second addendum, measured 2026-11-04**: the TSan job must use **clang**, not GCC. With
`g++ 11.4.0` on glibc 2.35, six tests report `double lock of a mutex` and a `data race` inside
`BoundedQueue::pop`, all of them tests that use a timed wait. The cause is the tool, not the code:
glibc 2.35 implements a `steady_clock` `wait_for`/`wait_until` with `pthread_cond_clockwait`, and
GCC 11's `libtsan.so` contains zero interceptor symbols for it (verified with
`nm -D .../libtsan.so | grep -c clockwait`, which returns 0). TSan therefore never learns that the
mutex was released during the wait, so every later acquisition looks like a double lock and every
access under it looks unsynchronized. Building the same tests with `clang++ 14.0.0`, whose TSan does
intercept `pthread_cond_clockwait`, gives 49/49 passing with no report at all. Suppressions were
deliberately not used: they would have masked genuine races in the same functions.

**NEEDS OPERATOR CONFIRMATION**: this is the one decision that cannot be settled from the spec alone.
If Windows/MSVC is meant to be a first-class target, both the constitution's Technology Constraints
section and this entry change, and the sanitizer acceptance criterion needs a replacement.

---

## R-01 Event payload type safety

**Decision**: `std::variant` of concrete payload structs, exactly as spec section 18.1 shows.
`EventType` stays `std::uint32_t` and is derived from the variant index, so type and payload can
never disagree. No `std::shared_ptr<const void>`, no unchecked `static_cast`.

**Rationale**: spec section 5 offers type erasure but then recommends the variant for the lab because
the event set is known; section 18.1 already writes the variant out. The variant makes the
type/payload mapping a compile-time property instead of an invariant that tests must police.

**Alternatives considered**:
- *`shared_ptr<const void>` plus a type registry*: lets later projects add event types without
  touching project 01, but every consumer needs a checked cast and a wrong registration becomes a
  runtime bug. Rejected for v1.
- *`std::any`*: same checking burden, plus an allocation per event. Rejected.

**Consequence for projects 05-30**: each new event type adds an alternative to the variant in
`event.hpp`, which recompiles consumers. Accepted - the alternative is unchecked casts spread over 25
projects. If the variant becomes unwieldy, that is the trigger to revisit, not now.

---

## R-02 `publish()` before `start()`

**Decision**: returns `PublishStatus::Stopped`. No `NotRunning` value is added.

**Rationale**: FR-06 enumerates exactly five statuses. Section 17.2 allows either but demands the
choice be settled before implementation. Adding a sixth value changes a contract that projects 05-30
will switch on, in order to distinguish a case callers handle identically ("the bus is not accepting
work"). Constitution Principle I: the spec's enumeration wins.

**Alternatives considered**: adding `NotRunning` - more precise diagnostics, but widens the public
enum for a programming error that belongs in review, not in a runtime branch. Rejected.

**Test**: UT-14 is extended to assert `Stopped` both before `start()` and after `stop()`.

---

## R-03 Unsubscribe-and-wait

**Decision**: not in v1. `unsubscribe()` guarantees only that no *new* invocation of that handler
begins after it returns; an in-flight invocation may run to completion (spec section 17.5).

**Rationale**: explicitly listed as an open question in section 14.1 and absent from the v1 scope
list. A correct `unsubscribeAndWait()` must also define what happens when it is called from inside
the subscription's own callback, which is a deadlock trap.

**Alternatives considered**: shipping it with "undefined if called from its own callback" - a
documented footgun in the foundation library that every later project depends on. Rejected.

**Consequence**: consumers must keep their callback target alive until the handler can no longer run,
via `weak_ptr` capture or shutdown ordering. Spec section 17.5 already says this; it is repeated in
the API contract and must appear in the README.

---

## R-04 Subscriber invocation order

**Decision**: registration order, and it **is** part of the contract for v1. Subscribers of one event
type are invoked in the order in which their `subscribe()` calls completed.

**Rationale**: spec section 17.1 proposes registration order; section 14.1 asks whether it is a
contract. Making it one costs nothing with a single dispatcher and a sequence container, and section
20 step 4 asks the demo to verify ordering - only checkable if ordering is promised.

**Alternatives considered**: leaving order unspecified - frees a future multi-worker dispatcher, but
that is in "Ngoai pham vi v1". Rejected now; relaxing the guarantee later is a MAJOR change that the
project taking on parallel dispatch must own.

---

## R-05 Event priority

**Decision**: not in v1. One FIFO queue for all event types.

**Rationale**: section 14.1 raises it; the v1 scope excludes event priority. Section 14.1 itself
warns that if priority is added, critical and best-effort events must not share one FIFO - that means
a second queue and a changed dispatcher, i.e. a redesign.

**Alternatives considered**: a priority queue - breaks the FIFO guarantee of sections 17.1 and 17.4,
and B09 asserts FIFO by enqueue order. Rejected.

---

## R-06 Metrics exposure

**Decision**: a `MetricsSnapshot` returned by value from `EventBus::metrics()`, plus the structured
log lines of section 20. No Prometheus exporter, no HTTP endpoint, no metrics registry.

**Rationale**: section 14.1 asks exporter or log; FR-09 asks only for counters and queue depth. The
component table in section 4.1 explicitly places "Metrics exporter/cloud" outside the bus's
responsibility.

**Alternatives considered**: a Prometheus client library - adds a third-party dependency to a core
library that NFR-01 says has none. Rejected; export belongs to a later project that owns
observability.

---

## R-07 Slow-callback timeout

**Decision**: no timeout is enforced. A slow handler delays the dispatcher; that is observable via
`queue_depth`, `queue_high_watermark` and the demo log, and nothing more.

**Rationale**: section 14.1 asks at which layer a callback timeout lives; section 17.6 states plainly
that a C++ callback cannot be safely interrupted, and section 20.1 says v1 does not guarantee that
one slow subscriber will not affect others.

**Alternatives considered**: running each callback on its own thread with a timeout - turns one
dispatcher into a thread pool ("Ngoai pham vi v1") and still cannot safely kill a stuck handler.
Rejected.

---

## R-08 Repeated `start()`

**Decision**: `start()` is valid only from `Created`. A second call throws `std::logic_error` and
creates no second worker.

**Rationale**: spec section 17.2 proposes exactly this and requires that no silent second worker be
created. `publish()` uses status codes for expected conditions (section 8), but calling `start()`
twice is a programming error rather than an expected runtime condition, so an exception is the right
channel.

**Alternatives considered**: ignoring the second call - hides a lifecycle bug in the consumer.
Rejected.

---

## R-09 Concurrent `stop()`

**Decision**: the first caller to move the state to `Stopping` performs the shutdown and joins the
worker. Other concurrent callers block until the state is `Stopped`, then return. `stop()` is
idempotent; a later call on an already-`Stopped` bus returns immediately. `stop()` must not be called
from the dispatcher thread.

**Rationale**: section 17.7 recommends exactly this ("khuyen nghi cung cho `Stopped`") and forbids
double-join. A single contract is testable (UT-12); "return immediately while shutdown is still in
progress" would let a caller destroy the bus mid-shutdown.

**Alternatives considered**: letting non-first callers return immediately - faster, but the caller
then cannot tell whether draining finished, which breaks the B07 assertion that all accepted events
are dispatched before `stop()` returns. Rejected.

---

## R-10 `BlockUntil` from the dispatcher thread

**Decision**: rejected with `PublishStatus::InvalidArgument`, detected by comparing
`std::this_thread::get_id()` against the stored worker thread id.

**Rationale**: spec sections 6.1 and 17.6 both require this to prevent self-deadlock - the only
thread that can free a queue slot would be the one waiting for it.

**Alternatives considered**: documenting it as undefined behaviour - the deadlock would surface as a
hung test in some later project, far from its cause. Rejected; the check is one thread-id comparison.

**Test**: UT-15 asserts `InvalidArgument` for `BlockUntil` from inside a callback, and `Accepted` or
`QueueFull` for `Reject` from inside a callback.

---

## R-11 `BlockUntil` with a zero timeout

**Decision**: a poll. Space available gives `Accepted`; a full queue gives `TimedOut`, never
`QueueFull`.

**Rationale**: stated verbatim in section 17.4 ("Timeout contract"). It is recorded here because
`TimedOut` rather than `QueueFull` on a full queue is the surprising half, and section 19 requires
that `rejected_queue_full_total` not count timeouts.

**Alternatives considered**: treating `timeout == 0` as `Reject` - contradicts section 17.4 and would
corrupt both counters. Rejected.

---

## R-12 Counting semantics

**Decision**: `dispatched_total` counts **events whose dispatch round completed**, including events
with no subscriber and events whose handler threw. Callback-level activity is counted separately by
`callback_invocations_total` and `callback_failures_total`. The authority for correctness is the
conservation equations in [contracts/metrics.md](./contracts/metrics.md).

**Rationale**: sections 17.3 and 19 state this, and section 19's conservation equations
(`accepted == dispatched` after Drain; `accepted == dispatched + discarded_on_stop` after Discard)
only hold with this definition.

**Alternatives considered**: `dispatched` counting callback invocations - breaks both equations as
soon as an event has zero or two subscribers. Rejected.

---

## R-13 Admission control for in-flight callbacks

**Decision**: each subscription owns a small shared state holding an `active` flag and an in-flight
mutex. The dispatcher snapshots the subscription list under the registry lock, releases that lock,
then for each entry takes that subscription's own mutex, re-checks `active`, and invokes the handler
while holding only that per-subscription mutex. `unsubscribe()` clears `active` and then acquires the
same mutex, which makes it wait out an in-flight invocation without ever holding the registry or
queue lock.

**Rationale**: section 17.5 states that snapshotting alone is **not enough** - the worker must
synchronize with `active` immediately before invoking - while section 6 forbids holding the queue or
registry lock across a callback. A per-subscription mutex satisfies both.

**Alternatives considered**:
- *Generation counter with no per-subscription lock*: cheaper, but leaves the window between the
  `active` check and the call, which is exactly what section 17.5 rules out. Rejected.
- *One global dispatch mutex*: callbacks would serialize against `subscribe()`/`unsubscribe()` from
  other threads and risks the ban on holding a lock across a callback. Rejected.

**Caveat**: `unsubscribe()` called on a subscription from inside its own callback would self-deadlock
on that per-subscription mutex. It therefore needs an owner thread-id check (or a recursive guard):
if the calling thread already holds the mutex, clear `active` and return without acquiring. This is
the behaviour section 17.5 requires - current invocation finishes, no new one starts - and it is a
named test case, UT-09, not an assumption.

---

## R-14 GoogleTest acquisition

**Decision**: CMake `FetchContent` pinned to GoogleTest v1.14.0, guarded by
`option(TCU_BUILD_TESTS ON)` so the library still builds without network access when tests are off.

**Rationale**: NFR-01 permits GoogleTest for tests only. Pinning a version keeps the build
reproducible, which the benchmark requirement of NFR-06 depends on.

**Alternatives considered**: `find_package(GTest)` against a system package - needs a documented host
setup step and yields a different version per machine. Rejected. Vendoring the source - adds
thousands of lines to the repository. Rejected.

---

## Open questions remaining

None blocking. Everything in spec sections 14.1, 17.2 and 21 is closed above, except R-00, which
needs operator confirmation rather than research.
