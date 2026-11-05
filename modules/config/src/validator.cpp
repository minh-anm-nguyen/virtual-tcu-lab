#include "validator.hpp"

#include <algorithm>
#include <set>
#include <string_view>
#include <vector>

#include "tcu/config/config_schema.hpp"

namespace tcu::config::internal {
namespace {

ValidateOutcome fail(LoadError error, std::string reason) {
  ValidateOutcome outcome;
  outcome.error = error;
  outcome.reason = std::move(reason);
  return outcome;
}

std::string missing(std::string_view pointer) {
  return "missing required key " + std::string(pointer);
}

std::string wrongType(std::string_view pointer, FieldType type) {
  return "wrong type at " + std::string(pointer) + ": expected " +
         std::string(fieldTypeName(type));
}

std::string outOfRange(const FieldSpec& spec) {
  return "out of range at " + std::string(spec.pointer) + ": allowed " +
         std::to_string(spec.min) + ".." + std::to_string(spec.max);
}

std::string endpointRange(const FieldSpec& spec) {
  return "out of range at " + std::string(spec.pointer) +
         ": allowed scheme http/https, host <= " + std::to_string(spec.max) +
         " bytes";
}

// Drops a trailing :port so the host comparison below sees a host. Only a
// numeric suffix is removed, which leaves a bare IPv6 literal like ::1 alone.
std::string withoutPort(const std::string& host) {
  const std::size_t colon = host.rfind(':');
  if (colon == std::string::npos || colon + 1 == host.size()) {
    return host;
  }
  for (std::size_t i = colon + 1; i < host.size(); ++i) {
    if (host[i] < '0' || host[i] > '9') {
      return host;
    }
  }
  return host.substr(0, colon);
}

// A nonlocal endpoint must use TLS. Loopback and .local are the two exceptions
// the lab needs, because the simulated cloud lives there.
bool isLocalHost(const std::string& raw_host) {
  const std::string host = withoutPort(raw_host);
  if (host == "localhost" || host == "127.0.0.1" || host == "::1") {
    return true;
  }
  const std::string suffix = ".local";
  return host.size() > suffix.size() &&
         host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Scheme and host only: no query, no credentials, no path semantics. The
// endpoint is validated for shape, not reachability - this is a simulator and
// nothing here opens a socket (Constitution Principle IV).
bool splitEndpoint(const std::string& endpoint,
                   std::string& scheme_out,
                   std::string& host_out) {
  const std::string separator = "://";
  const std::size_t at = endpoint.find(separator);
  if (at == std::string::npos) {
    return false;
  }
  scheme_out = endpoint.substr(0, at);
  const std::size_t host_start = at + separator.size();
  const std::size_t host_end = endpoint.find('/', host_start);
  host_out = host_end == std::string::npos
                 ? endpoint.substr(host_start)
                 : endpoint.substr(host_start, host_end - host_start);
  return true;
}

}  // namespace

ValidateOutcome validate(const JsonDocument& document, const std::string& source) {
  // Check order is fixed by contracts/config_schema.md section 3: unknown keys,
  // then presence, then type, then range, then cross-field. The first failure
  // wins and reporting stops there, so one reason comes back rather than a list
  // an operator has to triage.

  // Unknown keys first: with a typo both "unknown key /logging/levl" and
  // "missing required key /logging/level" are true, and the first one points at
  // what the operator actually typed (research R-05).
  std::set<std::string_view> known;
  for (const FieldSpec& spec : kFieldSpecs) {
    known.insert(spec.pointer);
  }
  for (const std::string& pointer : document.collectPointers()) {
    if (known.find(std::string_view(pointer)) == known.end()) {
      return fail(LoadError::SchemaError, "unknown key " + pointer);
    }
  }

  ConfigSnapshot snapshot;
  snapshot.source = source;

  for (const FieldSpec& spec : kFieldSpecs) {
    if (!document.has(spec.pointer)) {
      // No defaults layer exists, so a missing key is always fatal: a value can
      // never be silently substituted (research R-16, BC-02).
      return fail(LoadError::SchemaError, missing(spec.pointer));
    }

    switch (spec.type) {
      case FieldType::Int: {
        std::int64_t value = 0;
        if (!document.getInt(spec.pointer, value)) {
          return fail(LoadError::SchemaError, wrongType(spec.pointer, spec.type));
        }
        if (value < spec.min || value > spec.max) {
          return fail(LoadError::InvalidValue, outOfRange(spec));
        }
        if (spec.pointer == "/connectivity/retry_base_ms") {
          snapshot.connectivity.retryBaseMs = value;
        } else if (spec.pointer == "/connectivity/retry_max_ms") {
          snapshot.connectivity.retryMaxMs = value;
        } else if (spec.pointer == "/connectivity/max_attempts") {
          snapshot.connectivity.maxAttempts = value;
        } else if (spec.pointer == "/telemetry/period_ms") {
          snapshot.telemetry.periodMs = value;
        } else if (spec.pointer == "/telemetry/offline_queue_capacity") {
          snapshot.telemetry.offlineQueueCapacity = value;
        } else if (spec.pointer == "/logging/queue_capacity") {
          snapshot.logging.queueCapacity = value;
        } else if (spec.pointer == "/logging/file_max_bytes") {
          snapshot.logging.fileMaxBytes = value;
        } else if (spec.pointer == "/logging/file_count") {
          snapshot.logging.fileCount = value;
        }
        // /schema_version is checked by the loader and carried nowhere: the
        // snapshot is version 1 by construction.
        break;
      }

      case FieldType::Bool: {
        bool value = false;
        if (!document.getBool(spec.pointer, value)) {
          return fail(LoadError::SchemaError, wrongType(spec.pointer, spec.type));
        }
        if (spec.pointer == "/logging/console_enabled") {
          snapshot.logging.consoleEnabled = value;
        } else if (spec.pointer == "/network/tls_required") {
          snapshot.network.tlsRequired = value;
        }
        break;
      }

      case FieldType::Enum: {
        std::string value;
        if (!document.getString(spec.pointer, value)) {
          return fail(LoadError::SchemaError, wrongType(spec.pointer, spec.type));
        }
        tcu::logging::Severity level{};
        if (!tcu::logging::parseSeverity(value, level)) {
          // The reason names the key and the allowed set, never the value the
          // operator typed (research R-10).
          return fail(LoadError::InvalidValue,
                      "out of range at " + std::string(spec.pointer) +
                          ": allowed TRACE/DEBUG/INFO/WARN/ERROR/FATAL");
        }
        snapshot.logging.level = level;
        break;
      }

      case FieldType::String: {
        std::string value;
        if (!document.getString(spec.pointer, value)) {
          return fail(LoadError::SchemaError, wrongType(spec.pointer, spec.type));
        }
        if (value.size() > static_cast<std::size_t>(spec.max)) {
          return fail(LoadError::InvalidValue, endpointRange(spec));
        }
        std::string scheme;
        std::string host;
        if (!splitEndpoint(value, scheme, host) ||
            (scheme != "http" && scheme != "https") || host.empty()) {
          return fail(LoadError::InvalidValue, endpointRange(spec));
        }
        snapshot.network.cloudEndpoint = value;
        break;
      }
    }
  }

  // Cross-field rules (data-model.md section 2.3). These are relations between
  // keys, so they cannot live in the per-field table.
  if (snapshot.connectivity.retryMaxMs < snapshot.connectivity.retryBaseMs) {
    return fail(LoadError::InvalidValue,
                "cross-field: /connectivity/retry_max_ms must be >= "
                "/connectivity/retry_base_ms");
  }

  std::string scheme;
  std::string host;
  splitEndpoint(snapshot.network.cloudEndpoint, scheme, host);
  if (!isLocalHost(host) && !snapshot.network.tlsRequired) {
    return fail(LoadError::InvalidValue,
                "cross-field: /network/tls_required must be true for a nonlocal "
                "endpoint");
  }

  ValidateOutcome outcome;
  outcome.snapshot = std::move(snapshot);
  return outcome;
}

}  // namespace tcu::config::internal
