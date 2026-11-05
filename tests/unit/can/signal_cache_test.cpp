// UT-11, UT-12, UT-13, UT-22, B-02, B-04, B-05, B-07, B-08, B-09, B-10, B-11,
// B-18, B-20.
//
// Time is driven by tcu::scheduler::FakeClockAdapter (project 03), never by
// sleeping: the deadline cases below assert behaviour one millisecond either
// side of a boundary, which a real clock cannot do reliably.

#include "tcu/can/signal_cache.hpp"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>

#include "tcu/can/frame_catalog.hpp"

namespace {

using namespace tcu::can;
using tcu::scheduler::FakeClockAdapter;

constexpr auto kSpeedTimeout = std::chrono::milliseconds{500};
constexpr auto kHeartbeatTimeout = std::chrono::milliseconds{3000};

struct Fixture {
  std::shared_ptr<FakeClockAdapter> clock = std::make_shared<FakeClockAdapter>();
  SignalCache cache{{}, clock};

  SignalSample update(const std::string& name, double value, std::uint32_t source_id) {
    SignalSample sample;
    sample.signal_name = name;
    sample.value = value;
    sample.source_id = source_id;
    sample.last_good_at = clock->now();
    return sample;
  }

  /// Applies a value at the current fake time, then refreshes - exactly what
  /// the decode worker does, so these tests exercise the real sequence.
  std::vector<QualityTransition> applySpeed(double kmh) {
    cache.apply(update("speed_kmh", kmh, 0x101));
    return cache.refresh(clock->now());
  }
};

// B-02, UT-22, and spec section 13.1: "Startup must not report ignition OFF or
// speed zero before receiving a valid frame."
TEST(Cache, NeverReceivedHasNoValue) {
  Fixture f;

  for (const auto& definition : kSignals) {
    const auto sample = f.cache.get(definition.name);
    ASSERT_TRUE(sample.has_value()) << definition.name;
    EXPECT_EQ(sample->quality, SignalQuality::NeverReceived) << definition.name;
    EXPECT_FALSE(sample->value.has_value())
        << definition.name << " reported a value before any frame arrived";
    EXPECT_EQ(sample->generation, 0u) << definition.name;
  }
}

// An unknown name is a different answer from a known-but-unreceived signal, and
// a caller has to be able to tell them apart.
TEST(Cache, UnknownNameReturnsNullopt) {
  Fixture f;
  EXPECT_FALSE(f.cache.get("not_a_signal").has_value());
  EXPECT_TRUE(f.cache.get("speed_kmh").has_value());
}

TEST(Cache, ApplyRejectsUnknownName) {
  Fixture f;
  EXPECT_FALSE(f.cache.apply(f.update("doors", 1.0, 0x103)));
  EXPECT_TRUE(f.cache.apply(f.update("door_state", 1.0, 0x103)));
}

TEST(Cache, ApplyRejectsAValuelessSample) {
  // The decoder never produces one, but a cache that accepted it would store a
  // "fresh" signal with nothing in it.
  Fixture f;
  SignalSample empty;
  empty.signal_name = "speed_kmh";
  EXPECT_FALSE(f.cache.apply(empty));
  EXPECT_EQ(f.cache.get("speed_kmh")->quality, SignalQuality::NeverReceived);
}

// B-04. The case the whole optional-value design exists for.
TEST(Cache, ZeroIsAValue) {
  Fixture f;
  f.applySpeed(0.0);

  const auto sample = f.cache.get("speed_kmh");
  ASSERT_TRUE(sample->value.has_value()) << "a valid zero must not read as missing";
  EXPECT_DOUBLE_EQ(*sample->value, 0.0);
  EXPECT_EQ(sample->quality, SignalQuality::Fresh);
}

// B-05.
TEST(Cache, GenerationIncrements) {
  Fixture f;
  f.applySpeed(60.0);
  const auto first = f.cache.get("speed_kmh")->generation;

  f.applySpeed(70.0);
  const auto second = f.cache.get("speed_kmh")->generation;

  EXPECT_GT(second, first);
  EXPECT_DOUBLE_EQ(*f.cache.get("speed_kmh")->value, 70.0);
}

TEST(Cache, GenerationIncrementsEvenWhenTheValueRepeats) {
  // "Same value, new frame" is information a consumer may need: a repeated 0.0
  // is evidence the ECU is still talking.
  Fixture f;
  f.applySpeed(0.0);
  const auto first = f.cache.get("speed_kmh")->generation;
  f.applySpeed(0.0);

  EXPECT_GT(f.cache.get("speed_kmh")->generation, first);
}

TEST(Cache, RecordsSourceIdentifier) {
  Fixture f;
  f.applySpeed(60.0);
  EXPECT_EQ(f.cache.get("speed_kmh")->source_id, 0x101u);
}

// UT-11, B-06, research R-12. At exactly the deadline the signal is STILL
// fresh: the comparison is strictly greater.
TEST(Cache, StaleAtBoundary) {
  Fixture f;
  f.applySpeed(60.0);

  f.clock->advance(kSpeedTimeout);

  EXPECT_EQ(f.cache.get("speed_kmh")->quality, SignalQuality::Fresh)
      << "exactly last_good_at + timeout is fresh, not stale";
  EXPECT_TRUE(f.cache.refresh(f.clock->now()).empty()) << "and nothing is reported";
}

// UT-12, B-07.
TEST(Cache, StaleAfterBoundary) {
  Fixture f;
  f.applySpeed(60.0);

  f.clock->advance(kSpeedTimeout + std::chrono::milliseconds{1});
  const auto transitions = f.cache.refresh(f.clock->now());

  ASSERT_EQ(transitions.size(), 1u);
  EXPECT_EQ(transitions[0].signal_name, "speed_kmh");
  EXPECT_EQ(transitions[0].from, SignalQuality::Fresh);
  EXPECT_EQ(transitions[0].to, SignalQuality::Stale);

  const auto sample = f.cache.get("speed_kmh");
  EXPECT_EQ(sample->quality, SignalQuality::Stale);
  EXPECT_DOUBLE_EQ(*sample->value, 60.0) << "the last good value survives staleness";
}

// UT-12, B-08. The reason the quality is computed rather than stored.
TEST(Cache, OneStaleTransitionOnly) {
  Fixture f;
  f.applySpeed(60.0);
  f.clock->advance(kSpeedTimeout + std::chrono::milliseconds{1});

  EXPECT_EQ(f.cache.refresh(f.clock->now()).size(), 1u) << "the crossing is reported once";

  // A health tick every 50 ms for the next two seconds must say nothing more.
  for (int tick = 0; tick < 40; ++tick) {
    f.clock->advance(std::chrono::milliseconds{50});
    EXPECT_TRUE(f.cache.refresh(f.clock->now()).empty())
        << "tick " << tick << " re-reported a transition that had not changed";
  }
}

// UT-13, B-09.
TEST(Cache, RecoversOnce) {
  Fixture f;
  f.applySpeed(60.0);
  f.clock->advance(kSpeedTimeout + std::chrono::milliseconds{1});
  ASSERT_EQ(f.cache.refresh(f.clock->now()).size(), 1u);

  const auto recovery = f.applySpeed(61.0);

  ASSERT_EQ(recovery.size(), 1u);
  EXPECT_EQ(recovery[0].from, SignalQuality::Stale);
  EXPECT_EQ(recovery[0].to, SignalQuality::Fresh);

  // And not again on the next tick.
  EXPECT_TRUE(f.cache.refresh(f.clock->now()).empty());
}

TEST(Cache, FirstValueReportsNeverReceivedToFresh) {
  Fixture f;
  const auto transitions = f.applySpeed(60.0);

  ASSERT_EQ(transitions.size(), 1u);
  EXPECT_EQ(transitions[0].from, SignalQuality::NeverReceived);
  EXPECT_EQ(transitions[0].to, SignalQuality::Fresh);
}

TEST(Cache, FreshToFreshReportsNothing) {
  // A value update is not a quality change. The service publishes a value event
  // for it separately; the transition list must stay quiet.
  Fixture f;
  f.applySpeed(60.0);
  EXPECT_TRUE(f.applySpeed(61.0).empty());
  EXPECT_TRUE(f.applySpeed(62.0).empty());
}

// B-18. Each signal has its own deadline and they do not interfere.
TEST(Cache, OnlyExpiredSignalGoesStale) {
  Fixture f;
  f.cache.apply(f.update("speed_kmh", 60.0, 0x101));
  f.cache.apply(f.update("heartbeat", 7.0, 0x110));
  f.cache.refresh(f.clock->now());

  // Past speed's 500 ms deadline, well inside heartbeat's 3000 ms one.
  f.clock->advance(std::chrono::milliseconds{600});
  const auto transitions = f.cache.refresh(f.clock->now());

  ASSERT_EQ(transitions.size(), 1u);
  EXPECT_EQ(transitions[0].signal_name, "speed_kmh");
  EXPECT_EQ(f.cache.get("speed_kmh")->quality, SignalQuality::Stale);
  EXPECT_EQ(f.cache.get("heartbeat")->quality, SignalQuality::Fresh);
}

TEST(Cache, HeartbeatEventuallyGoesStaleToo) {
  Fixture f;
  f.cache.apply(f.update("heartbeat", 7.0, 0x110));
  f.cache.refresh(f.clock->now());

  f.clock->advance(kHeartbeatTimeout);
  EXPECT_EQ(f.cache.get("heartbeat")->quality, SignalQuality::Fresh) << "inclusive boundary";

  f.clock->advance(std::chrono::milliseconds{1});
  EXPECT_EQ(f.cache.get("heartbeat")->quality, SignalQuality::Stale);
}

// B-10, B-11, UT-09, spec section 13.4. A malformed frame must not refresh a
// stale signal or disturb the good value it holds.
TEST(Cache, InvalidDoesNotRefresh) {
  Fixture f;
  f.applySpeed(60.0);
  const auto good_at = f.cache.get("speed_kmh")->last_good_at;
  const auto generation = f.cache.get("speed_kmh")->generation;

  f.clock->advance(std::chrono::milliseconds{100});
  f.cache.recordError("speed_kmh", RejectReason::Range, f.clock->now());

  const auto sample = f.cache.get("speed_kmh");
  EXPECT_EQ(sample->quality, SignalQuality::Invalid);
  EXPECT_DOUBLE_EQ(*sample->value, 60.0) << "the last good value is preserved";
  EXPECT_EQ(sample->last_good_at, good_at) << "the deadline was reset by a rejected frame";
  EXPECT_EQ(sample->generation, generation) << "a rejection is not an update";
}

TEST(Cache, RejectedFrameCannotRescueAStaleSignal) {
  // The sharpest form of the rule: if recordError moved last_good_at, a stream
  // of malformed frames would keep a dead signal looking alive.
  Fixture f;
  f.applySpeed(60.0);
  f.clock->advance(kSpeedTimeout + std::chrono::milliseconds{1});
  f.cache.refresh(f.clock->now());
  ASSERT_EQ(f.cache.get("speed_kmh")->quality, SignalQuality::Stale);

  for (int i = 0; i < 10; ++i) {
    f.clock->advance(std::chrono::milliseconds{50});
    f.cache.recordError("speed_kmh", RejectReason::Range, f.clock->now());
    EXPECT_EQ(f.cache.get("speed_kmh")->quality, SignalQuality::Stale)
        << "malformed frame " << i << " made a stale signal look fresh";
  }
}

TEST(Cache, StaleBeatsInvalidInThePrecedenceOrder) {
  // data-model.md section 6.1 puts step 3 before step 4: the deadline is the
  // stronger fact for a consumer deciding whether to use the value.
  Fixture f;
  f.applySpeed(60.0);
  f.clock->advance(kSpeedTimeout + std::chrono::milliseconds{1});
  f.cache.recordError("speed_kmh", RejectReason::Range, f.clock->now());

  EXPECT_EQ(f.cache.get("speed_kmh")->quality, SignalQuality::Stale);
}

TEST(Cache, AValidUpdateClearsTheErrorState) {
  Fixture f;
  f.applySpeed(60.0);

  // The error has to be newer than the last good value for step 5 to see it,
  // and the refresh is what the decode worker does after recordError - without
  // it the Invalid state would never be published, so there would be no
  // transition to come back from.
  f.clock->advance(std::chrono::milliseconds{10});
  f.cache.recordError("speed_kmh", RejectReason::Range, f.clock->now());
  const auto to_invalid = f.cache.refresh(f.clock->now());
  ASSERT_EQ(to_invalid.size(), 1u);
  EXPECT_EQ(to_invalid[0].to, SignalQuality::Invalid);
  ASSERT_EQ(f.cache.get("speed_kmh")->quality, SignalQuality::Invalid);

  const auto transitions = f.applySpeed(61.0);

  ASSERT_EQ(transitions.size(), 1u);
  EXPECT_EQ(transitions[0].from, SignalQuality::Invalid);
  EXPECT_EQ(transitions[0].to, SignalQuality::Fresh);
}

TEST(Cache, AnErrorNoNewerThanTheGoodValueDoesNotInvalidate) {
  // Step 5 compares timestamps strictly: an error recorded at the same instant
  // as the value it would discredit cannot have been caused by a later frame.
  Fixture f;
  f.applySpeed(60.0);
  f.cache.recordError("speed_kmh", RejectReason::Range, f.clock->now());

  EXPECT_EQ(f.cache.get("speed_kmh")->quality, SignalQuality::Fresh);
}

// B-20 again, with the outage shorter than the deadline - the case the
// deadline alone cannot catch, and the reason State carries a fifth field.
TEST(Cache, BriefOutageStillRequiresANewFrame) {
  Fixture f;
  f.applySpeed(60.0);

  f.cache.setTransportAvailable(false);
  f.clock->advance(std::chrono::milliseconds{10});  // well inside the 500 ms deadline
  f.cache.setTransportAvailable(true);

  EXPECT_EQ(f.cache.get("speed_kmh")->quality, SignalQuality::Stale)
      << "a value that predates an outage is unproven, however brief the outage";
  EXPECT_DOUBLE_EQ(*f.cache.get("speed_kmh")->value, 60.0) << "but it is retained";

  f.applySpeed(60.0);
  EXPECT_EQ(f.cache.get("speed_kmh")->quality, SignalQuality::Fresh)
      << "and one new frame is enough to prove it again";
}

TEST(Cache, RecordErrorOnUnknownNameIsIgnored) {
  Fixture f;
  f.cache.recordError("not_a_signal", RejectReason::Range, f.clock->now());
  // And no catalog signal was collaterally damaged.
  EXPECT_EQ(f.cache.get("speed_kmh")->quality, SignalQuality::NeverReceived);
}

TEST(Cache, InvalidBeforeAnyGoodValueStaysNeverReceived) {
  // An error cannot be the first thing a signal knows: there is no value to
  // distrust yet, and reporting Invalid would imply there is one.
  Fixture f;
  f.cache.recordError("speed_kmh", RejectReason::Range, f.clock->now());

  const auto sample = f.cache.get("speed_kmh");
  EXPECT_EQ(sample->quality, SignalQuality::NeverReceived);
  EXPECT_FALSE(sample->value.has_value());
}

// B-19: every signal becomes LinkUnavailable, regardless of how fresh it was.
TEST(Cache, TransportLossOverridesFreshness) {
  Fixture f;
  f.applySpeed(60.0);
  ASSERT_EQ(f.cache.get("speed_kmh")->quality, SignalQuality::Fresh);

  const auto transitions = f.cache.setTransportAvailable(false);

  EXPECT_FALSE(transitions.empty());
  for (const auto& sample : f.cache.snapshot()) {
    EXPECT_EQ(sample.quality, SignalQuality::LinkUnavailable) << sample.signal_name;
  }
  EXPECT_DOUBLE_EQ(*f.cache.get("speed_kmh")->value, 60.0)
      << "values are retained while the link is down (spec section 13.6)";
}

// B-20. The case that catches a transport-recovery path which fakes freshness.
TEST(Cache, LinkRestoredStaysNonFresh) {
  Fixture f;
  f.applySpeed(60.0);
  f.cache.setTransportAvailable(false);

  f.cache.setTransportAvailable(true);

  EXPECT_NE(f.cache.get("speed_kmh")->quality, SignalQuality::Fresh)
      << "transport recovery alone must not restore freshness";

  // Only a new valid frame does.
  f.applySpeed(61.0);
  EXPECT_EQ(f.cache.get("speed_kmh")->quality, SignalQuality::Fresh);
}

TEST(Cache, LinkRestoredFallsBackToTheDeadline) {
  // After a long outage the signal is stale, not merely "unknown": the value is
  // real but old, and saying so is more useful than pretending it never came.
  Fixture f;
  f.applySpeed(60.0);
  f.cache.setTransportAvailable(false);
  f.clock->advance(std::chrono::seconds{10});
  f.cache.setTransportAvailable(true);

  EXPECT_EQ(f.cache.get("speed_kmh")->quality, SignalQuality::Stale);
}

TEST(Cache, LinkRestoredLeavesNeverReceivedAlone) {
  Fixture f;
  f.cache.setTransportAvailable(false);
  f.cache.setTransportAvailable(true);

  EXPECT_EQ(f.cache.get("ignition")->quality, SignalQuality::NeverReceived);
}

TEST(Cache, RepeatedTransportStateChangeReportsOnce) {
  Fixture f;
  f.applySpeed(60.0);

  EXPECT_FALSE(f.cache.setTransportAvailable(false).empty());
  EXPECT_TRUE(f.cache.setTransportAvailable(false).empty()) << "no change, no event";
}

TEST(Cache, SnapshotCoversEverySignalAtOneInstant) {
  Fixture f;
  f.applySpeed(60.0);

  const auto snapshot = f.cache.snapshot();
  EXPECT_EQ(snapshot.size(), kSignalCount);

  for (const auto& sample : snapshot) {
    if (sample.signal_name == "speed_kmh") {
      EXPECT_EQ(sample.quality, SignalQuality::Fresh);
    } else {
      EXPECT_EQ(sample.quality, SignalQuality::NeverReceived) << sample.signal_name;
    }
  }
}

TEST(Cache, ConfiguredTimeoutOverridesTheCatalogDefault) {
  auto clock = std::make_shared<FakeClockAdapter>();
  std::map<std::string, SignalConfig> configured;
  configured.emplace("speed_kmh", SignalConfig{0x101, std::chrono::milliseconds{50}});
  SignalCache cache(configured, clock);

  SignalSample sample;
  sample.signal_name = "speed_kmh";
  sample.value = 60.0;
  sample.source_id = 0x101;
  sample.last_good_at = clock->now();
  cache.apply(sample);

  clock->advance(std::chrono::milliseconds{50});
  EXPECT_EQ(cache.get("speed_kmh")->quality, SignalQuality::Fresh) << "inclusive boundary";

  clock->advance(std::chrono::milliseconds{1});
  EXPECT_EQ(cache.get("speed_kmh")->quality, SignalQuality::Stale)
      << "the configured 50 ms deadline was ignored in favour of the catalog default";
}

TEST(Cache, UnconfiguredSignalKeepsTheCatalogDeadline) {
  auto clock = std::make_shared<FakeClockAdapter>();
  std::map<std::string, SignalConfig> configured;
  configured.emplace("speed_kmh", SignalConfig{0x101, std::chrono::milliseconds{50}});
  SignalCache cache(configured, clock);

  SignalSample sample;
  sample.signal_name = "heartbeat";
  sample.value = 1.0;
  sample.source_id = 0x110;
  sample.last_good_at = clock->now();
  cache.apply(sample);

  clock->advance(kHeartbeatTimeout);
  EXPECT_EQ(cache.get("heartbeat")->quality, SignalQuality::Fresh);
  clock->advance(std::chrono::milliseconds{1});
  EXPECT_EQ(cache.get("heartbeat")->quality, SignalQuality::Stale);
}

// The invariants of spec section 17.2, asserted directly.
TEST(Cache, InvariantsHold) {
  Fixture f;

  for (const auto& sample : f.cache.snapshot()) {
    // "A committed cache update contains a validated value and nonzero
    // signal-generation number" - contrapositive: generation 0 means no value.
    if (sample.generation == 0) {
      EXPECT_FALSE(sample.value.has_value()) << sample.signal_name;
    }
    // "No signal is FRESH without a valid frame."
    if (sample.quality == SignalQuality::Fresh) {
      EXPECT_TRUE(sample.value.has_value()) << sample.signal_name;
    }
  }

  f.applySpeed(60.0);
  f.cache.setTransportAvailable(false);
  for (const auto& sample : f.cache.snapshot()) {
    // "No signal is FRESH ... with unavailable transport."
    EXPECT_NE(sample.quality, SignalQuality::Fresh) << sample.signal_name;
  }
}

}  // namespace
