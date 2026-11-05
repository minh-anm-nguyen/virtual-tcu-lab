#pragma once

// Wire-level and catalog-level admission. Runs before the decoder and before
// any cache commit (contracts/can_api.md section 3, contracts/frame_catalog.md
// section 5).
//
// The socket filter already stops most of this at the kernel (research R-11),
// but the validator is not redundant: --broad-filter mode (B-12) and
// FakeCanTransport (B-10, B-13, B-14, B-15) both bypass the kernel, and spec
// section 18.1's test note says those malformed frames cannot be placed on a
// real vcan0 at all.

#include "tcu/can/can_types.hpp"

namespace tcu::can {

struct ValidationResult {
  bool ok{false};
  RejectReason reason{RejectReason::None};
  /// Informational only. A known frame with nonzero reserved bits is ACCEPTED
  /// and counted; V1 does not reject it (research R-15, spec section 6.1).
  bool reserved_nonzero{false};
};

class FrameValidator {
 public:
  /// Pure: no allocation, no clock read, no state. Checks in order - flags,
  /// known identifier, length, reserved bits - and returns on the first
  /// failure.
  ValidationResult validate(const CanMessage& frame) const noexcept;
};

}  // namespace tcu::can
