# Contract: Timing and Periodic Semantics

**Feature**: `003-timer-task-scheduler` | **Spec sections 11, 12.6, 12.7, 10.1** | **Research**: R-10, R-13, R-14

This is the contract a caller reasons about when it picks a `PeriodicMode`, and the contract the
fake-clock tests encode. Every statement here is observable; nothing is a hope about the OS
scheduler.

---

## 1. What the scheduler guarantees about time

| # | Guarantee |
|---|---|
| G1 | A callback never starts **before** its deadline, measured on the `ClockAdapter` in use (T2). |
| G2 | A callback may start arbitrarily **late**. Lateness comes from worker occupancy, ready-queue backpressure and OS scheduling, and is measured, not bounded (`start_lateness_ms`). |
| G3 | All comparisons use one monotonic clock. Changing the system wall clock, the timezone or NTP stepping has no effect on any deadline (spec section 11.3). |
| G4 | A one-shot callback starts at most once (T1). |
| G5 | Two callbacks of the **same** periodic timer never overlap (T4), in either mode. |
| G6 | There is no catch-up burst. A timer that fell behind does not fire several times back to back (T7). |

**Not guaranteed**: any upper bound on lateness, any behaviour across process suspend/resume, and
sub-millisecond precision. Spec section 6 states the p99 < 50 ms figure as a benchmark target on an
idle reference host, and section 19.4 forbids reading it as a real-time guarantee. No test fails on a
latency threshold (R-18).

---

## 2. One-shot timers

```
deadline = now() + delay          // scheduleAfter, read once inside the call
deadline = argument               // scheduleAt, may already be in the past
```

| Input | Behaviour |
|---|---|
| `delay > 0` | eligible at `deadline` |
| `delay == 0` | eligible immediately (B-03) |
| `delay < 0` | `InvalidArgument`, nothing registered (B-04) |
| `scheduleAt` with a past deadline | eligible immediately (FR-02) |

The record leaves the registry when the callback returns or throws, so a one-shot holds its
`maxTimers` slot only for its own lifetime (R-03).

---

## 3. `PeriodicMode::FixedDelay` (spec section 11.1)

```
first   = now() + interval
next    = completion_time + interval
```

- Cadence drifts by the callback duration. After a 250 ms callback on a 100 ms interval, starts are
  roughly 350 ms apart (flow 12.6).
- Never overlaps itself: the timer is not rearmed until the callback has returned (G5).
- `periods_skipped_total` is never incremented in this mode — nothing is skipped, everything is
  simply later.
- Cancellation during the callback (`RunningCancelRequested`) suppresses the rearm, so the timer ends
  after that invocation (flow 12.5).

**Use it for**: background checks where cadence is not important and self-overlap would be wrong —
OTA progress polling, inactivity sweeps.

### Worked example (fake clock, interval 100)

| Event | Clock | Note |
|---|---|---|
| `scheduleEvery(100, FixedDelay)` | 0 | first deadline 100 |
| callback 1 starts | 100 | |
| callback 1 returns | 350 | 250 long |
| next deadline set | 350 | = 350 + 100 → 450 |
| callback 2 starts | 450 | 350 after callback 1 started |

---

## 4. `PeriodicMode::FixedRateSkipMissed` (spec section 11.2)

```
phase       = first deadline = now() + interval   // t0, fixed for the timer's life
deadline(i) = phase + i * interval                // deadline(0) == phase
periodIndex = cadence index of the current deadline; 0 at registration

after a callback for slot periodIndex completes at c:
    nextIdx = max(periodIndex + 1, (c - phase) / interval + 1)   // integer division
    periods_skipped_total += nextIdx - periodIndex - 1
    periodIndex = nextIdx
    next = phase + periodIndex * interval
```

- Deadlines stay aligned to the original phase; they do not drift with callback duration.
- `nextIdx` is strictly greater than `(c - phase) / interval`, so a callback finishing exactly on a
  cadence boundary waits for the **next** slot rather than firing again at once ("strictly after",
  spec section 11.2).
- Missed slots are counted and dropped. There is no burst (G6).
- Never overlaps itself (G5).

**Use it for**: sampling that should stay on nominal period boundaries — telemetry every 1,000 ms,
heartbeat every 10 s.

### Worked example (fake clock, interval 100, phase 100)

Cadence slots are at 100 (index 0), 200, 300, 400, 500 …

| Event | Clock | `periodIndex` | `periods_skipped_total` | Next deadline |
|---|---|---|---|---|
| `scheduleEvery(100, FixedRateSkipMissed)` | 0 | 0 | 0 | 100 |
| callback 1 starts (slot 0) | 100 | 0 | 0 | — |
| callback 1 returns (250 long) | 350 | 3 | 2 (slots at 200 and 300 missed) | 400 |
| callback 2 starts (slot 3) | 400 | 3 | 2 | — |
| callback 2 returns (10 long) | 410 | 4 | 2 | 500 |

On completion at 350, `(350-100)/100 + 1 = 3`, so `nextIdx = max(1, 3) = 3`: the two slots the long
callback ran through — 200 and 300 — are counted as skipped, and the next deadline is
`100 + 3*100 = 400`. A callback shorter than its interval skips nothing.

### Backpressure interaction

If the ready queue is full when a fixed-rate tick comes due, the tick is **skipped**:
`periods_skipped_total` increments and the next aligned deadline is computed, rather than the tick
waiting (spec section 10.1). This is the one place where a skip happens without a slow callback.

---

## 5. Backpressure for one-shot timers (spec section 10.1, flow 12.10)

A due one-shot whose ready queue is full stays `Scheduled` with its deadline unchanged, so it
remains at the heap head and is dispatched first when space appears (R-10).

| Property | Value |
|---|---|
| Dropped? | never (Q2) |
| Deadline rewritten? | no — lateness stays truthful |
| Coordinator behaviour | waits on a condition variable for queue space; never polls (S5) |
| Observable effect | `ready_queue_depth` pinned at `maxReadyTasks`, `start_lateness_ms` grows, one `backpressure` log line per transition into the full state |
| Second-order effect | the registry fills, so `schedule*` eventually returns `CapacityExceeded` (Q3) |

Memory is bounded by `maxTimers + maxReadyTasks` regardless of how long the overload lasts.

---

## 6. Shutdown and time (research R-13)

`stop(Drain)` reads the clock **once** at entry, at time `t`:

| Timer at `stop(Drain)` | Fate |
|---|---|
| `Scheduled`, `deadline <= t` | runs |
| `Scheduled`, `deadline > t` | cancelled |
| `Queued` | runs |
| `Running` | finishes; periodic not rearmed |
| any periodic timer | not rearmed after its current invocation |

That single clock reading is what makes the drain terminate: without it a 1 ms periodic timer would
keep producing due work and `stop(Drain)` would never return.

`stop(Discard)` keeps only `Running` timers; everything else is cancelled and the ready queue is
cleared, with the cleared jobs counted as cancelled.

---

## 7. Determinism rules for tests

1. Every deadline assertion uses `FakeClockAdapter`. No unit test asserts a real duration.
2. Equal deadlines dispatch in ascending `TimerId` order, so a sequence — not just a set — can be
   asserted (H1).
3. A test that needs "the callback has started" or "the callback may now return" uses a `TestGate`
   (condition variable + predicate), never a sleep.
4. `FakeClockAdapter::advance` may be called from the test thread or from inside a callback - no
   scheduler lock is held across a callback, so there is nothing to deadlock against. A callback
   that advances the clock is how a slow callback becomes a deterministic assertion.
5. Integration tests use the real clock but assert **ordering and counts**, never elapsed times.

A test that cannot be written under these rules is evidence that the behaviour is not actually
specified; the fix is a spec clarification, not a sleep.
