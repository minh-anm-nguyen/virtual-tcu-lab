#include "schema_validator.hpp"

#include <cmath>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "bit_span.hpp"

namespace tcu::dbc::detail {
namespace {

constexpr std::uint32_t kDbcExtendedFlag = 0x80000000u;
constexpr std::uint32_t kStandardIdMax = 0x7FFu;
constexpr std::uint32_t kExtendedIdMax = 0x1FFFFFFFu;

struct Reporter {
  std::string_view file;
  std::vector<ParseDiagnostic>& diagnostics;
  bool failed{false};

  void operator()(std::uint32_t line, std::uint32_t column, DbcErrorCode code,
                  std::string message) {
    failed = true;
    diagnostics.push_back(ParseDiagnostic{std::string(file), line, column, code,
                                          Severity::Error, std::move(message)});
  }
};

/// Spec section 6.1: bit 31 of the BO_ identifier carries the extended flag and
/// the 29-bit identifier sits underneath. Nothing is truncated silently - a
/// reserved bit or an out-of-range identifier is DBC_MESSAGE (invariant D-04).
bool resolveIdentifier(double rawValue, std::uint32_t& arbitrationId, bool& isExtended,
                       std::string& problem) {
  if (!std::isfinite(rawValue) || rawValue < 0 || rawValue > 4294967295.0) {
    problem = "identifier is outside the 32-bit range";
    return false;
  }
  const std::uint32_t raw = static_cast<std::uint32_t>(rawValue);
  isExtended = (raw & kDbcExtendedFlag) != 0u;
  if (isExtended) {
    if ((raw & ~(kDbcExtendedFlag | kExtendedIdMax)) != 0u) {
      problem = "extended identifier has reserved bits set";
      return false;
    }
    arbitrationId = raw & kExtendedIdMax;
  } else {
    if (raw > kStandardIdMax) {
      problem = "standard identifier is above 0x7FF; set bit 31 for an extended identifier";
      return false;
    }
    arbitrationId = raw;
  }
  return true;
}

std::string clip(const std::string& text) { return text.substr(0, 64); }


bool checkOverlap(const RawSignal& signal, const std::vector<std::uint32_t>& positions,
                  std::set<std::uint32_t>& unconditionalBits,
                  std::unordered_map<std::uint32_t, std::set<std::uint32_t>>& branchBits,
                  Reporter& report);
void attachValueTables(ParsedFile& file, Reporter& report);

}  // namespace

bool validate(ParsedFile& file, std::string_view fileLabel, const ParseLimits& limits,
              std::vector<ParseDiagnostic>& diagnostics) {
  Reporter report{fileLabel, diagnostics, false};

  std::size_t totalSignals = 0;
  std::unordered_set<std::uint64_t> seenKeys;
  std::unordered_set<std::string> seenNames;

  for (auto& message : file.messages) {
    std::string problem;
    if (!resolveIdentifier(message.rawId, message.arbitrationId, message.isExtended, problem)) {
      report(message.line, message.column, DbcErrorCode::DbcMessage, problem);
      continue;
    }
    if (!std::isfinite(message.length) || message.length < 0 ||
        message.length > static_cast<double>(limits.maxMessageLength)) {
      report(message.line, message.column, DbcErrorCode::DbcMessage,
             "payload length must be 0.." + std::to_string(limits.maxMessageLength) +
                 " bytes for classical CAN in v1");
      continue;
    }
    const std::uint8_t length = static_cast<std::uint8_t>(message.length);

    const std::uint64_t key =
        (static_cast<std::uint64_t>(message.isExtended ? 1u : 0u) << 32) | message.arbitrationId;
    if (!seenKeys.insert(key).second) {
      report(message.line, message.column, DbcErrorCode::DbcMessage,
             "duplicate message: this frame format and identifier are already defined");
      continue;
    }
    if (!seenNames.insert(message.name).second) {
      report(message.line, message.column, DbcErrorCode::DbcMessage,
             "duplicate message name '" + clip(message.name) + "'");
      continue;
    }

    std::unordered_set<std::string> signalNames;
    std::size_t selectors = 0;
    std::set<std::uint32_t> unconditionalBits;
    std::unordered_map<std::uint32_t, std::set<std::uint32_t>> branchBits;

    for (auto& signal : message.signals) {
      if (++totalSignals > limits.maxTotalSignals) {
        report(signal.line, signal.column, DbcErrorCode::DbcLimit, "max_total_signals exceeded");
        break;
      }
      if (!signalNames.insert(signal.name).second) {
        report(signal.line, signal.column, DbcErrorCode::DbcSignal,
               "duplicate signal name '" + clip(signal.name) + "' in this message");
        continue;
      }
      if (!std::isfinite(signal.width) || signal.width < 1 || signal.width > 64) {
        report(signal.line, signal.column, DbcErrorCode::DbcSignal,
               "signal width must be 1..64 bits");
        continue;
      }
      if (!std::isfinite(signal.startBit) || signal.startBit < 0 || signal.startBit > 511) {
        report(signal.line, signal.column, DbcErrorCode::DbcSignal,
               "start bit is outside any possible payload");
        continue;
      }
      if (!std::isfinite(signal.factor) || signal.factor == 0.0) {
        report(signal.line, signal.column, DbcErrorCode::DbcRange,
               "factor must be finite and non-zero");
        continue;
      }
      if (!std::isfinite(signal.offset)) {
        report(signal.line, signal.column, DbcErrorCode::DbcRange, "offset must be finite");
        continue;
      }
      if (!std::isfinite(signal.minimum) || !std::isfinite(signal.maximum)) {
        report(signal.line, signal.column, DbcErrorCode::DbcRange, "range bounds must be finite");
        continue;
      }
      if (signal.minimum > signal.maximum) {
        report(signal.line, signal.column, DbcErrorCode::DbcRange,
               "minimum is greater than maximum");
        continue;
      }
      if (signal.muxRole == MuxRole::Selector && ++selectors > 1) {
        report(signal.line, signal.column, DbcErrorCode::DbcMux,
               "a message may declare at most one multiplex selector in v1");
        continue;
      }

      std::vector<std::uint32_t> positions;
      if (!enumeratePositions(static_cast<std::uint32_t>(signal.startBit),
                              static_cast<std::uint8_t>(signal.width), signal.byteOrder, length,
                              positions)) {
        report(signal.line, signal.column, DbcErrorCode::DbcOutOfBounds,
               "signal '" + clip(signal.name) + "' needs bits outside the declared " +
                   std::to_string(length) + "-byte payload");
        continue;
      }
      if (!checkOverlap(signal, positions, unconditionalBits, branchBits, report)) continue;
    }

    if (selectors == 0) {
      for (const auto& signal : message.signals) {
        if (signal.muxRole != MuxRole::Branch) continue;
        report(signal.line, signal.column, DbcErrorCode::DbcMux,
               "signal '" + clip(signal.name) +
                   "' declares a multiplex branch but the message has no selector");
      }
    }
  }

  attachValueTables(file, report);
  return !report.failed;
}


namespace {

/// Invariant D-09: two signals overlap illegally only when they can be active
/// in the same frame. Two different mux branches may share bits (B-19); the
/// same branch, two unconditionals, or an unconditional against any branch may
/// not (B-18).
bool checkOverlap(const RawSignal& signal, const std::vector<std::uint32_t>& positions,
                  std::set<std::uint32_t>& unconditionalBits,
                  std::unordered_map<std::uint32_t, std::set<std::uint32_t>>& branchBits,
                  Reporter& report) {
  const bool isBranch = signal.muxRole == MuxRole::Branch;
  std::set<std::uint32_t>& own = isBranch ? branchBits[signal.muxValue] : unconditionalBits;

  for (const std::uint32_t position : positions) {
    if (own.count(position) != 0u) {
      report(signal.line, signal.column, DbcErrorCode::DbcOverlap,
             "signal '" + clip(signal.name) +
                 "' shares bits with another signal active in the same frame");
      return false;
    }
    if (isBranch && unconditionalBits.count(position) != 0u) {
      report(signal.line, signal.column, DbcErrorCode::DbcOverlap,
             "branch signal '" + clip(signal.name) +
                 "' shares bits with an unconditional signal");
      return false;
    }
  }
  if (!isBranch) {
    for (const auto& branch : branchBits) {
      for (const std::uint32_t position : positions) {
        if (branch.second.count(position) == 0u) continue;
        report(signal.line, signal.column, DbcErrorCode::DbcOverlap,
               "unconditional signal '" + clip(signal.name) +
                   "' shares bits with a branch signal");
        return false;
      }
    }
  }
  own.insert(positions.begin(), positions.end());
  return true;
}

/// VAL_ is attached only after the messages are known, so an enum naming a
/// signal that does not exist is reported at the VAL_ line (invariant D-11).
void attachValueTables(ParsedFile& file, Reporter& report) {
  for (const auto& table : file.valueTables) {
    std::uint32_t arbitrationId = 0;
    bool isExtended = false;
    std::string problem;
    if (!resolveIdentifier(table.rawId, arbitrationId, isExtended, problem)) {
      report(table.line, table.column, DbcErrorCode::DbcEnum,
             "VAL_ identifier is not a valid message identifier");
      continue;
    }
    RawSignal* target = nullptr;
    for (auto& message : file.messages) {
      if (message.arbitrationId != arbitrationId || message.isExtended != isExtended) continue;
      for (auto& signal : message.signals) {
        if (signal.name == table.signalName) target = &signal;
      }
    }
    if (target == nullptr) {
      report(table.line, table.column, DbcErrorCode::DbcEnum,
             "VAL_ names a signal that this database does not define");
      continue;
    }
    std::unordered_set<std::int64_t> numbers;
    std::unordered_set<std::string> labels;
    bool bad = false;
    for (const auto& pair : table.pairs) {
      if (!numbers.insert(pair.first).second) {
        report(table.line, table.column, DbcErrorCode::DbcEnum,
               "duplicate enum value in VAL_ for signal '" + clip(table.signalName) + "'");
        bad = true;
        break;
      }
      if (!labels.insert(pair.second).second) {
        report(table.line, table.column, DbcErrorCode::DbcEnum,
               "duplicate enum label in VAL_ for signal '" + clip(table.signalName) + "'");
        bad = true;
        break;
      }
    }
    if (!bad) target->valueDescriptions = table.pairs;
  }
}

}  // namespace

}  // namespace tcu::dbc::detail
