#pragma once

// The observation axis of the quality model. Spec section 8, verbatim.
//
// Stale is a DERIVED label, not a state the decoder can produce: freshness
// belongs to the cache in tcu_signal, which compares the last-good timestamp
// against an injected clock (research R-12). The decoder never emits Stale.

namespace tcu::dbc {

enum class SignalQuality {
  NotReceived,
  Valid,
  InvalidRaw,
  OutOfRange,
  Stale,
  DecodeError,
  NotActive,
  ConfigUnavailable,
};

const char* toString(SignalQuality quality) noexcept;

}  // namespace tcu::dbc
