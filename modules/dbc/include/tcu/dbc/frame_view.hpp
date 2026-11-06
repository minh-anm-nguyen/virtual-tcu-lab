#pragma once

// One CAN frame, as the decoder sees it, plus the closed set of reasons a frame
// can be refused. Spec section 8, verbatim.
//
// This is a BORROWED view: `data` points at bytes somebody else owns, and the
// view must not outlive them. Project 05's tcu::can::CanMessage owns its
// payload, and tcu::signal::toFrameView builds one of these over it for the
// duration of a single decode (research R-22). tcu_dbc deliberately does not
// include project 05's header - the pure library depends on nothing.

#include <chrono>
#include <cstdint>

namespace tcu::dbc {

/// Kept distinct for the whole decode: a standard and an extended frame with
/// the same numeric identifier are two different messages (FR-07, B-07).
enum class FrameFormat { Standard11, Extended29 };

enum class DecodeError {
  None,
  UnknownMessage,
  WrongFormat,
  WrongLength,
  ErrorFrame,
  RemoteFrame,
  UnsupportedFeature,
  InvalidBitLayout,
  NonFinitePhysical,
  InternalError,
};

const char* toString(DecodeError error) noexcept;

/// Classical CAN only in v1: 0..8 payload bytes (research R-01).
static constexpr std::uint8_t kMaxClassicalPayload = 8;

struct CanFrameView {
  /// Identifier bits only. Whoever produces the frame has already stripped the
  /// Linux flag bits; this type never sees them (spec section 8).
  std::uint32_t arbitrationId{0};
  FrameFormat format{FrameFormat::Standard11};
  const std::uint8_t* data{nullptr};
  /// Payload byte count, 0..8. Not the CAN FD wire DLC encoding.
  std::uint8_t size{0};
  bool isRemote{false};
  bool isError{false};
  /// Monotonic, stamped by whoever received the frame. The decoder never
  /// re-stamps it (research R-06, spec section 10.5).
  std::chrono::steady_clock::time_point rxTime{};
  std::uint32_t busId{0};
};

}  // namespace tcu::dbc
