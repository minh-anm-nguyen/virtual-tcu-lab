#pragma once

// Public vocabulary of the logging library.
//
// These declarations are fixed by spec section 10.1 and
// specs/004-configuration-logging-service/contracts/logging_api.md section 1.
// Adding an enum value here is a spec change, not an implementation detail
// (Constitution Principle I).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace tcu::logging {

enum class Severity { Trace, Debug, Info, Warn, Error, Fatal };

inline constexpr std::size_t kSeverityCount = 6;

// How much a value is allowed to be seen. Anything but Public is replaced by
// the literal [REDACTED] at ingestion and again at formatting (research R-11).
// There is deliberately no masking policy: no "last 4 of the VIN".
enum class DataClass { Public, VehicleSensitive, Secret };

inline std::string_view severityName(Severity level) noexcept {
  switch (level) {
    case Severity::Trace: return "TRACE";
    case Severity::Debug: return "DEBUG";
    case Severity::Info:  return "INFO";
    case Severity::Warn:  return "WARN";
    case Severity::Error: return "ERROR";
    case Severity::Fatal: return "FATAL";
  }
  return "INFO";
}

// Accepts exactly the six names above, upper-case. The configuration schema
// treats /logging/level as an enum, so a typo is rejected, never guessed.
inline bool parseSeverity(std::string_view name, Severity& out) noexcept {
  if (name == "TRACE") { out = Severity::Trace; return true; }
  if (name == "DEBUG") { out = Severity::Debug; return true; }
  if (name == "INFO")  { out = Severity::Info;  return true; }
  if (name == "WARN")  { out = Severity::Warn;  return true; }
  if (name == "ERROR") { out = Severity::Error; return true; }
  if (name == "FATAL") { out = Severity::Fatal; return true; }
  return false;
}

// A typed structured field. The variant type decides how the value is written:
// numbers unquoted, booleans bare, strings quoted (invariant L5).
struct Field {
  std::string key;
  std::variant<std::int64_t, double, bool, std::string> value;
  DataClass cls{DataClass::Public};
};

// Convenience makers, so a call site reads as data. Public is the default; a
// non-public field has to say so.
inline Field f(std::string key, std::int64_t value) {
  return Field{std::move(key), value, DataClass::Public};
}
inline Field f(std::string key, int value) {
  return Field{std::move(key), static_cast<std::int64_t>(value), DataClass::Public};
}
inline Field f(std::string key, double value) {
  return Field{std::move(key), value, DataClass::Public};
}
inline Field f(std::string key, bool value) {
  return Field{std::move(key), value, DataClass::Public};
}
inline Field f(std::string key, std::string value) {
  return Field{std::move(key), std::move(value), DataClass::Public};
}
inline Field f(std::string key, const char* value) {
  return Field{std::move(key), std::string(value), DataClass::Public};
}
inline Field classified(std::string key, std::string value, DataClass cls) {
  return Field{std::move(key), std::move(value), cls};
}

// One record, owning every byte it references (invariant L1): nothing here may
// point into a caller buffer, because the writer thread reads it later.
struct LogRecord {
  std::chrono::system_clock::time_point timestampUtc{};
  std::chrono::steady_clock::time_point timestampMono{};
  Severity severity{Severity::Info};
  std::string component;
  std::string eventName;
  std::string message;
  std::string correlationId;
  std::vector<Field> fields;
};

}  // namespace tcu::logging
