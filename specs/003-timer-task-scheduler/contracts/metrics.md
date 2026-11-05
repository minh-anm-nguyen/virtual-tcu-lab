# Contract: Metrics and Structured Logs

**Feature**: `003-timer-task-scheduler` | **Spec section 15** | **Research**: R-15, R-16

Two surfaces: a public `SchedulerMetrics` snapshot, and a private structured log. Both have a
**closed** field set, for the reason project 02 gives for its own: a value that has no field cannot
leak through one (Principle V).

---

## 1. Metric definitions

Spec section 15's list, mapped to snapshot fields. Counters are monotonic `std::uint64_t` and never
reset while the scheduler lives; gauges are instantaneous.

| Spec metric | Field | Kind | Incremented when |
|---|---|---|---|
| `scheduler_timers_active` | `timers_active` | gauge | registry size; see R-03 for what counts |
| `scheduler_ready_queue_depth` | `ready_queue_depth` | gauge | current `ReadyQueue` size |
| — (FR-10 "scheduled") | `timers_scheduled_total` | counter | a `schedule*` call returns `Error::None` |
| `scheduler_callbacks_started_total` | `callbacks_started_total` | counter | a worker claims `Queued -> Running`, immediately before invoking |
| `scheduler_callbacks_completed_total` | `callbacks_completed_total` | counter | a callback returns normally |
| `scheduler_callbacks_failed_total` | `callbacks_failed_total` | counter | a callback throws |
| `scheduler_timers_cancelled_total` | `timers_cancelled_total` | counter | `cancel()` returns `Cancelled`, or `stop()` cancels a timer |
| `scheduler_schedule_rejected_total{reason="invalid_argument"}` | `rejected_invalid_argument_total` | counter | empty callback, negative delay, non-positive interval |
| `…{reason="invalid_state"}` | `rejected_invalid_state_total` | counter | `schedule*` outside `Running` |
| `…{reason="capacity"}` | `rejected_capacity_total` | counter | `timers_active == maxTimers` |
| `scheduler_periods_skipped_total` | `periods_skipped_total` | counter | a fixed-rate cadence slot passes without a start (slow callback, or a full ready queue) |
| — (B-06 observation) | `callbacks_skipped_cancelled_total` | counter | a worker pops a job whose timer was cancelled while `Queued` |
| `scheduler_callback_start_lateness_ms` | `start_lateness_ms` | histogram | one sample per start: `actual_start - job.deadline`, clamped at 0 |
| `scheduler_callback_duration_ms` | `callback_duration_ms` | histogram | one sample per finish, success or throw |

Two fields are beyond spec section 15's list: `timers_scheduled_total`, which FR-10 names ("Track
scheduled, executed, cancelled, rejected, failed, skipped…"), and
`callbacks_skipped_cancelled_total`, without which B-06 ("worker skips callback… metric reports
cancellation") has nothing to assert. Neither adds behaviour.

### Histogram

```cpp
struct Histogram {
  std::array<std::uint64_t, 9> buckets{};   // cumulative counts are NOT stored; each bucket is exclusive
  std::uint64_t count{0};
  std::uint64_t sum_ms{0};
};
```

Upper bounds, in ms: `{1, 5, 10, 25, 50, 100, 250, 1000, +inf}`. A sample lands in the first bucket
whose bound it does not exceed. No percentile is computed in the library — the benchmark derives
p50/p95/p99 from the buckets (R-18), which is enough for spec section 19.4's report and keeps the
hot path to one increment and one add.

---

## 2. Conservation equations

These hold at a **quiescent point**: the scheduler is `Running` or `Stopped` and no callback is
in flight. They are the machine-checkable form of "nothing was lost", and each is a named unit test
(see data-model.md section 3, M1-M6).

| ID | Equation | What it catches |
|---|---|---|
| M1 | `callbacks_started_total == callbacks_completed_total + callbacks_failed_total` | a callback path that neither completes nor counts a failure — e.g. an exception escaping the worker's `catch` |
| M2 | `timers_scheduled_total == finished_one_shots + disabled_periodics + timers_cancelled_total + timers_active` | a timer that vanished without an outcome |
| M3 | `start_lateness_ms.count == callbacks_started_total` | a start that skipped the lateness sample |
| M4 | `callback_duration_ms.count == callbacks_completed_total + callbacks_failed_total` | a finish that skipped the duration sample |
| M5 | `ready_queue_depth == 0` after `stop()` returns | a job left behind by shutdown |
| M6 | each rejection moves exactly one `rejected_*` counter | double-counting a rejection that fails two checks at once |

M2 counts **timers**, not invocations, because one periodic timer produces many callbacks. The test
tracks the periodic timers it created separately and adds them under `timers_active` (still running),
`timers_cancelled_total` (cancelled) or `disabled_periodics` (threw, R-09).

`callbacks_skipped_cancelled_total` is deliberately **not** a term: a timer cancelled while queued is
already counted once in `timers_cancelled_total`, and the skip counter only says where the
cancellation landed. Adding it would double-count that timer.

**Not an invariant**: `timers_active + ready_queue_depth <= maxTimers + maxReadyTasks` is true but
uninteresting; the one worth asserting is `timers_active <= maxTimers` (Q3) and
`ready_queue_depth <= maxReadyTasks` (Q1), which bound memory individually.

---

## 3. Structured log

One line per call, written to `stderr`, newline terminated. Field order is fixed:

```
timestamp level service timer_id mode event reason value
```

| Field | Type | Absent renders as |
|---|---|---|
| `timestamp` | ISO-8601 UTC, ms precision, e.g. `2026-11-04T09:15:02.371Z` | always present |
| `level` | `DEBUG` \| `INFO` \| `WARN` \| `ERROR` | always present |
| `service` | fixed string `scheduler` | always present |
| `timer_id` | `TimerId` | `-` |
| `mode` | `once` \| `fixed_delay` \| `fixed_rate` | `-` |
| `event` | closed vocabulary, below | always present |
| `reason` | short token, e.g. `exception`, `capacity`, `cancelled` | `-` |
| `value` | one numeric detail whose meaning the `event` fixes (delay_ms, depth, in_flight…) | `-` |

`timestamp` is the only place a wall clock is read, and it is for humans only — no deadline uses it
(G3 in [timing_semantics.md](./timing_semantics.md)).

### Event vocabulary

| `event` | Level | When | `value` means |
|---|---|---|---|
| `start` | INFO | `start()` succeeds | `workerThreads` |
| `schedule` | DEBUG | a `schedule*` succeeds | delay or interval, ms |
| `cancel` | INFO | `cancel()` returns `Cancelled` or `RunningCancelRequested` | — |
| `backpressure` | WARN | the ready queue becomes full | `ready_queue_depth` |
| `callback` | ERROR | a callback throws | — |
| `stop` | INFO / ERROR | `stop()` begins and ends; ERROR for the misuse of R-12 | in-flight callback count |

Spec section 15's example lines map onto this exactly:

```
INFO  2026-11-04T09:15:02.371Z scheduler -  -          start         -                     2
DEBUG 2026-11-04T09:15:02.372Z scheduler 42 once       schedule      -                     3000
INFO  2026-11-04T09:15:03.100Z scheduler 42 once       cancel        cancelled             -
WARN  2026-11-04T09:15:04.002Z scheduler -  -          backpressure  -                     1024
ERROR 2026-11-04T09:15:05.510Z scheduler 99 fixed_rate callback      exception             -
INFO  2026-11-04T09:15:06.000Z scheduler -  -          stop          discard               0
```

(Columns are shown padded for readability; the emitter writes single-space-separated fields.)

### What must never appear

- Callback captures, user payloads, or anything derived from them. There is no field for it.
- Credentials, tokens, keys, vehicle identifiers.
- A full telemetry sample (spec section 15: "Avoid logging full telemetry payload, credentials, or
  secrets").

`log_test.cpp` asserts the rendered field set and that the renderer has no parameter through which a
payload could travel — a compile-level guarantee, not a review habit.

### Volume

`schedule` is DEBUG because a 10k-timer run would otherwise emit 10k lines, and DEBUG is **off by
default**: `emit` drops anything below a minimum level read once from `TCU_SCHEDULER_LOG_LEVEL`
(`debug` | `info` | `warn` | `error`, default `info`). Set it to `debug` to see per-schedule lines.

`shortcut: the threshold is one environment variable and the sink is always stderr; upgrade when
project 04 (Configuration & Logging Service) owns both.` Independently of the threshold, the `backpressure` line is emitted once
per transition **into** the full state, not once per blocked dispatch, so a sustained overload does
not flood the log.
