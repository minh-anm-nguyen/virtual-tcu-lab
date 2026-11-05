#pragma once

// Change detection, as a fixed-delay timer on the scheduler the service was
// handed - not a thread of its own, and not inotify (research R-03).
//
// shortcut: the probe compares last_write_time and file_size, so a write that
// preserves both is missed, and detection latency is up to one interval.
// Upgrade when a consumer needs sub-interval latency or same-size-same-mtime
// detection; then add a content hash or a platform watcher behind this same
// interface.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>

#include "tcu/scheduler/timer_scheduler.hpp"

namespace tcu::config::internal {

class FilePoller {
 public:
  FilePoller(tcu::scheduler::TimerScheduler& scheduler,
             std::filesystem::path path,
             std::chrono::milliseconds interval,
             std::function<void()> on_change);

  // Arms the timer. A zero interval arms nothing, leaving explicit reload as
  // the only trigger.
  void start();

  // Cancels the timer. Idempotent; a probe already running may finish.
  void stop();

 private:
  void probe();
  void probeInitial();

  tcu::scheduler::TimerScheduler& scheduler_;
  std::filesystem::path path_;
  std::chrono::milliseconds interval_;
  std::function<void()> on_change_;
  tcu::scheduler::TimerId timer_{0};

  // The stat as of the last probe. Updated whether the reload that followed
  // succeeded or was rejected, so a bad file is read once rather than on every
  // interval for the life of the process.
  bool have_stat_{false};
  std::int64_t last_write_{0};
  std::uintmax_t last_size_{0};
};

}  // namespace tcu::config::internal
