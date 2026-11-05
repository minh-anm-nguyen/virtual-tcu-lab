#pragma once

// The schema table: the ONE place a key, its type, its range and its reload
// policy are written (research R-04). Three consumers read it - the validator
// checks presence, type and range; the diff classifies a changed field as hot
// or restart-only; contracts/config_schema.md section 1 holds the same rows for
// human readers, and Schema_SampleFileMatchesTable asserts the sample file
// agrees with this array.
//
// Every key is mandatory: there is no defaults layer, so a missing key fails
// startup and fails a reload (research R-16, BC-02).

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tcu::config {

enum class FieldType { Int, Bool, String, Enum };

enum class ReloadPolicy {
  // May change in a running process: a committed change becomes a new
  // generation and observers are notified.
  Hot,
  // Read once at startup. A reload whose candidate differs here is rejected
  // with RestartRequired, and the whole candidate goes with it (BC-08).
  Restart,
};

struct FieldSpec {
  // RFC 6901 JSON pointer.
  std::string_view pointer;
  FieldType type;
  // Int rows only; both bounds inclusive. Unused for Bool, String and Enum.
  std::int64_t min;
  std::int64_t max;
  ReloadPolicy policy;
};

// Spec section 7.2, in file order. 13 rows: 6 hot, 7 restart.
inline constexpr std::array<FieldSpec, 13> kFieldSpecs{{
    {"/schema_version", FieldType::Int, 1, 1, ReloadPolicy::Restart},
    {"/connectivity/retry_base_ms", FieldType::Int, 100, 60000, ReloadPolicy::Hot},
    {"/connectivity/retry_max_ms", FieldType::Int, 100, 300000, ReloadPolicy::Hot},
    {"/connectivity/max_attempts", FieldType::Int, 0, 100, ReloadPolicy::Hot},
    {"/telemetry/period_ms", FieldType::Int, 200, 3600000, ReloadPolicy::Hot},
    {"/telemetry/offline_queue_capacity", FieldType::Int, 16, 100000, ReloadPolicy::Restart},
    {"/logging/level", FieldType::Enum, 0, 0, ReloadPolicy::Hot},
    {"/logging/queue_capacity", FieldType::Int, 128, 65536, ReloadPolicy::Restart},
    {"/logging/file_max_bytes", FieldType::Int, 1048576, 104857600, ReloadPolicy::Restart},
    {"/logging/file_count", FieldType::Int, 1, 20, ReloadPolicy::Restart},
    {"/logging/console_enabled", FieldType::Bool, 0, 0, ReloadPolicy::Hot},
    {"/network/cloud_endpoint", FieldType::String, 0, 255, ReloadPolicy::Restart},
    {"/network/tls_required", FieldType::Bool, 0, 0, ReloadPolicy::Restart},
}};

inline constexpr std::size_t kFieldCount = kFieldSpecs.size();

// The only schema version v1 understands. A change in the meaning of a key
// needs a bump here, and since this field is Restart by policy a running
// process rejects the new file rather than misreading it (BC-28).
inline constexpr std::int64_t kSupportedSchemaVersion = 1;

inline constexpr std::string_view fieldTypeName(FieldType type) noexcept {
  switch (type) {
    case FieldType::Int:    return "int";
    case FieldType::Bool:   return "bool";
    case FieldType::String: return "string";
    case FieldType::Enum:   return "enum";
  }
  return "int";
}

}  // namespace tcu::config
