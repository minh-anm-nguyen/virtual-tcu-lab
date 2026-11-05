#pragma once

// The synthetic CAN frame and signal catalog: the ONE place an identifier, a
// byte layout, a range and a freshness deadline are written. Four consumers
// read it - FrameValidator checks flags, known ID and length; DemoSignalDecoder
// extracts and range-checks; SocketCanTransport builds its CAN_RAW_FILTER list;
// ConfigLoader validates that every configured signal agrees with it.
// contracts/frame_catalog.md holds the same rows for human readers, and
// frame_catalog_test.cpp asserts the two agree.
//
// SAFETY: every identifier here is invented for a lab. None of it is an OEM
// DBC and none of it may reach a physical vehicle (spec section 1, ADR-07).
//
// A change here is a spec change, not an implementation detail: project 06
// replaces this table with DBC-driven decoding (ADR-02).

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tcu::can {

enum class Direction { ToTcu, FromTcu };

/// How one signal is extracted from the 8 payload bytes.
enum class Decode {
  /// One bit: (data[byte_offset] >> bit_offset) & 1
  Bit,
  /// One whole byte: data[byte_offset]
  Byte,
  /// bit_width bits: (data[byte_offset] >> bit_offset) & ((1 << bit_width) - 1)
  BitMask,
  /// Little-endian uint16 at byte_offset, DIVIDED by scale_divisor.
  ///
  /// A divisor rather than a 0.01 multiplier on purpose: raw / 100.0 is exact
  /// for every value in this catalog, whereas raw * 0.01 is only exact because
  /// the rounding happens to land correctly. The inclusive range checks sit
  /// right on these results - 250.00 and 6.00 are accepted values - so the
  /// arithmetic should not need an error analysis to be trusted.
  U16LeScaled,
};

struct FrameDefinition {
  std::uint32_t can_id;
  Direction direction;
  /// 8 for every V1 row. FR-06 rejects any other length for a known ID.
  std::uint8_t expected_length;
  /// Per-byte bitmask of the reserved bits. Nonzero reserved bits are counted,
  /// never rejected, in V1 (research R-15).
  std::array<std::uint8_t, 8> reserved_mask;
};

struct SignalDefinition {
  /// Stable: this is the key of readSignal() and of the event payload.
  std::string_view name;
  std::uint32_t source_id;
  Decode decode;
  std::uint8_t byte_offset;
  std::uint8_t bit_offset;
  std::uint8_t bit_width;
  /// U16LeScaled only; 1.0 for the other kinds. See Decode::U16LeScaled.
  double scale_divisor;
  /// Inclusive bounds, checked on the DECODED PHYSICAL VALUE before any cache
  /// commit (spec section 13.2 step 5).
  double min;
  double max;
  std::chrono::milliseconds timeout;
};

// Spec section 6.1, in identifier order. 7 rows: 6 ToTcu, 1 FromTcu.
inline constexpr std::array<FrameDefinition, 7> kFrames{{
    {0x100, Direction::ToTcu,   8, {0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}},
    {0x101, Direction::ToTcu,   8, {0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}},
    {0x102, Direction::ToTcu,   8, {0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}},
    {0x103, Direction::ToTcu,   8, {0xE0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}},
    {0x110, Direction::ToTcu,   8, {0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}},
    {0x200, Direction::FromTcu, 8, {0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}},
    {0x201, Direction::ToTcu,   8, {0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}},
}};

// Spec section 6.3. Ranges and timeouts are LAB EXAMPLE VALUES, not production
// requirements. `ecu_status` is not in the section 22 sample configuration; it
// exists because sections 6.1 and 6.3 both define byte 1 of 0x110 as a decoded
// field with its own range, and a status that cannot be read cannot drive the
// "ECU DEGRADED" observation section 4 expects (recorded in plan.md Complexity
// Tracking).
//
// battery_voltage_v + battery_soc_pct share 0x102, and heartbeat + ecu_status
// share 0x110. Both pairs are governed by atomic frame reject: if either field
// is out of range, NEITHER is committed (research R-14).
inline constexpr std::array<SignalDefinition, 7> kSignals{{
    {"ignition",          0x100, Decode::Bit,         0, 0, 1, 1.0,
     0.0, 1.0,     std::chrono::milliseconds{3000}},
    {"speed_kmh",         0x101, Decode::U16LeScaled, 0, 0, 0, 100.0,
     0.0, 250.0,   std::chrono::milliseconds{500}},
    {"battery_voltage_v", 0x102, Decode::U16LeScaled, 0, 0, 0, 100.0,
     6.0, 18.0,    std::chrono::milliseconds{3000}},
    {"battery_soc_pct",   0x102, Decode::Byte,        2, 0, 0, 1.0,
     0.0, 100.0,   std::chrono::milliseconds{3000}},
    {"door_state",        0x103, Decode::BitMask,     0, 0, 5, 1.0,
     0.0, 31.0,    std::chrono::milliseconds{3000}},
    {"heartbeat",         0x110, Decode::Byte,        0, 0, 0, 1.0,
     0.0, 255.0,   std::chrono::milliseconds{3000}},
    {"ecu_status",        0x110, Decode::Byte,        1, 0, 0, 1.0,
     0.0, 1.0,     std::chrono::milliseconds{3000}},
}};

inline constexpr std::size_t kFrameCount = kFrames.size();
inline constexpr std::size_t kSignalCount = kSignals.size();

/// The 11-bit identifier mask. Declared here rather than taken from
/// <linux/can.h> so this header stays OS-neutral (NFR-10, research R-02);
/// socketcan_transport.cpp static_asserts it equals CAN_SFF_MASK.
inline constexpr std::uint32_t kStandardIdMask = 0x7FFu;

inline constexpr const FrameDefinition* findFrame(std::uint32_t can_id) noexcept {
  for (const auto& frame : kFrames) {
    if (frame.can_id == can_id) {
      return &frame;
    }
  }
  return nullptr;
}

inline constexpr const SignalDefinition* findSignal(std::string_view name) noexcept {
  for (const auto& signal : kSignals) {
    if (signal.name == name) {
      return &signal;
    }
  }
  return nullptr;
}

inline constexpr bool isKnownFrame(std::uint32_t can_id) noexcept {
  return findFrame(can_id) != nullptr;
}

/// The identifiers the TCU receive socket filters on: every ToTcu row
/// (contracts/frame_catalog.md section 4).
inline constexpr std::array<std::uint32_t, 6> kTcuRxFilterIds{
    {0x100, 0x101, 0x102, 0x103, 0x110, 0x201}};

/// The identifier the BCM simulator filters on.
inline constexpr std::uint32_t kBcmRxFilterId = 0x200;

// Command bytes of 0x200 and result bytes of 0x201 (spec section 6.1).
inline constexpr std::uint8_t kCommandLock = 0x01;
inline constexpr std::uint8_t kCommandUnlock = 0x02;
inline constexpr std::uint8_t kResultOk = 0x00;
inline constexpr std::uint8_t kResultRejected = 0x01;
inline constexpr std::uint8_t kResultInvalid = 0x02;

}  // namespace tcu::can
