// UT-08, UT-09 (B-08, B-09) and invariants T4, T7.
//
// The two worked examples of contracts/timing_semantics.md sections 3 and 4 are
// reproduced tick for tick. The callback consumes fake time by advancing the
// clock itself, which is safe precisely because no scheduler lock is held
// across a callback (FR-11, S1).

#include <gtest/gtest.h>

#include <atomic>
#include <vector>

#include "support/fixture.hpp"

using namespace std::chrono_literals;
using tcu::scheduler::CancelResult;
using tcu::scheduler::Config;
using tcu::scheduler::PeriodicMode;
using tcu::scheduler::StopMode;
using tcu::scheduler::TimerId;
using Periodic = tcu::test::FakeClockTest;

TEST_F(Periodic, FixedDelay_NextIsCompletionPlusInterval) {  // UT-08, B-08
  const auto base = clock_->now();
  auto s = started(Config{1, 16, 8});
  ASSERT_TRUE(s->scheduleEvery(100ms, PeriodicMode::FixedDelay, [&] {
    rec_.record(msSince(base, clock_->now()));
    clock_->advance(250ms);
  }));

  clock_->advance(100ms);
  rec_.waitFor(1);
  ASSERT_TRUE(waitForFinished(*s, 1));

  clock_->advanceTo(base + 449ms);
  EXPECT_TRUE(rec_.stayedBelow(2)) << "next start is completion plus interval, i.e. 450";
  clock_->advanceTo(base + 450ms);
  rec_.waitFor(2);

  EXPECT_EQ(rec_.tags(), (std::vector<int>{100, 450}));
  EXPECT_EQ(s->metrics().periods_skipped_total, 0u);
  s->stop(StopMode::Discard);
}

TEST_F(Periodic, FixedDelay_NeverSkips) {
  const auto base = clock_->now();
  auto s = started(Config{1, 16, 8});
  ASSERT_TRUE(s->scheduleEvery(50ms, PeriodicMode::FixedDelay, [&] {
    rec_.record(msSince(base, clock_->now()));
    clock_->advance(120ms);
  }));
  for (int fire = 1; fire <= 3; ++fire) {
    clock_->advanceTo(clock_->now() + 50ms);
    rec_.waitFor(static_cast<std::size_t>(fire));
    ASSERT_TRUE(waitForFinished(*s, static_cast<std::uint64_t>(fire)));
  }
  EXPECT_EQ(s->metrics().periods_skipped_total, 0u)
      << "fixed-delay drifts; it never skips";
  s->stop(StopMode::Discard);
}

TEST_F(Periodic, FixedDelay_NoOverlap) {  // T4
  auto s = started(Config{4, 16, 8});
  std::atomic<int> concurrent{0};
  std::atomic<int> peak{0};
  ASSERT_TRUE(s->scheduleEvery(10ms, PeriodicMode::FixedDelay, [&] {
    const int n = ++concurrent;
    int prev = peak.load();
    while (prev < n && !peak.compare_exchange_weak(prev, n)) {
    }
    rec_.record();
    --concurrent;
  }));
  for (int fire = 1; fire <= 5; ++fire) {
    clock_->advance(10ms);
    rec_.waitFor(static_cast<std::size_t>(fire));
    ASSERT_TRUE(waitForFinished(*s, static_cast<std::uint64_t>(fire)));
  }
  EXPECT_EQ(peak.load(), 1);
  s->stop(StopMode::Discard);
}

TEST_F(Periodic, FixedDelay_FiresRepeatedlyUntilCancelled) {
  auto s = started(Config{1, 16, 8});
  const TimerId id =
      s->scheduleEvery(10ms, PeriodicMode::FixedDelay, [&] { rec_.record(); }).id;
  ASSERT_NE(id, 0u);
  for (int fire = 1; fire <= 4; ++fire) {
    clock_->advance(10ms);
    rec_.waitFor(static_cast<std::size_t>(fire));
    ASSERT_TRUE(waitForFinished(*s, static_cast<std::uint64_t>(fire)));
  }
  EXPECT_EQ(s->cancel(id), CancelResult::Cancelled);
  clock_->advance(1h);
  EXPECT_TRUE(rec_.stayedBelow(5));
  s->stop(StopMode::Discard);
}

TEST_F(Periodic, FixedRate_SkipsMissedTicks) {  // UT-09, B-09
  // Slots sit at 100, 200, 300, 400. A 250-long callback starting at 100 runs
  // through 200 and 300, so two slots are skipped and the next start is 400.
  const auto base = clock_->now();
  auto s = started(Config{1, 16, 8});
  ASSERT_TRUE(s->scheduleEvery(100ms, PeriodicMode::FixedRateSkipMissed, [&] {
    rec_.record(msSince(base, clock_->now()));
    clock_->advance(250ms);
  }));

  clock_->advance(100ms);
  rec_.waitFor(1);
  ASSERT_TRUE(waitForFinished(*s, 1));
  EXPECT_EQ(s->metrics().periods_skipped_total, 2u);

  clock_->advanceTo(base + 399ms);
  EXPECT_TRUE(rec_.stayedBelow(2)) << "deadlines stay aligned to the phase";
  clock_->advanceTo(base + 400ms);
  rec_.waitFor(2);
  ASSERT_TRUE(waitForFinished(*s, 2));

  EXPECT_EQ(rec_.tags(), (std::vector<int>{100, 400}));
  EXPECT_EQ(s->metrics().periods_skipped_total, 4u);
  s->stop(StopMode::Discard);
}

TEST_F(Periodic, FixedRate_DeadlinesAlignToPhase) {
  const auto base = clock_->now();
  auto s = started(Config{1, 16, 8});
  ASSERT_TRUE(s->scheduleEvery(100ms, PeriodicMode::FixedRateSkipMissed,
                               [&] { rec_.record(msSince(base, clock_->now())); }));
  for (int fire = 1; fire <= 4; ++fire) {
    clock_->advanceTo(base + 100ms * fire);
    rec_.waitFor(static_cast<std::size_t>(fire));
    ASSERT_TRUE(waitForFinished(*s, static_cast<std::uint64_t>(fire)));
  }
  EXPECT_EQ(rec_.tags(), (std::vector<int>{100, 200, 300, 400}));
  EXPECT_EQ(s->metrics().periods_skipped_total, 0u);
  s->stop(StopMode::Discard);
}

TEST_F(Periodic, FixedRate_CompletionExactlyOnBoundaryWaitsForNextSlot) {
  // Strictly after: a callback finishing exactly on slot 1 (t=200) does not
  // fire again at once; it waits for slot 2 (t=300), and slot 1 is skipped.
  const auto base = clock_->now();
  auto s = started(Config{1, 16, 8});
  ASSERT_TRUE(s->scheduleEvery(100ms, PeriodicMode::FixedRateSkipMissed, [&] {
    rec_.record(msSince(base, clock_->now()));
    clock_->advance(100ms);
  }));

  clock_->advance(100ms);
  rec_.waitFor(1);
  ASSERT_TRUE(waitForFinished(*s, 1));
  EXPECT_EQ(s->metrics().periods_skipped_total, 1u);

  clock_->advanceTo(base + 299ms);
  EXPECT_TRUE(rec_.stayedBelow(2));
  clock_->advanceTo(base + 300ms);
  rec_.waitFor(2);
  EXPECT_EQ(rec_.tags(), (std::vector<int>{100, 300}));
  s->stop(StopMode::Discard);
}

TEST_F(Periodic, FixedRate_NoBurst) {  // T7
  const auto base = clock_->now();
  auto s = started(Config{1, 16, 8});
  ASSERT_TRUE(s->scheduleEvery(100ms, PeriodicMode::FixedRateSkipMissed, [&] {
    rec_.record(msSince(base, clock_->now()));
    clock_->advance(530ms);
  }));

  clock_->advance(100ms);
  rec_.waitFor(1);
  ASSERT_TRUE(waitForFinished(*s, 1));
  clock_->advance(1ms);
  EXPECT_TRUE(rec_.stayedBelow(2)) << "a nudge must not unleash the missed ticks";

  clock_->advanceTo(base + 700ms);
  rec_.waitFor(2);
  const auto tags = rec_.tags();
  ASSERT_EQ(tags.size(), 2u);
  EXPECT_GE(tags[1] - tags[0], 100) << "consecutive starts are an interval apart";
  s->stop(StopMode::Discard);
}

TEST_F(Periodic, FixedRate_NoOverlap) {  // T4
  auto s = started(Config{4, 16, 8});
  std::atomic<int> concurrent{0};
  std::atomic<int> peak{0};
  ASSERT_TRUE(s->scheduleEvery(10ms, PeriodicMode::FixedRateSkipMissed, [&] {
    const int n = ++concurrent;
    int prev = peak.load();
    while (prev < n && !peak.compare_exchange_weak(prev, n)) {
    }
    rec_.record();
    --concurrent;
  }));
  for (int fire = 1; fire <= 5; ++fire) {
    clock_->advance(10ms);
    rec_.waitFor(static_cast<std::size_t>(fire));
    ASSERT_TRUE(waitForFinished(*s, static_cast<std::uint64_t>(fire)));
  }
  EXPECT_EQ(peak.load(), 1);
  s->stop(StopMode::Discard);
}

TEST_F(Periodic, HoldsOneCapacitySlotAcrossManyFires) {  // research R-03
  auto s = started(Config{1, 1, 8});
  ASSERT_TRUE(s->scheduleEvery(10ms, PeriodicMode::FixedDelay, [&] { rec_.record(); }));
  for (int fire = 1; fire <= 3; ++fire) {
    clock_->advance(10ms);
    rec_.waitFor(static_cast<std::size_t>(fire));
    ASSERT_TRUE(waitForFinished(*s, static_cast<std::uint64_t>(fire)));
    EXPECT_EQ(s->metrics().timers_active, 1u) << "after fire " << fire;
  }
  s->stop(StopMode::Discard);
}
