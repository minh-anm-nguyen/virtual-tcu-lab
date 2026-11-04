#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>

#include "tcu/eventbus/event.hpp"
#include "tcu/eventbus/metrics.hpp"

namespace tcu::eventbus {

// Exactly these five values: FR-06. Research R-02 declined to add NotRunning.
enum class PublishStatus { Accepted, QueueFull, TimedOut, Stopped, InvalidArgument };

enum class FullQueuePolicy { Reject, BlockUntil };

enum class StopMode { Drain, Discard };

struct BusConfig {
  std::size_t queue_capacity = 1024;
};

struct PublishOptions {
  FullQueuePolicy on_full = FullQueuePolicy::Reject;
  // Ignored when on_full == Reject. With BlockUntil, 0 means poll (research
  // R-11); negative yields InvalidArgument.
  std::chrono::milliseconds timeout{0};
};

using Handler = std::function<void(const Event&)>;

namespace detail {
struct SubscriptionState;
}

// Move-only RAII handle. Holds weak ownership, so a handle outliving its
// EventBus is inert rather than dangling (invariant U3).
class Subscription {
 public:
  Subscription() noexcept = default;
  Subscription(Subscription&& other) noexcept;
  Subscription& operator=(Subscription&& other) noexcept;
  Subscription(const Subscription&) = delete;
  Subscription& operator=(const Subscription&) = delete;
  ~Subscription();

  /// Stops any *new* invocation of this handler, and waits out one already in
  /// progress. Idempotent, and a no-op once the bus is gone.
  ///
  /// v1 has no unsubscribe-and-wait variant beyond this (research R-03).
  /// Calling it from inside this subscription's own callback is safe and does
  /// not deadlock: the current invocation finishes, no new one starts.
  void unsubscribe();

 private:
  friend class EventBus;
  explicit Subscription(std::weak_ptr<detail::SubscriptionState> state) noexcept;

  std::weak_ptr<detail::SubscriptionState> state_;
};

// In-process event bus: many publisher threads, a bounded FIFO queue, one
// dispatcher worker.
//
// Caller obligation C3: no other thread may call a method on the bus while it
// is being destroyed. Ordinary C++ object lifetime rules apply and the bus
// does not defend against this (spec section 17.7). The destructor stops a
// still-running bus with StopMode::Drain.
//
// A Subscription handle outliving the bus is the one exception that IS
// handled: it becomes inert rather than dangling (invariant U3).
class EventBus {
 public:
  explicit EventBus(BusConfig config);
  ~EventBus();
  EventBus(const EventBus&) = delete;
  EventBus& operator=(const EventBus&) = delete;

  /// Spawns the single dispatcher worker and moves Created -> Running.
  /// @throws std::logic_error from any other state; no second worker is ever
  ///         created (research R-08, invariant S2).
  void start();
  /// Offers an event to the queue. Never throws for an expected condition.
  ///
  /// Accepted means *enqueued*, nothing more: no handler has run, and none is
  /// promised to succeed. A non-Accepted return means the event is not in the
  /// queue - v1 never drops an accepted event silently.
  ///
  /// Safe from any number of threads. Never invokes a handler on the calling
  /// thread. BlockUntil from the dispatcher thread returns InvalidArgument
  /// rather than self-deadlocking (research R-10).
  PublishStatus publish(Event event, PublishOptions opts = {});
  /// Registers a handler for one event type. Handlers of the same type are
  /// invoked in registration order, and that order is part of the contract in
  /// v1 (research R-04). A handler is never invoked while the bus holds its
  /// queue or registry lock.
  ///
  /// Caller obligation C1: the handler must not capture a raw `this` that can
  /// be destroyed before the handler can no longer run. Subscription manages
  /// the registration, not the lifetime of what the callback captured.
  ///
  /// Caller obligation C2: a handler should not block for long. There is no
  /// callback timeout, and a slow handler delays every later event.
  ///
  /// @throws std::invalid_argument if `callback` is empty.
  /// @throws std::logic_error if the bus is Stopping or Stopped.
  Subscription subscribe(EventType type, Handler callback);
  /// Shuts the bus down and joins the worker exactly once.
  ///
  /// From the instant the state leaves Running every publish returns Stopped,
  /// and every publisher blocked in BlockUntil is woken with Stopped.
  ///
  /// Drain dispatches every event accepted before the stop before returning,
  /// with **no time limit**: a handler that never returns hangs stop().
  /// Discard drops the queued events, counts them in discarded_on_stop_total,
  /// and lets the handler already running finish.
  ///
  /// Idempotent. Concurrent callers all wait until the state is Stopped
  /// (research R-09). Must not be called from the dispatcher thread.
  void stop(StopMode mode = StopMode::Drain);
  /// Returns a by-value snapshot. Each field is a consistent read of its own
  /// counter, but the snapshot is not atomic across fields: the conservation
  /// equations of contracts/metrics.md hold only after stop() has returned.
  MetricsSnapshot metrics() const;

 private:
  // Pimpl: bounded_queue.hpp and subscription_registry.hpp stay private to
  // src/, so they never widen the contract projects 05-30 depend on.
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tcu::eventbus
