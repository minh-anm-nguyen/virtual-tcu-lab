#include "fault_hooks.hpp"

namespace tcu::signal::detail {

FaultHooks& testHooks() noexcept {
  static FaultHooks hooks;
  return hooks;
}

}  // namespace tcu::signal::detail
