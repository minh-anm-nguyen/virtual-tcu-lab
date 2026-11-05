#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <variant>

namespace tcu::eventbus {

// Payload types: independently owned, immutable after construction, no raw
// pointer or reference into a producer's stack frame (invariant P1).
struct IgnitionChanged {
  bool on;
};

struct VehicleSpeedUpdated {
  double kmh;
};

struct NetworkStatusChanged {
  bool online;
};

// Project 04 (Configuration & Logging Service): a committed configuration
// generation. The generation number and nothing else - spec section 22 of
// project 04 forbids putting configuration values on the bus, because a
// consumer must re-read the snapshot rather than trust a copy that can go
// stale (its research R-20).
struct ConfigChanged {
  std::uint64_t generation;
};

// Project 05 (Virtual CAN Bus Laboratory): a decoded vehicle signal, carrying
// the quality that says how much to trust it.
//
// IgnitionChanged and VehicleSpeedUpdated above are NOT reused for CAN signals:
// neither carries a quality or a generation, so a subscriber could not tell a
// fresh 0 km/h from a stale one - exactly what project 05's NFR-07 and its spec
// section 6.4 forbid. They stay as they are for project 01's own demo.
//
// The quality and state fields are narrowed to uint8_t deliberately: the bus
// must not depend on tcu_can's headers, so the dependency stays one-way
// (project 05 research R-07).
struct VehicleSignalUpdated {
  std::string signal_name;
  double value;
  std::uint32_t source_can_id;
  std::uint64_t generation;
  std::int64_t monotonic_age_ms;
};

// One event for every quality change - stale, recovered, invalid, link lost -
// because all four are the same fact, and project 05's B-08 requires exactly
// one event per transition either way.
struct SignalQualityChanged {
  std::string signal_name;
  std::uint8_t quality;  // tcu::can::SignalQuality
  std::uint64_t generation;
};

struct CanLinkStatusChanged {
  bool available;
  std::uint8_t service_state;  // tcu::can::ServiceState
};

// Projects 05-30 append alternatives here. Reordering existing alternatives
// changes every EventType value and is a breaking change (research R-01).
using EventPayload =
    std::variant<IgnitionChanged, VehicleSpeedUpdated, NetworkStatusChanged, ConfigChanged,
                 VehicleSignalUpdated, SignalQualityChanged, CanLinkStatusChanged>;

using EventType = std::uint32_t;

namespace detail {

template <typename T, typename Variant, std::size_t I = 0>
constexpr std::size_t variant_index() noexcept {
  if constexpr (I >= std::variant_size_v<Variant>) {
    static_assert(I < std::variant_size_v<Variant>, "T is not an alternative of EventPayload");
    return I;
  } else if constexpr (std::is_same_v<T, std::variant_alternative_t<I, Variant>>) {
    return I;
  } else {
    return variant_index<T, Variant, I + 1>();
  }
}

}  // namespace detail

// Derived from the variant index, so type and payload cannot disagree
// (invariant T1).
template <typename T>
constexpr EventType event_type_of() noexcept {
  return static_cast<EventType>(detail::variant_index<T, EventPayload>());
}

/// One unit of work on the bus. Immutable from the moment publish() returns
/// Accepted: every subscriber of that event observes identical data.
struct Event {
  std::uint64_t id{0};
  EventType type{0};
  std::chrono::steady_clock::time_point created_at{};
  EventPayload payload{};
};

}  // namespace tcu::eventbus
