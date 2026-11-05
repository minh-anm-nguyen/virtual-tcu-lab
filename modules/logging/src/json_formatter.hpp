#pragma once

// One record becomes one line of newline-delimited JSON, with the fixed key
// order of contracts/logging_api.md section 3. Hand-written rather than
// delegated to a JSON library, because tcu_logging deliberately has no
// dependency: every project from 01 to 30 links it.

#include <cstddef>
#include <string>

#include "tcu/logging/log_types.hpp"

namespace tcu::logging::internal {

struct FormatResult {
  std::string line;
  // The record did not fit maxRecordBytes: the message was shortened first,
  // then fields were dropped from the end, and the line says so (research
  // R-12, BC-29).
  bool truncated{false};
  // Something in formatting failed and the line must not be trusted. Counted
  // in formatFailuresTotal; the record fails closed (invariant L9).
  bool failed{false};
};

FormatResult formatRecord(const LogRecord& record, std::size_t maxRecordBytes);

// Exposed for the formatter tests and for the emergency path, which needs a
// timestamp without building a whole record.
std::string formatTimestampUtc(std::chrono::system_clock::time_point tp);

}  // namespace tcu::logging::internal
