#pragma once

// AST to immutable database. Runs only on a ParsedFile that passed every
// validation check, which is what lets decode skip layout validation entirely
// (invariant P-01, ADR-02).

#include <memory>
#include <string>

#include "dbc_ast.hpp"
#include "tcu/dbc/dbc_database.hpp"

namespace tcu::dbc::detail {

std::shared_ptr<const CompiledDbc> compile(const ParsedFile& file, std::uint64_t version,
                                           std::string hash, std::string sourcePath);

}  // namespace tcu::dbc::detail
