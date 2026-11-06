#pragma once

// The tcu_signal half of spec section 15.2's nine fault-injection points.
//
// Two structs rather than one shared header: tcu_dbc owns the two points inside
// the pure decoder and may not depend on this library, while every other point
// is a lifecycle point and lives here. Each module declares exactly the seam it
// has, and neither exposes it publicly (research R-23).
//
// Each hook is an ATOMIC POINTER to a callable the test owns, not a
// std::function stored here. The first version stored std::function members and
// ThreadSanitizer was right to object: worker threads read them on every frame
// while the test thread assigned and cleared them, which is a data race on the
// function object's internals however carefully the test is written. An atomic
// pointer makes the handover race-free by construction and is cheaper on the
// hot path than std::function's own bool conversion.
//
// The test still owns the lifetime: install a hook, exercise the code, and
// clear it (or stop the service) BEFORE the callable goes out of scope.
//
// src/-private: a test includes this directly because the test target puts
// src/ on its include path; a consumer of tcu_signal cannot reach it, and there
// is no #ifdef build variant.

#include <atomic>
#include <functional>

namespace tcu::signal::detail {

using Hook = std::function<void()>;

class HookSlot {
 public:
  /// Called from the test thread.
  void set(Hook* hook) noexcept { hook_.store(hook, std::memory_order_release); }
  void clear() noexcept { hook_.store(nullptr, std::memory_order_release); }

  /// Called from whatever thread reached the injection point.
  void operator()() const {
    Hook* hook = hook_.load(std::memory_order_acquire);
    if (hook != nullptr) (*hook)();
  }

 private:
  std::atomic<Hook*> hook_{nullptr};
};

struct FaultHooks {
  /// Between phase 1 and phase 2 of the freshness scan. The seam race recipe B
  /// needs: a valid frame commits while the scan has already decided a signal
  /// is stale (research R-13).
  HookSlot freshnessSampled;
  HookSlot beforeCacheCommit;
  HookSlot afterCacheCommit;
  HookSlot afterPinSnapshot;
  HookSlot beforeEventPublish;
  HookSlot afterReloadParse;
  HookSlot beforeReloadCommit;
  HookSlot onStopRequested;

  void clear() noexcept {
    freshnessSampled.clear();
    beforeCacheCommit.clear();
    afterCacheCommit.clear();
    afterPinSnapshot.clear();
    beforeEventPublish.clear();
    afterReloadParse.clear();
    beforeReloadCommit.clear();
    onStopRequested.clear();
  }
};

FaultHooks& testHooks() noexcept;

}  // namespace tcu::signal::detail
