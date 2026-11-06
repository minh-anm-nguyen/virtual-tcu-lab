#pragma once

// The immutable, versioned, hashed database a valid DBC file compiles into.
//
// Everything here is const after publication: the only way to obtain one is
// loadDbcFile/loadDbcBytes, which hand back a shared_ptr<const CompiledDbc>
// (invariant P-07). A reload builds a new one and swaps the pointer; nothing
// mutates a published database (research R-14).

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "tcu/dbc/dbc_types.hpp"
#include "tcu/dbc/frame_view.hpp"

namespace tcu::dbc {

/// Per-signal decode plan, computed once at load from an AST that already
/// passed every validation check - so decode performs no layout validation
/// (invariant P-01, ADR-02).
struct BitPlan {
  std::uint16_t signalIndex{0};
  std::uint32_t startBit{0};
  std::uint8_t width{0};
  ByteOrder byteOrder{ByteOrder::Intel};
  bool isSigned{false};
  double factor{1.0};
  double offset{0.0};
  /// Highest payload byte index this plan touches, for the cheap length check.
  std::uint8_t lastByteTouched{0};
  MuxRole muxRole{MuxRole::None};
  std::uint32_t muxValue{0};
};

/// Everything about a signal that is not needed to extract its bits.
struct SignalMeta {
  std::string name;
  std::string unit;
  double minimum{0.0};
  double maximum{0.0};
  /// Retained as metadata; enforced only for signals the configuration lists
  /// in range_enforcement (research R-04).
  bool isSigned{false};
  std::vector<std::pair<std::int64_t, std::string>> valueDescriptions;
  /// FNV-1a over the canonical semantic tuple of research R-09. Two signals
  /// with the same semantic hash decode the same physical quantity, so a reload
  /// may keep the cached last-good; a change means a different quantity.
  std::uint64_t semanticHash{0};
};

struct MessageKey {
  std::uint32_t busId{0};
  bool isExtended{false};
  std::uint32_t arbitrationId{0};

  bool operator==(const MessageKey& other) const noexcept {
    return busId == other.busId && isExtended == other.isExtended &&
           arbitrationId == other.arbitrationId;
  }
};

struct MessageKeyHash {
  std::size_t operator()(const MessageKey& key) const noexcept {
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](std::uint64_t value) {
      h ^= value;
      h *= 1099511628211ull;
    };
    mix(key.busId);
    mix(key.isExtended ? 1u : 0u);
    mix(key.arbitrationId);
    return static_cast<std::size_t>(h);
  }
};

struct CompiledMessage {
  MessageKey key;
  std::string name;
  std::uint8_t expectedLength{0};
  /// Present only for a multiplexed message.
  std::optional<BitPlan> selector;
  std::vector<BitPlan> unconditional;
  /// muxValue -> the plans active for that selector value.
  std::unordered_map<std::uint32_t, std::vector<BitPlan>> branches;
  std::vector<SignalMeta> signals;
};

struct DbcStats {
  std::size_t messages{0};
  std::size_t signals{0};
  std::size_t ignoredStatements{0};
  std::size_t warnings{0};
};

class CompiledDbc {
 public:
  CompiledDbc(std::uint64_t version, std::string hash, std::string sourcePath,
              std::unordered_map<MessageKey, CompiledMessage, MessageKeyHash> byKey,
              DbcStats stats);

  /// O(1), and never conflates Standard11 with Extended29 for the same number
  /// (invariant P-08, FR-07, B-07). Null when the message is unknown.
  const CompiledMessage* find(std::uint32_t busId, FrameFormat format,
                              std::uint32_t arbitrationId) const noexcept;

  const SignalMeta* findSignal(std::uint32_t busId, FrameFormat format,
                               std::uint32_t arbitrationId,
                               const std::string& signalName) const noexcept;

  std::uint64_t version() const noexcept { return version_; }
  const std::string& hash() const noexcept { return hash_; }
  const std::string& sourcePath() const noexcept { return sourcePath_; }
  const DbcStats& stats() const noexcept { return stats_; }

  /// Every message, in no guaranteed order. For diagnostics, the CLI and the
  /// cache's reload migration.
  const std::unordered_map<MessageKey, CompiledMessage, MessageKeyHash>& messages()
      const noexcept {
    return byKey_;
  }

 private:
  std::uint64_t version_;
  std::string hash_;
  std::string sourcePath_;
  std::unordered_map<MessageKey, CompiledMessage, MessageKeyHash> byKey_;
  DbcStats stats_;
};

}  // namespace tcu::dbc
