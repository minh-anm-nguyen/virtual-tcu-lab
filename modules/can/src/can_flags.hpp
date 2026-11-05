#pragma once

// SocketCAN identifier flag bits, and the one place a raw can_id is split into
// a numeric identifier plus its frame format and kind.
//
// The values are declared here rather than taken from <linux/can.h> so this
// header is usable from fake_can_transport.cpp, which must be able to build the
// malformed frames the kernel refuses to carry (spec section 18.1 test note).
// socketcan_transport.cpp static_asserts each one against the real UAPI
// constant, so a divergence is a compile error rather than a silent
// misclassification.

#include <cstdint>

#include "tcu/can/can_types.hpp"
#include "tcu/can/frame_catalog.hpp"

namespace tcu::can::flags {

inline constexpr std::uint32_t kEffFlag = 0x80000000u;
inline constexpr std::uint32_t kRtrFlag = 0x40000000u;
inline constexpr std::uint32_t kErrFlag = 0x20000000u;
inline constexpr std::uint32_t kSffMask = 0x000007FFu;
inline constexpr std::uint32_t kEffMask = 0x1FFFFFFFu;

static_assert(kSffMask == kStandardIdMask,
              "frame_catalog.hpp and can_flags.hpp disagree about the 11-bit mask");

struct RawId {
  std::uint32_t id;
  FrameFormat format;
  FrameKind kind;
};

/// Spec section 6.2: "SocketCAN can_id also contains flag bits; mask them
/// before using a numeric identifier." The flags are preserved as `format` and
/// `kind` so a rejection can say what it rejected (B-13, B-14, B-15).
///
/// An error frame is reported as such regardless of its other bits: the ERR
/// flag changes what the whole frame means, so classifying it by its
/// identifier would be misleading.
inline constexpr RawId decodeRawId(std::uint32_t raw_id) noexcept {
  const bool eff = (raw_id & kEffFlag) != 0;
  const bool rtr = (raw_id & kRtrFlag) != 0;
  const bool err = (raw_id & kErrFlag) != 0;

  const FrameKind kind =
      err ? FrameKind::Error : (rtr ? FrameKind::Remote : FrameKind::Data);
  const FrameFormat format = eff ? FrameFormat::Extended29 : FrameFormat::Standard11;
  const std::uint32_t id = eff ? (raw_id & kEffMask) : (raw_id & kSffMask);

  return {id, format, kind};
}

}  // namespace tcu::can::flags
