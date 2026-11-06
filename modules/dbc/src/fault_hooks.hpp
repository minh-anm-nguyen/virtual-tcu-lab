#pragma once

// The nine fault-injection points of spec section 15.2.
//
// src/-private on purpose: section 15.2 says not to put test hooks into the
// public production API. A test includes this header directly (the test target
// puts src/ on its include path); a consumer of tcu_dbc or tcu_signal has no
// way to reach it, and there is no #ifdef build variant to keep in sync
// (research R-23).
//
// Every member is an empty std::function by default, so a normal build calls
// through an empty target at each site and does nothing.

#include <functional>

namespace tcu::dbc::detail {

struct FaultHooks {
  std::function<void()> beforeValidate;
  std::function<void()> beforeBitRead;
  std::function<void()> afterPinSnapshot;
  std::function<void()> beforeCacheCommit;
  std::function<void()> afterCacheCommit;
  std::function<void()> beforeEventPublish;
  std::function<void()> afterReloadParse;
  std::function<void()> beforeReloadCommit;
  std::function<void()> onStopRequested;

  void clear() { *this = FaultHooks{}; }
};

/// One instance per process. Tests set a member, exercise the code, then call
/// clear() in teardown.
FaultHooks& testHooks() noexcept;

}  // namespace tcu::dbc::detail
