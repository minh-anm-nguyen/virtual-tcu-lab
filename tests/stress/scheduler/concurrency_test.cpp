// ST-01 (B-17), ST-02 and spec section 14 rows 5 and 6.
//
// Duration comes from TCU_SCHEDULER_STRESS_SECONDS (default 5) so CI finishes;
// raise it for a soak run.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "support/recorder.hpp"
#include "tcu/scheduler/timer_scheduler.hpp"

using namespace std::chrono_literals;
using tcu::scheduler::CancelResult;
using tcu::scheduler::Config;
using tcu::scheduler::Error;
using tcu::scheduler::StopMode;
using tcu::scheduler::TimerId;
using tcu::scheduler::TimerScheduler;
using tcu::test::Recorder;

namespace {

std::chrono::seconds stressDuration() {
  const char* raw = std::getenv("TCU_SCHEDULER_STRESS_SECONDS");
  if (raw == nullptr) return 5s;
  const int seconds = std::atoi(raw);
  return std::chrono::seconds(seconds > 0 ? seconds : 5);
}

}  // namespace

TEST(Stress, ConcurrentScheduleCancel) {  // ST-01, B-17
  // Eight threads scheduling and cancelling at once. The assertion is not a
  // count but a conservation law: every scheduled timer reached exactly one
  // outcome, and no callback went uncounted.
  TimerScheduler s(Config{4, 4096, 256});
  ASSERT_EQ(s.start(), Error::None);

  std::atomic<bool> running{true};
  std::atomic<std::uint64_t> accepted{0};
  std::vector<std::thread> threads;

  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937 rng(static_cast<unsigned>(t) + 1u);
      std::uniform_int_distribution<int> delay(0, 4);
      std::uniform_int_distribution<int> coin(0, 2);
      while (running.load()) {
        const auto r = s.scheduleAfter(std::chrono::milliseconds(delay(rng)), [] {});
        if (!r) continue;  // CapacityExceeded under pressure is a valid answer
        ++accepted;
        if (coin(rng) == 0) s.cancel(r.id);
      }
    });
  }

  const auto deadline = std::chrono::steady_clock::now() + stressDuration();
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  running = false;
  for (std::thread& th : threads) th.join();

  s.stop(StopMode::Drain);
  const auto m = s.metrics();
  EXPECT_EQ(m.timers_scheduled_total, accepted.load());
  EXPECT_EQ(m.callbacks_started_total,
            m.callbacks_completed_total + m.callbacks_failed_total);  // M1
  EXPECT_EQ(m.timers_active, 0u);
  EXPECT_EQ(m.timers_scheduled_total, m.callbacks_completed_total +
                                          m.callbacks_failed_total +
                                          m.timers_cancelled_total)
      << "a timer reached zero or two outcomes";
}

TEST(Stress, RepeatedFreshInstances) {  // ST-02, invariant S2
  // The scheduler is single-use, so a restart means a fresh instance. Two
  // hundred of them must leave no lingering thread; a leak shows up as the
  // CTest timeout or as an exhausted thread limit.
  for (int cycle = 0; cycle < 200; ++cycle) {
    TimerScheduler s(Config{2, 32, 8});
    ASSERT_EQ(s.start(), Error::None) << "cycle " << cycle;
    Recorder rec;
    for (int i = 0; i < 4; ++i) {
      ASSERT_TRUE(s.scheduleAfter(0ms, [&rec] { rec.record(); }));
    }
    s.stop(cycle % 2 == 0 ? StopMode::Drain : StopMode::Discard);
    EXPECT_EQ(s.metrics().ready_queue_depth, 0u) << "cycle " << cycle;
  }
}

TEST(Stress, StopRacesSchedule) {  // spec section 14, row 5
  // Either the schedule was accepted before the transition, or it was
  // rejected; and nothing runs after stop() returns.
  for (int attempt = 0; attempt < 20; ++attempt) {
    TimerScheduler s(Config{2, 512, 64});
    ASSERT_EQ(s.start(), Error::None);
    Recorder rec;

    std::atomic<bool> go{false};
    std::atomic<int> accepted{0};
    std::atomic<int> rejected{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
      threads.emplace_back([&] {
        while (!go.load()) std::this_thread::yield();
        for (int i = 0; i < 50; ++i) {
          const auto r = s.scheduleAfter(0ms, [&rec] { rec.record(); });
          if (r) {
            ++accepted;
          } else {
            EXPECT_TRUE(r.error == Error::InvalidState ||
                        r.error == Error::CapacityExceeded)
                << static_cast<int>(r.error);
            ++rejected;
          }
        }
      });
    }

    go = true;
    s.stop(StopMode::Drain);
    for (std::thread& th : threads) th.join();

    const std::size_t after_stop = rec.count();
    EXPECT_TRUE(rec.stayedBelow(after_stop + 1))
        << "a callback ran after stop() returned";
    EXPECT_EQ(accepted.load() + rejected.load(), 200);
  }
}

TEST(Stress, ConcurrentCancelSameId) {  // spec section 14, row 6
  // One transition wins; every other caller gets a stable answer.
  for (int attempt = 0; attempt < 200; ++attempt) {
    TimerScheduler s(Config{2, 32, 8});
    ASSERT_EQ(s.start(), Error::None);
    Recorder rec;
    const TimerId id = s.scheduleAfter(1h, [&rec] { rec.record(); }).id;
    ASSERT_NE(id, 0u);

    std::atomic<bool> go{false};
    std::atomic<int> cancelled{0};
    std::atomic<int> already{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
      threads.emplace_back([&] {
        while (!go.load()) std::this_thread::yield();
        switch (s.cancel(id)) {
          case CancelResult::Cancelled: ++cancelled; break;
          case CancelResult::AlreadyFinished: ++already; break;
          default: ADD_FAILURE() << "unexpected cancel result"; break;
        }
      });
    }
    go = true;
    for (std::thread& th : threads) th.join();

    EXPECT_EQ(cancelled.load(), 1) << "exactly one cancellation may win";
    EXPECT_EQ(already.load(), 7);
    s.stop(StopMode::Discard);
    EXPECT_EQ(rec.count(), 0u);
  }
}
