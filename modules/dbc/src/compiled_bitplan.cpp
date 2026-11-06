#include "compiled_bitplan.hpp"

#include <cstring>
#include <utility>

#include "bit_span.hpp"

namespace tcu::dbc::detail {
namespace {

/// FNV-1a over the canonical semantic tuple of research R-09. Two signals with
/// the same hash decode the same physical quantity from the same bits, so a
/// reload may keep the cached last-good; any difference means the number means
/// something else now, and the cache entry is reset rather than reinterpreted.
class SemanticHash {
 public:
  void mixU64(std::uint64_t value) {
    hash_ ^= value;
    hash_ *= 1099511628211ull;
  }
  void mixText(const std::string& text) {
    for (const char c : text) mixU64(static_cast<std::uint64_t>(static_cast<unsigned char>(c)));
    mixU64(0x1fu);  // field separator, so "ab"+"c" and "a"+"bc" differ
  }
  void mixDouble(double value) {
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "double is not 64-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    mixU64(bits);
  }
  std::uint64_t value() const { return hash_; }

 private:
  std::uint64_t hash_{1469598103934665603ull};
};

std::uint64_t semanticHashOf(const RawMessage& message, const RawSignal& signal) {
  SemanticHash hash;
  hash.mixU64(static_cast<std::uint64_t>(message.arbitrationId));
  hash.mixU64(message.isExtended ? 1ull : 0ull);
  hash.mixU64(0ull);  // busId: v1 loads one database for bus 0 (spec section 12 rule 4)
  hash.mixText(signal.name);
  hash.mixU64(static_cast<std::uint64_t>(signal.startBit));
  hash.mixU64(static_cast<std::uint64_t>(signal.width));
  hash.mixU64(signal.byteOrder == ByteOrder::Intel ? 1ull : 0ull);
  hash.mixU64(signal.isSigned ? 1ull : 0ull);
  hash.mixDouble(signal.factor);
  hash.mixDouble(signal.offset);
  hash.mixU64(static_cast<std::uint64_t>(signal.muxRole));
  hash.mixU64(static_cast<std::uint64_t>(signal.muxValue));
  hash.mixText(signal.unit);
  return hash.value();
}

}  // namespace

std::shared_ptr<const CompiledDbc> compile(const ParsedFile& file, std::uint64_t version,
                                           std::string hash, std::string sourcePath) {
  std::unordered_map<MessageKey, CompiledMessage, MessageKeyHash> byKey;
  DbcStats stats;
  stats.ignoredStatements = file.ignoredStatements;
  stats.warnings = file.warnings;

  for (const auto& message : file.messages) {
    CompiledMessage compiled;
    compiled.key = MessageKey{0u, message.isExtended, message.arbitrationId};
    compiled.name = message.name;
    compiled.expectedLength = static_cast<std::uint8_t>(message.length);
    compiled.signals.reserve(message.signals.size());

    for (const auto& signal : message.signals) {
      const auto index = static_cast<std::uint16_t>(compiled.signals.size());

      std::vector<std::uint32_t> positions;
      (void)enumeratePositions(static_cast<std::uint32_t>(signal.startBit),
                               static_cast<std::uint8_t>(signal.width), signal.byteOrder,
                               compiled.expectedLength, positions);

      BitPlan plan;
      plan.signalIndex = index;
      plan.startBit = static_cast<std::uint32_t>(signal.startBit);
      plan.width = static_cast<std::uint8_t>(signal.width);
      plan.byteOrder = signal.byteOrder;
      plan.isSigned = signal.isSigned;
      plan.factor = signal.factor;
      plan.offset = signal.offset;
      plan.lastByteTouched = lastByteTouched(positions);
      plan.muxRole = signal.muxRole;
      plan.muxValue = signal.muxValue;

      SignalMeta meta;
      meta.name = signal.name;
      meta.unit = signal.unit;
      meta.minimum = signal.minimum;
      meta.maximum = signal.maximum;
      meta.isSigned = signal.isSigned;
      meta.valueDescriptions = signal.valueDescriptions;
      meta.semanticHash = semanticHashOf(message, signal);
      compiled.signals.push_back(std::move(meta));

      switch (signal.muxRole) {
        case MuxRole::Selector:
          compiled.selector = plan;
          break;
        case MuxRole::Branch:
          compiled.branches[signal.muxValue].push_back(plan);
          break;
        case MuxRole::None:
          compiled.unconditional.push_back(plan);
          break;
      }
      ++stats.signals;
    }

    ++stats.messages;
    const MessageKey key = compiled.key;
    byKey.emplace(key, std::move(compiled));
  }

  return std::make_shared<const CompiledDbc>(version, std::move(hash), std::move(sourcePath),
                                             std::move(byKey), stats);
}

}  // namespace tcu::dbc::detail
