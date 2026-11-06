#include "tcu/dbc/bit_extractor.hpp"

#include <cstdint>
#include <limits>

namespace tcu::dbc {

std::optional<std::uint64_t> extractRaw(const std::uint8_t* data, std::size_t bytes,
                                        std::size_t start, unsigned width,
                                        ByteOrder order) noexcept {
  if (data == nullptr || width == 0 || width > 64) return std::nullopt;
  std::uint64_t raw = 0;
  std::size_t bitPos = start;
  for (unsigned i = 0; i < width; ++i) {
    if (bitPos >= bytes * 8u) return std::nullopt;
    const std::uint64_t bit = (data[bitPos / 8u] >> (bitPos % 8u)) & 1ull;
    if (order == ByteOrder::Intel) {
      raw |= (bit << i);
    } else {
      raw = (raw << 1) | bit;
    }
    if (i + 1u == width) break;
    if (order == ByteOrder::Intel) {
      ++bitPos;
    } else {
      bitPos = (bitPos % 8u == 0u) ? bitPos + 15u : bitPos - 1u;
    }
  }
  return raw;
}

std::optional<std::int64_t> signExtend(std::uint64_t raw, unsigned bits) noexcept {
  if (bits == 0 || bits > 64) return std::nullopt;
  const std::uint64_t mask =
      (bits == 64) ? std::numeric_limits<std::uint64_t>::max() : ((std::uint64_t{1} << bits) - 1u);
  raw &= mask;
  const std::uint64_t signBit = std::uint64_t{1} << (bits - 1u);
  if ((raw & signBit) == 0u) return static_cast<std::int64_t>(raw);
  // Magnitude first, then negate: casting a value above INT64_MAX to int64_t
  // would be implementation-defined, and -(INT64_MIN) would overflow.
  const std::uint64_t magnitude = ((~raw) & mask) + std::uint64_t{1};
  if (magnitude == (std::uint64_t{1} << 63)) return std::numeric_limits<std::int64_t>::min();
  return -static_cast<std::int64_t>(magnitude);
}

}  // namespace tcu::dbc
