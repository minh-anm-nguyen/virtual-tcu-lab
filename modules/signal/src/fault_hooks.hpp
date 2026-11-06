#pragma once

// The tcu_signal half of spec section 15.2's nine fault-injection points.
//
// Two structs rather than one shared header: tcu_dbc owns the two points inside
// the pure decoder (beforeValidate, beforeBitRead) and may not depend on this
// library, while every other point is a lifecycle point and lives here. Each
// module therefore declares exactly the seam it has, and neither exposes it
// publicly (research R-23).
//
// src/-private: a test includes this directly because the test target puts
// src/ on its include path; a consumer of tcu_signal cannot reach it, and there
// is no #ifdef build variant.

#include <functional>

namespace tcu::signal::detail {

struct FaultHooks {
  /// Between phase 1 and phase 2 of the freshness scan. This is the seam race
  /// recipe B needs: a valid frame commits while the scan has already decided
  /// a signal is stale (research R-13).
  std::function<void()> freshnessSampled;
  std::function<void()> beforeCacheCommit;
  std::function<void()> afterCacheCommit;
  std::function<void()> afterPinSnapshot;
  std::function<void()> beforeEventPublish;
  std::function<void()> afterReloadParse;
  std::function<void()> beforeReloadCommit;
  std::function<void()> onStopRequested;

  void clear() { *this = FaultHooks{}; }
};

FaultHooks& testHooks() noexcept;

}  // namespace tcu::signal::detail
