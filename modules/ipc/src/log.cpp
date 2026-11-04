#include "log.hpp"

#include <ctime>

#include <array>
#include <chrono>
#include <cstdio>
#include <string>

namespace tcu::ipc::detail {
namespace {

const char* levelName(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Debug:
      return "debug";
    case LogLevel::Info:
      return "info";
    case LogLevel::Warn:
      return "warn";
    case LogLevel::Error:
      return "error";
  }
  return "info";
}

/// 2026-11-04T09:12:33.481Z - exactly 24 characters, which log_test asserts.
std::string isoTimestampUtc() {
  using clock = std::chrono::system_clock;
  const auto now = clock::now();
  const auto secs = std::chrono::time_point_cast<std::chrono::seconds>(now);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - secs).count();

  const std::time_t t = clock::to_time_t(secs);
  std::tm tm{};
  ::gmtime_r(&t, &tm);

  std::array<char, 32> buf{};
  const int n = std::snprintf(buf.data(), buf.size(), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                              tm.tm_sec, static_cast<int>(ms));
  return std::string(buf.data(), n > 0 ? static_cast<std::size_t>(n) : 0u);
}

/// A field value is never empty on the wire: an absent one is "-", so the key
/// set stays fixed and the line stays greppable.
std::string orDash(const std::string& value) { return value.empty() ? "-" : value; }

std::string orDash(const std::optional<std::uint64_t>& value) {
  return value.has_value() ? std::to_string(*value) : "-";
}

}  // namespace

std::string format(const LogFields& fields) {
  std::string line;
  line.reserve(192);
  line += "timestamp=";
  line += isoTimestampUtc();
  line += " level=";
  line += levelName(fields.level);
  line += " service=";
  line += orDash(fields.service);
  line += " connection_id=";
  line += orDash(fields.connection_id);
  line += " request_id=";
  line += orDash(fields.request_id);
  line += " method=";
  line += orDash(fields.method);
  line += " event=";
  line += orDash(fields.event);
  line += " reason=";
  line += orDash(fields.reason);
  line += " duration_ms=";
  line += orDash(fields.duration_ms);
  return line;
}

void emit(const LogFields& fields) {
  const std::string line = format(fields);
  // One fprintf so two threads cannot interleave halves of a line.
  std::fprintf(stderr, "%s\n", line.c_str());
}

}  // namespace tcu::ipc::detail
