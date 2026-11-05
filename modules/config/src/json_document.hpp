#pragma once

// The nlohmann/json wrapper. It exists so the dependency appears in NO public
// header of tcu_config: projects 05 to 30 include tcu/config/config_types.hpp
// and get typed structs, never a json object, so swapping the parser later
// touches this one translation unit (research R-01).
//
// Strictness is part of the contract, not a setting: comments, trailing data,
// NaN, single quotes and invalid UTF-8 are rejected by the library defaults,
// and duplicate keys plus excessive nesting are rejected by the callback this
// wrapper installs (contracts/config_schema.md section 3).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tcu::config::internal {

enum class ParseStatus {
  Ok,
  Malformed,      // syntax, trailing data, bad UTF-8
  DuplicateKey,
  TooDeep,
  NotAnObject,    // a valid JSON scalar or array is not a configuration file
};

struct ParseOutcome {
  ParseStatus status{ParseStatus::Ok};
  // Byte offset of a syntax error. Never the offending text: an error message
  // must not quote input (research R-10).
  std::size_t byteOffset{0};
  // Pointer of the duplicated key, when status is DuplicateKey.
  std::string duplicatePointer;
};

// The maximum nesting the loader accepts. Same limit project 02 chose for its
// payloads, for the same reason: a bounded parser cannot be made to recurse.
inline constexpr std::size_t kMaxDepth = 8;

class JsonDocument {
 public:
  JsonDocument();
  ~JsonDocument();
  JsonDocument(JsonDocument&&) noexcept;
  JsonDocument& operator=(JsonDocument&&) noexcept;
  JsonDocument(const JsonDocument&) = delete;
  JsonDocument& operator=(const JsonDocument&) = delete;

  ParseOutcome parse(std::string_view text);

  bool has(std::string_view pointer) const;

  // Each getter answers two questions at once: is the value there, and is it
  // the type the schema demands. A float where an int is required is a type
  // error, not a value to round (contracts/config_schema.md section 3).
  bool getInt(std::string_view pointer, std::int64_t& out) const;
  bool getBool(std::string_view pointer, bool& out) const;
  bool getString(std::string_view pointer, std::string& out) const;

  // Every leaf pointer the document actually contains, in document order. The
  // unknown-key check compares this against the schema table.
  std::vector<std::string> collectPointers() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tcu::config::internal
