// T046: the service. Ready only with a schema, observers isolated, stop ordered
// and bounded, and the whole pipeline running over project 05's transport.
// Invariants V-05..V-08; contracts B-01, B-13, B-14, B-15.

#include <gtest/gtest.h>

#include <memory>

#include "support/service_fixture.hpp"

namespace {

using namespace tcu::signal::test;
using tcu::dbc::SignalQuality;
using tcu::signal::DecoderService;
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

struct Harness {
  TempDir dir;
  std::shared_ptr<tcu::scheduler::TimerScheduler> scheduler = startedScheduler();
  tcu::can::FakeCanTransport* transport{nullptr};
  std::unique_ptr<DecoderService> service;
  std::shared_ptr<RecordingObserver> observer = std::make_shared<RecordingObserver>();

  explicit Harness(const char* dbc = labDbc(), const char* name = "lab.dbc") {
    const std::string path = dir.write(name, dbc);
    auto fake = std::make_unique<tcu::can::FakeCanTransport>();
    transport = fake.get();

    tcu::signal::DecoderServiceDeps deps;
    deps.transport = std::move(fake);
    deps.scheduler = scheduler;
    service = std::make_unique<DecoderService>(configFor(path), std::move(deps));
  }

  ~Harness() {
    if (service) service->stop();
    scheduler->stop(tcu::scheduler::StopMode::Discard);
  }
};

TEST(Service_ReadyPublishesVersionAndHash, B01) {
  Harness harness;
  const auto result = harness.service->start();
  ASSERT_TRUE(result.ready);
  EXPECT_EQ(result.version, 1u);
  EXPECT_EQ(result.hash.size(), 64u);
  EXPECT_EQ(harness.service->state(), ServiceState::Ready);
  EXPECT_EQ(harness.service->schema().version, 1u);
  EXPECT_EQ(harness.service->schema().hash, result.hash);
}

TEST(Service_NotReadyWithoutValidDbc, B01Negative) {
  // An overlap makes the file invalid; the service must refuse to come up and
  // must never answer a query with something that looks like a reading.
  Harness harness(
      "VERSION \"lab\"\n"
      "BO_ 256 M: 8 ECU\n"
      " SG_ A : 0|16@1+ (1,0) [0|1] \"\" TCU\n"
      " SG_ B : 8|16@1+ (1,0) [0|1] \"\" TCU\n");
  const auto result = harness.service->start();
  EXPECT_FALSE(result.ready);
  EXPECT_FALSE(result.diagnostics.empty());
  EXPECT_EQ(harness.service->state(), ServiceState::NotReady);

  const auto query = harness.service->query(speedKey());
  ASSERT_TRUE(query.has_value());
  EXPECT_EQ(query->lastObservationQuality, SignalQuality::ConfigUnavailable);
  EXPECT_FALSE(query->lastGood.has_value());
  EXPECT_TRUE(harness.service->queryAll().empty());
}

TEST(Service_PipelineDecodesFromTheTransport, EndToEnd) {
  Harness harness;
  harness.service->addObserver(harness.observer);
  ASSERT_TRUE(harness.service->start().ready);

  harness.transport->pushFrame(frameOf(256, {0x10, 0x27, 0x01, 0, 0, 0, 0, 0}, 1000));
  ASSERT_TRUE(waitFor([&] { return harness.observer->batchCount() >= 1; }));

  const auto snapshot = harness.service->query(speedKey());
  ASSERT_TRUE(snapshot.has_value());
  ASSERT_TRUE(snapshot->lastGood.has_value());
  EXPECT_DOUBLE_EQ(snapshot->lastGood->physical, 100.0);
  EXPECT_EQ(snapshot->lastObservationQuality, SignalQuality::Valid);
  EXPECT_EQ(snapshot->dbcVersion, 1u);

  const auto metrics = harness.service->metrics();
  EXPECT_EQ(metrics.framesReceived, 1u);
  EXPECT_EQ(metrics.frames[static_cast<std::size_t>(tcu::signal::FrameResult::Ok)], 1u);
}

TEST(Service_PipelineDecodesFromTheTransport, UnknownIdChangesNothing) {
  Harness harness;
  ASSERT_TRUE(harness.service->start().ready);
  harness.transport->pushFrame(frameOf(0x321, {0, 0, 0, 0, 0, 0, 0, 0}, 1000));
  ASSERT_TRUE(waitFor([&] {
    return harness.service->metrics()
               .frames[static_cast<std::size_t>(tcu::signal::FrameResult::UnknownId)] == 1u;
  }));
  const auto snapshot = harness.service->query(speedKey());
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_FALSE(snapshot->lastGood.has_value());
}

TEST(Service_PipelineDecodesFromTheTransport, WrongLengthKeepsLastGood) {
  Harness harness;
  ASSERT_TRUE(harness.service->start().ready);
  harness.transport->pushFrame(frameOf(256, {0x10, 0x27, 0x01, 0, 0, 0, 0, 0}, 1000));
  ASSERT_TRUE(waitFor([&] {
    const auto snapshot = harness.service->query(speedKey());
    return snapshot.has_value() && snapshot->lastGood.has_value();
  }));

  // B-06 and B-08: a 7-byte frame for an 8-byte message is refused, the
  // observation moves, the value stays.
  harness.transport->pushFrame(frameOf(256, {0x00, 0x00, 0x00, 0, 0, 0, 0}, 1100));
  ASSERT_TRUE(waitFor([&] {
    const auto snapshot = harness.service->query(speedKey());
    return snapshot.has_value() && snapshot->lastObservationQuality == SignalQuality::DecodeError;
  }));
  const auto snapshot = harness.service->query(speedKey());
  ASSERT_TRUE(snapshot->lastGood.has_value());
  EXPECT_DOUBLE_EQ(snapshot->lastGood->physical, 100.0);
  EXPECT_EQ(snapshot->epoch, 1u);
}

TEST(Service_ObserverThrowIsolated, B14) {
  Harness harness;
  harness.observer->throwOnBatch = true;
  harness.service->addObserver(harness.observer);
  ASSERT_TRUE(harness.service->start().ready);

  for (int i = 0; i < 3; ++i) {
    harness.transport->pushFrame(frameOf(256, {0x10, 0x27, 0x01, 0, 0, 0, 0, 0}, 1000 + i));
  }
  ASSERT_TRUE(waitFor([&] { return harness.service->metrics().observerFailures >= 3u; }));

  // The worker survived every throw and kept decoding.
  EXPECT_EQ(harness.service->state(), ServiceState::Ready);
  const auto snapshot = harness.service->query(speedKey());
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_TRUE(snapshot->lastGood.has_value());
}

TEST(Service_WeakObserverExpired, DestroyedObserverIsNeverCalled) {
  Harness harness;
  {
    auto temporary = std::make_shared<RecordingObserver>();
    harness.service->addObserver(temporary);
  }  // destroyed here
  ASSERT_TRUE(harness.service->start().ready);
  harness.transport->pushFrame(frameOf(256, {0x10, 0x27, 0x01, 0, 0, 0, 0, 0}, 1000));
  ASSERT_TRUE(waitFor([&] { return harness.service->metrics().framesReceived == 1u; }));
  // No crash and no use-after-free; ASan is the other half of this assertion.
  EXPECT_EQ(harness.service->state(), ServiceState::Ready);
}

TEST(Service_RemoveObserverStopsFutureCallbacks, Unsubscribe) {
  Harness harness;
  const auto id = harness.service->addObserver(harness.observer);
  ASSERT_TRUE(harness.service->start().ready);
  harness.transport->pushFrame(frameOf(256, {0x10, 0x27, 0x01, 0, 0, 0, 0, 0}, 1000));
  ASSERT_TRUE(waitFor([&] { return harness.observer->batchCount() >= 1; }));

  harness.service->removeObserver(id);
  const std::size_t before = harness.observer->batchCount();
  harness.transport->pushFrame(frameOf(256, {0x10, 0x27, 0x01, 0, 0, 0, 0, 0}, 1100));
  ASSERT_TRUE(waitFor([&] { return harness.service->metrics().framesReceived >= 2u; }));
  EXPECT_EQ(harness.observer->batchCount(), before);
}

TEST(Service_StopIsIdempotent, AndSafeBeforeStart) {
  Harness harness;
  EXPECT_TRUE(harness.service->stop().complete) << "stop before start is not an error";
  EXPECT_EQ(harness.service->state(), ServiceState::Stopped);
}

TEST(Service_StopIsIdempotent, TwiceAfterStart) {
  Harness harness;
  ASSERT_TRUE(harness.service->start().ready);
  EXPECT_TRUE(harness.service->stop().complete);
  EXPECT_TRUE(harness.service->stop().complete);
  EXPECT_EQ(harness.service->state(), ServiceState::Stopped);
}

TEST(Service_StopJoinsWithinDeadline, B15) {
  Harness harness;
  harness.service->addObserver(harness.observer);
  ASSERT_TRUE(harness.service->start().ready);
  for (int i = 0; i < 20; ++i) {
    harness.transport->pushFrame(frameOf(256, {0x10, 0x27, 0x01, 0, 0, 0, 0, 0}, 1000 + i));
  }

  const auto began = std::chrono::steady_clock::now();
  const auto report = harness.service->stop(std::chrono::milliseconds{2000});
  const auto elapsed = std::chrono::steady_clock::now() - began;

  EXPECT_TRUE(report.complete);
  EXPECT_LT(elapsed, std::chrono::milliseconds{3000});
  EXPECT_EQ(harness.service->state(), ServiceState::Stopped);

  // No callback after stop() returned.
  const std::size_t after = harness.observer->batchCount();
  harness.transport->pushFrame(frameOf(256, {0x10, 0x27, 0x01, 0, 0, 0, 0, 0}, 2000));
  EXPECT_EQ(harness.observer->batchCount(), after);
}

TEST(Service_QueueDropsAreBoundedAndCounted, B13) {
  Harness harness;
  ASSERT_TRUE(harness.service->start().ready);
  for (int i = 0; i < 500; ++i) {
    harness.transport->pushFrame(frameOf(256, {0x10, 0x27, 0x01, 0, 0, 0, 0, 0}, 1000 + i));
  }
  ASSERT_TRUE(waitFor([&] { return harness.service->metrics().framesReceived >= 500u; }));
  const auto metrics = harness.service->metrics();
  EXPECT_LE(metrics.queueDepth, 64u) << "the queue must never exceed its capacity";
  EXPECT_EQ(harness.service->state(), ServiceState::Ready) << "no deadlock under overload";
}

}  // namespace
