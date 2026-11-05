# Contract: Public Scheduler API

**Feature**: `003-timer-task-scheduler` | **Spec section 9** | **Research**: R-05, R-07, R-12

The public surface projects 04-30 may depend on. Anything not listed here is private to
`modules/scheduler/src/` and may change without a spec revision.

```text
modules/scheduler/include/tcu/scheduler/
  scheduler_types.hpp     Clock, TimerId, Callback, enums, Config, ScheduleResult
  clock_adapter.hpp       ClockAdapter, SteadyClockAdapter, FakeClockAdapter
  scheduler_metrics.hpp   Histogram, SchedulerMetrics
  timer_scheduler.hpp     TimerScheduler
```

`FakeClockAdapter` is public on purpose: spec section 6 requires deterministic deadline tests, and
projects 04-30 will need the same seam to test their own timeout logic. Shipping it privately would
force each of them to write it again (Principle II).

---

## 1. Types

```cpp
namespace tcu::scheduler {

using Clock = std::chrono::steady_clock;
using TimerId = std::uint64_t;             // 0 is never issued; it means "no timer"
using Callback = std::function<void()>;

enum class PeriodicMode { FixedDelay, FixedRateSkipMissed };
enum class StopMode { Drain, Discard };
enum class Error { None, InvalidArgument, InvalidState, CapacityExceeded };
enum class CancelResult { Cancelled, RunningCancelRequested, AlreadyFinished, NotFound };

struct Config {
  std::size_t workerThreads{2};
  std::size_t maxTimers{10000};
  std::size_t maxReadyTasks{1024};
};

struct ScheduleResult {
  TimerId id{0};
  Error error{Error::None};
  explicit operator bool() const { return error == Error::None; }
};

}  // namespace tcu::scheduler
```

Identical to spec section 9. No value is added to `Error` or `CancelResult`.

---

## 2. `TimerScheduler`

```cpp
explicit TimerScheduler(Config config);
TimerScheduler(Config config, std::shared_ptr<ClockAdapter> clock);
~TimerScheduler();
TimerScheduler(const TimerScheduler&) = delete;
TimerScheduler& operator=(const TimerScheduler&) = delete;
```

The two-argument constructor is the one deviation from spec section 9's listing, justified in
research R-05. A null `clock` is replaced by a `SteadyClockAdapter`, so the argument cannot make
construction fail. Construction never throws other than `std::bad_alloc` and never starts a thread.

The destructor calls `stop(StopMode::Discard)` if the scheduler is still `Running`, then joins.
**Precondition**: no other thread is calling any method on this object while the destructor runs
(spec section 10, "Destruction precondition"). The library cannot check this, so it is stated, not
asserted.

### 2.1 `Error start()`

| Precondition | Result |
|---|---|
| state `Created`, `workerThreads > 0`, `maxTimers > 0`, `maxReadyTasks > 0` | `None`; state becomes `Running`; 1 coordinator + `workerThreads` workers exist |
| state `Created`, any config field is 0 | `InvalidArgument`; state stays `Created`; no thread created |
| state `Running`, `StoppingDrain`, `StoppingDiscard` or `Stopped` | `InvalidState`; no thread created (B-02, S3) |

Thread-safe. Logs `INFO scheduler.start workers=N max_timers=N max_ready=N` on success.

### 2.2 `ScheduleResult scheduleAfter(Clock::duration delay, Callback cb)`

| Condition | `error` | `id` |
|---|---|---|
| `cb` empty | `InvalidArgument` | 0 |
| `delay < 0` | `InvalidArgument` | 0 |
| state != `Running` | `InvalidState` | 0 |
| `timers_active == maxTimers` | `CapacityExceeded` | 0 |
| otherwise | `None` | the new id |

Checks are applied in that order, so an empty callback is reported as `InvalidArgument` even when the
scheduler is also stopped — exactly one `rejected_*` counter moves per call (M6).

`delay == 0` is valid and makes the timer immediately eligible (spec section 9.1, B-03). The deadline
is `clock->now() + delay`, read once inside the call.

Returns as soon as the timer is registered. It does **not** wait for the callback (flow 12.2).

May be called from inside a callback (flow 12.15) and from any thread.

### 2.3 `ScheduleResult scheduleAt(Clock::time_point deadline, Callback cb)`

Same table, without the negative-delay row: a deadline already in the past is valid and immediately
eligible (FR-02). There is no "too far in the future" rejection.

### 2.4 `ScheduleResult scheduleEvery(Clock::duration interval, PeriodicMode mode, Callback cb)`

Same table as `scheduleAfter`, with `interval <= 0` → `InvalidArgument` (spec section 9.1). The first
deadline is `clock->now() + interval`; that value is also the `phase` used by
`FixedRateSkipMissed` (see [timing_semantics.md](./timing_semantics.md)).

### 2.5 `CancelResult cancel(TimerId id)`

The full resolution table is research R-07. Summary:

| Timer situation | Result | Effect |
|---|---|---|
| `Scheduled` or `Queued` | `Cancelled` | callback will never start; slot released; `timers_cancelled_total++` |
| `Running` | `RunningCancelRequested` | callback continues to completion; a periodic timer is not rearmed |
| `CancelRequested` | `AlreadyFinished` | none |
| issued but gone (`1 <= id < nextId`) | `AlreadyFinished` | none |
| never issued (`id == 0` or `id >= nextId`) | `NotFound` | none |

Idempotent and crash-free for any input, including `0` and `UINT64_MAX` (FR-06, B-16). Thread-safe;
callable from inside a callback, including on the timer currently running — that yields
`RunningCancelRequested` and suppresses the rearm, which is the documented way for a periodic
callback to stop itself.

`cancel()` never blocks waiting for a callback to finish. A caller that needs "the callback is
definitely not running any more" must arrange that itself, e.g. with a `shared_ptr` the callback
holds weakly (spec section 14, row 7).

### 2.6 `void stop(StopMode mode)`

| State | `Drain` | `Discard` |
|---|---|---|
| `Created` | → `Stopped`, no threads to join | same |
| `Running` | bounded drain, research R-13 | cancel everything not `Running`, clear the queue, join |
| `StoppingDrain` / `StoppingDiscard` | waits for the in-progress stop to finish, then returns | same |
| `Stopped` | no-op (B-15) | no-op |

Blocks until every thread joins. No timeout and no force-kill (R-04): a callback that never returns
blocks `stop()` for ever, by design.

**Called from a coordinator or worker thread** (i.e. from inside a callback): logs
`ERROR scheduler.stop reason=called_from_scheduler_thread` and returns immediately, changing
nothing (R-12). This is a precondition violation, reported rather than fatal.

After `stop()` returns in state `Stopped`, no callback runs again (S6) and every `schedule*` returns
`InvalidState`.

### 2.7 `SchedulerMetrics metrics() const`

Returns a consistent-enough snapshot: each counter is read atomically, but the set is not read under
one lock, so two counters may come from moments microseconds apart. The conservation equations of
[metrics.md](./metrics.md) therefore hold **at a quiescent point** (no in-flight callback), which is
how the tests check them. Callable in any state, including `Created` and `Stopped`.

---

## 3. `ClockAdapter`

```cpp
class ClockAdapter {
 public:
  virtual ~ClockAdapter() = default;
  virtual Clock::time_point now() const = 0;
  virtual void waitUntil(std::unique_lock<std::mutex>& lk,
                         std::condition_variable& cv,
                         Clock::time_point deadline) = 0;
  /// Called once by TimerScheduler::start(). Default: does nothing.
  virtual void attach(std::mutex& mu, std::condition_variable& cv);
  /// Called once every scheduler thread has joined. Default: does nothing.
  virtual void detach();
};
```

**Contract for an implementer**:

1. `waitUntil` is entered with `lk` held and must return with `lk` held.
2. It must return when `now() >= deadline`, and must return when `cv` is notified.
3. Spurious returns are permitted — the caller re-checks its own predicate.
4. It must **not** return early otherwise, or the coordinator busy-spins (S5).
5. `now()` must be non-decreasing. A clock that goes backwards breaks T2.
6. `attach` is called once by `start()`, with the mutex and condition variable the coordinator
   waits on, before any coordinator thread exists. A clock whose time is driven from outside needs
   it to know what to wake; a real clock ignores it. Without it there is a lost-wakeup window: a
   fake clock would have to discover the mutex from its first `waitUntil`, and an `advance()`
   racing that registration could move time without notifying anyone.
7. `detach` is called once every scheduler thread has joined, before the mutex and condition
   variable `attach` was given are destroyed. An adapter that stored them **must** forget them
   here. An injected clock normally outlives the scheduler, so without this an `advance()` after
   destruction locks freed memory — a real defect, found by ThreadSanitizer rather than review.

`SteadyClockAdapter` is `Clock::now()` and `cv.wait_until(lk, deadline)`.

`FakeClockAdapter`:

```cpp
Clock::time_point now() const;             // thread-safe
void advance(Clock::duration d);           // moves time forward and wakes the waiter
void advanceTo(Clock::time_point t);       // never moves backwards
```

`advance` locks the scheduler mutex internally, and that is safe from **anywhere** a caller can
reach - including from inside a callback, because no scheduler lock is ever held across one (FR-11,
invariant S1). The periodic tests rely on exactly this: the callback consumes fake time by advancing
the clock itself, which is how a 250 ms callback on a 100 ms interval becomes a deterministic
assertion instead of a real-time wait.

---

## 4. Thread safety summary

| Method | Callable from any thread | Callable from inside a callback |
|---|---|---|
| `start` | yes | no (returns `InvalidState`, since the state is `Running`) |
| `scheduleAfter` / `scheduleAt` / `scheduleEvery` | yes | yes |
| `cancel` | yes | yes |
| `stop` | yes | **no** — logged no-op (R-12) |
| `metrics` | yes | yes |
| destructor | owner only | no |

No method invokes a user callback on the caller's thread. No lock is held across a callback, so a
callback may call back into the scheduler freely (S1).

---

## 5. Not in v1

These do not exist and adding one requires a spec revision first (Principle I):

- Callback priorities, or any ordering other than `(deadline, id)` then FIFO (R-02).
- Runtime worker-pool resizing (R-01).
- `stop()` with a timeout, or any way to interrupt a running callback (R-04).
- Restarting a `Stopped` scheduler (spec section 8.1 V1 policy).
- Cron/calendar expressions, wall-clock deadlines, persistence, cross-process scheduling
  (spec section 1, "Out of scope").
- A callback that takes arguments or returns a value, or any exception propagation to the caller.
- Jitter, backoff or retry policy — the caller owns those (spec section 16, project 10 note).
