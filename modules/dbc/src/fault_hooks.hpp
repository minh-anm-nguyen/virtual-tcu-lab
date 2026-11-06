#pragma once

// The two fault-injection points inside the pure decoder (spec section 15.2).
//
// Atomic pointers to callables the test owns, for the reason the tcu_signal
// copy of this header explains at length: std::function members here were a
// data race between the decoder worker reading them and the test thread
// assigning them, and ThreadSanitizer was right about it.
//
// src/-private on purpose: section 15.2 says not to put test hooks into the
// public production API (research R-23).

#include <atomic>
#include <functional>

namespace tcu::dbc::detail {

using Hook = std::function<void()>;

class HookSlot {
 public:
  void set(Hook* hook) noexcept { hook_.store(hook, std::memory_order_release); }
  void clear() noexcept { hook_.store(nullptr, std::memory_order_release); }

  void operator()() const {
    Hook* hook = hook_.load(std::memory_order_acquire);
    if (hook != nullptr) (*hook)();
  }

 private:
  std::atomic<Hook*> hook_{nullptr};
};

struct FaultHooks {
  HookSlot beforeValidate;
  HookSlot beforeBitRead;

  void clear() noexcept {
    beforeValidate.clear();
    beforeBitRead.clear();
  }
};

FaultHooks& testHooks() noexcept;

}  // namespace tcu::dbc::detail
