#include "fault_hooks.hpp"

namespace tcu::dbc::detail {

FaultHooks& testHooks() noexcept {
  static FaultHooks hooks;
  return hooks;
}

}  // namespace tcu::dbc::detail
