#include "tcu/can/frame_validator.hpp"

#include "tcu/can/frame_catalog.hpp"

namespace tcu::can {

ValidationResult FrameValidator::validate(const CanMessage& frame) const noexcept {
  // 1. Flags first. An extended identifier whose low 11 bits equal a known
  // standard one must never alias onto it (B-14), and a remote or error frame
  // carries no payload to decode (B-13, B-15).
  if (frame.format != FrameFormat::Standard11 || frame.kind != FrameKind::Data) {
    return {false, RejectReason::Flags, false};
  }

  // 2. Known identifier. Reported separately from an invalid frame because the
  // two counters mean different things: an unknown ID is normal traffic the
  // filter happened to let through, not a malformed frame
  // (contracts/metrics.md, UT-05, B-12).
  const FrameDefinition* definition = findFrame(frame.id);
  if (definition == nullptr) {
    return {false, RejectReason::UnknownId, false};
  }

  // 3. Length. Spec section 6 fixes 8 bytes for every V1 row, so a known ID
  // with any other length is malformed rather than a shorter variant (UT-08).
  if (frame.length != definition->expected_length) {
    return {false, RejectReason::Length, false};
  }

  // 4. Reserved bits. Counted, never rejected: rejecting would make the lab
  // brittle against cangen traffic for no safety gain, since reserved bytes
  // carry no signal. Padding must still never be read as a new signal
  // (research R-15, UT-20).
  bool reserved_nonzero = false;
  for (std::size_t i = 0; i < frame.data.size(); ++i) {
    if ((frame.data[i] & definition->reserved_mask[i]) != 0) {
      reserved_nonzero = true;
      break;
    }
  }

  return {true, RejectReason::None, reserved_nonzero};
}

}  // namespace tcu::can
