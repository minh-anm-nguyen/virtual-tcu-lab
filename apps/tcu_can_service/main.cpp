// tcu_can_service - the subject of the laboratory.
//
// Receives synthetic vehicle frames from vcan0, decodes them into
// quality-tagged signals, and serves the spec section 16.2 health surface and
// the metrics snapshot over project 02's IPC so can_control_cli can read them
// (research R-08).
//
// SAFETY: lab only. The configuration loader refuses any interface that is not
// vcan* while mode is lab_only, and lab_only is the only mode V1 parses
// (NFR-01, ADR-07, research R-16).

#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "tcu/can/can_config.hpp"
#include "tcu/can/can_service.hpp"
#include "tcu/can/frame_catalog.hpp"
#include "tcu/eventbus/event_bus.hpp"
#include "tcu/ipc/ipc_server.hpp"
#include "tcu/logging/async_logger.hpp"
#include "tcu/logging/console_sink.hpp"

namespace {

// Written by the signal handler, read by main. A handler may touch almost
// nothing else safely, so it does exactly this and the main thread does the
// work - which is also what keeps stop() off a callback thread (spec 9.4).
volatile std::sig_atomic_t g_stop_requested = 0;

extern "C" void onSignal(int) { g_stop_requested = 1; }

void printUsage() {
  std::cout << "usage: tcu_can_service [--config PATH] [--interface NAME] [--broad-filter]\n"
            << "                       [--socket PATH] [--health] [--metrics]\n\n"
            << "  --config PATH     lab configuration (default config/can_lab.json)\n"
            << "  --interface NAME  override the configured interface; must be vcan*\n"
            << "  --broad-filter    install no CAN_RAW_FILTER, so unknown identifiers reach\n"
            << "                    the application. For B-12 only; not for normal use\n"
            << "  --socket PATH     IPC control socket (default /tmp/tcu_can_service.sock)\n"
            << "  --health          print the health surface once and exit\n"
            << "  --metrics         print the metrics snapshot once and exit\n";
}

std::string metricsJson(const tcu::can::CanMetricsSnapshot& m) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"can_rx_read_total\": " << m.can_rx_read_total << ",\n";
  out << "  \"can_rx_enqueued_total\": " << m.can_rx_enqueued_total << ",\n";
  out << "  \"can_rx_queue_drop_total\": " << m.can_rx_queue_drop_total << ",\n";
  out << "  \"can_rx_invalid_total\": " << m.can_rx_invalid_total << ",\n";
  out << "  \"can_unknown_id_total\": " << m.can_unknown_id_total << ",\n";
  out << "  \"can_reserved_nonzero_total\": " << m.can_reserved_nonzero_total << ",\n";
  out << "  \"can_decoded_total\": " << m.can_decoded_total << ",\n";
  out << "  \"can_reject_flags_total\": " << m.can_reject_flags_total << ",\n";
  out << "  \"can_reject_length_total\": " << m.can_reject_length_total << ",\n";
  out << "  \"can_reject_range_total\": " << m.can_reject_range_total << ",\n";
  out << "  \"signal_update_total\": " << m.signal_update_total << ",\n";
  out << "  \"signal_stale_transitions_total\": " << m.signal_stale_transitions_total << ",\n";
  out << "  \"signal_recovered_total\": " << m.signal_recovered_total << ",\n";
  out << "  \"signal_invalid_transitions_total\": " << m.signal_invalid_transitions_total << ",\n";
  out << "  \"can_tx_attempt_total\": " << m.can_tx_attempt_total << ",\n";
  out << "  \"can_tx_success_total\": " << m.can_tx_success_total << ",\n";
  out << "  \"can_ack_matched_total\": " << m.can_ack_matched_total << ",\n";
  out << "  \"can_ack_unmatched_total\": " << m.can_ack_unmatched_total << ",\n";
  out << "  \"can_ack_timeout_total\": " << m.can_ack_timeout_total << ",\n";
  out << "  \"can_link_error_total\": " << m.can_link_error_total << ",\n";
  out << "  \"can_link_reconnect_total\": " << m.can_link_reconnect_total << ",\n";
  out << "  \"can_shutdown_discarded_total\": " << m.can_shutdown_discarded_total << ",\n";
  out << "  \"event_publish_total\": " << m.event_publish_total << ",\n";
  out << "  \"event_publish_failed_total\": " << m.event_publish_failed_total << ",\n";
  out << "  \"can_queue_depth\": " << m.can_queue_depth << ",\n";
  out << "  \"can_queue_capacity\": " << m.can_queue_capacity << "\n";
  out << "}";
  return out.str();
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path = "config/can_lab.json";
  std::string socket_path = "/tmp/tcu_can_service.sock";
  std::string interface_override;
  bool broad_filter = false;
  bool print_health = false;
  bool print_metrics = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      printUsage();
      return 0;
    } else if (arg == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else if (arg == "--interface" && i + 1 < argc) {
      interface_override = argv[++i];
    } else if (arg == "--socket" && i + 1 < argc) {
      socket_path = argv[++i];
    } else if (arg == "--broad-filter") {
      broad_filter = true;
    } else if (arg == "--health") {
      print_health = true;
    } else if (arg == "--metrics") {
      print_metrics = true;
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      printUsage();
      return 2;
    }
  }

  tcu::can::CanLabConfig config;
  const auto load = tcu::can::loadCanLabConfig(config_path, config);
  if (!load) {
    // The reason comes from a closed template set and never echoes a value from
    // the file (Constitution Principle V).
    std::cerr << "configuration rejected: " << tcu::can::loadErrorName(load.error) << " at "
              << (load.pointer.empty() ? "(file)" : load.pointer) << ": " << load.publicReason
              << "\n";
    return 1;
  }
  if (!interface_override.empty()) {
    config.interface_name = interface_override;
  }

  auto logger = std::make_shared<tcu::logging::AsyncLogger>(
      tcu::logging::LoggerOptions{},
      std::vector<std::shared_ptr<tcu::logging::ILogSink>>{
          std::make_shared<tcu::logging::ConsoleSink>()});
  logger->start();

  auto bus = std::make_shared<tcu::eventbus::EventBus>(tcu::eventbus::BusConfig{});
  bus->start();

  tcu::can::CanServiceDeps deps;
  deps.logger = logger;
  deps.bus = bus;
  deps.broad_filter = broad_filter;
  tcu::can::CanService service(std::move(deps));

  if (!service.start(config)) {
    std::cerr << "service refused to start: the configuration is not usable\n";
    return 1;
  }

  if (print_health || print_metrics) {
    // One-shot diagnostic mode, so a demo script can read the surface without
    // an IPC client. Nothing has arrived yet, which is itself the point of
    // quickstart scenario 6.1: every signal reports NEVER_RECEIVED.
    if (print_health) {
      std::cout << service.healthJson() << "\n";
    }
    if (print_metrics) {
      std::cout << metricsJson(service.metrics()) << "\n";
    }
    service.stop();
    bus->stop(tcu::eventbus::StopMode::Drain);
    logger->stop();
    return 0;
  }

  // The control plane. Reusing project 02 rather than inventing a second
  // protocol is Constitution Principle II applied to the obvious temptation
  // (research R-08).
  tcu::ipc::ServerOptions server_options;
  server_options.socket_path = socket_path;
  tcu::ipc::IpcServer control(server_options);

  control.registerHandler("health", [&service](std::string) {
    tcu::ipc::IpcResponse response;
    response.payload_json = service.healthJson();
    return response;
  });
  control.registerHandler("metrics", [&service](std::string) {
    tcu::ipc::IpcResponse response;
    response.payload_json = metricsJson(service.metrics());
    return response;
  });
  control.registerHandler("lock", [&service](std::string) {
    tcu::ipc::IpcResponse response;
    const auto status = service.sendDemoCommand(tcu::can::kCommandLock, 0x2A);
    response.payload_json = std::string("{\"sent\": ") +
                            (status == tcu::can::WriteStatus::Sent ? "true" : "false") + "}";
    return response;
  });
  control.registerHandler("unlock", [&service](std::string) {
    tcu::ipc::IpcResponse response;
    const auto status = service.sendDemoCommand(tcu::can::kCommandUnlock, 0x2B);
    response.payload_json = std::string("{\"sent\": ") +
                            (status == tcu::can::WriteStatus::Sent ? "true" : "false") + "}";
    return response;
  });

  if (!control.start()) {
    std::cerr << "warning: control socket " << socket_path
              << " is unavailable; the service runs without it\n";
  }

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  std::cout << "tcu_can_service on " << config.interface_name << " (state "
            << (service.state() == tcu::can::ServiceState::Running ? "RUNNING" : "DEGRADED")
            << "), control socket " << socket_path << "\n";
  std::cout << "Ctrl-C or SIGTERM to stop.\n";

  // stop() runs on this thread, never from inside a callback (spec 9.4).
  while (g_stop_requested == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
  }

  std::cout << "\nstopping...\n";
  control.stop();
  service.stop();
  bus->stop(tcu::eventbus::StopMode::Drain);
  logger->stop();
  std::cout << service.healthJson() << "\n";
  return 0;
}
