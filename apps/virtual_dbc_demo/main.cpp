// virtual_dbc_demo: the spec section 20 sequence, over a real vcan0 or a
// scripted fake transport.
//
//   --iface vcan0   project 05's SocketCanTransport, so frames come off a real
//                   virtual bus and cansend/candump work against it.
//   --fake          project 05's FakeCanTransport, which is the ONLY way to
//                   inject the frames the kernel refuses to put on vcan0 - a
//                   wrong length, an RTR, an error frame. The demo says so on
//                   screen rather than implying they went on the wire, which
//                   is project 05's section 18.1 rule.
//
// The Event Bus adapter here is the whole of FR-18: tcu_signal defines the
// observer interface and never links the bus, so neither library depends on a
// consumer (research R-18).

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "tcu/can/fake_can_transport.hpp"
#include "tcu/can/socketcan_transport.hpp"
#include "tcu/eventbus/event_bus.hpp"
#include "tcu/signal/decoder_service.hpp"

namespace {

using tcu::signal::SignalKey;

/// Republishes decoded signals onto project 01's bus. It reuses the existing
/// VehicleSignalUpdated and SignalQualityChanged payloads, which project 05
/// added for exactly this purpose - appending a new alternative to the shared
/// variant for one demo would change nothing for the better.
class EventBusAdapter : public tcu::signal::ISignalObserver {
 public:
  explicit EventBusAdapter(std::shared_ptr<tcu::eventbus::EventBus> bus) : bus_(std::move(bus)) {}

  void onBatch(const tcu::dbc::DecodedBatch& batch) override {
    for (const auto& value : batch.signals) {
      if (value.quality != tcu::dbc::SignalQuality::Valid) continue;
      tcu::eventbus::Event event;
      event.type = tcu::eventbus::event_type_of<tcu::eventbus::VehicleSignalUpdated>();
      event.created_at = std::chrono::steady_clock::now();
      event.payload = tcu::eventbus::VehicleSignalUpdated{
          value.signalName, value.physical, batch.arbitrationId, batch.dbcVersion, 0};
      if (bus_->publish(event) != tcu::eventbus::PublishStatus::Accepted) ++publishFailures;
    }
  }

  void onQualityChanged(const SignalKey& key, tcu::dbc::SignalQuality quality) override {
    tcu::eventbus::Event event;
    event.type = tcu::eventbus::event_type_of<tcu::eventbus::SignalQualityChanged>();
    event.created_at = std::chrono::steady_clock::now();
    event.payload = tcu::eventbus::SignalQualityChanged{
        key.signalName, static_cast<std::uint8_t>(quality), 0};
    if (bus_->publish(event) != tcu::eventbus::PublishStatus::Accepted) ++publishFailures;
  }

  void onSchemaChanged(std::uint64_t version, const std::string& hash, bool committed) override {
    // Deliberately NOT on the bus: the shared EventPayload variant has no
    // reload alternative, and adding one to a finished project for a demo is
    // not justified. The observer interface already carries it (FR-18 is a
    // SHOULD, and the signal path is what it is about).
    std::printf("  [schema] %s version=%llu hash=%.12s\n",
                committed ? "committed" : "rejected",
                static_cast<unsigned long long>(version), hash.c_str());
  }

  std::uint64_t publishFailures{0};

 private:
  std::shared_ptr<tcu::eventbus::EventBus> bus_;
};

void banner(const char* step, const char* what) {
  std::printf("\n=== %s: %s ===\n", step, what);
}

std::vector<std::uint8_t> bytes(std::initializer_list<std::uint8_t> values) {
  return std::vector<std::uint8_t>(values);
}

tcu::can::CanMessage frameOf(std::uint32_t id, std::vector<std::uint8_t> data) {
  tcu::can::CanMessage message;
  message.id = id;
  message.length = static_cast<std::uint8_t>(data.size());
  for (std::size_t i = 0; i < data.size() && i < 8; ++i) message.data[i] = data[i];
  message.received_at = tcu::scheduler::Clock::now();
  return message;
}

int usage(const char* program) {
  std::fprintf(stderr, "usage: %s --config <decoder.json> [--iface vcan0 | --fake]\n", program);
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  std::string configPath = "configs/decoder.json";
  std::string iface;
  bool fake = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--fake") {
      fake = true;
    } else if (arg == "--config" && i + 1 < argc) {
      configPath = argv[++i];
    } else if (arg == "--iface" && i + 1 < argc) {
      iface = argv[++i];
    } else {
      return usage(argv[0]);
    }
  }
  if (!fake && iface.empty()) fake = true;

  const auto loaded = tcu::signal::loadDecoderConfig(configPath);
  if (!loaded.ok) {
    for (const auto& error : loaded.errors) {
      std::fprintf(stderr, "%s: %s: %s %s\n", configPath.c_str(), error.pointer.c_str(),
                   tcu::signal::toString(error.kind), error.allowed.c_str());
    }
    return 2;
  }

  // The reload steps below genuinely replace the DBC file, so the demo works on
  // its own copy rather than editing configs/ in the repository.
  auto config = loaded.config;
  const auto demoDbc = std::filesystem::temp_directory_path() / "virtual_dbc_demo.dbc";
  {
    std::error_code ec;
    std::filesystem::copy_file(config.dbcPath, demoDbc,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      std::fprintf(stderr, "cannot copy %s for the demo: %s\n", config.dbcPath.c_str(),
                   ec.message().c_str());
      return 2;
    }
    config.dbcPath = demoDbc.string();
  }

  auto bus = std::make_shared<tcu::eventbus::EventBus>(tcu::eventbus::BusConfig{});
  bus->start();
  auto signalCount = std::make_shared<std::atomic<std::uint64_t>>(0);
  auto subscription =
      bus->subscribe(tcu::eventbus::event_type_of<tcu::eventbus::VehicleSignalUpdated>(),
                     [signalCount](const tcu::eventbus::Event&) { ++*signalCount; });

  tcu::scheduler::Config schedulerConfig;
  schedulerConfig.workerThreads = 2;
  auto scheduler = std::make_shared<tcu::scheduler::TimerScheduler>(schedulerConfig);
  scheduler->start();

  tcu::can::FakeCanTransport* fakeTransport = nullptr;
  tcu::signal::DecoderServiceDeps deps;
  if (fake) {
    auto transport = std::make_unique<tcu::can::FakeCanTransport>();
    fakeTransport = transport.get();
    deps.transport = std::move(transport);
    std::printf("transport: FakeCanTransport (no kernel involved)\n");
  } else {
    // A factory, not a throwing constructor: a missing interface is an expected
    // condition, not an exception (project 05's spec section 9.2).
    tcu::can::SocketCanTransport::Options options;
    options.interface_name = iface;
    auto opened = tcu::can::SocketCanTransport::open(options);
    if (std::holds_alternative<tcu::can::OpenError>(opened)) {
      const auto error = std::get<tcu::can::OpenError>(opened);
      std::fprintf(stderr,
                   "cannot open %s: failed at %s with errno %d.\n"
                   "Run 'sudo scripts/setup_vcan.sh' first, or use --fake.\n",
                   iface.c_str(), tcu::can::openErrorStepName(error.step), error.os_error);
      scheduler->stop(tcu::scheduler::StopMode::Discard);
      bus->stop();
      return 2;
    }
    deps.transport = std::move(std::get<std::unique_ptr<tcu::can::SocketCanTransport>>(opened));
    std::printf("transport: SocketCanTransport on %s\n", iface.c_str());
  }
  deps.scheduler = scheduler;

  tcu::signal::DecoderService service(config, std::move(deps));
  auto adapter = std::make_shared<EventBusAdapter>(bus);
  service.addObserver(adapter);

  banner("A", "cold start");
  const auto started = service.start();
  if (!started.ready) {
    std::printf("  NOT READY: the DBC was rejected, so no signal will ever be published\n");
    for (const auto& diagnostic : started.diagnostics) {
      std::printf("  %s:%u:%u: %s: %s\n", diagnostic.file.c_str(), diagnostic.line,
                  diagnostic.column, tcu::dbc::toString(diagnostic.code),
                  diagnostic.message.c_str());
    }
    service.stop();
    scheduler->stop(tcu::scheduler::StopMode::Discard);
    bus->stop();
    return 1;
  }
  std::printf("  ready, version %llu, sha256 %.12s...\n",
              static_cast<unsigned long long>(started.version), started.hash.c_str());
  for (const auto& key : started.unmatchedPolicyKeys) {
    std::printf("  WARNING: configured policy names no signal: %s\n", key.c_str());
  }

  const SignalKey speed{0, false, 256, "VehicleSpeed"};
  const SignalKey torque{0, false, 257, "Torque"};
  const auto show = [&service](const SignalKey& key, const char* label) {
    const auto snapshot = service.query(key);
    if (!snapshot.has_value()) {
      std::printf("  %-14s unknown signal\n", label);
      return;
    }
    std::printf("  %-14s observed=%-18s", label,
                tcu::dbc::toString(snapshot->lastObservationQuality));
    if (snapshot->lastGood.has_value()) {
      std::printf(" last_good=%.4g %s%s", snapshot->lastGood->physical,
                  snapshot->lastGood->unit.c_str(), snapshot->lastGoodIsStale ? " STALE" : "");
    } else {
      std::printf(" last_good=<none>");
    }
    std::printf(" epoch=%llu\n", static_cast<unsigned long long>(snapshot->epoch));
  };

  const auto settle = [] { std::this_thread::sleep_for(std::chrono::milliseconds{150}); };

  banner("B", "normal traffic - the spec section 7.3 values");
  if (fakeTransport != nullptr) {
    fakeTransport->pushFrame(frameOf(256, bytes({0x10, 0x27, 0x01, 0x00, 0x50, 0, 0, 0})));
    fakeTransport->pushFrame(frameOf(257, bytes({0xAB, 0xC0, 0, 0, 0, 0, 0, 0})));
    fakeTransport->pushFrame(frameOf(258, bytes({0x01, 0x7F, 0xD2, 0x04, 0, 0, 0, 0})));
    fakeTransport->pushFrame(frameOf(258, bytes({0x02, 0x7F, 0x9C, 0xFF, 0, 0, 0, 0})));
  } else {
    std::printf("  inject with: cansend %s 100#1027010050000000\n", iface.c_str());
    std::printf("               cansend %s 101#ABC0000000000000\n", iface.c_str());
    std::printf("               cansend %s 102#017FD20400000000\n", iface.c_str());
  }
  settle();
  show(speed, "VehicleSpeed");
  show(torque, "Torque");
  show(SignalKey{0, false, 258, "BatteryVoltage"}, "BatteryVoltage");
  show(SignalKey{0, false, 258, "MotorTemp"}, "MotorTemp");

  banner("C", "wrong length - last-good must survive");
  if (fakeTransport != nullptr) {
    fakeTransport->pushFrame(frameOf(256, bytes({0x00, 0x00, 0x00, 0, 0, 0, 0})));
    settle();
    show(speed, "VehicleSpeed");
  } else {
    std::printf("  skipped: the kernel refuses a short frame for an 8-byte message on %s.\n",
                iface.c_str());
    std::printf("  Run with --fake to inject it. This test did NOT run on the wire.\n");
  }

  banner("D", "unknown identifier, then the same number as an extended frame");
  if (fakeTransport != nullptr) {
    fakeTransport->pushFrame(frameOf(0x321, bytes({0, 0, 0, 0, 0, 0, 0, 0})));
    tcu::can::CanMessage extended = frameOf(256, bytes({0x10, 0x27, 0x01, 0, 0, 0, 0, 0}));
    extended.format = tcu::can::FrameFormat::Extended29;
    fakeTransport->pushFrame(extended);
    settle();
    const auto metrics = service.metrics();
    std::printf("  unknown_id count=%llu (the extended 0x100 was NOT aliased onto the standard)\n",
                static_cast<unsigned long long>(
                    metrics.frames[static_cast<std::size_t>(tcu::signal::FrameResult::UnknownId)]));
  } else {
    std::printf("  inject with: cansend %s 321#0000000000000000\n", iface.c_str());
  }

  banner("E", "stale - stop sending and let the deadline pass");
  std::this_thread::sleep_for(config.freshness.defaultTimeout +
                              std::chrono::milliseconds{150});
  show(speed, "VehicleSpeed");
  show(torque, "Torque");

  banner("F", "recovery - one valid frame brings it back");
  if (fakeTransport != nullptr) {
    fakeTransport->pushFrame(frameOf(256, bytes({0x20, 0x4E, 0x01, 0x00, 0x50, 0, 0, 0})));
    settle();
  }
  show(speed, "VehicleSpeed");

  const auto writeDbc = [&config](const char* contents) {
    // Written to a temporary and renamed over the target, the way a real tool
    // does it, so a reload never sees a half-written file.
    const auto staging = std::filesystem::path(config.dbcPath + ".tmp");
    std::ofstream out(staging, std::ios::binary | std::ios::trunc);
    out << contents;
    out.close();
    std::filesystem::rename(staging, config.dbcPath);
  };
  const auto awaitReload = [&config] {
    std::this_thread::sleep_for(config.reload.debounce + std::chrono::milliseconds{400});
  };

  banner("G", "rejected reload - the active version must not move");
  const auto before = service.schema();
  std::printf("  active version %llu\n", static_cast<unsigned long long>(before.version));
  writeDbc(
      "VERSION \"broken\"\n"
      "BO_ 256 VEHICLE_STATUS: 8 ECU\n"
      " SG_ VehicleSpeed : 0|16@1+ (0.01,0) [0|250] \"km/h\" TCU\n"
      " SG_ Ignition : 8|16@1+ (1,0) [0|1] \"\" TCU\n");
  if (service.requestReload() == tcu::signal::ReloadRequest::Accepted) {
    awaitReload();
    const auto after = service.schema();
    std::printf("  overlapping DBC rejected: version %llu -> %llu, state=%s\n",
                static_cast<unsigned long long>(before.version),
                static_cast<unsigned long long>(after.version),
                tcu::signal::toString(service.state()));
    show(speed, "VehicleSpeed");
  }

  banner("G2", "accepted reload - a new signal, and last-good survives");
  writeDbc(
      "VERSION \"lab-v2\"\n"
      "BO_ 256 VEHICLE_STATUS: 8 ECU\n"
      " SG_ VehicleSpeed : 0|16@1+ (0.01,0) [0|250] \"km/h\" TCU\n"
      " SG_ Ignition : 16|1@1+ (1,0) [0|1] \"\" TCU\n"
      " SG_ CabinTemp : 24|8@1- (1,-40) [-40|125] \"degC\" TCU\n");
  if (service.requestReload() == tcu::signal::ReloadRequest::Accepted) {
    awaitReload();
    std::printf("  committed: version %llu, state=%s\n",
                static_cast<unsigned long long>(service.schema().version),
                tcu::signal::toString(service.state()));
    show(speed, "VehicleSpeed");
    show(SignalKey{0, false, 256, "CabinTemp"}, "CabinTemp");
  }

  banner("H", "overload - a burst far above the queue capacity");
  if (fakeTransport != nullptr) {
    for (int i = 0; i < 2000; ++i) {
      fakeTransport->pushFrame(frameOf(256, bytes({0x10, 0x27, 0x01, 0x00, 0x50, 0, 0, 0})));
    }
    settle();
    const auto metrics = service.metrics();
    std::printf("  received=%llu depth=%zu capacity=%zu drops=%llu (no deadlock, bounded depth)\n",
                static_cast<unsigned long long>(metrics.framesReceived), metrics.queueDepth,
                config.rxQueueCapacity,
                static_cast<unsigned long long>(metrics.queueDrops));
  }

  banner("I", "shutdown");
  const auto report = service.stop();
  std::printf("  complete=%s drained=%llu discarded=%llu\n", report.complete ? "true" : "false",
              static_cast<unsigned long long>(report.drained),
              static_cast<unsigned long long>(report.discarded));

  const auto metrics = service.metrics();
  std::printf("\nmetrics: frames_received=%llu ok=%llu unknown_id=%llu wrong_length=%llu\n",
              static_cast<unsigned long long>(metrics.framesReceived),
              static_cast<unsigned long long>(
                  metrics.frames[static_cast<std::size_t>(tcu::signal::FrameResult::Ok)]),
              static_cast<unsigned long long>(
                  metrics.frames[static_cast<std::size_t>(tcu::signal::FrameResult::UnknownId)]),
              static_cast<unsigned long long>(
                  metrics.frames[static_cast<std::size_t>(tcu::signal::FrameResult::WrongLength)]));
  std::printf("         stale=%llu recovered=%llu observer_failures=%llu bus_publish_failures=%llu\n",
              static_cast<unsigned long long>(metrics.staleTransitions),
              static_cast<unsigned long long>(metrics.recoveredTransitions),
              static_cast<unsigned long long>(metrics.observerFailures),
              static_cast<unsigned long long>(adapter->publishFailures));
  std::printf("         events seen by the bus subscriber: %llu\n",
              static_cast<unsigned long long>(signalCount->load()));

  std::printf("\nNot demonstrated here: CAN arbitration, bus-off, bit timing and transceiver\n");
  std::printf("behaviour - vcan does not simulate them. A successful decode is also not\n");
  std::printf("evidence that the sender was genuine: there is no CRC, counter or signature.\n");

  subscription.unsubscribe();
  scheduler->stop(tcu::scheduler::StopMode::Discard);
  bus->stop();
  return 0;
}
