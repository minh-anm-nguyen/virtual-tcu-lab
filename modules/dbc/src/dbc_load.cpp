// The load pipeline: source checks, hash, lex, parse, validate, compile.
//
// Either a whole database comes back or none does, and in both cases every
// problem found is reported (spec section 6.3). Neither entry point throws for
// any byte sequence - that is what research R-21's fuzz target relies on.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include "compiled_bitplan.hpp"
#include "dbc_lexer.hpp"
#include "schema_validator.hpp"
#include "sha256.hpp"
#include "tcu/dbc/dbc_parser.hpp"

namespace tcu::dbc {
namespace {

ParseDiagnostic sourceError(std::string_view fileLabel, std::string message) {
  return ParseDiagnostic{std::string(fileLabel), 0, 0, DbcErrorCode::DbcSource, Severity::Error,
                         std::move(message)};
}

void sortDiagnostics(std::vector<ParseDiagnostic>& diagnostics) {
  // Stable, by (line, column): a reader works top to bottom through the file.
  for (std::size_t i = 1; i < diagnostics.size(); ++i) {
    for (std::size_t j = i; j > 0; --j) {
      const auto& left = diagnostics[j - 1];
      const auto& right = diagnostics[j];
      const bool inOrder =
          left.line < right.line || (left.line == right.line && left.column <= right.column);
      if (inOrder) break;
      std::swap(diagnostics[j - 1], diagnostics[j]);
    }
  }
}

}  // namespace

CompiledDbc::CompiledDbc(std::uint64_t version, std::string hash, std::string sourcePath,
                         std::unordered_map<MessageKey, CompiledMessage, MessageKeyHash> byKey,
                         DbcStats stats)
    : version_(version), hash_(std::move(hash)), sourcePath_(std::move(sourcePath)),
      byKey_(std::move(byKey)), stats_(stats) {}

const CompiledMessage* CompiledDbc::find(std::uint32_t busId, FrameFormat format,
                                         std::uint32_t arbitrationId) const noexcept {
  const MessageKey key{busId, format == FrameFormat::Extended29, arbitrationId};
  const auto it = byKey_.find(key);
  return it == byKey_.end() ? nullptr : &it->second;
}

const SignalMeta* CompiledDbc::findSignal(std::uint32_t busId, FrameFormat format,
                                          std::uint32_t arbitrationId,
                                          const std::string& signalName) const noexcept {
  const CompiledMessage* message = find(busId, format, arbitrationId);
  if (message == nullptr) return nullptr;
  for (const auto& meta : message->signals) {
    if (meta.name == signalName) return &meta;
  }
  return nullptr;
}

bool LoadResult::hasError() const noexcept {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.severity == Severity::Error) return true;
  }
  return false;
}

const ParseDiagnostic* LoadResult::firstWithCode(DbcErrorCode code) const noexcept {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.code == code) return &diagnostic;
  }
  return nullptr;
}

LoadResult loadDbcBytes(std::string_view text, std::string_view fileLabel,
                        const ParseLimits& limits) {
  LoadResult result;

  // Checked before anything is allocated from the content (invariant D-12).
  if (text.size() > limits.dbcMaxBytes) {
    result.diagnostics.push_back(sourceError(
        fileLabel, "dbc_max_bytes exceeded: the file is " + std::to_string(text.size()) +
                       " bytes, the limit is " + std::to_string(limits.dbcMaxBytes)));
    result.diagnostics.back().code = DbcErrorCode::DbcLimit;
    return result;
  }

  std::vector<detail::Token> tokens;
  if (!detail::tokenize(text, fileLabel, limits, tokens, result.diagnostics)) {
    sortDiagnostics(result.diagnostics);
    return result;
  }

  detail::ParsedFile parsed;
  const bool parsedOk = detail::parseTokens(tokens, fileLabel, limits, parsed, result.diagnostics);
  // Validation runs even after a parse error, so one run reports layout
  // problems in the statements that did parse (spec section 6.3).
  const bool validOk = detail::validate(parsed, fileLabel, limits, result.diagnostics);
  sortDiagnostics(result.diagnostics);

  if (!parsedOk || !validOk) return result;

  result.db = detail::compile(parsed, 1u, detail::Sha256::hexOf(text.data(), text.size()),
                              std::string(fileLabel));
  return result;
}

LoadResult loadDbcFile(const std::string& path, const ParseLimits& limits) {
  LoadResult result;
  std::error_code ec;

  // symlink_status, not status: a symlink must be refused rather than followed
  // (research R-08). The message names the configured path as written, never a
  // resolved absolute path (invariant D-02).
  const auto status = std::filesystem::symlink_status(path, ec);
  if (ec) {
    result.diagnostics.push_back(sourceError(path, "cannot stat the DBC path"));
    return result;
  }
  if (std::filesystem::is_symlink(status)) {
    result.diagnostics.push_back(sourceError(path, "the DBC path is a symlink, which v1 refuses"));
    return result;
  }
  if (!std::filesystem::is_regular_file(status)) {
    result.diagnostics.push_back(sourceError(path, "the DBC path is not a regular file"));
    return result;
  }

  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    result.diagnostics.push_back(sourceError(path, "cannot determine the DBC file size"));
    return result;
  }
  if (size > limits.dbcMaxBytes) {
    auto diagnostic = sourceError(path, "dbc_max_bytes exceeded before reading the file");
    diagnostic.code = DbcErrorCode::DbcLimit;
    result.diagnostics.push_back(std::move(diagnostic));
    return result;
  }

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    result.diagnostics.push_back(sourceError(path, "cannot open the DBC file for reading"));
    return result;
  }
  std::string text;
  text.resize(static_cast<std::size_t>(size));
  if (size > 0) {
    in.read(&text[0], static_cast<std::streamsize>(size));
    if (!in) {
      result.diagnostics.push_back(sourceError(path, "the DBC file could not be read in full"));
      return result;
    }
  }

  return loadDbcBytes(text, path, limits);
}

}  // namespace tcu::dbc
