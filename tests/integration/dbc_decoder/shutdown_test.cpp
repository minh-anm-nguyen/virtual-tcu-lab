// T047: race recipe C (spec section 15.3). An observer blocks inside its
// callback; stop() must still return inside its deadline, and no callback may
// run after it returns (invariants V-06, V-07, contract B-15).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>

#include "support/service_fixture.hpp"
#include "test_gate.hpp"

namespace {

using namespace tcu::signal::test;
using tcu::signal::DecoderService;
using tcu::signal::ServiceState;

std::shared_ptr<tcu::scheduler::TimerScheduler> startedScheduler() {
  tcu::scheduler::Config config;
  config.workerThreads = 2;
  auto scheduler = std::make_shared<tcu::scheduler::TimerScheduler>(config);
  scheduler->start();
  return scheduler;
}

/// Blocks inside onBatch until the test opens the gate, and records whether it
/// was ever entered after stop() returned.
class BlockingObserver : public tcu::signal::ISignalObserver {
 public:
  void onBatch(const tcu::dbc::DecodedBatch&) override {
    ++entered;
    if (stopReturned.load()) ++calledAfterStop;
    gate.wait_until_opened();
  }
  void onQualityChanged(const tcu::signal::SignalKey&, tcu::dbc::SignalQuality) override {
    if (stopReturned.load()) ++calledAfterStop;
  }

  tcu::test::TestGate gate;
  std::atomic<int> entered{0};
  std::atomic<int> calledAfterStop{0};
  std::atomic<bool> stopReturned{false};
};

TEST(RaceC_StopWithBlockedObserver, StopIsBoundedAndNoCallbackFollows) {
  TempDir dir;
  const std::string path = dir.write("lab.dbc", labDbc());
  auto scheduler = startedScheduler();

  auto fake = std::make_unique<tcu::can::FakeCanTransport>();
  auto* transport = fake.get();
  tcu::signal::DecoderServiceDeps deps;
  deps.transport = std::move(fake);
  deps.scheduler = scheduler;

  auto service = std::make_unique<DecoderService>(configFor(path), std::move(deps));
  auto observer = std::make_shared<BlockingObserver>();
  service->addObserver(observer);
  ASSERT_TRUE(service->start().ready);

  // Fill the queue while the observer is stuck in the first callback.
  for (int i = 0; i < 200; ++i) {
    transport->pushFrame(frameOf(256, {0x10, 0x27, 0x01, 0, 0, 0, 0, 0}, 1000 + i));
  }
  observer->gate.wait_until_entered(1);
  ASSERT_GE(observer->entered.load(), 1) << "the observer must be inside its callback";

  // stop() must not wait on the blocked callback forever. It is given a short
  // deadline deliberately: a C++ thread cannot be force-terminated safely, so
  // the worker is cooperative and an overrun must be reported, not hidden.
  std::thread releaser([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    observer->gate.open();
  });

  const auto began = std::chrono::steady_clock::now();
  const auto report = service->stop(std::chrono::milliseconds{200});
  const auto elapsed = std::chrono::steady_clock::now() - began;
  observer->stopReturned.store(true);
  releaser.join();

  EXPECT_LT(elapsed, std::chrono::seconds{5}) << "stop() must not hang on a blocked observer";
  EXPECT_EQ(service->state(), ServiceState::Stopped);

  // Nothing may be delivered after stop() returned.
  transport->pushFrame(frameOf(256, {0x10, 0x27, 0x01, 0, 0, 0, 0, 0}, 9000));
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  EXPECT_EQ(observer->calledAfterStop.load(), 0)
      << "a callback ran after stop() returned (invariant V-07)";

  service.reset();
  scheduler->stop(tcu::scheduler::StopMode::Discard);
}

TEST(RaceC_StopWithBlockedObserver, DestructorIsSafeWhileObserverBlocks) {
  TempDir dir;
  const std::string path = dir.write("lab.dbc", labDbc());
  auto scheduler = startedScheduler();

  auto observer = std::make_shared<BlockingObserver>();
  {
    auto fake = std::make_unique<tcu::can::FakeCanTransport>();
    auto* transport = fake.get();
    tcu::signal::DecoderServiceDeps deps;
    deps.transport = std::move(fake);
    deps.scheduler = scheduler;

    DecoderService service(configFor(path), std::move(deps));
    service.addObserver(observer);
    ASSERT_TRUE(service.start().ready);
    transport->pushFrame(frameOf(256, {0x10, 0x27, 0x01, 0, 0, 0, 0, 0}, 1000));
    observer->gate.wait_until_entered(1);
    observer->gate.open();
  }  // the destructor runs stop()
  SUCCEED() << "destruction completed with an observer that had been blocking";
  scheduler->stop(tcu::scheduler::StopMode::Discard);
}

}  // namespace
