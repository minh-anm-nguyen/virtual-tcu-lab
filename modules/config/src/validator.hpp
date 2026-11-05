#pragma once

// Stage 5: the schema table walk and the cross-field rules. The table is the
// single source of truth (research R-04), so this file contains no key names -
// only the three cross-field rules of data-model.md section 2.3, which are
// relations between keys rather than properties of one.

#include <string>

#include "json_document.hpp"
#include "tcu/config/config_types.hpp"

namespace tcu::config::internal {

struct ValidateOutcome {
  LoadError error{LoadError::None};
  std::string reason;
  // Only meaningful when error is None. generation and loadedAt are filled in
  // by the service at publish time, not here.
  ConfigSnapshot snapshot;
};

ValidateOutcome validate(const JsonDocument& document, const std::string& source);

}  // namespace tcu::config::internal
