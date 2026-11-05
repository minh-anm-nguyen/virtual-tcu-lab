// can_control_cli - the operator console for the laboratory.
//
// Talks to vehicle_simulator and tcu_can_service over project 02's IPC
// (research R-08). It is the "Control API" the spec section 4 actor table gives
// can_control_cli, and the tool every quickstart section 6 scenario drives.

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "tcu/ipc/ipc_client.hpp"

namespace {

using namespace std::chrono_literals;

constexpr const char* kSimulatorSocket = "/tmp/tcu_can_sim.sock";
constexpr const char* kServiceSocket = "/tmp/tcu_can_service.sock";

void printUsage() {
  std::cout
      << "usage: can_control_cli <command> [args]\n\n"
      << "Read the TCU service:\n"
      << "  health                    the section 16.2 health surface\n"
      << "  metrics                   the metrics snapshot\n"
      << "  lock | unlock             send a synthetic 0x200 command\n\n"
      << "Drive the simulator:\n"
      << "  pause-id <ID>             stop transmitting one identifier\n"
      << "  resume-id <ID>            resume it (and clear any corruption)\n"
      << "  inject-invalid <ID>       emit an out-of-range value on it\n"
      << "  set-signal <NAME> <VAL>   set a signal value\n"
      << "  scenario-status           what is paused, corrupt and current\n\n"
      << "Options:\n"
      << "  --sim-socket PATH         default " << kSimulatorSocket << "\n"
      << "  --service-socket PATH     default " << kServiceSocket << "\n\n"
      << "Identifiers accept decimal or 0x hex, so 257 and 0x101 are the same.\n";
}

/// One request, one answer, then the client goes away. A CLI has nothing to
/// keep a connection open for, and a short-lived client is also what proves the
/// server handles reconnection.
int callOnce(const std::string& socket_path, const std::string& method,
             const std::string& payload) {
  tcu::ipc::ClientOptions options;
  options.socket_path = socket_path;
  options.request_timeout = 2000ms;
  tcu::ipc::IpcClient client(options);

  if (!client.start()) {
    std::cerr << "cannot connect to " << socket_path << "\n"
              << "Is the process running? See quickstart.md section 5.\n";
    return 1;
  }

  auto future = client.request(method, payload, options.request_timeout);
  const auto response = future.get();
  client.stop();

  if (response.error != tcu::ipc::IpcError::None) {
    // detail is a short reason, never a payload excerpt - project 02's
    // invariant P2, which is the same rule project 05 applies to its own
    // configuration errors.
    std::cerr << "request failed: " << response.detail << "\n";
    return 1;
  }

  std::cout << response.payload_json << "\n";
  return 0;
}

std::string idPayload(const std::string& text) {
  // Accepts 257 and 0x101 alike, because the spec writes identifiers in hex and
  // the configuration file writes them in decimal.
  const auto id = static_cast<std::uint32_t>(std::stoul(text, nullptr, 0));
  return "{\"can_id\": " + std::to_string(id) + "}";
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args;
  std::string sim_socket = kSimulatorSocket;
  std::string service_socket = kServiceSocket;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--sim-socket" && i + 1 < argc) {
      sim_socket = argv[++i];
    } else if (arg == "--service-socket" && i + 1 < argc) {
      service_socket = argv[++i];
    } else {
      args.push_back(arg);
    }
  }

  if (args.empty() || args[0] == "--help" || args[0] == "-h") {
    printUsage();
    return args.empty() ? 2 : 0;
  }

  const std::string& command = args[0];

  if (command == "health") {
    return callOnce(service_socket, "health", "{}");
  }
  if (command == "metrics") {
    return callOnce(service_socket, "metrics", "{}");
  }
  if (command == "lock") {
    return callOnce(service_socket, "lock", "{}");
  }
  if (command == "unlock") {
    return callOnce(service_socket, "unlock", "{}");
  }
  if (command == "scenario-status") {
    return callOnce(sim_socket, "scenario_status", "{}");
  }

  if (command == "pause-id" || command == "resume-id" || command == "inject-invalid") {
    if (args.size() < 2) {
      std::cerr << command << " needs an identifier\n";
      return 2;
    }
    const std::string method = command == "pause-id"    ? "pause_id"
                               : command == "resume-id" ? "resume_id"
                                                        : "inject_invalid";
    return callOnce(sim_socket, method, idPayload(args[1]));
  }

  if (command == "set-signal") {
    if (args.size() < 3) {
      std::cerr << "set-signal needs a name and a value\n";
      return 2;
    }
    // The value is passed through unchecked on purpose: a scenario has to be
    // able to request an out-of-range value so the TCU can reject it (B-11).
    const std::string payload = "{\"signal\": \"" + args[1] + "\", \"value\": " + args[2] + "}";
    return callOnce(sim_socket, "set_signal", payload);
  }

  std::cerr << "unknown command: " << command << "\n";
  printUsage();
  return 2;
}
