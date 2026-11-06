// T039: freshness is exact at the deadline, derived from the last-good
// timestamp, and a late timer can never overwrite a value that just landed.
// Invariants S-05, S-10; contracts B-09, B-10; research R-13 (race recipe B).

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "fault_hooks.hpp"
#include "tcu/dbc/dbc_parser.hpp"
#include "tcu/signal/signal_cache.hpp"

namespace {

using tcu::dbc::SignalQuality;
using tcu::signal::QualityEventKind;
using tcu::signal::SignalCache;
using tcu::signal::SignalKey;
using tcu::signal::SteadyTime;

SteadyTime at(long long ms) { return SteadyTime{std::chrono::milliseconds{ms}}; }

tcu::dbc::DecodedBatch validAt(long long rxMs, double physical) {
  tcu::dbc::SignalValue value;
  value.signalName = "VehicleSpeed";
  value.physical = physical;
  value.quality = SignalQuality::Valid;

  tcu::dbc::DecodedBatch batch;
  batch.arbitrationId = 256;
  batch.format = tcu::dbc::FrameFormat::Standard11;
  batch.messageName = "VEHICLE_STATUS";
  batch.dbcVersion = 1;
  batch.rxTime = at(rxMs);
  batch.signals.push_back(value);
  return batch;
}

SignalKey speedKey() { return SignalKey{0, false, 256, "VehicleSpeed"}; }

tcu::signal::FreshnessConfig freshnessOf(long long defaultMs) {
  tcu::signal::FreshnessConfig config;
  config.defaultTimeout = std::chrono::milliseconds{defaultMs};
  return config;
}

class FreshnessTest : public ::testing::Test {
 protected:
  // Belt and braces: each test that installs a hook also clears it before its
  // callable goes out of scope. TearDown runs after the test body's locals are
  // already gone, so it cannot be the only clear.
  void TearDown() override { tcu::signal::detail::testHooks().clear(); }
};

TEST_F(FreshnessTest, ExactDeadlineIsStale) {
  // B-09: with a 500 ms timeout and last-good at t=1000, t=1499 is fresh and
  // t=1500 is stale. The boundary belongs to the stale side.
  SignalCache cache(freshnessOf(500));
  cache.commit(validAt(1000, 100.0));

  EXPECT_FALSE(cache.get(speedKey(), at(1499))->lastGoodIsStale);
  EXPECT_TRUE(cache.get(speedKey(), at(1500))->lastGoodIsStale);
  EXPECT_TRUE(cache.get(speedKey(), at(1501))->lastGoodIsStale);
}

TEST_F(FreshnessTest, StaleDoesNotClearLastGood) {
  SignalCache cache(freshnessOf(500));
  cache.commit(validAt(1000, 100.0));
  const auto events = cache.scanFreshness(at(1500));

  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].kind, QualityEventKind::SignalQualityChanged);
  EXPECT_EQ(events[0].to, SignalQuality::Stale);

  const auto snapshot = cache.get(speedKey(), at(1500));
  ASSERT_TRUE(snapshot.has_value());
  ASSERT_TRUE(snapshot->lastGood.has_value()) << "a stale value is still readable (B-09)";
  EXPECT_DOUBLE_EQ(snapshot->lastGood->physical, 100.0);
  EXPECT_EQ(snapshot->lastGoodRxTime, at(1000));
}

TEST_F(FreshnessTest, StaleIsAnnouncedOnlyOnce) {
  SignalCache cache(freshnessOf(500));
  cache.commit(validAt(1000, 100.0));
  EXPECT_EQ(cache.scanFreshness(at(1500)).size(), 1u);
  EXPECT_TRUE(cache.scanFreshness(at(1600)).empty()) << "no repeated stale events";
  EXPECT_TRUE(cache.scanFreshness(at(9000)).empty());
}

TEST_F(FreshnessTest, RecoveryAfterValid) {
  // B-10: a valid frame after a stale period recovers, with a new timestamp.
  SignalCache cache(freshnessOf(500));
  cache.commit(validAt(1000, 100.0));
  cache.scanFreshness(at(1500));

  const auto events = cache.commit(validAt(1600, 120.0));
  bool recovered = false;
  for (const auto& event : events) {
    if (event.kind == QualityEventKind::SignalRecovered) recovered = true;
  }
  EXPECT_TRUE(recovered) << "Stale -> Valid must emit SignalRecovered";

  const auto snapshot = cache.get(speedKey(), at(1600));
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_FALSE(snapshot->lastGoodIsStale);
  EXPECT_EQ(snapshot->lastGoodRxTime, at(1600));
  EXPECT_DOUBLE_EQ(snapshot->lastGood->physical, 120.0);
  EXPECT_EQ(snapshot->epoch, 2u);
}

TEST_F(FreshnessTest, NeverReceivedIsNotStale) {
  SignalCache cache(freshnessOf(500));
  const auto loaded = tcu::dbc::loadDbcBytes(
      "VERSION \"lab\"\n"
      "BO_ 256 VEHICLE_STATUS: 8 ECU\n"
      " SG_ VehicleSpeed : 0|16@1+ (0.01,0) [0|250] \"km/h\" TCU\n",
      "lab.dbc");
  ASSERT_TRUE(loaded.ok());
  cache.seed(*loaded.db);

  EXPECT_TRUE(cache.scanFreshness(at(999999)).empty())
      << "a signal that was never received cannot time out";
  const auto snapshot = cache.get(speedKey(), at(999999));
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ(snapshot->lastObservationQuality, SignalQuality::NotReceived);
  EXPECT_FALSE(snapshot->lastGoodIsStale);
}

TEST_F(FreshnessTest, PerSignalOverrideApplies) {
  tcu::signal::FreshnessConfig config = freshnessOf(500);
  config.overrides["VEHICLE_STATUS.VehicleSpeed"] = std::chrono::milliseconds{2000};
  SignalCache cache(config);
  cache.commit(validAt(1000, 100.0));

  EXPECT_FALSE(cache.get(speedKey(), at(1500))->lastGoodIsStale)
      << "the default must not win over the override";
  EXPECT_TRUE(cache.get(speedKey(), at(3000))->lastGoodIsStale);
}

TEST_F(FreshnessTest, EpochRecheckBeatsLateTimer) {
  // Race recipe B (spec section 15.3): the scan samples a signal as stale, a
  // valid frame commits before the scan writes its verdict, and the scan must
  // then NOT overwrite Valid with Stale. The hook is the rendezvous, so this
  // is deterministic rather than a sleep-and-hope (research R-13, S-10).
  SignalCache cache(freshnessOf(500));
  cache.commit(validAt(1000, 100.0));

  bool committed = false;
  tcu::signal::detail::Hook hook = [&] {
    // Runs between phase 1 and phase 2 of the scan, on this same thread: no
    // gate is needed to order it, which is why this case is deterministic.
    cache.commit(validAt(1490, 120.0));
    committed = true;
  };
  tcu::signal::detail::testHooks().freshnessSampled.set(&hook);

  const auto events = cache.scanFreshness(at(1500));
  tcu::signal::detail::testHooks().freshnessSampled.clear();
  EXPECT_TRUE(committed);
  EXPECT_TRUE(events.empty()) << "a stale verdict from before the commit must be dropped";

  const auto snapshot = cache.get(speedKey(), at(1500));
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ(snapshot->lastObservationQuality, SignalQuality::Valid);
  EXPECT_DOUBLE_EQ(snapshot->lastGood->physical, 120.0);
  EXPECT_EQ(snapshot->epoch, 2u);
}

TEST_F(FreshnessTest, EpochRecheckUnderRealThreads) {
  // The same hazard with two threads and 2,000 iterations.
  //
  // Note what is NOT asserted here: whether a stale event was emitted. With a
  // real thread the commit may land either side of phase 2, so both "stale
  // emitted, then recovered" and "no stale at all" are correct outcomes, and a
  // test that demanded one would be asserting a scheduling accident. The
  // deterministic case above is what pins the epoch recheck itself.
  //
  // What must hold under every interleaving is that the valid value wins the
  // final state: a signal holding a fresh value must never be left reporting
  // Stale, and no update may be lost (invariants S-01, S-10).
  for (int iteration = 0; iteration < 2000; ++iteration) {
    SignalCache cache(freshnessOf(500));
    cache.commit(validAt(1000, 100.0));

    std::thread writer;
    tcu::signal::detail::Hook hook = [&] {
      writer = std::thread([&] { cache.commit(validAt(1490, 120.0)); });
    };
    tcu::signal::detail::testHooks().freshnessSampled.set(&hook);
    (void)cache.scanFreshness(at(1500));
    // Cleared and joined before `hook` dies at the end of this iteration.
    tcu::signal::detail::testHooks().freshnessSampled.clear();
    if (writer.joinable()) writer.join();

    const auto snapshot = cache.get(speedKey(), at(1500));
    ASSERT_TRUE(snapshot.has_value()) << "iteration " << iteration;
    ASSERT_EQ(snapshot->epoch, 2u) << "the commit must never be lost, iteration " << iteration;
    ASSERT_EQ(snapshot->lastObservationQuality, SignalQuality::Valid)
        << "a fresh value was left reporting stale, iteration " << iteration;
    ASSERT_DOUBLE_EQ(snapshot->lastGood->physical, 120.0) << "iteration " << iteration;
    ASSERT_FALSE(snapshot->lastGoodIsStale) << "iteration " << iteration;
  }
}

}  // namespace
