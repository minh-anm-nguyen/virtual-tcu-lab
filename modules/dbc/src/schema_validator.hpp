#pragma once

// The eleven validation classes of contracts/dbc_subset.md section 4.
// Returns false when any Severity::Error diagnostic was produced; the caller
// then publishes nothing, because a partially valid database does not exist
// (spec section 6.3).

#include <vector>

#include "dbc_ast.hpp"

namespace tcu::dbc::detail {

bool validate(ParsedFile& file, std::string_view fileLabel, const ParseLimits& limits,
              std::vector<ParseDiagnostic>& diagnostics);

}  // namespace tcu::dbc::detail
