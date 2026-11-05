# Phase 1 Data Model: Timer & Task Scheduler

**Feature**: `003-timer-task-scheduler` | **Spec**: [spec.md](./spec.md) | **Research**: [research.md](./research.md)

Entities, states and the invariants tests assert. Everything here is in-process and in-memory; there
is no persistence (spec section 2, non-goal "durable jobs after a process restart").

---

## 1. Entities

### 1.1 `Config` — public, `scheduler_types.hpp`

| Field | Type | Default | Validation |
|---|---|---|---|
| `workerThreads` | `std::size_t` | 2 | `> 0`, else `start()` → `InvalidArgument` |
| `maxTimers` | `std::size_t` | 10000 | `> 0`, else `start()` → `InvalidArgument` |
| `maxReadyTasks` | `std::size_t` | 1024 | `> 0`, else `start()` → `InvalidArgument` |

Spec section 9's three fields exactly. Validation happens in `start()`, not in the constructor, so
construction cannot fail and the error is reportable (spec section 9.1).

### 1.2 `TimerRecord` — private, owned by the registry

| Field | Type | Meaning |
|---|---|---|
| `id` | `TimerId` | Monotonic, never reused (R-07) |
| `state` | `TimerState` | See section 2.2 |
| `generation` | `std::uint64_t` | Bumped on every rearm and on cancel; invalidates heap entries (R-11) |
| `deadline` | `Clock::time_point` | Next time this timer becomes eligible |
| `callback` | `Callback` | User `std::function<void()>`; non-empty by construction |
| `kind` | `TimerKind` | `OneShot` or `Periodic` |
| `mode` | `PeriodicMode` | Meaningful only when `kind == Periodic` |
| `interval` | `Clock::duration` | `> 0` when `kind == Periodic`; zero otherwise |
| `phase` | `Clock::time_point` | `t0`, the first deadline; fixed-rate alignment base (R-14) |
| `periodIndex` | `std::uint64_t` | Cadence index of the current `deadline`, where index 0 is `phase` (R-14); 0 at registration |

Held as `std::shared_ptr<TimerRecord>`: the registry owns the entry, the ready queue and the heap
hold observing references, so a record outlives its removal from the registry only for as long as a
worker is still running its callback.

The callback is **moved out** of the record into the `ReadyJob` for one-shot timers and **copied**
for periodic ones. A one-shot's captured state is therefore released when its job finishes, not when
`stop()` runs.

### 1.3 `HeapEntry` — private, `DeadlineQueue`

| Field | Type |
|---|---|
| `deadline` | `Clock::time_point` |
| `id` | `TimerId` |
| `generation` | `std::uint64_t` |

Ordered by `(deadline, id)` ascending — the `id` tiebreak makes the dispatch order of equal
deadlines deterministic, which is what lets fake-clock tests assert a sequence instead of a set.
`std::priority_queue` with a greater-than comparator; `std::make_heap` on compaction.

An entry is **stale** when the registry has no record for `id`, or
`entry.generation != record->generation`.

### 1.4 `ReadyJob` — private, `ReadyQueue`

| Field | Type | Meaning |
|---|---|---|
| `record` | `std::shared_ptr<TimerRecord>` | The timer being dispatched |
| `generation` | `std::uint64_t` | Generation at dispatch time |
| `callback` | `Callback` | Moved (one-shot) or copied (periodic) out of the record |
| `deadline` | `Clock::time_point` | The deadline this job is for; the lateness baseline |

FIFO (`std::deque`), capacity `maxReadyTasks` (R-02, R-10).

### 1.5 `SchedulerMetrics` — public snapshot, `scheduler_metrics.hpp`

Plain values, copied out under no lock (counters are atomic). Names match spec section 15:

| Field | Spec metric |
|---|---|
| `timers_active` | `scheduler_timers_active` (gauge) |
| `ready_queue_depth` | `scheduler_ready_queue_depth` (gauge) |
| `callbacks_started_total` | `scheduler_callbacks_started_total` |
| `callbacks_completed_total` | `scheduler_callbacks_completed_total` |
| `callbacks_failed_total` | `scheduler_callbacks_failed_total` |
| `timers_scheduled_total` | FR-10 "scheduled" |
| `timers_cancelled_total` | `scheduler_timers_cancelled_total` |
| `rejected_invalid_argument_total` | `scheduler_schedule_rejected_total{reason="invalid_argument"}` |
| `rejected_invalid_state_total` | `…{reason="invalid_state"}` |
| `rejected_capacity_total` | `…{reason="capacity"}` |
| `periods_skipped_total` | `scheduler_periods_skipped_total` |
| `callbacks_skipped_cancelled_total` | The B-06 observation: a job reached a worker but was cancelled |
| `start_lateness_ms` | `scheduler_callback_start_lateness_ms` histogram |
| `callback_duration_ms` | `scheduler_callback_duration_ms` histogram |

`Histogram` is `{ std::array<std::uint64_t, 9> buckets; std::uint64_t count; std::uint64_t sum_ms; }`
with boundaries `{1, 5, 10, 25, 50, 100, 250, 1000, +inf}` (R-15).

---

## 2. State models

### 2.1 Scheduler state (spec section 8.1)

| From | Event | To |
|---|---|---|
| `Created` | `start()` with valid config | `Running` |
| `Created` | `start()` with invalid config | `Created` (returns `InvalidArgument`) |
| `Created` | `stop(any)` | `Stopped`, no threads were ever created |
| `Running` | `start()` | `Running` (returns `InvalidState`, no extra threads — B-02) |
| `Running` | `stop(Drain)` | `StoppingDrain` |
| `Running` | `stop(Discard)` | `StoppingDiscard` |
| `StoppingDrain` | due/queued work finished | `Stopped` |
| `StoppingDiscard` | in-flight callbacks finished | `Stopped` |
| `Stopped` | `start()` | `Stopped` (returns `InvalidState` — single-use, spec section 8.1 V1 policy) |
| `Stopped` | `stop(any)` | `Stopped` (no-op — B-15) |

`schedule*` succeeds only in `Running`. In every other state it returns `InvalidState` and increments
`rejected_invalid_state_total` (B-15, flow 12.11 step 1).

### 2.2 Timer state (spec section 8.2)

| From | Event | To | Notes |
|---|---|---|---|
| — | `schedule*` accepted | `Scheduled` | `timers_scheduled_total++`, `timers_active++` |
| `Scheduled` | coordinator dispatches, queue has room | `Queued` | heap entry popped |
| `Scheduled` | coordinator dispatches, queue full | `Scheduled` | deadline unchanged, retried (R-10) |
| `Scheduled` | `cancel()` | `Cancelled` → removed | returns `Cancelled` |
| `Queued` | worker claims the job | `Running` | `callbacks_started_total++`, lateness sampled |
| `Queued` | `cancel()` | `Cancelled` → removed | worker later finds the job stale, `callbacks_skipped_cancelled_total++` (B-06) |
| `Running` | one-shot callback returns | removed | `callbacks_completed_total++` |
| `Running` | periodic callback returns | `Scheduled` | rearmed per R-14 / section 11.1 |
| `Running` | callback throws | removed | `callbacks_failed_total++`; periodic is **disabled** (R-09) |
| `Running` | `cancel()` | `CancelRequested` | returns `RunningCancelRequested`; callback not interrupted |
| `CancelRequested` | callback returns | removed | periodic **not** rearmed (flow 12.5) |
| `CancelRequested` | `cancel()` | `CancelRequested` | returns `AlreadyFinished` |

`Cancelled` is a transient label only: a cancelled record is erased from the registry in the same
critical section, and `cancel()` of its id afterwards answers from the monotonic counter (R-07). The
spec's `Completed` state is likewise not stored — a finished one-shot is simply gone.

---

## 3. Invariants

Each invariant names the test that asserts it. These are the properties a reviewer should be able to
check mechanically rather than by reading the implementation.

### Scheduler (S)

| ID | Invariant | Asserted by |
|---|---|---|
| S1 | No user callback is invoked while `mu_` is held (FR-11) | `Lock_CallbackCanScheduleAndCancel` (UT-15); deadlock would hang the test |
| S2 | Every thread created by `start()` is joined by `stop()` or the destructor; none is detached | `Lifecycle_StopJoinsEveryThread`, repeated-instance stress ST-02 |
| S3 | `start()` creates threads at most once per instance | `Lifecycle_StartTwiceIsInvalidState` (UT-02) |
| S4 | `metrics().timers_active == ` registry size, at every quiescent point | `Metrics_ActiveGaugeTracksRegistry` |
| S5 | The coordinator never spins: between two dispatches with no due timer it is blocked in `waitUntil` | `Coordinator_NoWakeupWithoutDueTimerOrNotify` (counts wakeups under the fake clock) |
| S6 | After `stop()` returns, no callback ever runs again | `Lifecycle_NoCallbackAfterStop` (UT-12 partner), IT-03 |

### Timer (T)

| ID | Invariant | Asserted by |
|---|---|---|
| T1 | A one-shot callback starts at most once | `OneShot_StartsExactlyOnce` (UT-03), ST-01 |
| T2 | A callback never starts before its deadline, per the clock in use | `OneShot_NotBeforeDeadline` (UT-16), `FixedRate_DeadlinesAlign` |
| T3 | A cancelled timer that had not started never starts | `Cancel_Scheduled`, `Cancel_Queued` (UT-05, UT-06) |
| T4 | A periodic timer's callbacks never overlap | `FixedDelay_NoOverlap` (UT-08), `FixedRate_NoOverlap` (UT-09) |
| T5 | `TimerId` is strictly increasing and never reused | `Cancel_IdSemantics` (UT-13) |
| T6 | A periodic timer whose callback threw is not rearmed | `Callback_ThrowDisablesPeriodic` (UT-10) |
| T7 | Fixed-rate never emits a catch-up burst: between two consecutive starts at least `interval` of clock time elapses | `FixedRate_SkipsMissedTicks` (UT-09) |

### Heap (H)

| ID | Invariant | Asserted by |
|---|---|---|
| H1 | The entry the coordinator selects is the live entry with the smallest `(deadline, id)` | `DeadlineQueue_EarliestWins` |
| H2 | A stale entry never causes a callback | `DeadlineQueue_StaleEntryIgnored` |
| H3 | Heap size stays `O(live timers)`: after `N` create/cancel cycles with `L` live timers, `heap_.size() <= 2*L + 64` | `DeadlineQueue_CompactionBoundsSize`, churn fault ST-03 |

### Queue (Q)

| ID | Invariant | Asserted by |
|---|---|---|
| Q1 | `ready_.size() <= maxReadyTasks` always | `ReadyQueue_NeverExceedsCapacity` (UT-17) |
| Q2 | A due one-shot is never dropped because the queue was full — it runs or it is cancelled | `Backpressure_OneShotNotLost` (IT-01) |
| Q3 | `timers_active <= maxTimers` always; the `maxTimers + 1`-th `schedule*` returns `CapacityExceeded` and allocates no record | `Capacity_RejectsBeyondMaxTimers` (UT-11) |
| Q4 | Jobs are popped in push order | `ReadyQueue_Fifo` |

### Metrics conservation (M)

Checked at a quiescent point (no in-flight callback). These equations turn "nothing was lost" into
arithmetic, the way project 02's metrics contract does.

| ID | Equation | Asserted by |
|---|---|---|
| M1 | `callbacks_started_total == callbacks_completed_total + callbacks_failed_total` | `Metrics_StartedEqualsCompletedPlusFailed` |
| M2 | `timers_scheduled_total == finished_one_shots + disabled_periodics + timers_cancelled_total + timers_active`, where each term counts **timers**, not callback invocations | `Metrics_ConservationAfterMixedOutcomes` |
| M3 | `start_lateness_ms.count == callbacks_started_total` | `Metrics_LatenessSampledPerStart` |
| M4 | `callback_duration_ms.count == callbacks_completed_total + callbacks_failed_total` | `Metrics_DurationSampledPerFinish` |
| M5 | `ready_queue_depth == 0` after `stop()` returns | `Metrics_QueueEmptyAfterStop` |
| M6 | Every rejection increments exactly one of the three `rejected_*` counters, and no two | `Metrics_RejectionReasonsDisjoint` |

M2 counts timers rather than invocations because a periodic timer is scheduled once and can complete
many times; the test therefore tracks periodic timers separately from their callback counts.

---

## 4. Cross-project entities (spec section 16)

Nothing is added to project 01 or project 02. The integration points are callbacks the demos install:

| Integration | What the scheduler holds | What the other project holds |
|---|---|---|
| Event Bus heartbeat | a `Periodic` timer whose callback calls `EventBus::publish` and checks the result | the event type and the bounded queue |
| IPC request deadline | a `OneShot` timer whose callback flips the caller's request state | `request_id`, the pending map, the socket |

The **single-completion** rule of spec sections 12.13 and 16 lives in the caller, not the scheduler:
a `std::atomic<RequestState>` with `compare_exchange_strong` from `Pending` to `Replied` or
`TimedOut`. `cancel()` alone is explicitly not sufficient, because `RunningCancelRequested` means the
timeout callback is already executing. The demo app owns that atomic; the library has no request
concept (Principle I).
