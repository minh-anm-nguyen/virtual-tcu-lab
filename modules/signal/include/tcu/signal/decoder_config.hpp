#pragma once

// The decoder.json sidecar.
//
// contracts/decoder_config.md section 1 is the schema; this header is its
// typed form. One table, three consumers - the validator checks presence, type
// and range, the loader fills DecoderConfig, and a test asserts that the
// shipped configs/decoder.json agrees with the table (invariant V-14).
//
// This is NOT loaded through tcu_config: that library's schema table and
// snapshot ARE the TCU application's configuration, not a generic engine, and
// spec section 17 defines this sidecar as separate (research R-16).

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "tcu/dbc/dbc_parser.hpp"
#include "tcu/dbc/signal_decoder.hpp"
#include "tcu/signal/signal_cache.hpp"

namespace tcu::signal {

enum class ConfigErrorKind {
  MissingKey,
  UnknownKey,
  WrongType,
  OutOfRange,
  UnsupportedValue,
  NotAnObject,
  ParseError,
  TooLarge,
};

const char* toString(ConfigErrorKind kind) noexcept;

/// Names the JSON pointer and the kind, never the value (invariant V-17,
/// Constitution Principle V).
struct ConfigError {
  std::string pointer;
  ConfigErrorKind kind{ConfigErrorKind::ParseError};
  /// Only for OutOfRange and UnsupportedValue: what WAS allowed, never what
  /// was supplied.
  std::string allowed;
};

enum class EventPublishMode { Sync, Async };
enum class QueueOverflowPolicy { DropAndCount, BlockBounded };

struct ReloadConfig {
  bool enabled{true};
  std::chrono::milliseconds debounce{300};
  std::uint32_t maxAttemptsPerMinute{6};
};

struct DecoderConfig {
  std::uint32_t schemaVersion{1};
  std::string dbcPath;
  bool strict{true};
  std::size_t maxDbcBytes{2u * 1024u * 1024u};
  /// classical_can only in v1 (research R-01).
  std::string frameMode{"classical_can"};
  /// exact only in v1.
  std::string payloadLengthPolicy{"exact"};
  std::size_t rxQueueCapacity{4096};
  std::uint32_t decoderWorkers{1};
  ReloadConfig reload;
  FreshnessConfig freshness;
  /// How often the freshness scan runs. Bounds stale-event latency, never
  /// the answer to isFresh() (research R-06).
  std::chrono::milliseconds scanPeriod{50};
  /// Raw, pre-scaling values keyed by "MESSAGE.Signal" (research R-05).
  dbc::InvalidRawTable invalidRawCodes;
  dbc::RangeEnforcement rangeEnforcement;
  EventPublishMode publishMode{EventPublishMode::Async};
  QueueOverflowPolicy onPublishFull{QueueOverflowPolicy::DropAndCount};
  std::uint32_t logRateLimitPerReasonPerMinute{60};

  /// The parse limits the DBC loader should use, derived from this config.
  dbc::ParseLimits parseLimits() const;
};

struct ConfigLoadResult {
  bool ok{false};
  DecoderConfig config;
  /// Every problem in the file, not only the first.
  std::vector<ConfigError> errors;
};

ConfigLoadResult loadDecoderConfig(const std::string& path);
ConfigLoadResult parseDecoderConfig(const std::string& text, const std::string& label);

/// Keys of invalid_raw_codes, range_enforcement and freshness.overrides that
/// match no signal in the loaded database. A typo there would silently disable
/// a check, so the caller warns and counts rather than ignoring it (V-16).
std::vector<std::string> unmatchedPolicyKeys(const DecoderConfig& config,
                                             const dbc::CompiledDbc& db);

}  // namespace tcu::signal
