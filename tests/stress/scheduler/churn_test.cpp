// ST-03 and the fault matrix of spec section 14, rows 1, 3 and 7.

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "support/fixture.hpp"
#include "support/recorder.hpp"
#include "support/test_gate.hpp"
#include "tcu/scheduler/timer_scheduler.hpp"

using namespace std::chrono_literals;
using tcu::scheduler::CancelResult;
using tcu::scheduler::Config;
using tcu::scheduler::Error;
using tcu::scheduler::StopMode;
using tcu::scheduler::TimerId;
using tcu::scheduler::TimerScheduler;
using Churn = tcu::test::FakeClockTest;

TEST_F(Churn, TimerChurnLeavesNoStaleCallback) {  // spec section 14, row 3
  // Rapid create/cancel against a hundred long-lived timers. The heap-size
  // bound itself is asserted at unit level (H3); here the question is whether
  // any cancelled timer can still fire.
  auto s = started(Config{2, 4096, 64});

  std::vector<TimerId> alive;
  for (int i = 0; i < 100; ++i) {
    const TimerId id = s->scheduleAfter(10h, [this] { rec_.record(1); }).id;
    ASSERT_NE(id, 0u);
    alive.push_back(id);
  }

  for (int cycle = 0; cycle < 20000; ++cycle) {
    const auto r = s->scheduleAfter(1ms, [this] { rec_.record(2); });
    ASSERT_TRUE(r) << "cycle " << cycle;
    ASSERT_EQ(s->cancel(r.id), CancelResult::Cancelled);
  }

  EXPECT_EQ(s->metrics().timers_active, 100u);
  clock_->advance(1s);  // every churned deadline is long past
  EXPECT_TRUE(rec_.stayedBelow(1)) << "a cancelled timer fired";
  EXPECT_EQ(s->metrics().timers_cancelled_total, 20000u);
  s->stop(StopMode::Discard);
}

TEST_F(Churn, BlockingCallbackDoesNotUnboundQueue) {  // spec section 14, row 1
  // A callback that never returns on its own would block stop() for ever
  // (research R-04), so the test releases it before joining. While it is held,
  // memory must stay inside both configured caps.
  constexpr std::size_t kMaxTimers = 64;
  constexpr std::size_t kMaxReady = 4;
  auto s = started(Config{1, kMaxTimers, kMaxReady});
  tcu::test::TestGate gate;

  ASSERT_TRUE(s->scheduleAfter(0ms, [&] { gate.wait_until_opened(); }));
  gate.wait_until_entered();

  std::size_t accepted = 0;
  for (int i = 0; i < 500; ++i) {
    if (s->scheduleAfter(0ms, [this] { rec_.record(); })) ++accepted;
    const auto m = s->metrics();
    ASSERT_LE(m.ready_queue_depth, kMaxReady);
    ASSERT_LE(m.timers_active, kMaxTimers);
  }
  EXPECT_LT(accepted, 500u) << "the bounded registry must start refusing";
  EXPECT_GE(s->metrics().rejected_capacity_total, 1u);

  gate.open();  // release before joining, or stop() would never return
  s->stop(StopMode::Drain);
  EXPECT_EQ(s->metrics().ready_queue_depth, 0u);
}

TEST_F(Churn, OwnerDestroyedCallbackUsesWeakPtr) {  // spec section 14, row 7
  // The scheduler cannot keep a caller alive; the documented pattern is weak
  // ownership plus cancellation. A dangling deref would be an ASan finding.
  struct Owner {
    std::atomic<int> calls{0};
  };

  auto s = started(Config{2, 64, 16});
  std::weak_ptr<Owner> weak;
  {
    auto owner = std::make_shared<Owner>();
    weak = owner;
    ASSERT_TRUE(s->scheduleEvery(10ms, tcu::scheduler::PeriodicMode::FixedDelay,
                                 [weak, this] {
                                   if (auto strong = weak.lock()) {
                                     ++strong->calls;
                                     rec_.record(1);
                                   } else {
                                     rec_.record(2);  // observed the death safely
                                   }
                                 }));
    clock_->advance(10ms);
    rec_.waitFor(1);
    ASSERT_TRUE(waitForFinished(*s, 1));
  }  // the owner dies while the timer is still armed

  EXPECT_TRUE(weak.expired());
  clock_->advance(10ms);
  rec_.waitFor(2);
  const auto tags = rec_.tags();
  EXPECT_EQ(tags.front(), 1);
  EXPECT_EQ(tags.back(), 2) << "the callback must see the expiry, not deref it";
  s->stop(StopMode::Discard);
}
