#include "event_bus_adapter.hpp"

#include <chrono>
#include <cstdint>
#include <utility>

namespace tcu::can {
namespace {

template <typename Payload>
tcu::eventbus::Event makeEvent(Payload payload, SteadyTime now) {
  tcu::eventbus::Event event;
  // Derived from the variant index, so the type and the payload cannot
  // disagree - project 01's invariant T1.
  event.type = tcu::eventbus::event_type_of<Payload>();
  event.created_at = now;
  event.payload = std::move(payload);
  return event;
}

}  // namespace

EventBusAdapter::EventBusAdapter(std::shared_ptr<tcu::eventbus::EventBus> bus,
                                 std::shared_ptr<tcu::scheduler::ClockAdapter> clock,
                                 CanMetricsStore& metrics)
    : bus_(std::move(bus)), clock_(std::move(clock)), metrics_(metrics) {}

void EventBusAdapter::publishValue(const SignalSample& sample) {
  if (bus_ == nullptr || !sample.value.has_value()) {
    return;
  }

  const SteadyTime now = clock_->now();
  // An age rather than a timestamp: spec section 25 warns against transporting
  // a raw steady_clock::time_point, because it means nothing outside this
  // process. A duration is interpretable anywhere.
  const auto age =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - sample.last_good_at).count();

  tcu::eventbus::VehicleSignalUpdated payload;
  payload.signal_name = sample.signal_name;
  payload.value = *sample.value;
  payload.source_can_id = sample.source_id;
  payload.generation = sample.generation;
  payload.monotonic_age_ms = static_cast<std::int64_t>(age);

  const auto status = bus_->publish(makeEvent(std::move(payload), now));
  if (status == tcu::eventbus::PublishStatus::Accepted) {
    CanMetricsStore::bump(metrics_.event_publish_total);
  } else {
    // Counted, not thrown and not retried. A full or stopped bus is a telemetry
    // problem; the CAN service keeps receiving either way.
    CanMetricsStore::bump(metrics_.event_publish_failed_total);
  }
}

void EventBusAdapter::publishTransitions(const std::vector<QualityTransition>& transitions) {
  if (bus_ == nullptr) {
    return;
  }

  const SteadyTime now = clock_->now();
  for (const auto& transition : transitions) {
    tcu::eventbus::SignalQualityChanged payload;
    payload.signal_name = transition.signal_name;
    payload.quality = static_cast<std::uint8_t>(transition.to);
    payload.generation = transition.generation;

    const auto status = bus_->publish(makeEvent(std::move(payload), now));
    if (status == tcu::eventbus::PublishStatus::Accepted) {
      CanMetricsStore::bump(metrics_.event_publish_total);
    } else {
      CanMetricsStore::bump(metrics_.event_publish_failed_total);
    }
  }
}

void EventBusAdapter::publishLinkStatus(bool available, ServiceState state) {
  if (bus_ == nullptr) {
    return;
  }

  tcu::eventbus::CanLinkStatusChanged payload;
  payload.available = available;
  payload.service_state = static_cast<std::uint8_t>(state);

  const auto status = bus_->publish(makeEvent(std::move(payload), clock_->now()));
  if (status == tcu::eventbus::PublishStatus::Accepted) {
    CanMetricsStore::bump(metrics_.event_publish_total);
  } else {
    CanMetricsStore::bump(metrics_.event_publish_failed_total);
  }
}

}  // namespace tcu::can
