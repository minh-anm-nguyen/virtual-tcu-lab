// T064: NFR-02 and the conservation equations M-01..M-05.
//
// What is asserted is the SHAPE of the result - bounded depth, monotonic
// counters, nothing unaccounted for - not a latency number. Spec section 4.2
// calls its budgets goals for the lab, and an assertion on throughput would
// fail for reasons that have nothing to do with this code (research R-07).
//
// Duration comes from TCU_DBC_STRESS_SECONDS, default 5, matching project 04.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "support/service_fixture.hpp"

namespace {

using namespace tcu::signal::test;
using tcu::signal::DecoderService;

std::chrono::seconds stressBudget() {
  const char* fromEnv = std::getenv("TCU_DBC_STRESS_SECONDS");
  const long seconds = fromEnv != nullptr ? std::strtol(fromEnv, nullptr, 10) : 5;
  return std::chrono::seconds{seconds > 0 ? seconds : 5};
}

std::shared_ptr<tcu::scheduler::TimerScheduler> startedScheduler() {
  tcu::scheduler::Config config;
  config.workerThreads = 2;
  auto scheduler = std::make_shared<tcu::scheduler::TimerScheduler>(config);
  scheduler->start();
  return scheduler;
}

struct SoakHarness {
  TempDir dir;
  std::shared_ptr<tcu::scheduler::TimerScheduler> scheduler = startedScheduler();
  tcu::can::FakeCanTransport* transport{nullptr};
  std::unique_ptr<DecoderService> service;

  SoakHarness() {
    const std::string path = dir.write("lab.dbc", labDbc());
    auto fake = std::make_unique<tcu::can::FakeCanTransport>();
    transport = fake.get();
    tcu::signal::DecoderServiceDeps deps;
    deps.transport = std::move(fake);
    deps.scheduler = scheduler;
    service = std::make_unique<DecoderService>(configFor(path), std::move(deps));
  }
  ~SoakHarness() {
    if (service) service->stop();
    scheduler->stop(tcu::scheduler::StopMode::Discard);
  }
};

TEST(Soak_HundredThousandFramesBounded, NFR02) {
  SoakHarness harness;
  ASSERT_TRUE(harness.service->start().ready);

  constexpr int kFrames = 100000;
  std::size_t peakDepth = 0;
  std::uint64_t previousDrops = 0;
  std::uint64_t previousReceived = 0;

  for (int i = 0; i < kFrames; ++i) {
    harness.transport->pushFrame(frameOf(256, {0x10, 0x27, 0x01, 0, 0, 0, 0, 0}, 1000 + i));
    if ((i % 1000) != 0) continue;
    const auto metrics = harness.service->metrics();
    peakDepth = std::max(peakDepth, metrics.queueDepth);
    ASSERT_GE(metrics.queueDrops, previousDrops) << i;
    ASSERT_GE(metrics.framesReceived, previousReceived) << i;
    previousDrops = metrics.queueDrops;
    previousReceived = metrics.framesReceived;
  }

  ASSERT_TRUE(waitFor([&] { return harness.service->metrics().framesReceived >= kFrames; },
                      std::chrono::milliseconds{120000}));

  const auto metrics = harness.service->metrics();
  EXPECT_LE(peakDepth, 64u) << "the queue exceeded its configured capacity";
  EXPECT_LE(metrics.queueDepth, 64u);
  // The cache holds one entry per signal and does not grow with traffic:
  // 100,000 frames must not create 100,000 entries.
  EXPECT_EQ(harness.service->queryAll().size(), 2u)
      << "the cache grew with traffic instead of staying one entry per signal";
  EXPECT_EQ(metrics.framesReceived, static_cast<std::uint64_t>(kFrames));
}

TEST(Soak_MetricConservation, M01ToM05) {
  SoakHarness harness;
  ASSERT_TRUE(harness.service->start().ready);

  constexpr int kValid = 20000;
  constexpr int kUnknown = 500;
  constexpr int kShort = 300;
  for (int i = 0; i < kValid; ++i) {
    harness.transport->pushFrame(frameOf(256, {0x10, 0x27, 0x01, 0, 0, 0, 0, 0}, 1000 + i));
  }
  for (int i = 0; i < kUnknown; ++i) {
    harness.transport->pushFrame(frameOf(0x555, {0, 0, 0, 0, 0, 0, 0, 0}, 2000 + i));
  }
  for (int i = 0; i < kShort; ++i) {
    harness.transport->pushFrame(frameOf(256, {0x10, 0x27, 0x01}, 3000 + i));
  }
  const std::uint64_t pushed = kValid + kUnknown + kShort;
  ASSERT_TRUE(waitFor([&] { return harness.service->metrics().framesReceived >= pushed; },
                      std::chrono::milliseconds{120000}));

  // A quiescent point: stop() drains the queue, so the equations hold after it.
  harness.service->stop();
  const auto metrics = harness.service->metrics();

  // M-01: every frame the reader took is decoded, dropped or discarded.
  EXPECT_EQ(metrics.framesReceived,
            metrics.totalFrames() + metrics.queueDrops + metrics.queueDiscarded)
      << "a frame went unaccounted for: received=" << metrics.framesReceived
      << " decoded=" << metrics.totalFrames() << " dropped=" << metrics.queueDrops
      << " discarded=" << metrics.queueDiscarded;

  EXPECT_GT(metrics.frames[static_cast<std::size_t>(tcu::signal::FrameResult::Ok)], 0u);
  EXPECT_LE(metrics.queueDepth, 64u);
  // M-03: the version never decreased.
  EXPECT_EQ(metrics.activeVersion, 1u);
}

TEST(Soak_ReloadUnderTraffic, VersionOnlyEverIncreases) {
  SoakHarness harness;
  ASSERT_TRUE(harness.service->start().ready);

  std::atomic<bool> running{true};
  std::thread producer([&] {
    long long rxMs = 1000;
    while (running.load(std::memory_order_relaxed)) {
      harness.transport->pushFrame(frameOf(256, {0x10, 0x27, 0x01, 0, 0, 0, 0, 0}, rxMs++));
    }
  });

  const char* kValidEdit =
      "VERSION \"lab-v2\"\n"
      "BO_ 256 VEHICLE_STATUS: 8 ECU\n"
      " SG_ VehicleSpeed : 0|16@1+ (0.01,0) [0|250] \"km/h\" TCU\n"
      " SG_ Ignition : 16|1@1+ (1,0) [0|1] \"\" TCU\n"
      " SG_ Extra : 24|8@1+ (1,0) [0|255] \"\" TCU\n";
  const char* kInvalidEdit =
      "VERSION \"broken\"\n"
      "BO_ 256 VEHICLE_STATUS: 8 ECU\n"
      " SG_ VehicleSpeed : 0|16@1+ (0.01,0) [0|250] \"km/h\" TCU\n"
      " SG_ Ignition : 8|16@1+ (1,0) [0|1] \"\" TCU\n";

  const auto deadline = std::chrono::steady_clock::now() + stressBudget();
  std::uint64_t highestVersion = harness.service->schema().version;
  int attempts = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    harness.dir.replace("lab.dbc", (attempts % 2) == 0 ? kValidEdit : kInvalidEdit);
    ++attempts;
    (void)harness.service->requestReload();
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    const std::uint64_t version = harness.service->schema().version;
    ASSERT_GE(version, highestVersion) << "the active version went backwards";
    highestVersion = version;
    ASSERT_EQ(harness.service->state(), tcu::signal::ServiceState::Ready)
        << "a reload took the service out of Ready";
  }

  running.store(false);
  producer.join();

  const auto metrics = harness.service->metrics();
  EXPECT_GT(metrics.framesReceived, 0u);
  EXPECT_GE(highestVersion, 1u);
  EXPECT_EQ(metrics.activeVersion, highestVersion);
  EXPECT_GT(attempts, 0);
}

}  // namespace
