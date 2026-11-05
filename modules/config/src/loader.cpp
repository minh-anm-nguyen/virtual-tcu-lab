#include "loader.hpp"

#include <fstream>
#include <system_error>

#include "tcu/config/config_schema.hpp"

namespace tcu::config::internal {
namespace {

LoadOutcome fail(LoadError error, std::string reason) {
  LoadOutcome outcome;
  outcome.error = error;
  outcome.reason = std::move(reason);
  return outcome;
}

}  // namespace

LoadOutcome loadFile(const std::filesystem::path& path, std::size_t maxBytes) {
  std::error_code ec;

  // Stage 1: the size is probed BEFORE anything is read, so an oversize file
  // costs one stat and no memory at all (spec section 9 rule 1, BC-15).
  if (!std::filesystem::exists(path, ec) || ec) {
    return fail(LoadError::FileNotFound, "cannot read configured config path");
  }
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    return fail(LoadError::ReadError, "cannot read configured config path");
  }
  if (static_cast<std::uint64_t>(size) > static_cast<std::uint64_t>(maxBytes)) {
    return fail(LoadError::TooLarge,
                "file exceeds " + std::to_string(maxBytes) + " bytes");
  }

  // Stage 2: one read into a buffer the probe already sized.
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return fail(LoadError::ReadError, "cannot read configured config path");
  }
  std::string text;
  text.resize(static_cast<std::size_t>(size));
  if (size > 0) {
    in.read(text.data(), static_cast<std::streamsize>(size));
    if (in.bad()) {
      return fail(LoadError::ReadError, "cannot read configured config path");
    }
    text.resize(static_cast<std::size_t>(in.gcount()));
  }

  // Stage 3: strict parse. A file caught mid-write by a non-atomic writer ends
  // up here, which is why a truncated candidate can never be published (BC-14).
  LoadOutcome outcome;
  const ParseOutcome parsed = outcome.document.parse(text);
  switch (parsed.status) {
    case ParseStatus::Ok:
      break;
    case ParseStatus::DuplicateKey:
      return fail(LoadError::ParseError,
                  "duplicate key " + parsed.duplicatePointer);
    case ParseStatus::TooDeep:
      return fail(LoadError::ParseError,
                  "parse error at byte offset " +
                      std::to_string(parsed.byteOffset));
    case ParseStatus::NotAnObject:
      return fail(LoadError::SchemaError, "missing required key /schema_version");
    case ParseStatus::Malformed:
      return fail(LoadError::ParseError,
                  "parse error at byte offset " +
                      std::to_string(parsed.byteOffset));
  }

  // Stage 4: the schema version, before any other field is looked at. A file
  // written for a different version is rejected rather than half understood
  // (BC-28). A missing or wrongly typed version is a schema error, not an
  // unsupported version: those are different operator actions.
  const std::string_view version_pointer = kFieldSpecs.front().pointer;
  std::int64_t version = 0;
  if (!outcome.document.has(version_pointer)) {
    return fail(LoadError::SchemaError,
                "missing required key " + std::string(version_pointer));
  }
  if (!outcome.document.getInt(version_pointer, version)) {
    return fail(LoadError::SchemaError,
                "wrong type at " + std::string(version_pointer) +
                    ": expected int");
  }
  if (version != kSupportedSchemaVersion) {
    return fail(LoadError::UnsupportedVersion,
                "unsupported schema_version: expected " +
                    std::to_string(kSupportedSchemaVersion));
  }

  return outcome;
}

}  // namespace tcu::config::internal
