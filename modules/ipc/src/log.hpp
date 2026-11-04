#pragma once

// Structured logging, one line per call.
//
// The field set is closed and comes from spec section 11:
//   timestamp level service connection_id request_id method event reason duration_ms
//
// There is deliberately no payload parameter. Principle V forbids secrets in
// logs, and the cheapest way to guarantee it is to give the payload no path
// into a log line at all (contracts/metrics.md, "What must never appear").

#include <cstdint>
#include <optional>
#include <string>

namespace tcu::ipc::detail {

enum class LogLevel { Debug, Info, Warn, Error };

/// Absent optionals render as "-".
struct LogFields {
  LogLevel level{LogLevel::Info};
  std::string service;
  std::optional<std::uint64_t> connection_id;
  std::optional<std::uint64_t> request_id;
  std::string method;
  std::string event;
  std::string reason;
  std::optional<std::uint64_t> duration_ms;
};

/// Renders the line without a trailing newline. Exposed for the unit test.
std::string format(const LogFields& fields);

/// Renders and writes the line to stderr, newline terminated.
void emit(const LogFields& fields);

}  // namespace tcu::ipc::detail
