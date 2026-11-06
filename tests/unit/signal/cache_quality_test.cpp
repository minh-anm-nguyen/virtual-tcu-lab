// T036: spec section 9.2's transition matrix, row by row. The invariant under
// all of it: a bad frame can neither produce nor destroy a good value
// (invariants S-01..S-09, contracts B-06, B-08).

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "tcu/dbc/dbc_parser.hpp"
#include "tcu/signal/signal_cache.hpp"

namespace {

using tcu::dbc::SignalQuality;
using tcu::signal::QualityEventKind;
using tcu::signal::SignalCache;
using tcu::signal::SignalKey;
using tcu::signal::SteadyTime;

SteadyTime at(long long ms) { return SteadyTime{std::chrono::milliseconds{ms}}; }

tcu::dbc::SignalValue valueOf(const char* name, double physical, SignalQuality quality,
                              std::uint64_t raw = 0) {
  tcu::dbc::SignalValue value;
  value.signalName = name;
  value.physical = physical;
  value.quality = quality;
  value.raw = raw;
  return value;
}

tcu::dbc::DecodedBatch batchOf(long long rxMs, std::vector<tcu::dbc::SignalValue> signals) {
  tcu::dbc::DecodedBatch batch;
  batch.arbitrationId = 256;
  batch.busId = 0;
  batch.format = tcu::dbc::FrameFormat::Standard11;
  batch.messageName = "VEHICLE_STATUS";
  batch.dbcVersion = 1;
  batch.dbcHash = "deadbeef";
  batch.rxTime = at(rxMs);
  batch.signals = std::move(signals);
  return batch;
}

SignalKey speedKey() { return SignalKey{0, false, 256, "VehicleSpeed"}; }

tcu::signal::FreshnessConfig defaultFreshness() {
  tcu::signal::FreshnessConfig config;
  config.defaultTimeout = std::chrono::milliseconds{500};
  return config;
}

bool hasKind(const std::vector<tcu::signal::QualityEvent>& events, QualityEventKind kind) {
  for (const auto& event : events) {
    if (event.kind == kind) return true;
  }
  return false;
}

TEST(Cache_ValidUpdatesLastGood, FirstValidValueBecomesLastGood) {
  SignalCache cache(defaultFreshness());
  const auto events = cache.commit(
      batchOf(1000, {valueOf("VehicleSpeed", 100.0, SignalQuality::Valid, 10000)}));

  const auto snapshot = cache.get(speedKey(), at(1000));
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ(snapshot->lastObservationQuality, SignalQuality::Valid);
  ASSERT_TRUE(snapshot->lastGood.has_value());
  EXPECT_DOUBLE_EQ(snapshot->lastGood->physical, 100.0);
  EXPECT_EQ(snapshot->lastGoodRxTime, at(1000));
  EXPECT_EQ(snapshot->epoch, 1u);
  EXPECT_FALSE(snapshot->lastGoodIsStale);
  EXPECT_TRUE(hasKind(events, QualityEventKind::SignalUpdated));
}

TEST(Cache_InvalidKeepsLastGood, EveryNonValidQualityLeavesTheValueAlone) {
  const SignalQuality bad[] = {SignalQuality::InvalidRaw, SignalQuality::OutOfRange,
                               SignalQuality::DecodeError, SignalQuality::NotActive,
                               SignalQuality::ConfigUnavailable};
  for (const SignalQuality quality : bad) {
    SignalCache cache(defaultFreshness());
    cache.commit(batchOf(1000, {valueOf("VehicleSpeed", 100.0, SignalQuality::Valid, 10000)}));
    const auto events =
        cache.commit(batchOf(1100, {valueOf("VehicleSpeed", 999.0, quality, 65535)}));

    const auto snapshot = cache.get(speedKey(), at(1100));
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->lastObservationQuality, quality);
    ASSERT_TRUE(snapshot->lastGood.has_value());
    EXPECT_DOUBLE_EQ(snapshot->lastGood->physical, 100.0) << "last-good must not change";
    EXPECT_EQ(snapshot->lastGoodRxTime, at(1000)) << "freshness must not be refreshed";
    EXPECT_EQ(snapshot->epoch, 1u) << "the epoch must not move";
    EXPECT_TRUE(hasKind(events, QualityEventKind::SignalQualityChanged));
  }
}

TEST(Cache_OutOfOrderFrameIgnored, LastGoodRxTimeNeverGoesBackwards) {
  SignalCache cache(defaultFreshness());
  cache.commit(batchOf(2000, {valueOf("VehicleSpeed", 100.0, SignalQuality::Valid)}));
  const auto events =
      cache.commit(batchOf(1500, {valueOf("VehicleSpeed", 50.0, SignalQuality::Valid)}));

  const auto snapshot = cache.get(speedKey(), at(2000));
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_DOUBLE_EQ(snapshot->lastGood->physical, 100.0);
  EXPECT_EQ(snapshot->lastGoodRxTime, at(2000));
  EXPECT_EQ(snapshot->epoch, 1u);
  EXPECT_TRUE(events.empty());
  EXPECT_EQ(cache.outOfOrderCount(), 1u);
}

TEST(Cache_EpochIncrementsOnCommit, OnePerAcceptedValue) {
  SignalCache cache(defaultFreshness());
  for (int i = 1; i <= 5; ++i) {
    cache.commit(batchOf(1000 + i, {valueOf("VehicleSpeed", i, SignalQuality::Valid)}));
    const auto snapshot = cache.get(speedKey(), at(1000 + i));
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->epoch, static_cast<std::uint64_t>(i));
  }
}

TEST(Cache_QueryReturnsBothAxes, InvalidAndTimedOutAtTheSameTime) {
  // Spec section 9.2's "Important": one enum cannot express this, so the
  // snapshot carries the observation and the staleness separately (R-12).
  SignalCache cache(defaultFreshness());
  cache.commit(batchOf(1000, {valueOf("VehicleSpeed", 100.0, SignalQuality::Valid)}));
  cache.commit(batchOf(1100, {valueOf("VehicleSpeed", 0.0, SignalQuality::InvalidRaw)}));

  const auto snapshot = cache.get(speedKey(), at(1600));
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ(snapshot->lastObservationQuality, SignalQuality::InvalidRaw);
  EXPECT_TRUE(snapshot->lastGoodIsStale);
  ASSERT_TRUE(snapshot->lastGood.has_value());
  EXPECT_DOUBLE_EQ(snapshot->lastGood->physical, 100.0);
}

TEST(Cache_RepeatedIdenticalObservationEmitsNoQualityEvent, OnlyTheBatchEvent) {
  SignalCache cache(defaultFreshness());
  cache.commit(batchOf(1000, {valueOf("VehicleSpeed", 100.0, SignalQuality::Valid)}));
  const auto again =
      cache.commit(batchOf(1050, {valueOf("VehicleSpeed", 101.0, SignalQuality::Valid)}));
  EXPECT_TRUE(hasKind(again, QualityEventKind::SignalUpdated));
  EXPECT_FALSE(hasKind(again, QualityEventKind::SignalQualityChanged));

  SignalCache invalidTwice(defaultFreshness());
  invalidTwice.commit(batchOf(1000, {valueOf("VehicleSpeed", 0.0, SignalQuality::InvalidRaw)}));
  const auto second = invalidTwice.commit(
      batchOf(1050, {valueOf("VehicleSpeed", 0.0, SignalQuality::InvalidRaw)}));
  EXPECT_TRUE(second.empty()) << "the same invalid observation twice is not a change";
}

TEST(Cache_UnknownSignalIsDistinctFromNeverReceived, NulloptVersusNotReceived) {
  SignalCache cache(defaultFreshness());
  EXPECT_FALSE(cache.get(SignalKey{0, false, 999, "Nope"}, at(0)).has_value());

  cache.commit(batchOf(1000, {valueOf("VehicleSpeed", 0.0, SignalQuality::InvalidRaw)}));
  const auto known = cache.get(speedKey(), at(1000));
  ASSERT_TRUE(known.has_value());
  EXPECT_FALSE(known->lastGood.has_value());
  EXPECT_FALSE(known->lastGoodIsStale) << "never-received is not stale";
}

TEST(Cache_NoCallbackUnderLock, ObserverMayReenterTheCache) {
  // The cache returns events instead of dispatching them, so a consumer that
  // reads the cache while handling one cannot deadlock. If commit() ever
  // invoked a callback under its mutex, this test would hang (invariant S-07).
  SignalCache cache(defaultFreshness());
  const auto events =
      cache.commit(batchOf(1000, {valueOf("VehicleSpeed", 100.0, SignalQuality::Valid)}));
  for (const auto& event : events) {
    EXPECT_TRUE(cache.get(event.key, at(1000)).has_value());
  }
  EXPECT_EQ(cache.size(), 1u);
}

TEST(Cache_FrameErrorMarksEverySignalOfTheMessage, B06AndB08) {
  // A wrong-length frame has no decoded signals at all, so the service tells
  // the cache at the message level. Last-good must survive (B-06, B-08).
  const auto loaded = tcu::dbc::loadDbcBytes(
      "VERSION \"lab\"\n"
      "BO_ 256 VEHICLE_STATUS: 8 ECU\n"
      " SG_ VehicleSpeed : 0|16@1+ (0.01,0) [0|250] \"km/h\" TCU\n"
      " SG_ Ignition : 16|1@1+ (1,0) [0|1] \"\" TCU\n",
      "lab.dbc");
  ASSERT_TRUE(loaded.ok());
  const auto* message = loaded.db->find(0, tcu::dbc::FrameFormat::Standard11, 256);
  ASSERT_NE(message, nullptr);

  SignalCache cache(defaultFreshness());
  cache.commit(batchOf(1000, {valueOf("VehicleSpeed", 100.0, SignalQuality::Valid),
                              valueOf("Ignition", 1.0, SignalQuality::Valid)}));
  const auto events = cache.recordFrameError(*message, 0, SignalQuality::DecodeError);
  EXPECT_EQ(events.size(), 2u);

  for (const char* name : {"VehicleSpeed", "Ignition"}) {
    const auto snapshot = cache.get(SignalKey{0, false, 256, name}, at(1000));
    ASSERT_TRUE(snapshot.has_value()) << name;
    EXPECT_EQ(snapshot->lastObservationQuality, SignalQuality::DecodeError) << name;
    ASSERT_TRUE(snapshot->lastGood.has_value()) << name;
    EXPECT_EQ(snapshot->lastGoodRxTime, at(1000)) << name;
    EXPECT_EQ(snapshot->epoch, 1u) << name;
  }
}

}  // namespace
