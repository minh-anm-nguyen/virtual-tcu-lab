// The catalog is the single source of truth for four consumers, so these tests
// assert its internal consistency and that it still agrees with
// contracts/frame_catalog.md. A silent edit here would otherwise surface as a
// decoder bug much later.

#include "tcu/can/frame_catalog.hpp"

#include <gtest/gtest.h>

#include <set>
#include <string_view>

namespace {

using namespace tcu::can;

TEST(Catalog, EverySignalIdIsAFrameRow) {
  for (const auto& signal : kSignals) {
    EXPECT_NE(findFrame(signal.source_id), nullptr)
        << "signal " << signal.name << " names identifier 0x" << std::hex
        << signal.source_id << " which is not in kFrames";
  }
}

TEST(Catalog, EveryFrameExpectsEightBytes) {
  // Spec section 6: classical CAN, 8 data bytes per frame in the V1 demo.
  for (const auto& frame : kFrames) {
    EXPECT_EQ(frame.expected_length, 8) << "identifier 0x" << std::hex << frame.can_id;
  }
}

TEST(Catalog, EveryFrameIdIsStandardElevenBit) {
  for (const auto& frame : kFrames) {
    EXPECT_LE(frame.can_id, kStandardIdMask) << "identifier 0x" << std::hex << frame.can_id;
  }
}

TEST(Catalog, FrameIdsAreUnique) {
  std::set<std::uint32_t> seen;
  for (const auto& frame : kFrames) {
    EXPECT_TRUE(seen.insert(frame.can_id).second)
        << "identifier 0x" << std::hex << frame.can_id << " appears twice";
  }
}

TEST(Catalog, SignalNamesAreUnique) {
  std::set<std::string_view> seen;
  for (const auto& signal : kSignals) {
    EXPECT_TRUE(seen.insert(signal.name).second) << signal.name << " appears twice";
  }
}

TEST(Catalog, RangesAreNonEmptyAndOrdered) {
  for (const auto& signal : kSignals) {
    EXPECT_LE(signal.min, signal.max) << signal.name;
  }
}

TEST(Catalog, TimeoutsArePositive) {
  // A nonpositive deadline would make a signal stale the instant it arrived.
  for (const auto& signal : kSignals) {
    EXPECT_GT(signal.timeout.count(), 0) << signal.name;
  }
}

TEST(Catalog, DecodeDescriptorsStayInsideThePayload) {
  for (const auto& signal : kSignals) {
    const std::size_t needed =
        signal.decode == Decode::U16LeScaled ? signal.byte_offset + 2u : signal.byte_offset + 1u;
    EXPECT_LE(needed, 8u) << signal.name << " reads past byte 7";
    if (signal.decode == Decode::Bit || signal.decode == Decode::BitMask) {
      EXPECT_LT(signal.bit_offset + signal.bit_width, 9u) << signal.name;
    }
  }
}

// contracts/frame_catalog.md section 1. Spot-checked rather than re-listed:
// these four rows are the ones whose reserved bits are not simply "all bytes
// after the used ones", so they are where a transcription error would hide.
TEST(Catalog, ReservedMaskMatchesContract) {
  EXPECT_EQ(findFrame(0x100)->reserved_mask[0], 0xFE);  // byte 0 bits 1-7
  EXPECT_EQ(findFrame(0x101)->reserved_mask[0], 0x00);  // bytes 0-1 carry speed
  EXPECT_EQ(findFrame(0x101)->reserved_mask[2], 0xFF);
  EXPECT_EQ(findFrame(0x102)->reserved_mask[2], 0x00);  // byte 2 carries SOC
  EXPECT_EQ(findFrame(0x102)->reserved_mask[3], 0xFF);
  EXPECT_EQ(findFrame(0x103)->reserved_mask[0], 0xE0);  // byte 0 bits 5-7
}

TEST(Catalog, TcuFilterCoversEveryToTcuFrame) {
  // A ToTcu frame missing from the filter list would be dropped by the kernel
  // and never decoded (FR-04).
  for (const auto& frame : kFrames) {
    if (frame.direction != Direction::ToTcu) {
      continue;
    }
    bool filtered = false;
    for (const auto id : kTcuRxFilterIds) {
      filtered = filtered || id == frame.can_id;
    }
    EXPECT_TRUE(filtered) << "identifier 0x" << std::hex << frame.can_id
                          << " is ToTcu but absent from kTcuRxFilterIds";
  }
}

TEST(Catalog, OnlyTheCommandFrameIsFromTcu) {
  // Spec section 6.1: 0x200 is the only frame the TCU sends.
  for (const auto& frame : kFrames) {
    if (frame.can_id == 0x200) {
      EXPECT_EQ(frame.direction, Direction::FromTcu);
    } else {
      EXPECT_EQ(frame.direction, Direction::ToTcu) << "identifier 0x" << std::hex << frame.can_id;
    }
  }
  EXPECT_EQ(kBcmRxFilterId, 0x200u);
}

TEST(Catalog, FindSignalReturnsNullForUnknownName) {
  EXPECT_EQ(findSignal("not_a_signal"), nullptr);
  EXPECT_NE(findSignal("speed_kmh"), nullptr);
}

// The seven rows contracts/frame_catalog.md and config_schema.md both name.
TEST(Catalog, HoldsTheSevenDocumentedSignals) {
  EXPECT_EQ(kSignalCount, 7u);
  for (const auto* name : {"ignition", "speed_kmh", "battery_voltage_v", "battery_soc_pct",
                           "door_state", "heartbeat", "ecu_status"}) {
    EXPECT_NE(findSignal(name), nullptr) << name;
  }
}

TEST(Catalog, SpeedHasTheShortestDeadline) {
  // Spec section 6.3: speed is the only 500 ms signal; everything else is
  // 3000 ms. B-18 depends on exactly this asymmetry.
  const auto speed = findSignal("speed_kmh")->timeout;
  for (const auto& signal : kSignals) {
    if (signal.name == "speed_kmh") {
      continue;
    }
    EXPECT_GT(signal.timeout, speed) << signal.name;
  }
}

}  // namespace
