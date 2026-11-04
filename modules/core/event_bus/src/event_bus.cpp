#include "tcu/eventbus/event_bus.hpp"

#include <atomic>
#include <memory>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "bounded_queue.hpp"
#include "subscription_registry.hpp"

namespace tcu::eventbus {

// Lifecycle states of data-model.md section 6. Internal: the public API
// exposes behaviour, not the state value.
enum class BusState { Created, Running, Stopping, Stopped };

struct EventBus::Impl {
  explicit Impl(const BusConfig& config) : queue(config.queue_capacity) {}

  void run();

  detail::BoundedQueue queue;
  detail::SubscriptionRegistry registry;

  std::mutex state_mutex;
  // Concurrent stop() callers wait here until the state is Stopped (R-09).
  std::condition_variable stopped_cv;
  BusState state = BusState::Created;
  std::thread worker;
  // Atomic so T070 can compare against it from a publisher thread without a
  // data race on the dispatcher-thread guard.
  std::atomic<std::thread::id> worker_id{std::thread::id{}};

  // One atomic per counter. Each field reads consistently; a snapshot is not
  // atomic across fields (invariant M2). queue_depth and queue_high_watermark
  // are owned by the queue, which knows them under its own mutex.
  std::atomic<std::uint64_t> accepted_total{0};
  std::atomic<std::uint64_t> rejected_queue_full_total{0};
  std::atomic<std::uint64_t> publish_timeout_total{0};
  std::atomic<std::uint64_t> dispatched_total{0};
  std::atomic<std::uint64_t> callback_invocations_total{0};
  std::atomic<std::uint64_t> callback_failures_total{0};
  std::atomic<std::uint64_t> discarded_on_stop_total{0};
};

namespace {

// T072: one structured line to stderr, silenced unless TCU_EVENTBUS_LOG is
// set so test output stays clean. Callers pass only the keys approved in
// contracts/metrics.md; payload contents, keys and tokens never appear
// (constitution Principle V). Writing to stderr does not recurse through the
// bus and does not block the dispatcher for long (spec section 8).
bool logging_enabled() {
  static const bool enabled = std::getenv("TCU_EVENTBUS_LOG") != nullptr;
  return enabled;
}

void log_line(const char* level, const std::string& fields) {
  if (!logging_enabled()) {
    return;
  }
  std::clog << level << "  " << fields << std::endl;
}

void log_callback_failure(std::uint64_t event_id, EventType event_type,
                          std::uint64_t subscriber, const char* reason) {
  if (!logging_enabled()) {
    return;
  }
  std::ostringstream fields;
  fields << "event_id=" << event_id << " event_type=" << event_type
         << " subscriber=" << subscriber << " callback_status=failed reason=" << reason;
  log_line("WARN ", fields.str());
}

// Marks the thread currently inside a callback, so unsubscribe() called from
// that very callback can detect it already holds the mutex (R-13 caveat).
// Exception-safe by construction, which matters once T069 adds try/catch.
class OwnerMark {
 public:
  explicit OwnerMark(detail::SubscriptionState& state) : state_(state) {
    state_.owner.store(std::this_thread::get_id(), std::memory_order_release);
  }
  ~OwnerMark() { state_.owner.store(std::thread::id{}, std::memory_order_release); }
  OwnerMark(const OwnerMark&) = delete;
  OwnerMark& operator=(const OwnerMark&) = delete;

 private:
  detail::SubscriptionState& state_;
};

}  // namespace

// T028: the dispatcher. No lock is held while a handler runs (invariant Q4,
// guarantee G11). Exception isolation is T069; full admission control is T056.
void EventBus::Impl::run() {
  for (;;) {
    std::optional<Event> event = queue.pop();
    if (!event) {
      return;  // queue closed and drained
    }
    for (const std::shared_ptr<detail::SubscriptionState>& subscription :
         registry.snapshot(event->type)) {
      // Snapshotting alone is not enough (spec section 17.5): active is
      // re-checked under this subscription's own mutex, immediately before the
      // call. Holding only that mutex keeps the queue and registry locks free
      // while the handler runs (invariant Q4) and lets unsubscribe() wait the
      // invocation out (research R-13).
      std::lock_guard<std::mutex> in_flight(subscription->in_flight);
      if (!subscription->active.load(std::memory_order_acquire)) {
        continue;
      }
      const OwnerMark owner_mark(*subscription);
      callback_invocations_total.fetch_add(1, std::memory_order_relaxed);
      try {
        subscription->handler(*event);
      } catch (const std::exception& error) {
        // Isolated and counted; the next subscriber still gets this event and
        // the worker lives on (FR-10, spec section 17.6).
        callback_failures_total.fetch_add(1, std::memory_order_relaxed);
        log_callback_failure(event->id, event->type, subscription->id, error.what());
      } catch (...) {
        callback_failures_total.fetch_add(1, std::memory_order_relaxed);
        log_callback_failure(event->id, event->type, subscription->id, "unknown exception");
      }
    }
    // One increment per completed dispatch round, including an event with no
    // subscriber (research R-12, contracts/metrics.md).
    dispatched_total.fetch_add(1, std::memory_order_relaxed);
  }
}

EventBus::EventBus(BusConfig config) {
  if (config.queue_capacity == 0) {
    throw std::invalid_argument("EventBus: queue_capacity must be greater than 0");
  }
  impl_ = std::make_unique<Impl>(config);
}

EventBus::~EventBus() {
  if (!impl_) {
    return;
  }
  stop(StopMode::Drain);
}

void EventBus::start() {
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  if (impl_->state != BusState::Created) {
    throw std::logic_error("EventBus::start: only valid from the Created state");
  }
  impl_->state = BusState::Running;
  {
    std::ostringstream fields;
    fields << "bus_state=Running capacity=" << impl_->queue.capacity();
    log_line("INFO ", fields.str());
  }
  Impl* impl = impl_.get();
  impl_->worker = std::thread([impl] { impl->run(); });
  impl_->worker_id.store(impl_->worker.get_id(), std::memory_order_release);
}

PublishStatus EventBus::publish(Event event, PublishOptions opts) {
  if (event.type != static_cast<EventType>(event.payload.index())) {
    return PublishStatus::InvalidArgument;  // invariant E2
  }
  if (opts.timeout.count() < 0) {
    return PublishStatus::InvalidArgument;
  }
  if (opts.on_full == FullQueuePolicy::BlockUntil &&
      impl_->worker_id.load(std::memory_order_acquire) == std::this_thread::get_id()) {
    // The only thread that can free a slot is the one that would wait for it,
    // so this would self-deadlock. Refused by a thread-id comparison, not a
    // heuristic (research R-10, spec sections 6.1 and 17.6).
    return PublishStatus::InvalidArgument;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    if (impl_->state != BusState::Running) {
      return PublishStatus::Stopped;  // research R-02: before start and after stop
    }
  }

  // The queue's closed flag is the real linearization point for enqueue
  // (invariant S1): a stop racing with the check above is refused here, not
  // enqueued after shutdown began. Rejecting BlockUntil from the dispatcher
  // thread is T070.
  detail::EnqueueResult result;
  if (opts.on_full == FullQueuePolicy::Reject) {
    result = impl_->queue.push(std::move(event));
  } else {
    result = impl_->queue.push_until(std::move(event),
                                     std::chrono::steady_clock::now() + opts.timeout);
  }

  switch (result.status) {
    case detail::EnqueueStatus::Accepted:
      impl_->accepted_total.fetch_add(1, std::memory_order_relaxed);
      return PublishStatus::Accepted;
    case detail::EnqueueStatus::Full:
      impl_->rejected_queue_full_total.fetch_add(1, std::memory_order_relaxed);
      return PublishStatus::QueueFull;
    case detail::EnqueueStatus::TimedOut:
      impl_->publish_timeout_total.fetch_add(1, std::memory_order_relaxed);
      return PublishStatus::TimedOut;
    case detail::EnqueueStatus::Closed:
      // Stopped increments nothing: a shutdown race is not backpressure
      // (contracts/metrics.md).
      return PublishStatus::Stopped;
  }
  return PublishStatus::Stopped;
}

Subscription EventBus::subscribe(EventType type, Handler callback) {
  if (!callback) {
    throw std::invalid_argument("EventBus::subscribe: callback must not be empty");
  }
  std::lock_guard<std::mutex> lock(impl_->state_mutex);
  if (impl_->state != BusState::Created && impl_->state != BusState::Running) {
    throw std::logic_error("EventBus::subscribe: the bus is shutting down or stopped");
  }
  return Subscription(impl_->registry.add(type, std::move(callback)));
}

void EventBus::stop(StopMode mode) {
  std::thread worker;
  {
    std::unique_lock<std::mutex> lock(impl_->state_mutex);
    if (impl_->state == BusState::Created) {
      impl_->state = BusState::Stopped;
      impl_->stopped_cv.notify_all();
      return;
    }
    if (impl_->state == BusState::Stopped) {
      return;  // idempotent (invariant S2)
    }
    if (impl_->state == BusState::Stopping) {
      // Someone else owns this shutdown. Wait for it rather than returning
      // early, so every caller sees a finished Drain (research R-09).
      impl_->stopped_cv.wait(lock, [this] { return impl_->state == BusState::Stopped; });
      return;
    }
    // The single linearization point: from here no publish is accepted
    // (invariant S1).
    impl_->state = BusState::Stopping;
    worker = std::move(impl_->worker);
  }

  {
    std::ostringstream fields;
    fields << "bus_state=Stopping mode=" << (mode == StopMode::Discard ? "Discard" : "Drain");
    log_line("INFO ", fields.str());
  }

  if (mode == StopMode::Discard) {
    const std::size_t dropped = impl_->queue.close_and_discard();
    impl_->discarded_on_stop_total.fetch_add(dropped, std::memory_order_relaxed);
  } else {
    // Drain: the queue keeps its backlog, so the worker dispatches all of it
    // before pop() reports the queue closed and empty. No time limit (G17).
    impl_->queue.close();
  }

  if (worker.joinable()) {
    worker.join();  // joined exactly once (invariant S2)
  }

  {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    impl_->state = BusState::Stopped;
  }
  impl_->stopped_cv.notify_all();

  {
    const MetricsSnapshot final_metrics = metrics();
    std::ostringstream fields;
    fields << "bus_state=Stopped accepted=" << final_metrics.accepted_total
           << " dispatched=" << final_metrics.dispatched_total
           << " discarded=" << final_metrics.discarded_on_stop_total
           << " callback_failures=" << final_metrics.callback_failures_total;
    log_line("INFO ", fields.str());
  }
}

MetricsSnapshot EventBus::metrics() const {
  MetricsSnapshot snapshot;
  snapshot.accepted_total = impl_->accepted_total.load(std::memory_order_relaxed);
  snapshot.rejected_queue_full_total =
      impl_->rejected_queue_full_total.load(std::memory_order_relaxed);
  snapshot.publish_timeout_total = impl_->publish_timeout_total.load(std::memory_order_relaxed);
  snapshot.dispatched_total = impl_->dispatched_total.load(std::memory_order_relaxed);
  snapshot.callback_invocations_total =
      impl_->callback_invocations_total.load(std::memory_order_relaxed);
  snapshot.callback_failures_total =
      impl_->callback_failures_total.load(std::memory_order_relaxed);
  snapshot.discarded_on_stop_total =
      impl_->discarded_on_stop_total.load(std::memory_order_relaxed);
  snapshot.queue_depth = impl_->queue.depth();
  snapshot.queue_high_watermark = impl_->queue.high_watermark();
  return snapshot;
}

// --- Subscription ----------------------------------------------------------

Subscription::Subscription(std::weak_ptr<detail::SubscriptionState> state) noexcept
    : state_(std::move(state)) {}

Subscription::Subscription(Subscription&& other) noexcept : state_(std::move(other.state_)) {
  other.state_.reset();
}

Subscription& Subscription::operator=(Subscription&& other) noexcept {
  if (this != &other) {
    unsubscribe();  // invariant U1: assignment drops the previous registration
    state_ = std::move(other.state_);
    other.state_.reset();
  }
  return *this;
}

Subscription::~Subscription() { unsubscribe(); }

void Subscription::unsubscribe() {
  const std::shared_ptr<detail::SubscriptionState> state = state_.lock();
  state_.reset();  // idempotent (U2); a no-op once the bus is gone (U3)
  if (!state) {
    return;
  }

  // Clearing active is what stops any *new* invocation (invariant U2).
  state->active.store(false, std::memory_order_release);

  if (state->owner.load(std::memory_order_acquire) == std::this_thread::get_id()) {
    // Called from inside this subscription's own callback: this thread already
    // holds in_flight, so acquiring it would self-deadlock. The flag is
    // cleared, the current invocation finishes, no new one starts - exactly
    // what spec section 17.5 requires (research R-13 caveat).
    return;
  }

  // Wait out an invocation already in progress. Neither the registry nor the
  // queue lock is held here, so this cannot block the dispatcher's other work.
  const std::lock_guard<std::mutex> in_flight(state->in_flight);
}

}  // namespace tcu::eventbus
