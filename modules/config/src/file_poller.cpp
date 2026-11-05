#include "file_poller.hpp"

#include <system_error>
#include <utility>

namespace tcu::config::internal {

FilePoller::FilePoller(tcu::scheduler::TimerScheduler& scheduler,
                       std::filesystem::path path,
                       std::chrono::milliseconds interval,
                       std::function<void()> on_change)
    : scheduler_(scheduler),
      path_(std::move(path)),
      interval_(interval),
      on_change_(std::move(on_change)) {}

void FilePoller::start() {
  if (interval_.count() <= 0) {
    return;  // explicit reload only
  }
  // The first probe records the current stat without calling back: the file the
  // service just loaded is not a change.
  probeInitial();
  const auto result = scheduler_.scheduleEvery(
      interval_, tcu::scheduler::PeriodicMode::FixedDelay, [this] { probe(); });
  if (result) {
    timer_ = result.id;
  }
}

void FilePoller::stop() {
  if (timer_ != 0) {
    scheduler_.cancel(timer_);
    timer_ = 0;
  }
}

void FilePoller::probeInitial() {
  std::error_code ec;
  const auto write_time = std::filesystem::last_write_time(path_, ec);
  if (!ec) {
    last_write_ = write_time.time_since_epoch().count();
    last_size_ = std::filesystem::file_size(path_, ec);
    have_stat_ = !ec;
  }
}

void FilePoller::probe() {
  std::error_code ec;
  const auto write_time = std::filesystem::last_write_time(path_, ec);
  if (ec) {
    // A file that is not there is not a change: the last known good snapshot
    // stays in force and the next interval looks again. No spin, no reload.
    return;
  }
  const std::int64_t stamp = write_time.time_since_epoch().count();
  const std::uintmax_t size = std::filesystem::file_size(path_, ec);
  if (ec) {
    return;
  }

  if (have_stat_ && stamp == last_write_ && size == last_size_) {
    return;
  }

  // Record BEFORE calling back, so a candidate that is rejected is not read
  // again on the next interval (and a burst of writes inside one interval
  // collapses into this single probe - BC-13).
  have_stat_ = true;
  last_write_ = stamp;
  last_size_ = size;

  if (on_change_) {
    on_change_();
  }
}

}  // namespace tcu::config::internal
