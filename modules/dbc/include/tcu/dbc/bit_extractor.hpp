#pragma once

// Raw bit extraction and two's-complement sign extension.
//
// Transcribed from spec sections 7.1 and 7.2. The hand-computed vectors of
// spec section 7.3 are the oracle; the generated cantools matrix is the second
// one (research R-20). Neither function reads outside `bytes`, and neither ever
// shifts by 64 or more - UBSan observes both (invariant P-02).

#include <cstddef>
#include <cstdint>
#include <optional>

#include "tcu/dbc/dbc_types.hpp"

namespace tcu::dbc {

/// Intel (@1): raw bit i comes from payload bit `start + i`, bit 0 being the
/// LSB of byte 0. Motorola (@0): reading starts at `start`, which is the
/// signal's MSB, and the position after a p with `p % 8 == 0` is `p + 15`,
/// otherwise `p - 1`.
///
/// Returns no value for a null pointer, a width outside 1..64, or any position
/// at or beyond `bytes * 8`.
std::optional<std::uint64_t> extractRaw(const std::uint8_t* data, std::size_t bytes,
                                        std::size_t start, unsigned width,
                                        ByteOrder order) noexcept;

/// The mathematical two's-complement value of `bits` low bits of `raw`, for
/// every width 1..64. INT64_MIN is handled without undefined behaviour.
std::optional<std::int64_t> signExtend(std::uint64_t raw, unsigned bits) noexcept;

}  // namespace tcu::dbc
