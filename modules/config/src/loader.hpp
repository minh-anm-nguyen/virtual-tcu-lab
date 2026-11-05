#pragma once

// Stages 1 to 4 of the reload pipeline (data-model.md section 2.2): probe the
// size, read the bytes, parse strictly, check the schema version. Nothing here
// touches the snapshot in force - that is what makes every failure path
// last-known-good by construction (invariant S8).

#include <cstddef>
#include <filesystem>
#include <string>

#include "json_document.hpp"
#include "tcu/config/config_types.hpp"

namespace tcu::config::internal {

struct LoadOutcome {
  LoadError error{LoadError::None};
  // Built only from the closed template set of contracts/config_schema.md
  // section 4. Never quotes input (research R-10).
  std::string reason;
  JsonDocument document;
};

LoadOutcome loadFile(const std::filesystem::path& path, std::size_t maxBytes);

}  // namespace tcu::config::internal
