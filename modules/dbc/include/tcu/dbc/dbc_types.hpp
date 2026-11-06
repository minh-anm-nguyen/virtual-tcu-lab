#pragma once

// The type vocabulary of the DBC subset.
//
// Fixed by spec section 6.2 and contracts/dbc_subset.md section 4: adding a
// DbcErrorCode value is a spec change, not an implementation detail
// (Constitution Principle I). DBC_SOURCE is the one addition beyond the spec's
// eleven, for the path checks research R-08 requires - recorded in plan.md
// Complexity Tracking.
//
// This header, and every other header of tcu_dbc, includes nothing outside the
// standard library: the parser must stay fuzzable and property-testable with no
// lifecycle at all (plan.md Structure Decision).

#include <cstdint>
#include <string>

namespace tcu::dbc {

/// DBC `@1` is Intel / little-endian, `@0` is Motorola / big-endian. The enum
/// keeps `bool littleEndian` out of every signature.
enum class ByteOrder { Intel, Motorola };

/// `M` is the selector, `mN` a branch, nothing means unconditional.
enum class MuxRole { None, Selector, Branch };

/// A Warning never prevents a load; an Error always does (invariant D-03).
enum class Severity { Warning, Error };

enum class DbcErrorCode {
  None,
  DbcSyntax,
  DbcLimit,
  DbcMessage,
  DbcSignal,
  DbcOutOfBounds,
  DbcOverlap,
  DbcMux,
  DbcEnum,
  DbcRange,
  UnsupportedFeature,
  /// The file itself could not be used: not a regular file, a symlink, too
  /// large, unreadable (research R-08).
  DbcSource,
};

/// The spelling used in diagnostics, CLI output and metric labels - one
/// spelling, so a test can assert on it (FR-15, NFR-10).
const char* toString(DbcErrorCode code) noexcept;

/// Spec section 6.3's shape, exactly. `message` never carries payload bytes,
/// never more than 64 characters of the offending token, and for DbcSource
/// never a resolved absolute path (invariant D-02).
struct ParseDiagnostic {
  std::string file;
  std::uint32_t line{0};
  std::uint32_t column{0};
  DbcErrorCode code{DbcErrorCode::None};
  Severity severity{Severity::Error};
  std::string message;
};

}  // namespace tcu::dbc
