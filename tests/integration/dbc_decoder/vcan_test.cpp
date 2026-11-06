// T049b: DBC-013 on the wire. Opens project 05's SocketCanTransport on the
// configured interface, sends the lab frames through it, and asserts the cache
// reports the spec section 7.3 values.
//
// Skips with an explicit message when the interface does not exist. A skip is
// honest; a pass would not be. Project 05's section 18.1 rule is that an
// impossible on-wire test must never be claimed as passed, and the inverse
// holds too - a test that silently did nothing must say so.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <variant>

#include "support/service_fixture.hpp"
#include "tcu/can/socketcan_transport.hpp"

namespace {

using namespace tcu::signal::test;
using tcu::signal::DecoderService;
using tcu::signal::SignalKey;

std::string interfaceName() {
  const char* fromEnv = std::getenv("TCU_VCAN_IFACE");
  return fromEnv != nullptr ? fromEnv : "vcan0";
}

std::unique_ptr<tcu::can::SocketCanTransport> openOrNull(const std::string& iface) {
  tcu::can::SocketCanTransport::Options options;
  options.interface_name = iface;
  auto opened = tcu::can::SocketCanTransport::open(std::move(options));
  if (std::holds_alternative<tcu::can::OpenError>(opened)) return nullptr;
  return std::move(std::get<std::unique_ptr<tcu::can::SocketCanTransport>>(opened));
}

TEST(Vcan_EndToEnd, LabFramesDecodeOverARealVirtualBus) {
  const std::string iface = interfaceName();
  auto probe = openOrNull(iface);
  if (probe == nullptr) {
    GTEST_SKIP() << "no usable CAN interface '" << iface
                 << "'. Run 'sudo scripts/setup_vcan.sh' to create it, or set "
                    "TCU_VCAN_IFACE. This test did NOT run on the wire.";
  }
  probe.reset();

  TempDir dir;
  const std::string path = dir.write("lab.dbc", labDbc());

  tcu::scheduler::Config schedulerConfig;
  schedulerConfig.workerThreads = 2;
  auto scheduler = std::make_shared<tcu::scheduler::TimerScheduler>(schedulerConfig);
  scheduler->start();

  auto transport = openOrNull(iface);
  ASSERT_NE(transport, nullptr);
  // A second handle on the same interface, used only to put frames on the bus:
  // the service's own transport is owned by its reader thread and must not be
  // touched from here (project 05's single-receiver rule).
  auto sender = openOrNull(iface);
  ASSERT_NE(sender, nullptr);

  tcu::signal::DecoderServiceDeps deps;
  deps.transport = std::move(transport);
  deps.scheduler = scheduler;

  DecoderService service(configFor(path), std::move(deps));
  auto observer = std::make_shared<RecordingObserver>();
  service.addObserver(observer);
  ASSERT_TRUE(service.start().ready);

  tcu::can::CanMessage frame;
  frame.id = 256;
  frame.length = 8;
  frame.data = {0x10, 0x27, 0x01, 0x00, 0x50, 0x00, 0x00, 0x00};
  ASSERT_EQ(sender->send(frame), tcu::can::WriteStatus::Sent);

  ASSERT_TRUE(waitFor([&] { return observer->batchCount() >= 1; }))
      << "no frame came back off " << iface;

  const auto speed = service.query(SignalKey{0, false, 256, "VehicleSpeed"});
  ASSERT_TRUE(speed.has_value());
  ASSERT_TRUE(speed->lastGood.has_value());
  EXPECT_DOUBLE_EQ(speed->lastGood->physical, 100.0);
  EXPECT_EQ(speed->lastObservationQuality, tcu::dbc::SignalQuality::Valid);

  const auto ignition = service.query(SignalKey{0, false, 256, "Ignition"});
  ASSERT_TRUE(ignition.has_value());
  ASSERT_TRUE(ignition->lastGood.has_value());
  EXPECT_DOUBLE_EQ(ignition->lastGood->physical, 1.0);

  // The rxTime came off the kernel path, not from this test.
  EXPECT_NE(speed->lastGoodRxTime.time_since_epoch().count(), 0);

  service.stop();
  scheduler->stop(tcu::scheduler::StopMode::Discard);
}

TEST(Vcan_EndToEnd, MissingInterfaceIsReportedNotThrown) {
  // The negative half needs no interface at all: opening a name that cannot
  // exist must come back as an error value (project 05's spec section 9.2).
  tcu::can::SocketCanTransport::Options options;
  options.interface_name = "vcan-no-such-iface";
  auto opened = tcu::can::SocketCanTransport::open(std::move(options));
  ASSERT_TRUE(std::holds_alternative<tcu::can::OpenError>(opened));
  const auto error = std::get<tcu::can::OpenError>(opened);
  EXPECT_NE(tcu::can::openErrorStepName(error.step), nullptr);
}

}  // namespace
