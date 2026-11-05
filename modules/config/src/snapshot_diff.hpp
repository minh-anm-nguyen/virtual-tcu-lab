#pragma once

// Which fields changed, and whether any of them is restart-only. The walk is
// over kFieldSpecs, so the hot/restart classification and the validator read
// the same table (research R-04): there is no second list of key names here.
//
// Comparison is on the typed snapshot, never on JSON text, so whitespace, key
// order and integer spelling are invisible to it (invariant C3).

#include <string_view>

#include "tcu/config/config_schema.hpp"
#include "tcu/config/config_types.hpp"

namespace tcu::config::internal {

struct DiffResult {
  bool changed{false};
  bool restartRequired{false};
  // The first restart-only pointer that moved, for the rejection reason.
  std::string_view firstRestartPointer;
};

inline bool fieldEqual(std::string_view pointer,
                       const ConfigSnapshot& a,
                       const ConfigSnapshot& b) {
  if (pointer == "/schema_version") {
    // Both candidates passed the loader, so both are the supported version.
    return true;
  }
  if (pointer == "/connectivity/retry_base_ms") {
    return a.connectivity.retryBaseMs == b.connectivity.retryBaseMs;
  }
  if (pointer == "/connectivity/retry_max_ms") {
    return a.connectivity.retryMaxMs == b.connectivity.retryMaxMs;
  }
  if (pointer == "/connectivity/max_attempts") {
    return a.connectivity.maxAttempts == b.connectivity.maxAttempts;
  }
  if (pointer == "/telemetry/period_ms") {
    return a.telemetry.periodMs == b.telemetry.periodMs;
  }
  if (pointer == "/telemetry/offline_queue_capacity") {
    return a.telemetry.offlineQueueCapacity == b.telemetry.offlineQueueCapacity;
  }
  if (pointer == "/logging/level") {
    return a.logging.level == b.logging.level;
  }
  if (pointer == "/logging/queue_capacity") {
    return a.logging.queueCapacity == b.logging.queueCapacity;
  }
  if (pointer == "/logging/file_max_bytes") {
    return a.logging.fileMaxBytes == b.logging.fileMaxBytes;
  }
  if (pointer == "/logging/file_count") {
    return a.logging.fileCount == b.logging.fileCount;
  }
  if (pointer == "/logging/console_enabled") {
    return a.logging.consoleEnabled == b.logging.consoleEnabled;
  }
  if (pointer == "/network/cloud_endpoint") {
    return a.network.cloudEndpoint == b.network.cloudEndpoint;
  }
  if (pointer == "/network/tls_required") {
    return a.network.tlsRequired == b.network.tlsRequired;
  }
  return true;
}

inline DiffResult diff(const ConfigSnapshot& current,
                       const ConfigSnapshot& candidate) {
  DiffResult result;
  for (const FieldSpec& spec : kFieldSpecs) {
    if (fieldEqual(spec.pointer, current, candidate)) {
      continue;
    }
    result.changed = true;
    if (spec.policy == ReloadPolicy::Restart && !result.restartRequired) {
      result.restartRequired = true;
      result.firstRestartPointer = spec.pointer;
    }
  }
  return result;
}

}  // namespace tcu::config::internal
