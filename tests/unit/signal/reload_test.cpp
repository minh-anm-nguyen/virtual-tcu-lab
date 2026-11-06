// T050/T051: hot reload either commits atomically with a new version, or
// changes nothing at all. Invariants V-09..V-13; contracts B-11, B-12.

#include <gtest/gtest.h>

#include <memory>

#include "fault_hooks.hpp"
#include "support/service_fixture.hpp"

namespace {

using namespace tcu::signal::test;
using tcu::dbc::SignalQuality;
using tcu::signal::DecoderService;
using tcu::signal::ReloadRequest;
using tcu::signal::ServiceState;
using tcu::signal::SignalKey;

std::shared_ptr<tcu::scheduler::TimerScheduler> startedScheduler() {
  tcu::scheduler::Config config;
  config.workerThreads = 2;
  auto scheduler = std::make_shared<tcu::scheduler::TimerScheduler>(config);
  scheduler->start();
  return scheduler;
}

SignalKey speedKey() { return SignalKey{0, false, 256, "VehicleSpeed"}; }
SignalKey ignitionKey() { return SignalKey{0, false, 256, "Ignition"}; }

/// The lab DBC plus one extra signal: every existing signal keeps its semantic
/// hash, so last-good must survive the swap (research R-09).
const char* kWithExtraSignal =
    "VERSION \"lab\"\n"
    "BO_ 256 VEHICLE_STATUS: 8 ECU\n"
    " SG_ VehicleSpeed : 0|16@1+ (0.01,0) [0|250] \"km/h\" TCU\n"
    " SG_ Ignition : 16|1@1+ (1,0) [0|1] \"\" TCU\n"
    " SG_ Odometer : 24|24@1+ (1,0) [0|16777215] \"km\" TCU\n";

/// VehicleSpeed's factor changed: the cached number now means something else,
/// so it must be reset rather than reinterpreted.
const char* kWithChangedFactor =
    "VERSION \"lab\"\n"
    "BO_ 256 VEHICLE_STATUS: 8 ECU\n"
    " SG_ VehicleSpeed : 0|16@1+ (0.5,0) [0|250] \"km/h\" TCU\n"
    " SG_ Ignition : 16|1@1+ (1,0) [0|1] \"\" TCU\n";

const char* kInvalidOverlap =
    "VERSION \"lab\"\n"
    "BO_ 256 VEHICLE_STATUS: 8 ECU\n"
    " SG_ VehicleSpeed : 0|16@1+ (0.01,0) [0|250] \"km/h\" TCU\n"
    " SG_ Ignition : 8|16@1+ (1,0) [0|1] \"\" TCU\n";

struct ReloadHarness {
  TempDir dir;
  std::shared_ptr<tcu::scheduler::TimerScheduler> scheduler = startedScheduler();
  tcu::can::FakeCanTransport* transport{nullptr};
  std::unique_ptr<DecoderService> service;
  std::shared_ptr<RecordingObserver> observer = std::make_shared<RecordingObserver>();
  std::string dbcPath;

  ReloadHarness() {
    dbcPath = dir.write("lab.dbc", labDbc());
    auto fake = std::make_unique<tcu::can::FakeCanTransport>();
    transport = fake.get();
    tcu::signal::DecoderServiceDeps deps;
    deps.transport = std::move(fake);
    deps.scheduler = scheduler;
    service = std::make_unique<DecoderService>(configFor(dbcPath), std::move(deps));
    service->addObserver(observer);
  }

  ~ReloadHarness() {
    tcu::signal::detail::testHooks().clear();
    if (service) service->stop();
    scheduler->stop(tcu::scheduler::StopMode::Discard);
  }

  void feedValidSpeed(long long rxMs = 1000) {
    transport->pushFrame(frameOf(256, {0x10, 0x27, 0x01, 0, 0, 0, 0, 0}, rxMs));
    ASSERT_TRUE(waitFor([&] {
      const auto snapshot = service->query(speedKey());
      return snapshot.has_value() && snapshot->lastGood.has_value();
    }));
  }

  /// Waits for the reload to be fully settled, which is the observer
  /// notification rather than the version bump: the version is published
  /// before the coordinator releases its in-flight flag, so a request made
  /// between the two correctly returns Busy (invariant V-09).
  bool reloadSettles(std::uint64_t expectedVersion) {
    const std::size_t before = observer->schemaChangeCount();
    return waitFor([&] {
      return service->schema().version == expectedVersion &&
             observer->schemaChangeCount() > before;
    });
  }
};

TEST(Reload_FailureKeepsActiveVersion, B11) {
  ReloadHarness harness;
  ASSERT_TRUE(harness.service->start().ready);
  harness.feedValidSpeed();
  const auto before = harness.service->schema();

  harness.dir.replace("lab.dbc", kInvalidOverlap);
  EXPECT_EQ(harness.service->requestReload(), ReloadRequest::Accepted);
  ASSERT_TRUE(waitFor([&] { return harness.observer->schemaChangeCount() >= 1; }));

  // Version, hash, cache and state are all exactly as they were.
  EXPECT_EQ(harness.service->schema().version, before.version);
  EXPECT_EQ(harness.service->schema().hash, before.hash);
  EXPECT_EQ(harness.service->state(), ServiceState::Ready);
  const auto snapshot = harness.service->query(speedKey());
  ASSERT_TRUE(snapshot.has_value());
  ASSERT_TRUE(snapshot->lastGood.has_value());
  EXPECT_DOUBLE_EQ(snapshot->lastGood->physical, 100.0);
  EXPECT_EQ(snapshot->epoch, 1u);
  EXPECT_FALSE(harness.observer->schemaChanges.back().second) << "the reload was rejected";
}

TEST(Reload_CommitIncrementsVersion, AndKeepsUnchangedSignals) {
  ReloadHarness harness;
  ASSERT_TRUE(harness.service->start().ready);
  harness.feedValidSpeed();

  harness.dir.replace("lab.dbc", kWithExtraSignal);
  EXPECT_EQ(harness.service->requestReload(), ReloadRequest::Accepted);
  ASSERT_TRUE(harness.reloadSettles(2));

  EXPECT_EQ(harness.service->schema().version, 2u);
  // Nothing about VehicleSpeed changed, so its last-good survives (R-09).
  const auto snapshot = harness.service->query(speedKey());
  ASSERT_TRUE(snapshot.has_value());
  ASSERT_TRUE(snapshot->lastGood.has_value());
  EXPECT_DOUBLE_EQ(snapshot->lastGood->physical, 100.0);
  EXPECT_EQ(snapshot->epoch, 1u);
  // The new signal exists and has never been received.
  const auto added = harness.service->query(SignalKey{0, false, 256, "Odometer"});
  ASSERT_TRUE(added.has_value());
  EXPECT_EQ(added->lastObservationQuality, SignalQuality::NotReceived);
}

TEST(Reload_MigrationResetsChangedSignals, SemanticHashChange) {
  ReloadHarness harness;
  ASSERT_TRUE(harness.service->start().ready);
  harness.feedValidSpeed();

  harness.dir.replace("lab.dbc", kWithChangedFactor);
  EXPECT_EQ(harness.service->requestReload(), ReloadRequest::Accepted);
  ASSERT_TRUE(harness.reloadSettles(2));

  // VehicleSpeed means something else now: keeping 100.0 as Valid would be the
  // silent semantics drift spec section 9.3 forbids.
  const auto speed = harness.service->query(speedKey());
  ASSERT_TRUE(speed.has_value());
  EXPECT_EQ(speed->lastObservationQuality, SignalQuality::NotReceived);
  EXPECT_FALSE(speed->lastGood.has_value());

  // Ignition did not change, so it is untouched.
  const auto ignition = harness.service->query(ignitionKey());
  ASSERT_TRUE(ignition.has_value());
  ASSERT_TRUE(ignition->lastGood.has_value());
}

TEST(Reload_MigrationResetsChangedSignals, RemovedSignalBecomesConfigUnavailable) {
  ReloadHarness harness;
  ASSERT_TRUE(harness.service->start().ready);
  harness.feedValidSpeed();

  harness.dir.replace("lab.dbc",
                      "VERSION \"lab\"\n"
                      "BO_ 256 VEHICLE_STATUS: 8 ECU\n"
                      " SG_ VehicleSpeed : 0|16@1+ (0.01,0) [0|250] \"km/h\" TCU\n");
  EXPECT_EQ(harness.service->requestReload(), ReloadRequest::Accepted);
  ASSERT_TRUE(harness.reloadSettles(2));

  const auto ignition = harness.service->query(ignitionKey());
  ASSERT_TRUE(ignition.has_value());
  EXPECT_EQ(ignition->lastObservationQuality, SignalQuality::ConfigUnavailable);
}

TEST(Reload_BusyRejectsSecondRequest, V09) {
  ReloadHarness harness;
  ASSERT_TRUE(harness.service->start().ready);

  // Hold the first reload inside its parse so a second request arrives while
  // one is genuinely in flight.
  tcu::signal::detail::testHooks().afterReloadParse = [] {
    std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
  };
  EXPECT_EQ(harness.service->requestReload(), ReloadRequest::Accepted);
  EXPECT_EQ(harness.service->requestReload(), ReloadRequest::Busy)
      << "a second reload must be rejected, never queued";
}

TEST(Reload_RateLimited, V13) {
  ReloadHarness harness;
  ASSERT_TRUE(harness.service->start().ready);

  // maxAttemptsPerMinute is 6 in the test configuration. Each request must
  // settle before the next, so they are counted rather than rejected as Busy.
  for (std::uint64_t attempt = 1; attempt <= 6; ++attempt) {
    ASSERT_TRUE(waitFor([&] {
      return harness.service->requestReload() != ReloadRequest::Busy;
    })) << "attempt " << attempt << " never got past an in-flight reload";
    ASSERT_TRUE(harness.reloadSettles(attempt + 1)) << attempt;
  }
  EXPECT_EQ(harness.service->requestReload(), ReloadRequest::RateLimited);
  // The active snapshot is untouched by a rate-limited attempt.
  EXPECT_EQ(harness.service->schema().version, 7u);
}

TEST(Reload_DisabledReturnsReloadDisabled, WhenTheConfigurationSaysSo) {
  TempDir dir;
  const std::string path = dir.write("lab.dbc", labDbc());
  auto scheduler = startedScheduler();
  auto fake = std::make_unique<tcu::can::FakeCanTransport>();

  auto config = configFor(path);
  config.reload.enabled = false;
  tcu::signal::DecoderServiceDeps deps;
  deps.transport = std::move(fake);
  deps.scheduler = scheduler;
  DecoderService service(config, std::move(deps));
  ASSERT_TRUE(service.start().ready);

  EXPECT_EQ(service.requestReload(), ReloadRequest::Disabled);
  EXPECT_EQ(service.schema().version, 1u);
  service.stop();
  scheduler->stop(tcu::scheduler::StopMode::Discard);
}

TEST(Reload_SourceErrorKeepsActiveVersion, MissingFileIsNotACommit) {
  ReloadHarness harness;
  ASSERT_TRUE(harness.service->start().ready);
  harness.feedValidSpeed();

  std::filesystem::remove(harness.dbcPath);
  EXPECT_EQ(harness.service->requestReload(), ReloadRequest::Accepted);
  ASSERT_TRUE(waitFor([&] { return harness.observer->schemaChangeCount() >= 1; }));

  EXPECT_EQ(harness.service->schema().version, 1u);
  EXPECT_EQ(harness.service->state(), ServiceState::Ready);
  const auto metrics = harness.service->metrics();
  EXPECT_EQ(
      metrics.loadAttempts[static_cast<std::size_t>(tcu::signal::LoadResultLabel::SourceError)],
      1u);
  const auto snapshot = harness.service->query(speedKey());
  ASSERT_TRUE(snapshot->lastGood.has_value()) << "a missing file must not erase the cache";
}

TEST(RaceA_InFlightFramePinsOldVersion, B12) {
  // Race recipe A (spec section 15.3): the worker pins v1 and waits, a reload
  // commits v2, the worker resumes - and its batch must still say v1, because
  // a batch carries exactly one schema identity (invariant P-13, NFR-06).
  //
  // The 10,000-iteration budget of the task list belongs to the integration
  // suite; each iteration here costs a real file write and a real reload, so
  // this unit case runs the barrier once and asserts the invariant exactly.
  ReloadHarness harness;
  ASSERT_TRUE(harness.service->start().ready);

  std::atomic<bool> armed{true};
  tcu::signal::detail::testHooks().afterPinSnapshot = [&] {
    if (!armed.exchange(false)) return;
    // Runs on the decoder worker, with v1 already pinned for this frame.
    harness.dir.replace("lab.dbc", kWithExtraSignal);
    if (harness.service->requestReload() != ReloadRequest::Accepted) return;
    (void)waitFor([&] { return harness.service->schema().version == 2u; });
  };

  harness.transport->pushFrame(frameOf(256, {0x10, 0x27, 0x01, 0, 0, 0, 0, 0}, 1000));
  ASSERT_TRUE(waitFor([&] { return harness.observer->batchCount() >= 1; }));

  {
    std::lock_guard<std::mutex> lock(harness.observer->mutex_);
    ASSERT_FALSE(harness.observer->batches.empty());
    EXPECT_EQ(harness.observer->batches.front().dbcVersion, 1u)
        << "the in-flight frame must complete against the version it pinned";
  }
  EXPECT_EQ(harness.service->schema().version, 2u) << "while the service has moved on to v2";

  // The next frame decodes against v2.
  tcu::signal::detail::testHooks().clear();
  harness.transport->pushFrame(frameOf(256, {0x10, 0x27, 0x01, 0, 0, 0, 0, 0}, 1100));
  ASSERT_TRUE(waitFor([&] { return harness.observer->batchCount() >= 2; }));
  {
    std::lock_guard<std::mutex> lock(harness.observer->mutex_);
    EXPECT_EQ(harness.observer->batches.back().dbcVersion, 2u);
  }
}

}  // namespace
