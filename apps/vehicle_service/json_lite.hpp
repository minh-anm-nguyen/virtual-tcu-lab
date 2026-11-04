#pragma once

// A depth-limited payload reader for the demo apps.
//
// It lives in apps/, never in the library: the transport treats a payload as
// opaque bytes and adds no JSON dependency (research R-01). Spec section 5.1
// asks for a JSON depth limit "o application layer", and this is that layer.
//
// It is deliberately small and strict rather than a general parser: it accepts
// the shapes the demo exchanges and rejects everything else with a clear
// failure, which is what B-18 requires of a handler.

#include <cstddef>
#include <string>

namespace jsonlite {

inline constexpr std::size_t kMaxDepth = 8;

/// Structural check only: balanced braces and brackets, terminated strings, and
/// nesting no deeper than kMaxDepth. Returns false on anything else.
inline bool isWellFormed(const std::string& text) {
  std::size_t depth = 0;
  bool in_string = false;
  bool escaped = false;
  bool saw_any = false;

  for (const char c : text) {
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    switch (c) {
      case '"':
        in_string = true;
        break;
      case '{':
      case '[':
        ++depth;
        saw_any = true;
        if (depth > kMaxDepth) {
          return false;
        }
        break;
      case '}':
      case ']':
        if (depth == 0) {
          return false;
        }
        --depth;
        break;
      default:
        break;
    }
  }
  return saw_any && depth == 0 && !in_string;
}

/// Value of a flat `"key":"value"` pair, or an empty string when absent.
inline std::string stringField(const std::string& text, const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  const std::size_t k = text.find(needle);
  if (k == std::string::npos) {
    return {};
  }
  const std::size_t colon = text.find(':', k + needle.size());
  if (colon == std::string::npos) {
    return {};
  }
  const std::size_t open = text.find('"', colon);
  if (open == std::string::npos) {
    return {};
  }
  const std::size_t close = text.find('"', open + 1);
  if (close == std::string::npos) {
    return {};
  }
  return text.substr(open + 1, close - open - 1);
}

/// Renders a JSON object from already-safe literals. The demo never puts
/// untrusted text in a payload, so no escaping is needed - and a payload is
/// never logged (Principle V).
inline std::string object(const std::string& body) { return "{" + body + "}"; }

inline std::string boolValue(bool value) { return value ? "true" : "false"; }

}  // namespace jsonlite
