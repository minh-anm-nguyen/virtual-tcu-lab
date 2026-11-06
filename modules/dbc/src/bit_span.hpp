#pragma once

// Which payload bit positions a signal occupies.
//
// One implementation, two consumers: the validator uses it for the bounds check
// (D-07) and the overlap check (D-09), and the extractor walks the same order
// at decode time. If these ever disagreed, a file would validate against one
// layout and decode against another.
//
// position = byteIndex * 8 + bitIndexWithinByte, bit 0 being the LSB of a byte
// (spec section 7.1).

#include <cstdint>
#include <vector>

#include "tcu/dbc/dbc_types.hpp"

namespace tcu::dbc::detail {

/// Appends the `width` positions of this signal to `out`, in decode order.
/// Returns false when any position falls outside `payloadBytes * 8`, which is
/// DBC_OUT_OF_BOUNDS at load time and WrongLength at decode time.
inline bool enumeratePositions(std::uint32_t startBit, std::uint8_t width, ByteOrder order,
                               std::uint8_t payloadBytes, std::vector<std::uint32_t>& out) {
  if (width == 0 || width > 64) return false;
  const std::uint32_t limit = static_cast<std::uint32_t>(payloadBytes) * 8u;
  std::uint32_t position = startBit;
  for (std::uint8_t i = 0; i < width; ++i) {
    if (position >= limit) return false;
    out.push_back(position);
    if (i + 1u == width) break;
    if (order == ByteOrder::Intel) {
      ++position;
    } else {
      // Motorola sawtooth: after the LSB of a byte, continue at the MSB of the
      // next byte (spec section 7.1).
      position = (position % 8u == 0u) ? position + 15u : position - 1u;
    }
  }
  return true;
}

inline std::uint8_t lastByteTouched(const std::vector<std::uint32_t>& positions) {
  std::uint32_t highest = 0;
  for (const std::uint32_t position : positions) {
    if (position / 8u > highest) highest = position / 8u;
  }
  return static_cast<std::uint8_t>(highest);
}

}  // namespace tcu::dbc::detail
