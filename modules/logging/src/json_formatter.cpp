#include "json_formatter.hpp"

#include <cmath>
#include <cstdio>
#include <ctime>
#include <string_view>
#include <variant>
#include <vector>

#include "redaction.hpp"

namespace tcu::logging::internal {
namespace {

// Appends the escaped form of one character. Returns the token so the limited
// variant can decide whether it still fits: that is what keeps a truncated line
// from ever ending inside an escape sequence.
std::string escapeChar(char c) {
  switch (c) {
    case '"':  return "\\\"";
    case '\\': return "\\\\";
    case '\n': return "\\n";
    case '\r': return "\\r";
    case '\t': return "\\t";
    case '\b': return "\\b";
    case '\f': return "\\f";
    default: break;
  }
  const unsigned char uc = static_cast<unsigned char>(c);
  if (uc < 0x20) {
    char buffer[7];
    std::snprintf(buffer, sizeof(buffer), "\\u%04x", uc);
    return std::string(buffer);
  }
  // Everything from 0x20 up passes through unchanged, so valid UTF-8 survives
  // byte for byte.
  return std::string(1, c);
}

void escapeInto(std::string_view in, std::string& out) {
  for (const char c : in) {
    out += escapeChar(c);
  }
}

// Escapes as much of `in` as fits in `budget` output bytes, never splitting an
// escape token. Returns true when the whole input fitted.
bool escapeLimited(std::string_view in, std::size_t budget, std::string& out) {
  std::size_t used = 0;
  for (const char c : in) {
    const std::string token = escapeChar(c);
    if (used + token.size() > budget) {
      return false;
    }
    out += token;
    used += token.size();
  }
  return true;
}

std::string renderDouble(double value) {
  if (!std::isfinite(value)) {
    // JSON has no NaN or Infinity. Emitting null keeps the line parseable; the
    // caller sees the field, not a broken file.
    return "null";
  }
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.17g", value);
  return std::string(buffer);
}

// One field as "key":value. The variant type decides the JSON type, never the
// caller (invariant L5). A non-public class is redacted here too - the second
// of the two stages section 10.5 requires.
std::string renderField(const Field& field) {
  std::string out;
  out += '"';
  escapeInto(field.key, out);
  out += "\":";

  if (isRedactable(field.cls)) {
    out += '"';
    escapeInto(kRedacted, out);
    out += '"';
    return out;
  }

  if (const auto* i = std::get_if<std::int64_t>(&field.value)) {
    out += std::to_string(*i);
  } else if (const auto* d = std::get_if<double>(&field.value)) {
    out += renderDouble(*d);
  } else if (const auto* b = std::get_if<bool>(&field.value)) {
    out += (*b ? "true" : "false");
  } else if (const auto* s = std::get_if<std::string>(&field.value)) {
    out += '"';
    escapeInto(*s, out);
    out += '"';
  } else {
    out += "null";
  }
  return out;
}

std::string renderFields(const std::vector<Field>& fields, std::size_t count) {
  std::string out;
  for (std::size_t i = 0; i < count && i < fields.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    out += renderField(fields[i]);
  }
  return out;
}

}  // namespace

std::string formatTimestampUtc(std::chrono::system_clock::time_point tp) {
  using namespace std::chrono;
  const auto since_epoch = tp.time_since_epoch();
  const auto secs = duration_cast<seconds>(since_epoch);
  auto ms = duration_cast<milliseconds>(since_epoch - secs).count();
  std::time_t t = static_cast<std::time_t>(secs.count());
  if (ms < 0) {  // pre-epoch input: keep the fraction positive
    ms += 1000;
    t -= 1;
  }

  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif

  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                tm.tm_min, tm.tm_sec, static_cast<int>(ms));
  return std::string(buffer);
}

FormatResult formatRecord(const LogRecord& record, std::size_t maxRecordBytes) {
  FormatResult result;

  // Fixed key order (contracts/logging_api.md section 3). The head and the two
  // separators are constant-shaped, which is what lets truncation be
  // arithmetic instead of a re-render loop.
  std::string head = "{\"timestamp_utc\":\"";
  head += formatTimestampUtc(record.timestampUtc);
  head += "\",\"severity\":\"";
  head += severityName(record.severity);
  head += "\",\"component\":\"";
  escapeInto(record.component, head);
  head += "\",\"event\":\"";
  escapeInto(record.eventName, head);
  head += "\",\"correlation_id\":\"";
  escapeInto(record.correlationId, head);
  head += "\",\"message\":\"";

  const std::string mid = "\",\"fields\":{";
  const std::string tail = "}}";
  const std::string truncated_tail = "},\"truncated\":true}";

  std::string message;
  escapeInto(record.message, message);
  std::string fields = renderFields(record.fields, record.fields.size());

  if (head.size() + message.size() + mid.size() + fields.size() + tail.size() <=
      maxRecordBytes) {
    result.line = head + message + mid + fields + tail;
    return result;
  }

  // Over the cap. The message is shortened first, then fields are dropped from
  // the end, because the structured keys are what a query filters on.
  result.truncated = true;
  const std::size_t fixed = head.size() + mid.size() + truncated_tail.size();
  if (fixed >= maxRecordBytes) {
    // The cap is smaller than an empty record. Emit the skeleton rather than
    // nothing: a line that says "truncated" is still a usable signal, and the
    // caller gets a counted truncation either way.
    result.line = head + mid + truncated_tail;
    return result;
  }

  std::size_t budget = maxRecordBytes - fixed;
  std::size_t keep = record.fields.size();
  while (keep > 0 && fields.size() > budget) {
    --keep;
    fields = renderFields(record.fields, keep);
  }
  if (fields.size() > budget) {
    fields.clear();
  }

  std::string clipped;
  escapeLimited(record.message, budget - fields.size(), clipped);
  result.line = head + clipped + mid + fields + truncated_tail;
  return result;
}

}  // namespace tcu::logging::internal
