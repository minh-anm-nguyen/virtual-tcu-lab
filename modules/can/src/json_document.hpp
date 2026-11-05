#pragma once

// A thin wrapper around nlohmann/json.
//
// Its only job is to keep nlohmann out of every public header, so no consumer
// of tcu_can inherits the dependency - the same technique project 04 applied in
// its research R-01, and the reason nlohmann is linked PRIVATE in
// modules/can/CMakeLists.txt.
//
// The surface is deliberately small: project 05 reads one flat-ish document
// once at startup. It is NOT a general JSON facade, and it is not a
// reimplementation of project 04's tcu_config - see research R-06 for why that
// library cannot express this document's per-signal object map.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tcu::can {

enum class JsonParseError { None, FileNotFound, ReadError, TooLarge, ParseError };

class JsonDocument {
 public:
  JsonDocument();
  ~JsonDocument();
  JsonDocument(JsonDocument&&) noexcept;
  JsonDocument& operator=(JsonDocument&&) noexcept;
  JsonDocument(const JsonDocument&) = delete;
  JsonDocument& operator=(const JsonDocument&) = delete;

  /// Reads and parses. `max_bytes` caps the file so a malformed or hostile
  /// input cannot be read into memory unbounded (contracts/config_schema.md
  /// section 4).
  JsonParseError parseFile(const std::string& path, std::size_t max_bytes);

  /// For tests: parse a literal document without touching the filesystem.
  JsonParseError parseString(const std::string& text);

  /// Every leaf in the document, as an RFC 6901 pointer. An array counts as one
  /// leaf, so `/can/allowed_rx_ids` appears once rather than per element. This
  /// is what makes unknown-key rejection possible: the loader compares this set
  /// against the schema table instead of trusting the document's shape.
  std::vector<std::string> leafPointers() const;

  /// Type-strict accessors: a value of the wrong JSON type returns nullopt
  /// rather than converting, so `"1024"` never passes as an integer.
  std::optional<std::int64_t> intAt(const std::string& pointer) const;
  std::optional<bool> boolAt(const std::string& pointer) const;
  std::optional<std::string> stringAt(const std::string& pointer) const;
  std::optional<double> numberAt(const std::string& pointer) const;
  std::optional<std::vector<std::int64_t>> intArrayAt(const std::string& pointer) const;

  /// The member names of an object, in document order. Empty when the pointer
  /// is missing or is not an object.
  std::vector<std::string> objectKeysAt(const std::string& pointer) const;

  bool has(const std::string& pointer) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tcu::can
