#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "tcu/eventbus/event.hpp"
#include "tcu/eventbus/event_bus.hpp"

namespace tcu::eventbus::detail {

// Per-subscription state, owned by the registry and referenced weakly by the
// public Subscription handle (invariant U3). in_flight and owner are what let
// unsubscribe() wait out a running callback while holding neither the registry
// nor the queue lock; they come into use with T056/T057 (research R-13).
struct SubscriptionState {
  SubscriptionState(EventType event_type, Handler event_handler, std::uint64_t registration_id)
      : type(event_type), handler(std::move(event_handler)), id(registration_id) {}

  const EventType type;
  const Handler handler;
  const std::uint64_t id;
  std::atomic<bool> active{true};
  std::mutex in_flight;
  std::atomic<std::thread::id> owner{std::thread::id{}};
};

class SubscriptionRegistry {
 public:
  std::shared_ptr<SubscriptionState> add(EventType type, Handler handler);

  // Ascending registration id, which is registration order (invariant U4).
  std::vector<std::shared_ptr<SubscriptionState>> snapshot(EventType type) const;

 private:
  mutable std::mutex mutex_;
  std::unordered_map<EventType, std::vector<std::shared_ptr<SubscriptionState>>> by_type_;
  std::uint64_t next_id_ = 0;
};

}  // namespace tcu::eventbus::detail
