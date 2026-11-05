#pragma once

// The schema table: the ONE place a configuration key, its type, its bounds and
// its reload policy are written. Two consumers read it - the validator checks
// presence, type and range; ConfigLoader_SampleFileMatchesTable asserts
// config/can_lab.json agrees with it. contracts/config_schema.md section 1
// holds the same rows for human readers.
//
// Every key is mandatory. There is no defaults layer and an unknown key is a
// load error, so a typo fails startup rather than being silently ignored - the
// discipline project 04 adopted in its R-05 and R-16.
//
// Per-signal keys are NOT in this array: their pointers depend on the seven
// signal names in frame_catalog.hpp, so the loader expands them from there.
// That keeps one source of truth for which signals exist.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tcu::can {

enum class FieldType { Int, String, Enum, IntArray };

struct FieldSpec {
  /// RFC 6901 JSON pointer.
  std::string_view pointer;
  FieldType type;
  /// Int rows only; both bounds inclusive. Unused for the other types.
  std::int64_t min;
  std::int64_t max;
};

// contracts/config_schema.md section 1, in file order. 13 fixed rows; the
// remaining 14 pointers are the seven signals times two keys each.
inline constexpr std::array<FieldSpec, 13> kConfigFields{{
    {"/schema_version",               FieldType::Int,      1, 1},
    {"/mode",                         FieldType::Enum,     0, 0},
    {"/can/interface",                FieldType::String,   1, 15},
    {"/can/format",                   FieldType::Enum,     0, 0},
    {"/can/rx_queue_capacity",        FieldType::Int,      1, 65536},
    {"/can/health_check_ms",          FieldType::Int,      1, 10000},
    {"/can/drain_deadline_ms",        FieldType::Int,      0, 5000},
    {"/can/reconnect/initial_ms",     FieldType::Int,      1, 300000},
    {"/can/reconnect/max_ms",         FieldType::Int,      1, 300000},
    {"/can/reconnect/jitter_percent", FieldType::Int,      0, 50},
    {"/can/allowed_rx_ids",           FieldType::IntArray, 0, 0x7FF},
    {"/simulator/speed_period_ms",    FieldType::Int,      10, 10000},
    {"/simulator/heartbeat_period_ms", FieldType::Int,     100, 60000},
}};

// fixed_seed sits outside the array above only because its upper bound does not
// fit the table's "readable literal" style; it is validated identically.
inline constexpr std::string_view kSeedPointer = "/simulator/fixed_seed";
inline constexpr std::int64_t kSeedMin = 0;
inline constexpr std::int64_t kSeedMax = 2147483647;

inline constexpr std::int64_t kSignalTimeoutMin = 1;
inline constexpr std::int64_t kSignalTimeoutMax = 600000;

/// The only accepted values for the two Enum rows.
inline constexpr std::string_view kModeLabOnly = "lab_only";
inline constexpr std::string_view kFormatClassicStandard = "classic_standard_11bit";

/// The only schema version V1 understands. A change in the meaning of a key
/// needs a bump here, and the resulting file is rejected rather than misread.
inline constexpr std::int64_t kSupportedSchemaVersion = 1;

/// Bounds on the identifier array, so a file cannot ask for an unbounded
/// filter list.
inline constexpr std::size_t kMinAllowedRxIds = 1;
inline constexpr std::size_t kMaxAllowedRxIds = 16;

/// The closed template set for LoadResult::publicReason. A reason is chosen
/// from here and never built from the file's contents.
namespace reason {
inline constexpr const char* kOutOfRange = "out_of_range";
inline constexpr const char* kWrongType = "wrong_type";
inline constexpr const char* kUnknownKey = "unknown_key";
inline constexpr const char* kMissingKey = "missing_key";
inline constexpr const char* kCrossField = "cross_field";
inline constexpr const char* kNotInCatalog = "not_in_catalog";
inline constexpr const char* kInterfaceNotLab = "interface_not_lab";
inline constexpr const char* kUnsupportedVersion = "unsupported_version";
inline constexpr const char* kFileNotFound = "file_not_found";
inline constexpr const char* kReadError = "read_error";
inline constexpr const char* kTooLarge = "too_large";
inline constexpr const char* kParseError = "parse_error";
}  // namespace reason

}  // namespace tcu::can
