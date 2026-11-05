# Phase 0 Research: Timer & Task Scheduler

**Feature**: `003-timer-task-scheduler` | **Date**: 2026-11-04 | **Spec**: [spec.md](./spec.md)

Purpose: close every decision the spec leaves open so implementation has no remaining ambiguity.
The spec already fixes the architecture (section 7), the two state machines (section 8), the
illustrative API (section 9), the threading rules (section 10), the periodic semantics (section 11),
the 15 end-to-end flows (section 12), the 20 behavioural contracts (section 13), the fault matrix
(section 14) and the metrics (section 15). Nothing below redesigns any of that.

Entries R-01..R-07 answer spec section 22's ADR candidates. Entries R-08..R-18 answer decisions the
spec states as a goal without naming a mechanism.

---

## R-00: Target platform — Linux **and** Windows, unlike project 02

**Decision**: no platform guard in CMake. `modules/scheduler` builds and tests on both Linux and
Windows.

**Rationale**: the scheduler needs only `<thread>`, `<mutex>`, `<condition_variable>`, `<chrono>`
and `<queue>`. There is no POSIX call anywhere in the design — the reason project 02 is
Linux-only (`AF_UNIX`, `poll`, `SO_PEERCRED`, research R-00 of 002) does not apply. Constitution
"Technology Constraints" asks for Linux to build and test, Windows best-effort; here best-effort is
free, so the root `CMakeLists.txt` adds project 03 outside the existing
`if(CMAKE_SYSTEM_NAME STREQUAL "Linux")` block.

**Alternatives considered**: copying project 02's Linux guard "for consistency". Rejected — it would
disable a project that works, and would make the 10k-timer benchmark unrunnable on the primary dev
host (Windows 11, this repo's working tree).

**Consequence**: sanitizer jobs stay Linux-only (that is a toolchain limit, not a code limit), so
the ASan/TSan acceptance criterion in spec section 21 is evaluated on Linux/WSL.

---

## R-01: Worker count — startup configuration only

**Spec question**: 22.1 "Should worker count be fixed or configurable at runtime? **V1: startup
configuration only.**"

**Decision**: `Config::workerThreads` is read once in `start()`. No `resize()` API.

**Rationale**: resizing a live pool means draining in-flight callbacks of the threads being removed,
which needs per-worker shutdown state the design has no other use for. Principle I: not in the spec,
not built.

---

## R-02: Callback priorities — not in v1

**Spec question**: 22.2. **V1: no.**

**Decision**: `ReadyQueue` is strict FIFO. `scheduler_ready_queue_depth` makes the case for
priorities measurable later; spec section 10.1's fairness note already says FIFO is an intent, not a
guarantee.

---

## R-03: What counts against `maxTimers`

**Spec question**: 22.3 "**V1: active state until completion/cancellation; document exact
implementation accounting.**"

**Decision**: a timer occupies one slot of `maxTimers` from the moment `schedule*` returns success
until its record leaves the registry. The record leaves on:

- a one-shot callback returning (or throwing, or being skipped because it was cancelled),
- a periodic timer being cancelled, or its callback throwing (R-09),
- `stop()` discarding it.

A periodic timer therefore holds exactly one slot for its whole life regardless of how many times it
fires, and `Scheduled`, `Queued`, `Running` and `CancelRequested` all count. `scheduler_timers_active`
is exactly this count, so the gauge and the capacity check read the same number — a divergence
between them is a bug a test can see (invariant S4 in [data-model.md](./data-model.md)).

**Alternatives considered**: counting only `Scheduled`, which would let a burst of due one-shots
admit new timers while the ready queue is still holding the old ones. Rejected: spec section 10.1
relies on the bounded registry to bound the backlog of due-but-unqueued one-shots ("If one-shot due
backlog exhausts configured timer capacity, later `schedule*` calls reject").

---

## R-04: `stop()` has no time limit

**Spec question**: 22.4. **V1: no force-kill.**

**Decision**: `stop()` blocks until the coordinator and every worker joins. A callback that never
returns blocks `stop()` forever, by design (spec section 6: "no guarantee for permanently blocked
callbacks"). The fault-injection test for an indefinitely blocking callback therefore releases its
gate before joining (spec section 14, row 1).

---

## R-05: Fake-clock wakeups — the clock adapter owns the wait

**Spec question**: 22.5 "**V1: clock adapter notifies scheduler CV on time advance.**"

**Decision**: `ClockAdapter` is an abstract class with two operations:

```cpp
class ClockAdapter {
 public:
  virtual ~ClockAdapter() = default;
  virtual Clock::time_point now() const = 0;
  // Releases lk while waiting. Returns when the deadline is reached per this
  // clock, or when cv is notified. Spurious returns are allowed; the caller
  // re-checks its own predicate.
  virtual void waitUntil(std::unique_lock<std::mutex>& lk,
                         std::condition_variable& cv,
                         Clock::time_point deadline) = 0;
};
```

- `SteadyClockAdapter::waitUntil` → `cv.wait_until(lk, deadline)`.
- `FakeClockAdapter::waitUntil` records `lk.mutex()` and `&cv` on first use, then `cv.wait(lk)`
  with no timeout. `FakeClockAdapter::advance(d)` locks that recorded mutex, advances `now_` and
  calls `cv.notify_all()`, so the coordinator re-evaluates its deadline against the new fake time.

**Rationale**: the coordinator loop is written once. The only thing that differs between real and
fake time is who ends the wait, which is exactly what the adapter abstracts. Putting the wait inside
the adapter (rather than exposing a `notify` hook) means the fake clock cannot be advanced without a
wakeup, which is the failure mode that makes injected-clock timer tests hang.

**Alternatives considered**:
- Templating `TimerScheduler` on the clock. Rejected: it moves the whole implementation into a
  header, and the public surface that projects 04-30 consume would become a template.
- Advancing a real `steady_clock` offset. Rejected: `wait_until` would still sleep for the real
  duration, so a 5-minute power-idle test would take 5 minutes.

**Correction found during implementation**: `attach` needs a matching `detach`, called once every
scheduler thread has joined. An injected clock is a `shared_ptr` and normally outlives the
scheduler, so without it an `advance()` after destruction locks the freed coordinator mutex.
ThreadSanitizer caught this; review did not (plan.md, post-implementation item 7).

**Consequence (deviation from spec section 9)**: a second constructor is added,
`TimerScheduler(Config, std::shared_ptr<ClockAdapter>)`. Spec section 9 shows only
`TimerScheduler(Config)` but labels the listing "illustrative, not production-compiled code", and
spec section 6 requires "deterministic deadline tests using an injected clock". The one-argument
constructor remains and defaults to the steady clock, so no caller is forced to know about the
adapter.

---

## R-06: No RTOS executor in v1

**Spec question**: 22.6. **Decision**: `std::thread` only. The `ClockAdapter` seam from R-05 is the
place a future QNX port would also swap the executor; nothing else is abstracted for it today
(Principle I).

---

## R-07: `cancel()` after the record is gone — arithmetic, not a tombstone table

**Spec question**: 22.7 "How to represent `cancel` after tombstones are collected? **V1:
`NotFound` allowed after retention.**"

**Decision**: `TimerId` is a strictly monotonic counter starting at 1; `0` is never issued and means
"no timer". `cancel(id)` resolves as:

| Condition | Result |
|---|---|
| `id` is in the registry, state `Scheduled` or `Queued` | `Cancelled` |
| `id` is in the registry, state `Running` | `RunningCancelRequested` |
| `id` is in the registry, state `CancelRequested` | `AlreadyFinished` |
| `id` not in the registry and `1 <= id < nextId_` | `AlreadyFinished` |
| `id == 0` or `id >= nextId_` | `NotFound` |

**Rationale**: this needs no retention window and no storage at all — the monotonic counter already
distinguishes "was issued and is gone" from "was never issued". It is strictly more informative than
the spec's permitted answer (`NotFound` after retention) and strictly cheaper. There is no `cancel`
result that depends on how long ago the timer finished, so B-16 has no flaky edge.

**Alternatives considered**: a bounded ring of recently finished IDs. Rejected: more code, a new
config knob, and a window inside which the answer changes with timing.

---

## R-08: One mutex for registry + heap + ready queue, two condition variables

**Decision**: a single `std::mutex mu_` guards the registry, the deadline heap, every `TimerRecord`
state field, the ready queue and the scheduler state. Two CVs:

- `coord_cv_` — the coordinator waits on it (through the clock adapter); notified by `schedule*`,
  `cancel`, `stop` and by a worker that frees ready-queue space or completes a periodic callback.
- `work_cv_` — workers wait on it; notified when a job is pushed or when stopping.

A callback is **never** invoked with `mu_` held: the worker claims `Queued -> Running` under the
lock, unlocks, invokes, re-locks to record the outcome (FR-11).

**Rationale**: spec section 10 offers either a fixed lock order across two mutexes or "a single
scheduling mutex plus condition variables". The second has no lock order to get wrong, and it is the
reason `scheduleAfter()` from inside a callback (flow 12.15) cannot deadlock — the callback runs with
nothing held. Contention is not a concern at the stated scale (spec section 6: 100-1,000
callbacks/s); the critical sections are a heap push/pop and a few field writes.

**Alternatives considered**: a separate ready-queue mutex. Rejected — the coordinator must, in one
atomic step, check ready-queue space and decide whether to leave a due timer `Scheduled` (R-10).
Splitting the mutex makes that step non-atomic and reintroduces the backlog double-count.

---

## R-09: A throwing periodic callback is disabled, not retried

**Spec**: section 12.8 states the V1 policy; this records the mechanism.

**Decision**: the worker catches `...`, increments `callbacks_failed_total`, logs
`event=callback reason=exception action=periodic_disabled` (spec section 15's exact line), and the
record is removed instead of rearmed — observably identical to `cancel()` winning. A throwing
one-shot is already finished, so it only logs and counts. No exception crosses a `std::thread`
boundary (spec section 19.2).

---

## R-10: Ready-queue backpressure — a due one-shot stays `Scheduled`

**Spec**: section 10.1 requires "remains pending for later submission; it is not dropped silently"
and "coordinator must never block forever trying to enqueue work".

**Decision**: the coordinator's step for the earliest due timer, all under `mu_`:

1. If `ready_.size() == maxReadyTasks`: leave the timer `Scheduled` with its deadline **unchanged**
   (so it stays at the heap head and is retried first), log `event=backpressure` once per transition
   into the full state, and wait on `coord_cv_` for queue space. Workers notify `coord_cv_` after
   every pop.
2. Otherwise: pop the heap entry, set `Queued`, push the job, notify `work_cv_`.

Lateness is measured at the moment the callback actually starts (`actual_start - deadline`), so time
spent parked in step 1 shows up in `scheduler_callback_start_lateness_ms` — this is what flow 12.10
asserts. There is no busy loop: the coordinator waits on a CV, not a poll.

For **fixed-rate** periodic timers the queue-full branch is different: the tick is skipped,
`periods_skipped_total` increments and the next cadence-aligned deadline is computed (spec
section 10.1). Fixed-delay timers cannot reach this branch while one of their own callbacks is
in flight, because they are not rearmed until completion.

**Consequence**: a sustained full queue plus a stream of due one-shots fills the registry, and
`schedule*` then returns `CapacityExceeded` — exactly the chain spec section 10.1 describes. Memory
stays bounded by `maxTimers + maxReadyTasks`.

---

## R-11: Heap invalidation — generation counters with threshold compaction

**Spec**: section 7 requires generation counters, lazy invalidation, pruning before deadline
selection, and "compaction when invalid entries exceed a configured threshold".

**Decision**: a heap entry is `{deadline, id, generation}`. `TimerRecord::generation` increments on
every rearm and on cancellation. An entry is stale if the record is gone or
`entry.generation != record->generation`. The coordinator discards stale entries at the heap head
before selecting a deadline.

Compaction: the scheduler counts entries it knows to be stale (`staleEstimate_`, incremented on every
cancel and rearm). When `staleEstimate_ * 2 > heap_.size()` **and** `heap_.size() > 64`, the heap is
rebuilt from the live records and `staleEstimate_` resets to 0. Both numbers are internal constants,
not config — spec section 9's `Config` has three fields and Principle I forbids widening it for a
knob no requirement names.

**Rationale**: the churn fault (spec section 14, row 3) is rapid create/cancel, which leaves stale
entries that are not at the head and so are never pruned lazily. Halving on a 50% ratio makes the
heap O(live timers) amortised with no per-cancel scan.

**Alternatives considered**: removing the entry on cancel by index. Rejected: it needs an
id→heap-index map kept in sync on every sift, which is more state than the stale check it replaces.

---

## R-12: `stop()` called from a callback is a logged no-op

**Spec**: section 9 "`stop()` must only be called from outside scheduler worker/coordinator threads
in V1" and "Callback may call `cancel()` and `schedule*()` but not synchronous `stop()`".

**Decision**: `stop()` compares `std::this_thread::get_id()` against the recorded coordinator and
worker IDs. On a match it logs `ERROR event=stop reason=called_from_scheduler_thread` and returns
without changing state — it does not attempt to join the calling thread (self-join is UB) and it
does not half-stop the scheduler.

`shortcut: a callback's stop() request is dropped, not queued; upgrade when the spec adds the
asynchronous requestStop() that section 9 anticipates.`

**Alternatives considered**: `std::terminate`/assert. Rejected: Principle V asks for failures to be
counted and logged rather than fatal, and a demo that aborts teaches the wrong habit. Silently
succeeding was rejected for the same reason — the operator must see the misuse.

---

## R-13: Bounded drain — the snapshot is taken at `stop(Drain)` entry

**Spec**: section 12.11 "only callbacks **already due or queued at stop time** finish; future timers
are cancelled... This is a **bounded drain policy**".

**Decision**: `stop(Drain)` under `mu_`:

1. State `Running -> StoppingDrain`; every later `schedule*` returns `InvalidState`.
2. Let `t = clock->now()`. Every `Scheduled` timer with `deadline > t` is cancelled and counted in
   `timers_cancelled_total`. Timers with `deadline <= t` are kept, as is everything already
   `Queued` or `Running`.
3. The coordinator drains the remaining due timers into the ready queue (still honouring
   `maxReadyTasks`, so a small queue just means several rounds), workers run them, and a periodic
   timer that completes during the drain is **not** rearmed.
4. When the registry is empty and the ready queue is empty, threads join, state becomes `Stopped`.

`stop(Discard)` skips the `deadline <= t` exemption: every timer not yet `Running` is cancelled,
the ready queue is cleared and the cleared jobs are counted as cancelled, in-flight callbacks finish,
threads join.

**Rationale**: step 2's single `now()` reading is what makes the drain bounded and the test
deterministic — without it, a 1 ms periodic timer would keep producing "due" work forever and
`stop(Drain)` would never return.

---

## R-14: Fixed-rate alignment arithmetic

**Spec**: section 11.2 defines `next_deadline` as the first cadence-aligned time strictly after
completion, i.e. `t0 + k*interval` for the smallest qualifying `k`.

**Decision**: each periodic record stores `phase` (`t0`, the first deadline) and `periodIndex`, the
**cadence index of its current deadline**, where index 0 is `phase` itself:

```
deadline(i) = phase + i * interval          // deadline(0) == phase
```

At registration `periodIndex = 0`. After a callback for slot `periodIndex` completes at time `c`:

```
elapsed = c - phase                                        // >= 0, see below
nextIdx = max(periodIndex + 1, elapsed / interval + 1)     // integer division, strictly after c
periods_skipped_total += nextIdx - periodIndex - 1
periodIndex = nextIdx
next_deadline = phase + periodIndex * interval
```

When a tick is skipped **without running** — the ready queue was full at its deadline (R-10) — the
slot that was due counts as skipped too, so the same step runs with
`periods_skipped_total += nextIdx - periodIndex` and `c` replaced by the current time.

Integer arithmetic on `Clock::duration` ticks; no floating point, so there is no drift to accumulate
and the sequence is reproducible under the fake clock. `elapsed >= 0` always holds: `c` is a
completion time and `phase` was already in the past when the first callback started. `nextIdx` is
strictly greater than `elapsed / interval` even when `c` lands exactly on a cadence boundary, which
is what "strictly after" in spec section 11.2 requires.

---

## R-15: Metrics — atomic counters plus fixed-bucket histograms, snapshot struct

**Decision**: an internal `Metrics` struct of `std::atomic<std::uint64_t>` counters, read out through
a plain-value `SchedulerMetrics` snapshot, exactly as project 02 does for `IpcMetricsSnapshot`.
Rejection reasons (spec's `scheduler_schedule_rejected_total{reason}`) are three separate counters,
not a map: the reason set is closed at `invalid_argument`, `invalid_state`, `capacity`.

Histograms (`callback_start_lateness_ms`, `callback_duration_ms`) are fixed-boundary bucket arrays
`{1, 5, 10, 25, 50, 100, 250, 1000, +inf}` plus `count` and `sum_ms`, which is enough to report the
p50/p95/p99 that spec section 19.4 asks for and nothing more. No percentile is computed inside the
library; the benchmark computes it from the buckets.

Lateness is clamped at 0: a callback that starts before its deadline cannot happen (the coordinator
never dispatches early), so the `<=1 ms` bucket absorbing a would-be-negative sample is the visible
sign of that invariant holding rather than an unsigned wrap.

**Alternatives considered**: reusing `modules/ipc/src/metrics.hpp`. Rejected and recorded here
because Principle II deserves the explicit answer: that header is **private** to project 02 (under
`src/`, not `include/`), it hard-codes IPC's counter names, and reusing it would mean promoting it to
project 02's public surface for every later project. Nothing public exists to reuse, so there is
nothing being re-implemented.

---

## R-16: Logging — project 02's discipline, project 03's own private header

**Decision**: `modules/scheduler/src/log.hpp`, a private header with a closed field set taken from
spec section 15's example lines:

```
timestamp level service timer_id mode event reason value
```

and a closed `event` vocabulary: `start`, `schedule`, `cancel`, `backpressure`, `callback`, `stop`.
There is deliberately **no callback-payload or captured-state field**, the same guarantee project 02
makes for IPC payloads (Principle V): a secret cannot leak through a path that does not exist.

**Rationale**: identical reasoning to R-15 — `modules/ipc/src/log.hpp` is private and
IPC-field-shaped. If a third project needs the same thing, the fix is one shared `tcu_log` target
owned by whichever project is lowest in the order, not a copy chain; that is a note for project 04
(Configuration & Logging Service), which the design list already schedules next.

`shortcut: the level threshold is one environment variable (TCU_SCHEDULER_LOG_LEVEL, default info)
and the sink is always stderr; upgrade when project 04 introduces the logging service this field
set is meant to feed.`

---

## R-17: Testing layout and the fake-clock-vs-real-thread split

**Decision**:

- **Unit tests** (`tests/unit/scheduler/`, label `scheduler-unit`) use `FakeClockAdapter` for every
  deadline assertion, and a `TestGate` (condition variable + predicate, modelled on
  `tests/unit/event_bus/support/test_gate.hpp`) for every "callback has started / may now return"
  rendezvous. No test uses `sleep` as a synchronisation device.
- **Integration tests** (`tests/integration/scheduler/`, label `scheduler-integration`) use the real
  `steady_clock` with short intervals and assert ordering and counts, not exact timings — plus the
  two cross-project cases (Event Bus heartbeat, IPC request deadline) that spec section 16 names.
- **Stress tests** (`tests/stress/scheduler/`, label `scheduler-stress`) carry the fault matrix of
  spec section 14 and the 10k-timer case. Duration comes from
  `TCU_SCHEDULER_STRESS_SECONDS` (default 5), copying project 02's R-14 so CI finishes.

**Rationale**: a deadline assertion under real time is the standard source of flaky timer tests. The
fake clock removes timing from the assertion entirely for the 14 unit cases; the integration suite
exists to prove the real `wait_until` path is wired up at all, which the fake clock cannot show.

GoogleTest is reused at the root `CMakeLists.txt` pin `v1.14.0`, not re-declared.

The IPC integration case (B-19) links `tcu_ipc`, which is Linux-only (R-00). That one test file is
guarded so the Windows build keeps configuring; everything else in the three suites is portable.

---

## R-18: Benchmark PT-01 lives in `benchmarks/`, not in the test suite

**Decision**: `benchmarks/scheduler_bench/` (sibling of the existing `benchmarks/event_bus_bench/`)
holds the 10k-timer measurement and prints the table spec section 19.4 asks for: host/compiler/flags,
timer count, callback rate, worker count, queue limits, p50/p95/p99 lateness from the histogram
buckets and skip counts. It is behind `TCU_BUILD_BENCHMARKS`, so `ctest` stays fast. Peak RSS is read
from the OS by the runner script rather than by the library, which has no portable way to ask.

The stress suite still *registers* 10k timers (B-20, spec section 19.3) and asserts bounded memory
and zero loss; what it does not do is assert a latency number. Spec section 6 calls the p99 target a
"benchmark, not guarantee" and section 19.4 says not to claim real-time guarantees from a
workstation run, so no CTest case fails on a latency threshold.

---

## Summary of deviations from the spec's own text

| Spec text | Deviation | Justification |
|---|---|---|
| Section 9 API listing: only `TimerScheduler(Config)` | Second constructor taking `shared_ptr<ClockAdapter>` | Section 6 requires an injected clock; section 9 is labelled illustrative (R-05) |
| Section 17 folder tree rooted at `03_timer_task_scheduler/` | Monorepo paths: `modules/scheduler/`, `apps/`, `tests/*/scheduler/`, `benchmarks/` | Principle II, single monorepo; same delta projects 01 and 02 recorded (see plan.md Structure Decision) |
| Section 17 `docs/system_design.md` | Not created | The design doc is already `spec.md`; a third copy would drift (Principle II) |
| Section 22.7 `NotFound` after retention | `AlreadyFinished` for every issued ID, forever | Strictly more informative and needs no storage (R-07) |
