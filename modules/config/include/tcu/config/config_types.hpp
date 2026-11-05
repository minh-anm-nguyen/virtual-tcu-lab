#pragma once

// Public vocabulary of the configuration library.
//
// Fixed by spec section 8 and contracts/config_api.md section 1. TooLarge and
// UnsupportedVersion extend the enum of section 8 because section 13 requires
// both to be distinguishable (BC-15, BC-28); both are recorded in plan.md
// Complexity Tracking. Adding a further value is a spec change.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "tcu/logging/log_types.hpp"

namespace tcu::config {

enum class LoadError {
  None,
  FileNotFound,
  ReadError,
  TooLarge,
  ParseError,
  SchemaError,
  InvalidValue,
  RestartRequired,
  UnsupportedVersion,
  ServiceStopped,
};

inline constexpr std::size_t kLoadErrorCount = 10;

// The metric label set. None never appears as a failure label.
inline std::string_view loadErrorName(LoadError error) noexcept {
  switch (error) {
    case LoadError::None:               return "None";
    case LoadError::FileNotFound:       return "FileNotFound";
    case LoadError::ReadError:          return "ReadError";
    case LoadError::TooLarge:           return "TooLarge";
    case LoadError::ParseError:         return "ParseError";
    case LoadError::SchemaError:        return "SchemaError";
    case LoadError::InvalidValue:       return "InvalidValue";
    case LoadError::RestartRequired:    return "RestartRequired";
    case LoadError::UnsupportedVersion: return "UnsupportedVersion";
    case LoadError::ServiceStopped:     return "ServiceStopped";
  }
  return "None";
}

inline std::size_t loadErrorIndex(LoadError error) noexcept {
  return static_cast<std::size_t>(error);
}

struct ReloadResult {
  LoadError error{LoadError::None};
  // The generation in force after the call, committed or kept.
  std::uint64_t generation{0};
  bool changed{false};
  // Redacted, safe for logs: built only from the closed template set of
  // contracts/config_schema.md section 4 (research R-10).
  std::string publicReason;

  explicit operator bool() const { return error == LoadError::None; }
};

// Integer fields are int64 so a range check happens on the parsed value rather
// than after a narrowing conversion.
struct ConnectivityConfig {
  std::int64_t retryBaseMs{0};
  std::int64_t retryMaxMs{0};
  std::int64_t maxAttempts{0};

  bool operator==(const ConnectivityConfig& other) const {
    return retryBaseMs == other.retryBaseMs && retryMaxMs == other.retryMaxMs &&
           maxAttempts == other.maxAttempts;
  }
};

struct TelemetryConfig {
  std::int64_t periodMs{0};
  std::int64_t offlineQueueCapacity{0};

  bool operator==(const TelemetryConfig& other) const {
    return periodMs == other.periodMs &&
           offlineQueueCapacity == other.offlineQueueCapacity;
  }
};

struct LoggingConfig {
  tcu::logging::Severity level{tcu::logging::Severity::Info};
  std::int64_t queueCapacity{0};
  std::int64_t fileMaxBytes{0};
  std::int64_t fileCount{0};
  bool consoleEnabled{true};

  bool operator==(const LoggingConfig& other) const {
    return level == other.level && queueCapacity == other.queueCapacity &&
           fileMaxBytes == other.fileMaxBytes && fileCount == other.fileCount &&
           consoleEnabled == other.consoleEnabled;
  }
};

struct NetworkConfig {
  std::string cloudEndpoint;
  bool tlsRequired{true};

  bool operator==(const NetworkConfig& other) const {
    return cloudEndpoint == other.cloudEndpoint &&
           tlsRequired == other.tlsRequired;
  }
};

// The immutable unit of consistency (spec section 7.3). Published through one
// atomic store; never mutated afterwards (invariant C1).
struct ConfigSnapshot {
  std::uint64_t generation{0};
  std::string source;
  std::chrono::system_clock::time_point loadedAt{};
  ConnectivityConfig connectivity;
  TelemetryConfig telemetry;
  LoggingConfig logging;
  NetworkConfig network;

  // Semantic equality: compares every field EXCEPT generation, source and
  // loadedAt (invariant C5). Two files differing only in whitespace, key order
  // or integer spelling therefore compare equal (invariant C3), which is what
  // makes a no-op reload detectable without comparing JSON text.
  bool contentEquals(const ConfigSnapshot& other) const {
    return connectivity == other.connectivity && telemetry == other.telemetry &&
           logging == other.logging && network == other.network;
  }
};

using ConfigPtr = std::shared_ptr<const ConfigSnapshot>;

// Strictly increasing and never reused. 0 is never issued.
using SubscriptionId = std::uint64_t;

// previous is null for the first notification after startup and for a resync
// after notification overflow (invariant N5, research R-06).
using ConfigObserver = std::function<void(ConfigPtr current, ConfigPtr previous)>;

}  // namespace tcu::config
