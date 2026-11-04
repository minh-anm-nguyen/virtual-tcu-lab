// Project 02 demo service: serves vehicle.getStatus over IPC and publishes
// vehicle.ignitionChanged notifications.
//
// The ignition event travels on project 01's event bus first and is forwarded
// to the IPC server by an in-process subscriber. That is the WORK <--> EB edge
// of spec section 4.1 and the boundary section 18 insists on: the event bus
// moves events inside a process, IPC moves them across a process boundary.
// Only this app links tcu_eventbus; the library does not (research R-05).

#include <sys/types.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "json_lite.hpp"
#include "tcu/eventbus/event_bus.hpp"
#include "tcu/ipc/ipc_server.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void onSignal(int) { g_stop = 1; }

struct Args {
  std::string socket_path{"/tmp/virtual-tcu/vehicle.sock"};
  std::vector<uid_t> allow_uids;
  int slow_ms{0};
  int ignition_period_ms{1000};
  bool ok{true};
};

Args parseArgs(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    const bool has_value = i + 1 < argc;
    if (flag == "--socket" && has_value) {
      a.socket_path = argv[++i];
    } else if (flag == "--allow-uid" && has_value) {
      a.allow_uids.push_back(static_cast<uid_t>(std::strtoul(argv[++i], nullptr, 10)));
    } else if (flag == "--slow-ms" && has_value) {
      a.slow_ms = std::atoi(argv[++i]);
    } else if (flag == "--ignition-period-ms" && has_value) {
      a.ignition_period_ms = std::atoi(argv[++i]);
    } else {
      std::cerr << "[server] unknown or incomplete flag: " << flag << "\n";
      a.ok = false;
      return a;
    }
  }
  return a;
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = parseArgs(argc, argv);
  if (!args.ok) {
    std::cerr << "usage: vehicle_service --socket PATH [--allow-uid N]... [--slow-ms N]"
              << " [--ignition-period-ms N]\n";
    return 2;
  }

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  // Ignition state lives here and is observed through the event bus, so the
  // status handler and the notification path agree on one source of truth.
  std::atomic<bool> ignition{true};

  tcu::ipc::ServerOptions options;
  options.socket_path = args.socket_path;
  options.allowed_uids = args.allow_uids;
  tcu::ipc::IpcServer server(options);

  const int slow_ms = args.slow_ms;
  const bool registered =
      server.registerHandler("vehicle.getStatus", [&ignition, slow_ms](std::string payload) {
        tcu::ipc::IpcResponse response;
        // An empty payload is accepted; anything present must be well formed
        // and within the depth cap (B-18).
        if (!payload.empty() && !jsonlite::isWellFormed(payload)) {
          response.error = tcu::ipc::IpcError::RemoteError;
          response.detail = "INVALID_ARGUMENT";
          return response;
        }
        if (slow_ms > 0) {
          // The delayed-handler fault case. It runs on the executor, so the
          // I/O loop keeps serving other sessions meanwhile.
          std::this_thread::sleep_for(std::chrono::milliseconds(slow_ms));
        }
        response.payload_json =
            jsonlite::object("\"ignition\":" + jsonlite::boolValue(ignition.load()) +
                             ",\"speed_kph\":0,\"battery_pct\":79");
        return response;
      });
  if (!registered) {
    std::cerr << "[server] failed to register vehicle.getStatus\n";
    return 1;
  }

  // Project 01's bus, with its existing IgnitionChanged payload - no new
  // alternative is added to its variant (Principle II).
  tcu::eventbus::EventBus bus(tcu::eventbus::BusConfig{});
  auto subscription =
      bus.subscribe(tcu::eventbus::event_type_of<tcu::eventbus::IgnitionChanged>(),
                    [&server](const tcu::eventbus::Event& event) {
                      const auto* payload =
                          std::get_if<tcu::eventbus::IgnitionChanged>(&event.payload);
                      if (payload == nullptr) {
                        return;
                      }
                      server.publishNotification(
                          "vehicle.ignitionChanged",
                          jsonlite::object("\"ignition\":" + jsonlite::boolValue(payload->on)));
                    });
  bus.start();

  if (!server.start()) {
    std::cerr << "[server] start failed for socket=" << args.socket_path << "\n";
    bus.stop();
    return 1;
  }
  std::cout << "[server] listening socket=" << args.socket_path << std::endl;

  // Toggle ignition on a timer so the demo has something to publish. The
  // period is a flag so a test can make it fast or switch it off.
  std::uint64_t event_id = 1;
  auto last = std::chrono::steady_clock::now();
  while (g_stop == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (args.ignition_period_ms <= 0) {
      continue;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::milliseconds(args.ignition_period_ms)) {
      continue;
    }
    last = now;
    const bool next = !ignition.load();
    ignition.store(next);
    tcu::eventbus::Event event;
    event.id = event_id++;
    event.type = tcu::eventbus::event_type_of<tcu::eventbus::IgnitionChanged>();
    event.created_at = now;
    event.payload = tcu::eventbus::IgnitionChanged{next};
    bus.publish(event);
  }

  std::cout << "[server] stopping" << std::endl;
  server.stop();
  bus.stop();
  return 0;
}
