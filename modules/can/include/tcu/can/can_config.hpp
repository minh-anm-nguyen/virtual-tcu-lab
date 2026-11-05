#pragma once

// The loaded, validated, immutable lab configuration.
//
// Read once at startup, before any socket or thread exists (spec section 13.1
// step 2). There is no hot reload in V1: no functional requirement asks for
// one, and research R-06 explains why this project does not route the document
// through project 04's tcu_config.
//
// Key table, ranges and error semantics: contracts/config_schema.md.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace tcu::can {

/// The only value V1 parses. "unrestricted" does not exist, so there is no
/// runtime path to a physical interface at all - the strongest form of the
/// section 24 mitigation "only vcan* interfaces accepted by default" (R-16).
enum class LabMode { LabOnly };

struct SignalConfig {
  std::uint32_t can_id{0};
  std::chrono::milliseconds timeout{0};
};

struct SimulatorConfig {
  std::chrono::milliseconds speed_period{0};
  std::chrono::milliseconds heartbeat_period{0};
  /// Seeds std::mt19937 so a scenario replay produces the same value sequence
  /// per identifier (NFR-09, B-29, research R-21).
  std::uint32_t fixed_seed{0};
};

struct CanLabConfig {
  std::int64_t schema_version{1};
  LabMode mode{LabMode::LabOnly};
  std::string interface_name;
  std::size_t rx_queue_capacity{0};
  std::chrono::milliseconds health_check_period{0};
  std::chrono::milliseconds drain_deadline{0};
  std::chrono::milliseconds reconnect_initial{0};
  std::chrono::milliseconds reconnect_max{0};
  int reconnect_jitter_pct{0};
  std::vector<std::uint32_t> allowed_rx_ids;
  /// Keyed by signal name. Every name is a frame_catalog.hpp row, and every
  /// can_id agrees with that row: the catalog, not the config, decides where a
  /// signal lives.
  std::map<std::string, SignalConfig> signals;
  SimulatorConfig simulator;
};

enum class LoadError {
  None,
  FileNotFound,
  ReadError,
  /// The file exceeded the size cap, so it was never parsed.
  TooLarge,
  ParseError,
  /// Unknown key, missing key, or wrong JSON type.
  SchemaError,
  /// Out of range, or a cross-field rule failed.
  InvalidValue,
  /// schema_version is not 1. Distinct from InvalidValue because an operator
  /// handed a future file needs to know that specifically.
  UnsupportedVersion,
};

inline constexpr std::size_t kLoadErrorCount = 8;

inline const char* loadErrorName(LoadError error) noexcept {
  switch (error) {
    case LoadError::None:               return "None";
    case LoadError::FileNotFound:       return "FileNotFound";
    case LoadError::ReadError:          return "ReadError";
    case LoadError::TooLarge:           return "TooLarge";
    case LoadError::ParseError:         return "ParseError";
    case LoadError::SchemaError:        return "SchemaError";
    case LoadError::InvalidValue:       return "InvalidValue";
    case LoadError::UnsupportedVersion: return "UnsupportedVersion";
  }
  return "None";
}

struct LoadResult {
  LoadError error{LoadError::None};
  /// RFC 6901, e.g. "/can/rx_queue_capacity". Empty for a whole-file failure.
  std::string pointer;
  /// From a closed template set, NEVER the offending value: an error message
  /// becomes a log line, and a log line must not carry unvalidated input
  /// (Constitution Principle V, project 04's R-10).
  std::string publicReason;

  explicit operator bool() const { return error == LoadError::None; }
};

/// The file size cap. 256 KiB is far above any legitimate lab configuration and
/// far below anything that could exhaust memory.
inline constexpr std::size_t kMaxConfigBytes = 256u * 1024u;

/// Loads and validates. On failure the output config is untouched and the
/// caller must not start anything (spec section 12.1 policy).
LoadResult loadCanLabConfig(const std::string& path, CanLabConfig& out);

/// Same validation, from a literal document. Used by the tests, and by any
/// caller that already holds the text.
LoadResult parseCanLabConfig(const std::string& text, CanLabConfig& out);

}  // namespace tcu::can
