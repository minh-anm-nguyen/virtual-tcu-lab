// T027: two's-complement for every width 1..64, including the INT64_MIN edge
// that a naive negate would turn into undefined behaviour (invariant P-04).

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "tcu/dbc/bit_extractor.hpp"

namespace {

using tcu::dbc::signExtend;

TEST(SignExtend_OneBitSignedIsMinusOne, SpecSection72) {
  const auto value = signExtend(1u, 1);
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, -1);
  const auto zero = signExtend(0u, 1);
  ASSERT_TRUE(zero.has_value());
  EXPECT_EQ(*zero, 0);
}

TEST(SignExtend_TwelveBitAbc, SpecSection72) {
  const auto value = signExtend(0xABCu, 12);
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, -1348);
  EXPECT_DOUBLE_EQ(static_cast<double>(*value) * 0.5, -674.0);
}

TEST(SignExtend_TwoComplementAllWidths, BoundariesForEveryWidth) {
  for (unsigned bits = 1; bits <= 64; ++bits) {
    const std::uint64_t mask =
        (bits == 64) ? std::numeric_limits<std::uint64_t>::max() : ((std::uint64_t{1} << bits) - 1u);
    const std::uint64_t signBit = std::uint64_t{1} << (bits - 1u);

    // Largest positive: sign bit clear, everything below set.
    const auto maxPositive = signExtend(signBit - 1u, bits);
    ASSERT_TRUE(maxPositive.has_value()) << bits;
    EXPECT_EQ(*maxPositive, static_cast<std::int64_t>(signBit - 1u)) << "width " << bits;

    // All ones is -1 at every width.
    const auto minusOne = signExtend(mask, bits);
    ASSERT_TRUE(minusOne.has_value()) << bits;
    EXPECT_EQ(*minusOne, -1) << "width " << bits;

    // Most negative: only the sign bit set.
    const auto mostNegative = signExtend(signBit, bits);
    ASSERT_TRUE(mostNegative.has_value()) << bits;
    if (bits == 64) {
      EXPECT_EQ(*mostNegative, std::numeric_limits<std::int64_t>::min()) << "width " << bits;
    } else {
      EXPECT_EQ(*mostNegative, -static_cast<std::int64_t>(signBit)) << "width " << bits;
    }

    // Zero is zero.
    const auto zero = signExtend(0u, bits);
    ASSERT_TRUE(zero.has_value()) << bits;
    EXPECT_EQ(*zero, 0) << "width " << bits;
  }
}

TEST(SignExtend_TwoComplementAllWidths, IgnoresBitsAboveTheWidth) {
  // Whatever junk sits above the declared width must not reach the result.
  EXPECT_EQ(*signExtend(0xFFFFFFFFFFFFFF01ull, 8), 1);
  EXPECT_EQ(*signExtend(0xFFFFFFFFFFFFFF81ull, 8), -127);
}

TEST(SignExtend_Int64MinNoUb, Width64MostNegative) {
  const auto value = signExtend(std::uint64_t{1} << 63, 64);
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, std::numeric_limits<std::int64_t>::min());
  // And the magnitude path did not overflow on the way there: UBSan is the
  // other half of this assertion (NFR-01).
  EXPECT_LT(*value, 0);
}

TEST(SignExtend_Int64MinNoUb, RejectsImpossibleWidths) {
  EXPECT_FALSE(signExtend(0u, 0).has_value());
  EXPECT_FALSE(signExtend(0u, 65).has_value());
}

}  // namespace
