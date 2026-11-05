#pragma once

// Publishes decoded signals and quality changes onto project 01's event bus.
//
// Everything this class does happens AFTER the cache lock has been released:
// the service hands it values and transition lists that cache calls already
// returned. Spec section 17.1 rule 7 is absolute - "Never emit a user callback
// under socket, queue, cache, or lifecycle mutex" - and the surest way to honour
// it is for the publishing code to have no access to the lock at all.
//
// A publish failure is counted and dropped, never propagated: a full bus must
// not take the CAN service down (NFR-06, Constitution Principle V).

#include <memory>
#include <vector>

#include "can_metrics_store.hpp"
#include "tcu/can/can_types.hpp"
#include "tcu/can/signal_cache.hpp"
#include "tcu/eventbus/event_bus.hpp"
#include "tcu/scheduler/clock_adapter.hpp"

namespace tcu::can {

class EventBusAdapter {
 public:
  /// A null bus disables publishing entirely, which is what the unit tests and
  /// a standalone service run use. Nothing else changes behaviour.
  EventBusAdapter(std::shared_ptr<tcu::eventbus::EventBus> bus,
                  std::shared_ptr<tcu::scheduler::ClockAdapter> clock, CanMetricsStore& metrics);

  /// FR-07: the value, where it came from, when, and how much to trust it.
  void publishValue(const SignalSample& sample);

  /// One event per transition the cache reported - no more, no less (B-08).
  void publishTransitions(const std::vector<QualityTransition>& transitions);

  void publishLinkStatus(bool available, ServiceState state);

  bool enabled() const noexcept { return bus_ != nullptr; }

 private:
  std::shared_ptr<tcu::eventbus::EventBus> bus_;
  std::shared_ptr<tcu::scheduler::ClockAdapter> clock_;
  CanMetricsStore& metrics_;
};

}  // namespace tcu::can
