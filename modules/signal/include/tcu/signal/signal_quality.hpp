#pragma once

// How a decoded signal is identified and what a change of quality looks like.
//
// SignalQuality itself belongs to tcu_dbc: the decoder attaches one to every
// value it produces, and tcu_dbc may not depend on this library. Only the
// cache-level vocabulary lives here.

#include <cstdint>
#include <functional>
#include <string>

#include "tcu/dbc/signal_quality.hpp"

namespace tcu::signal {

/// A signal name alone is not unique across a database (spec section 8), so the
/// cache is keyed by bus, frame format and identifier as well.
struct SignalKey {
  std::uint32_t busId{0};
  bool isExtended{false};
  std::uint32_t arbitrationId{0};
  std::string signalName;

  bool operator==(const SignalKey& other) const {
    return busId == other.busId && isExtended == other.isExtended &&
           arbitrationId == other.arbitrationId && signalName == other.signalName;
  }
};

struct SignalKeyHash {
  std::size_t operator()(const SignalKey& key) const noexcept {
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](std::uint64_t value) {
      h ^= value;
      h *= 1099511628211ull;
    };
    mix(key.busId);
    mix(key.isExtended ? 1u : 0u);
    mix(key.arbitrationId);
    for (const char c : key.signalName) mix(static_cast<unsigned char>(c));
    return static_cast<std::size_t>(h);
  }
};

enum class QualityEventKind {
  /// A valid value was committed (spec section 9.2 row 1 and 2).
  SignalUpdated,
  /// The reported quality changed for any other reason.
  SignalQualityChanged,
  /// Stale -> Valid (spec section 9.2 row 6, B-10).
  SignalRecovered,
  /// A reload changed or removed this signal's meaning (research R-09).
  SchemaChanged,
};

struct QualityEvent {
  QualityEventKind kind{QualityEventKind::SignalQualityChanged};
  SignalKey key;
  dbc::SignalQuality from{dbc::SignalQuality::NotReceived};
  dbc::SignalQuality to{dbc::SignalQuality::NotReceived};
};

const char* toString(QualityEventKind kind) noexcept;

}  // namespace tcu::signal
