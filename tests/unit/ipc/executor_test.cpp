// T012: the private callback executor. research R-05.

#include "executor.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

namespace {

using tcu::ipc::detail::Executor;

TEST(Executor, RunsTaskOffTheCallingThread) {
  Executor ex(16);
  ex.start();
  std::promise<std::thread::id> where;
  auto got = where.get_future();
  ASSERT_TRUE(ex.post([&where] { where.set_value(std::this_thread::get_id()); }));
  ASSERT_EQ(got.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  EXPECT_NE(got.get(), std::this_thread::get_id());
  ex.stop();
}

TEST(Executor, StopDrainRunsQueuedTasksThenJoins) {
  Executor ex(64);
  ex.start();
  std::atomic<int> ran{0};
  // Hold the worker so the remaining tasks must be drained by stop().
  std::promise<void> gate;
  auto wait_for_gate = gate.get_future();
  ASSERT_TRUE(ex.post([&wait_for_gate] { wait_for_gate.wait(); }));
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(ex.post([&ran] { ran.fetch_add(1); }));
  }
  gate.set_value();
  ex.stop(tcu::ipc::detail::DrainPolicy::Drain);
  EXPECT_EQ(ran.load(), 10);
}

TEST(Executor, StopDiscardIsBoundedAndCountsWhatItDropped) {
  Executor ex(256);
  ex.start();
  std::atomic<int> ran{0};
  std::promise<void> gate;
  auto wait_for_gate = gate.get_future();
  ASSERT_TRUE(ex.post([&wait_for_gate] { wait_for_gate.wait(); }));
  for (int i = 0; i < 100; ++i) {
    // Each would take 50 ms, so draining them would need 5 s - far past the
    // 3 s shutdown budget of spec section 3.2.
    ASSERT_TRUE(ex.post([&ran] {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      ran.fetch_add(1);
    }));
  }
  gate.set_value();

  const auto before = std::chrono::steady_clock::now();
  ex.stop();  // Discard is the default
  const auto elapsed = std::chrono::steady_clock::now() - before;

  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 1000);
  EXPECT_LT(ran.load(), 100) << "Discard still ran every queued task";
  // Dropped work is counted, never silently lost (Principle V).
  EXPECT_GT(ex.tasksDiscarded(), 0u);
}

TEST(Executor, SeveralWorkersRunTasksConcurrently) {
  // The server needs this: with one thread, a slow handler starves the others
  // (spec section 7, "worker executor N threads").
  Executor ex(64, 4);
  ex.start();
  std::atomic<int> finished{0};
  const auto before = std::chrono::steady_clock::now();
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(ex.post([&finished] {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      finished.fetch_add(1);
    }));
  }
  ex.stop(tcu::ipc::detail::DrainPolicy::Drain);
  const auto elapsed = std::chrono::steady_clock::now() - before;
  EXPECT_EQ(finished.load(), 4);
  // Serialised they would need 800 ms; four workers need about 200 ms.
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 600);
}

TEST(Executor, TaskExceptionIsCaughtCountedAndWorkerSurvives) {
  Executor ex(16);
  ex.start();
  ASSERT_TRUE(ex.post([] { throw std::runtime_error("boom"); }));
  std::promise<void> done;
  auto finished = done.get_future();
  ASSERT_TRUE(ex.post([&done] { done.set_value(); }));
  ASSERT_EQ(finished.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  EXPECT_EQ(ex.taskFailures(), 1u);
  ex.stop();
}

TEST(Executor, PostAfterStopIsRefusedNotSilentlyDropped) {
  Executor ex(16);
  ex.start();
  ex.stop();
  std::atomic<bool> ran{false};
  EXPECT_FALSE(ex.post([&ran] { ran.store(true); }));
  EXPECT_FALSE(ran.load());
}

TEST(Executor, PostBeyondCapacityIsRefused) {
  Executor ex(2);
  ex.start();
  std::promise<void> gate;
  auto wait_for_gate = gate.get_future();
  ASSERT_TRUE(ex.post([&wait_for_gate] { wait_for_gate.wait(); }));
  // The worker is busy, so the queue fills and the cap must bite.
  int accepted = 0;
  for (int i = 0; i < 10; ++i) {
    if (ex.post([] {})) {
      ++accepted;
    }
  }
  EXPECT_LE(accepted, 2);
  gate.set_value();
  ex.stop();
}

TEST(Executor, StopIsIdempotent) {
  Executor ex(4);
  ex.start();
  ex.stop();
  ex.stop();
  SUCCEED();
}

}  // namespace
