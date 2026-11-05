#pragma once

// Stage one of the two-stage redaction spec section 10.5 asks for: the value is
// replaced before the record is queued, so a secret never sits in memory that
// the writer thread, a core dump or a debugger could reach (research R-11).
//
// There is no masking policy on purpose. Section 10.5 makes a partial reveal of
// a VIN or a GPS fix conditional on an explicit legal basis, which v1 does not
// have, so a non-public value becomes the whole literal and nothing else.

#include <string>
#include <string_view>
#include <vector>

#include "tcu/logging/log_types.hpp"

namespace tcu::logging::internal {

inline constexpr std::string_view kRedacted = "[REDACTED]";

inline bool isRedactable(DataClass cls) noexcept {
  return cls != DataClass::Public;
}

inline void redactInPlace(std::vector<Field>& fields) {
  for (Field& field : fields) {
    if (isRedactable(field.cls)) {
      field.value = std::string(kRedacted);
    }
  }
}

}  // namespace tcu::logging::internal
