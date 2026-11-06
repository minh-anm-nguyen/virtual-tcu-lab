#pragma once

// Loading a DBC file: strict, fail-closed, all diagnostics at once.
//
// A file is either fully loaded or fully rejected - there is no partial
// database (spec section 6.3). Neither entry point throws for any byte
// sequence, which is what makes the fuzz target of research R-21 meaningful.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tcu/dbc/dbc_database.hpp"
#include "tcu/dbc/dbc_types.hpp"

namespace tcu::dbc {

/// Spec section 3.4's defaults. Every limit is checked before the allocation it
/// bounds, so a hostile file cannot make us allocate first and fail later
/// (invariant D-12).
struct ParseLimits {
  std::size_t dbcMaxBytes{2u * 1024u * 1024u};
  std::size_t maxMessages{2000};
  std::size_t maxSignalsPerMessage{128};
  std::size_t maxTotalSignals{10000};
  std::uint8_t maxMessageLength{8};
  std::size_t maxTokenBytes{1024};
  std::size_t maxStatements{100000};
};

struct LoadResult {
  /// Non-null if and only if no diagnostic has Severity::Error (invariant D-03).
  std::shared_ptr<const CompiledDbc> db;
  /// Every problem found, ordered by (line, column) - so one dbc_validate run
  /// fixes many errors rather than one (spec section 6.3).
  std::vector<ParseDiagnostic> diagnostics;

  bool ok() const noexcept { return db != nullptr; }
  bool hasError() const noexcept;
  /// First diagnostic with this code, or nullptr. For tests and the CLI.
  const ParseDiagnostic* firstWithCode(DbcErrorCode code) const noexcept;
};

/// Reads the file, validates it, compiles bit plans and hashes the exact bytes.
/// Rejects anything that is not a regular file - a symlink, a directory, a
/// FIFO - with DbcSource, before opening it (research R-08).
LoadResult loadDbcFile(const std::string& path, const ParseLimits& limits = {});

/// The same pipeline over bytes already in memory. Every unit test and both
/// fuzz targets use this one.
LoadResult loadDbcBytes(std::string_view text, std::string_view fileLabel,
                        const ParseLimits& limits = {});

}  // namespace tcu::dbc
