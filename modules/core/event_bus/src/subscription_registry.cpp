#include "subscription_registry.hpp"

#include <algorithm>

namespace tcu::eventbus::detail {

std::shared_ptr<SubscriptionState> SubscriptionRegistry::add(EventType type, Handler handler) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& entries = by_type_[type];

  // Drop entries already unsubscribed. Without this the vector grows by one
  // per subscribe/unsubscribe cycle forever, snapshot() copies an ever larger
  // vector on every event, and memory is no longer bounded (NFR-03).
  // Erasing is safe even if one of them is mid-callback: the dispatcher holds
  // its own shared_ptr from the snapshot it is iterating.
  entries.erase(std::remove_if(entries.begin(), entries.end(),
                               [](const std::shared_ptr<SubscriptionState>& entry) {
                                 return !entry->active.load(std::memory_order_acquire);
                               }),
                entries.end());

  auto state = std::make_shared<SubscriptionState>(type, std::move(handler), ++next_id_);
  entries.push_back(state);
  return state;
}

std::vector<std::shared_ptr<SubscriptionState>> SubscriptionRegistry::snapshot(
    EventType type) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = by_type_.find(type);
  if (found == by_type_.end()) {
    return {};
  }
  // Appended in registration order, so the copy is already ordered.
  return found->second;
}

}  // namespace tcu::eventbus::detail
