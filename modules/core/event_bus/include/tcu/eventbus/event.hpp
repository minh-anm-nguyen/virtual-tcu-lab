#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
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

// Projects 05-30 append alternatives here. Reordering existing alternatives
// changes every EventType value and is a breaking change (research R-01).
using EventPayload = std::variant<IgnitionChanged, VehicleSpeedUpdated, NetworkStatusChanged>;

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
