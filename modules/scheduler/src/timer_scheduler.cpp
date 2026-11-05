#include "tcu/scheduler/timer_scheduler.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "deadline_queue.hpp"
#include "log.hpp"
#include "metrics.hpp"
#include "ready_queue.hpp"
#include "timer_record.hpp"

namespace tcu::scheduler {
namespace {

using detail::LogEvent;
using detail::LogFields;
using detail::LogLevel;
using detail::LogMode;
using detail::LogReason;

void logLine(LogLevel level, LogEvent event, LogReason reason = LogReason::None,
             LogMode mode = LogMode::None,
             std::optional<TimerId> id = std::nullopt,
             std::optional<std::uint64_t> value = std::nullopt) {
  LogFields fields;
  fields.level = level;
  fields.event = event;
  fields.reason = reason;
  fields.mode = mode;
  fields.timer_id = id;
  fields.value = value;
  detail::emit(fields);
}

LogMode logModeOf(detail::TimerKind kind, PeriodicMode mode) {
  if (kind == detail::TimerKind::OneShot) return LogMode::Once;
  return mode == PeriodicMode::FixedDelay ? LogMode::FixedDelay : LogMode::FixedRate;
}

std::uint64_t toMillis(Clock::duration d) {
  if (d.count() <= 0) return 0;
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
}

}  // namespace

struct TimerScheduler::Impl {
  enum class State { Created, Running, StoppingDrain, StoppingDiscard, Stopped };

  Impl(Config cfg, std::shared_ptr<ClockAdapter> injected)
      : config(cfg),
        clock(injected ? std::move(injected)
                       : std::static_pointer_cast<ClockAdapter>(
                             std::make_shared<SteadyClockAdapter>())) {}

  Config config;
  std::shared_ptr<ClockAdapter> clock;

  // One mutex for the registry, the heap, every record state and the ready
  // queue (research R-08): there is no lock order to get wrong, and a callback
  // runs with nothing held, so it may call back in (FR-11, flow 12.15).
  mutable std::mutex mu;
  std::condition_variable coord_cv;
  std::condition_variable work_cv;
  // Serialises stop() so a second caller waits for the first instead of racing.
  std::mutex stop_mu;

  State state{State::Created};
  TimerId nextId{1};
  detail::Registry registry;
  detail::DeadlineQueue heap;
  std::unique_ptr<detail::ReadyQueue> ready;
  detail::Metrics metrics;

  std::thread coordinator;
  std::vector<std::thread> workers;
  bool coordinatorDone{false};
  bool backpressureLogged{false};
  std::uint64_t inFlight{0};

  bool stopping() const {
    return state == State::StoppingDrain || state == State::StoppingDiscard;
  }

  // No lock: the thread handles are written only by start(), before any
  // callback can run.
  bool onSchedulerThread() const {
    const std::thread::id self = std::this_thread::get_id();
    if (coordinator.get_id() == self) return true;
    for (const std::thread& w : workers) {
      if (w.get_id() == self) return true;
    }
    return false;
  }

  ScheduleResult rejectInvalidArgument() {
    metrics.rejected_invalid_argument_total.fetch_add(1, std::memory_order_relaxed);
    return {0, Error::InvalidArgument};
  }

  ScheduleResult addLocked(detail::TimerKind kind, PeriodicMode mode,
                           Clock::time_point deadline, Clock::duration interval,
                           Callback cb, std::uint64_t logValueMs);
  void eraseLocked(TimerId id, bool hasHeapEntry);
  void rearmFixedRateLocked(detail::TimerRecord& rec, Clock::time_point from,
                            bool fired);
  void coordinatorLoop();
  void workerLoop();
  void stopAndJoin(StopMode mode);
};

ScheduleResult TimerScheduler::Impl::addLocked(detail::TimerKind kind,
                                               PeriodicMode mode,
                                               Clock::time_point deadline,
                                               Clock::duration interval,
                                               Callback cb,
                                               std::uint64_t logValueMs) {
  if (state != State::Running) {
    metrics.rejected_invalid_state_total.fetch_add(1, std::memory_order_relaxed);
    return {0, Error::InvalidState};
  }
  if (registry.size() >= config.maxTimers) {
    metrics.rejected_capacity_total.fetch_add(1, std::memory_order_relaxed);
    return {0, Error::CapacityExceeded};
  }

  auto rec = std::make_shared<detail::TimerRecord>();
  rec->id = nextId++;
  rec->state = detail::TimerState::Scheduled;
  rec->generation = 1;
  rec->deadline = deadline;
  rec->callback = std::move(cb);
  rec->kind = kind;
  rec->mode = mode;
  rec->interval = interval;
  rec->phase = deadline;
  rec->periodIndex = 0;

  registry.emplace(rec->id, rec);
  heap.push({deadline, rec->id, rec->generation});
  metrics.timers_scheduled_total.fetch_add(1, std::memory_order_relaxed);
  metrics.timers_active.store(registry.size(), std::memory_order_relaxed);

  logLine(LogLevel::Debug, LogEvent::Schedule, LogReason::None,
          logModeOf(kind, mode), rec->id, logValueMs);
  coord_cv.notify_all();
  return {rec->id, Error::None};
}

void TimerScheduler::Impl::eraseLocked(TimerId id, bool hasHeapEntry) {
  const auto it = registry.find(id);
  if (it == registry.end()) return;
  ++it->second->generation;
  if (hasHeapEntry) heap.noteStale();
  registry.erase(it);
  metrics.timers_active.store(registry.size(), std::memory_order_relaxed);
  coord_cv.notify_all();
}

void TimerScheduler::Impl::rearmFixedRateLocked(detail::TimerRecord& rec,
                                                Clock::time_point from,
                                                bool fired) {
  // Cadence index 0 is the phase; deadline(i) == phase + i * interval
  // (research R-14).
  std::uint64_t slotsPassed = 0;
  const Clock::duration elapsed = from - rec.phase;
  if (elapsed > Clock::duration::zero() && rec.interval > Clock::duration::zero()) {
    slotsPassed = static_cast<std::uint64_t>(elapsed / rec.interval);
  }
  std::uint64_t next = rec.periodIndex + 1;
  if (slotsPassed + 1 > next) next = slotsPassed + 1;

  // A tick that never ran counts as skipped too; one that fired does not.
  const std::uint64_t skipped = next - rec.periodIndex - (fired ? 1 : 0);
  if (skipped > 0) {
    metrics.periods_skipped_total.fetch_add(skipped, std::memory_order_relaxed);
  }
  rec.periodIndex = next;
  rec.deadline =
      rec.phase + rec.interval * static_cast<Clock::duration::rep>(next);
}

void TimerScheduler::Impl::coordinatorLoop() {
  std::unique_lock<std::mutex> lk(mu);
  for (;;) {
    heap.pruneAndCompact(registry);

    if (heap.empty()) {
      if (stopping()) break;
      coord_cv.wait(lk);
      continue;
    }

    const detail::HeapEntry head = heap.peek();
    const Clock::time_point now = clock->now();
    if (now < head.deadline) {
      clock->waitUntil(lk, coord_cv, head.deadline);
      continue;
    }

    const auto it = registry.find(head.id);
    if (it == registry.end()) continue;  // pruned on the next pass
    const std::shared_ptr<detail::TimerRecord> rec = it->second;

    if (ready->full()) {
      if (rec->kind == detail::TimerKind::Periodic &&
          rec->mode == PeriodicMode::FixedRateSkipMissed) {
        // The tick is skipped rather than delayed (spec section 10.1).
        heap.pop();
        ++rec->generation;
        rearmFixedRateLocked(*rec, now, false);
        heap.push({rec->deadline, rec->id, rec->generation});
        continue;
      }
      if (!backpressureLogged) {
        backpressureLogged = true;
        logLine(LogLevel::Warn, LogEvent::Backpressure, LogReason::None,
                LogMode::None, std::nullopt, ready->size());
      }
      // The due timer keeps its deadline, so it stays at the head and is
      // retried first; the wait costs lateness, never a lost callback (R-10).
      coord_cv.wait(lk);
      continue;
    }
    backpressureLogged = false;

    heap.pop();
    rec->state = detail::TimerState::Queued;
    detail::ReadyJob job;
    job.record = rec;
    job.generation = rec->generation;
    job.deadline = head.deadline;
    job.callback = (rec->kind == detail::TimerKind::OneShot)
                       ? std::move(rec->callback)
                       : rec->callback;
    ready->push(std::move(job));
    metrics.ready_queue_depth.store(ready->size(), std::memory_order_relaxed);
    work_cv.notify_one();
  }

  coordinatorDone = true;
  work_cv.notify_all();
}

void TimerScheduler::Impl::workerLoop() {
  std::unique_lock<std::mutex> lk(mu);
  for (;;) {
    while (ready->empty()) {
      // Only the coordinator can add work, so its exit is what ends a worker.
      if (coordinatorDone) return;
      work_cv.wait(lk);
    }

    detail::ReadyJob job = ready->pop();
    metrics.ready_queue_depth.store(ready->size(), std::memory_order_relaxed);
    coord_cv.notify_all();  // queue space freed (research R-10)

    const std::shared_ptr<detail::TimerRecord> rec = job.record;
    if (rec->generation != job.generation ||
        rec->state != detail::TimerState::Queued) {
      // Cancelled after it was queued but before any worker claimed it (B-06).
      metrics.callbacks_skipped_cancelled_total.fetch_add(
          1, std::memory_order_relaxed);
      continue;
    }

    rec->state = detail::TimerState::Running;
    const Clock::time_point start = clock->now();
    metrics.recordLateness(start - job.deadline);
    metrics.callbacks_started_total.fetch_add(1, std::memory_order_relaxed);
    ++inFlight;

    // No scheduler lock is held across the callback (FR-11, invariant S1).
    lk.unlock();
    bool threw = false;
    try {
      job.callback();
    } catch (...) {
      threw = true;
    }
    lk.lock();

    --inFlight;
    const Clock::time_point end = clock->now();
    metrics.recordDuration(end - start);
    const bool periodic = rec->kind == detail::TimerKind::Periodic;
    if (threw) {
      metrics.callbacks_failed_total.fetch_add(1, std::memory_order_relaxed);
      logLine(LogLevel::Error, LogEvent::Callback,
              periodic ? LogReason::PeriodicDisabled : LogReason::Exception,
              logModeOf(rec->kind, rec->mode), rec->id);
    } else {
      metrics.callbacks_completed_total.fetch_add(1, std::memory_order_relaxed);
    }

    const bool cancelRequested =
        rec->state == detail::TimerState::CancelRequested;
    if (!periodic || threw || cancelRequested || state != State::Running) {
      // The record has no heap entry at this point: the coordinator popped it.
      eraseLocked(rec->id, false);
      continue;
    }

    ++rec->generation;
    rec->state = detail::TimerState::Scheduled;
    if (rec->mode == PeriodicMode::FixedDelay) {
      rec->deadline = end + rec->interval;
    } else {
      rearmFixedRateLocked(*rec, end, true);
    }
    heap.push({rec->deadline, rec->id, rec->generation});
    coord_cv.notify_all();
  }
}

void TimerScheduler::Impl::stopAndJoin(StopMode mode) {
  std::lock_guard<std::mutex> serial(stop_mu);
  {
    std::unique_lock<std::mutex> lk(mu);
    if (state == State::Stopped) return;
    if (state == State::Created) {
      state = State::Stopped;
      return;
    }

    state = (mode == StopMode::Drain) ? State::StoppingDrain
                                      : State::StoppingDiscard;
    const LogReason reason =
        (mode == StopMode::Drain) ? LogReason::Drain : LogReason::Discard;
    logLine(LogLevel::Info, LogEvent::Stop, reason, LogMode::None, std::nullopt,
            inFlight);

    // One clock reading is what makes the drain bounded (research R-13):
    // without it a fast periodic timer would keep producing due work.
    const Clock::time_point t = clock->now();
    std::vector<TimerId> doomed;
    doomed.reserve(registry.size());
    for (const auto& entry : registry) {
      const detail::TimerRecord& rec = *entry.second;
      if (rec.state == detail::TimerState::Running ||
          rec.state == detail::TimerState::CancelRequested) {
        continue;  // in flight: allowed to finish in both modes
      }
      if (mode == StopMode::Drain) {
        if (rec.state == detail::TimerState::Queued) continue;
        if (rec.deadline <= t) continue;  // already due at stop time
      }
      doomed.push_back(rec.id);
    }
    for (const TimerId id : doomed) {
      eraseLocked(id, true);
      metrics.timers_cancelled_total.fetch_add(1, std::memory_order_relaxed);
    }

    if (mode == StopMode::Discard) {
      ready->clear();
      metrics.ready_queue_depth.store(0, std::memory_order_relaxed);
    }

    coord_cv.notify_all();
    work_cv.notify_all();
  }

  if (coordinator.joinable()) coordinator.join();
  for (std::thread& w : workers) {
    if (w.joinable()) w.join();
  }

  // Every thread has joined, so nothing can be inside waitUntil any more: the
  // clock must forget this mutex and condition variable before they die with
  // Impl. Found by ThreadSanitizer (plan.md, post-implementation item 7).
  clock->detach();

  {
    std::lock_guard<std::mutex> lk(mu);
    state = State::Stopped;
    if (ready) {
      metrics.ready_queue_depth.store(ready->size(), std::memory_order_relaxed);
    }
    logLine(LogLevel::Info, LogEvent::Stop, LogReason::None, LogMode::None,
            std::nullopt, inFlight);
  }
}

TimerScheduler::TimerScheduler(Config config)
    : impl_(std::make_unique<Impl>(config, nullptr)) {}

TimerScheduler::TimerScheduler(Config config, std::shared_ptr<ClockAdapter> clock)
    : impl_(std::make_unique<Impl>(config, std::move(clock))) {}

TimerScheduler::~TimerScheduler() { impl_->stopAndJoin(StopMode::Discard); }

Error TimerScheduler::start() {
  std::lock_guard<std::mutex> lk(impl_->mu);
  if (impl_->state != Impl::State::Created) return Error::InvalidState;

  const Config& c = impl_->config;
  if (c.workerThreads == 0 || c.maxTimers == 0 || c.maxReadyTasks == 0) {
    return Error::InvalidArgument;
  }

  impl_->ready = std::make_unique<detail::ReadyQueue>(c.maxReadyTasks);
  impl_->state = Impl::State::Running;
  // Attached before any coordinator exists, so a clock driven from outside
  // cannot miss a wake-up (research R-05).
  impl_->clock->attach(impl_->mu, impl_->coord_cv);
  logLine(LogLevel::Info, LogEvent::Start, LogReason::None, LogMode::None,
          std::nullopt, c.workerThreads);

  // Created while holding mu: each thread blocks on it until start() returns,
  // so the worker vector is fully populated before anything reads it.
  Impl* impl = impl_.get();
  impl_->coordinator = std::thread([impl] { impl->coordinatorLoop(); });
  impl_->workers.reserve(c.workerThreads);
  for (std::size_t i = 0; i < c.workerThreads; ++i) {
    impl_->workers.emplace_back([impl] { impl->workerLoop(); });
  }
  return Error::None;
}

// Check order is fixed by contracts/scheduler_api.md section 2.2, so exactly
// one rejection counter moves per call (M6).
ScheduleResult TimerScheduler::scheduleAfter(Clock::duration delay, Callback cb) {
  if (!cb || delay < Clock::duration::zero()) {
    return impl_->rejectInvalidArgument();
  }
  std::lock_guard<std::mutex> lk(impl_->mu);
  const Clock::time_point deadline = impl_->clock->now() + delay;
  return impl_->addLocked(detail::TimerKind::OneShot, PeriodicMode::FixedDelay,
                          deadline, Clock::duration::zero(), std::move(cb),
                          toMillis(delay));
}

ScheduleResult TimerScheduler::scheduleAt(Clock::time_point deadline, Callback cb) {
  // An overdue deadline is valid and immediately eligible (FR-02); there is no
  // too-far-in-the-future rejection.
  if (!cb) return impl_->rejectInvalidArgument();
  std::lock_guard<std::mutex> lk(impl_->mu);
  const Clock::duration delay = deadline - impl_->clock->now();
  return impl_->addLocked(detail::TimerKind::OneShot, PeriodicMode::FixedDelay,
                          deadline, Clock::duration::zero(), std::move(cb),
                          toMillis(delay));
}

ScheduleResult TimerScheduler::scheduleEvery(Clock::duration interval,
                                             PeriodicMode mode, Callback cb) {
  if (!cb || interval <= Clock::duration::zero()) {
    return impl_->rejectInvalidArgument();
  }
  std::lock_guard<std::mutex> lk(impl_->mu);
  const Clock::time_point deadline = impl_->clock->now() + interval;
  return impl_->addLocked(detail::TimerKind::Periodic, mode, deadline, interval,
                          std::move(cb), toMillis(interval));
}

CancelResult TimerScheduler::cancel(TimerId id) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  // The monotonic counter alone separates "was issued and is gone" from "was
  // never issued", so no retention table is needed (research R-07).
  if (id == 0 || id >= impl_->nextId) return CancelResult::NotFound;

  const auto it = impl_->registry.find(id);
  if (it == impl_->registry.end()) return CancelResult::AlreadyFinished;

  detail::TimerRecord& rec = *it->second;
  const LogMode mode = logModeOf(rec.kind, rec.mode);
  switch (rec.state) {
    case detail::TimerState::Scheduled:
    case detail::TimerState::Queued:
      impl_->eraseLocked(id, true);
      impl_->metrics.timers_cancelled_total.fetch_add(1, std::memory_order_relaxed);
      logLine(LogLevel::Info, LogEvent::Cancel, LogReason::Cancelled, mode, id);
      return CancelResult::Cancelled;
    case detail::TimerState::Running:
      rec.state = detail::TimerState::CancelRequested;
      logLine(LogLevel::Info, LogEvent::Cancel, LogReason::CancelRequested, mode, id);
      return CancelResult::RunningCancelRequested;
    case detail::TimerState::CancelRequested:
      return CancelResult::AlreadyFinished;
  }
  return CancelResult::AlreadyFinished;
}

void TimerScheduler::stop(StopMode mode) {
  if (impl_->onSchedulerThread()) {
    // Precondition violation: reported, not fatal, and never a self-join
    // (research R-12).
    // shortcut: the request is dropped rather than queued; upgrade when the
    // spec adds the asynchronous requestStop() it anticipates.
    logLine(LogLevel::Error, LogEvent::Stop, LogReason::CalledFromSchedulerThread);
    return;
  }
  impl_->stopAndJoin(mode);
}

SchedulerMetrics TimerScheduler::metrics() const {
  return impl_->metrics.snapshot();
}

}  // namespace tcu::scheduler
