// vehicle_simulator - the synthetic ECUs.
//
// Emits the periodic vehicle frames of spec section 6.1 and accepts runtime
// fault injection over project 02's IPC, so can_control_cli can pause a single
// identifier mid-run without restarting anything (research R-08).
//
// Two decisions worth knowing before reading:
//
//   * Periodic transmission uses project 03's TimerScheduler with
//     PeriodicMode::FixedRateSkipMissed. Its contract keeps the 100 ms and 1 s
//     phases of section 6.1 and counts missed slots instead of replaying them
//     as a burst - a burst would make the TCU's queue-drop metrics meaningless
//     (research R-04).
//   * Values come from a seeded std::mt19937, so a scenario replay produces the
//     same value sequence per identifier (NFR-09, B-29). Timing is real
//     monotonic, which is why B-29 asserts on the value sequence and not on
//     arrival times (research R-21).
//
// SAFETY: every identifier here is invented for the lab.

#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "tcu/can/can_config.hpp"
#include "tcu/can/frame_catalog.hpp"
#include "tcu/can/socketcan_transport.hpp"
#include "tcu/ipc/ipc_server.hpp"
#include "tcu/scheduler/clock_adapter.hpp"
#include "tcu/scheduler/timer_scheduler.hpp"

namespace {

using namespace std::chrono_literals;

volatile std::sig_atomic_t g_stop_requested = 0;

extern "C" void onSignal(int) { g_stop_requested = 1; }

/// The values the simulated ECUs currently believe, and which identifiers are
/// paused or deliberately corrupted. One mutex, because the IPC handlers and
/// the scheduler callbacks both touch it.
class VehicleState {
 public:
  explicit VehicleState(std::uint32_t seed) : rng_(seed) {}

  void set(const std::string& signal, double value) {
    std::lock_guard<std::mutex> lock(mutex_);
    values_[signal] = value;
  }

  double get(const std::string& signal, double fallback = 0.0) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = values_.find(signal);
    return it == values_.end() ? fallback : it->second;
  }

  void pause(std::uint32_t can_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    paused_.insert(can_id);
  }

  void resume(std::uint32_t can_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    paused_.erase(can_id);
    corrupt_.erase(can_id);
  }

  void corrupt(std::uint32_t can_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    corrupt_.insert(can_id);
  }

  bool paused(std::uint32_t can_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return paused_.count(can_id) != 0;
  }

  bool corrupted(std::uint32_t can_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return corrupt_.count(can_id) != 0;
  }

  std::uint8_t nextHeartbeat() {
    std::lock_guard<std::mutex> lock(mutex_);
    return heartbeat_++;  // wraps at 256, which is normal (UT-19)
  }

  std::string statusJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream out;
    out << "{\"paused\": [";
    bool first = true;
    for (const auto id : paused_) {
      out << (first ? "" : ", ") << id;
      first = false;
    }
    out << "], \"corrupt\": [";
    first = true;
    for (const auto id : corrupt_) {
      out << (first ? "" : ", ") << id;
      first = false;
    }
    out << "], \"signals\": {";
    first = true;
    for (const auto& entry : values_) {
      out << (first ? "" : ", ") << "\"" << entry.first << "\": " << entry.second;
      first = false;
    }
    out << "}}";
    return out.str();
  }

 private:
  mutable std::mutex mutex_;
  std::map<std::string, double> values_;
  std::set<std::uint32_t> paused_;
  std::set<std::uint32_t> corrupt_;
  std::mt19937 rng_;
  std::uint8_t heartbeat_{0};
};

tcu::can::CanMessage makeFrame(std::uint32_t id, std::array<std::uint8_t, 8> data,
                               tcu::can::SteadyTime now) {
  tcu::can::CanMessage frame;
  frame.id = id;
  frame.length = 8;
  frame.data = data;
  frame.received_at = now;
  return frame;
}

std::array<std::uint8_t, 8> encodeU16(double physical, double multiplier) {
  const auto raw = static_cast<std::uint32_t>(physical * multiplier + 0.5);
  return {static_cast<std::uint8_t>(raw & 0xFF), static_cast<std::uint8_t>((raw >> 8) & 0xFF), 0,
          0, 0, 0, 0, 0};
}

/// One scenario step. `value` is deliberately NOT range-checked, so a scenario
/// can emit an out-of-range value on purpose for B-11
/// (contracts/config_schema.md section 3).
struct ScenarioStep {
  std::chrono::milliseconds at{0};
  std::string action;
  std::string signal;
  double value{0.0};
  std::uint32_t can_id{0};
};

/// Minimal scenario reader. It does not reuse the library's JSON wrapper on
/// purpose: that wrapper is private to tcu_can, and a 40-line reader here is
/// cheaper than widening a library's surface for one caller.
bool loadScenario(const std::string& path, std::vector<ScenarioStep>& out, std::string& error) {
  std::ifstream in(path);
  if (!in) {
    error = "cannot open " + path;
    return false;
  }
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  const auto steps_at = text.find("\"steps\"");
  if (steps_at == std::string::npos) {
    error = "no steps array";
    return false;
  }

  std::size_t cursor = steps_at;
  std::chrono::milliseconds previous{-1};
  while (true) {
    const auto open = text.find('{', cursor);
    if (open == std::string::npos) {
      break;
    }
    const auto close = text.find('}', open);
    if (close == std::string::npos) {
      break;
    }
    const std::string object = text.substr(open, close - open + 1);
    cursor = close + 1;

    auto field = [&object](const std::string& key) -> std::string {
      const auto key_at = object.find("\"" + key + "\"");
      if (key_at == std::string::npos) {
        return {};
      }
      const auto colon = object.find(':', key_at);
      if (colon == std::string::npos) {
        return {};
      }
      const auto value_start = object.find_first_not_of(" \t", colon + 1);
      if (value_start == std::string::npos) {
        return {};
      }
      if (object[value_start] == '"') {
        const auto end = object.find('"', value_start + 1);
        return object.substr(value_start + 1, end - value_start - 1);
      }
      const auto end = object.find_first_of(",}", value_start);
      return object.substr(value_start, end - value_start);
    };

    ScenarioStep step;
    const auto at_text = field("at_ms");
    if (at_text.empty()) {
      error = "a step has no at_ms";
      return false;
    }
    step.at = std::chrono::milliseconds{std::stoll(at_text)};
    step.action = field("action");
    step.signal = field("signal");
    const auto value_text = field("value");
    if (!value_text.empty()) {
      step.value = std::stod(value_text);
    }
    const auto id_text = field("can_id");
    if (!id_text.empty()) {
      step.can_id = static_cast<std::uint32_t>(std::stoul(id_text));
    }

    if (step.action != "set_signal" && step.action != "pause_id" && step.action != "resume_id" &&
        step.action != "inject_invalid") {
      error = "unknown action: " + step.action;
      return false;
    }
    // Monotonically non-decreasing, as the contract requires: a scenario that
    // went backwards would execute in an order nobody wrote.
    if (step.at < previous) {
      error = "steps are not ordered by at_ms";
      return false;
    }
    previous = step.at;
    out.push_back(step);
  }

  if (out.empty()) {
    error = "the steps array is empty";
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path = "config/can_lab.json";
  std::string scenario_path;
  std::string socket_path = "/tmp/tcu_can_sim.sock";
  std::string interface_override;
  std::vector<std::uint32_t> initially_paused;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      std::cout << "usage: vehicle_simulator [--config PATH] [--scenario PATH]\n"
                << "                         [--interface NAME] [--socket PATH]\n"
                << "                         [--pause-id ID]...\n\n"
                << "  --pause-id ID   start with this identifier paused (decimal or 0x hex).\n"
                << "                  The same effect as the pause_id IPC method, so the\n"
                << "                  section 15 fault matrix stays literally true.\n";
      return 0;
    } else if (arg == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else if (arg == "--scenario" && i + 1 < argc) {
      scenario_path = argv[++i];
    } else if (arg == "--socket" && i + 1 < argc) {
      socket_path = argv[++i];
    } else if (arg == "--interface" && i + 1 < argc) {
      interface_override = argv[++i];
    } else if (arg == "--pause-id" && i + 1 < argc) {
      initially_paused.push_back(static_cast<std::uint32_t>(std::stoul(argv[++i], nullptr, 0)));
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return 2;
    }
  }

  tcu::can::CanLabConfig config;
  const auto load = tcu::can::loadCanLabConfig(config_path, config);
  if (!load) {
    std::cerr << "configuration rejected: " << tcu::can::loadErrorName(load.error) << " at "
              << (load.pointer.empty() ? "(file)" : load.pointer) << ": " << load.publicReason
              << "\n";
    return 1;
  }
  if (!interface_override.empty()) {
    config.interface_name = interface_override;
  }

  std::vector<ScenarioStep> scenario;
  if (!scenario_path.empty()) {
    std::string error;
    if (!loadScenario(scenario_path, scenario, error)) {
      std::cerr << "scenario rejected: " << error << "\n";
      return 1;
    }
  }

  auto clock = std::make_shared<tcu::scheduler::SteadyClockAdapter>();

  tcu::can::SocketCanTransport::Options options;
  options.interface_name = config.interface_name;
  options.clock = clock;
  // Transmit only: the simulator has no reason to read the bus, so it installs
  // no filter and never receives (contracts/frame_catalog.md section 4).
  auto opened = tcu::can::SocketCanTransport::open(options);
  if (std::holds_alternative<tcu::can::OpenError>(opened)) {
    const auto error = std::get<tcu::can::OpenError>(opened);
    std::cerr << "cannot open " << config.interface_name << ": failed at "
              << tcu::can::openErrorStepName(error.step) << " (errno " << error.os_error << ")\n"
              << "Is the interface up? Try scripts/setup_vcan.sh\n";
    return 1;
  }
  auto transport = std::move(std::get<std::unique_ptr<tcu::can::SocketCanTransport>>(opened));

  VehicleState state(config.simulator.fixed_seed);
  // Sensible starting values, so a run without a scenario still produces
  // in-range traffic.
  state.set("ignition", 1.0);
  state.set("speed_kmh", 0.0);
  state.set("battery_voltage_v", 12.6);
  state.set("battery_soc_pct", 85.0);
  for (const auto id : initially_paused) {
    state.pause(id);
  }

  auto send = [&transport, &state, &clock](std::uint32_t id, std::array<std::uint8_t, 8> data) {
    if (state.paused(id)) {
      return;
    }
    transport->send(makeFrame(id, data, clock->now()));
  };

  tcu::scheduler::Config scheduler_config;
  scheduler_config.workerThreads = 2;
  tcu::scheduler::TimerScheduler scheduler(scheduler_config, clock);
  if (scheduler.start() != tcu::scheduler::Error::None) {
    std::cerr << "cannot start the timer scheduler\n";
    return 1;
  }

  // FixedRateSkipMissed keeps the documented phase and never replays a backlog
  // as a burst (research R-04).
  const auto mode = tcu::scheduler::PeriodicMode::FixedRateSkipMissed;

  scheduler.scheduleEvery(config.simulator.speed_period, mode,
                          [&] { send(0x101, encodeU16(state.get("speed_kmh"), 100.0)); });

  scheduler.scheduleEvery(config.simulator.heartbeat_period, mode, [&] {
    send(0x110, {state.nextHeartbeat(), 0, 0, 0, 0, 0, 0, 0});
  });

  scheduler.scheduleEvery(1s, mode, [&] {
    const bool on = state.get("ignition") >= 0.5;
    send(0x100, {static_cast<std::uint8_t>(on ? 1 : 0), 0, 0, 0, 0, 0, 0, 0});

    auto battery = encodeU16(state.get("battery_voltage_v"), 100.0);
    // inject_invalid puts SOC out of range on purpose, so the TCU exercises
    // atomic frame reject on a real bus (B-11, the section 15 matrix).
    battery[2] = state.corrupted(0x102) ? static_cast<std::uint8_t>(120)
                                        : static_cast<std::uint8_t>(state.get("battery_soc_pct"));
    send(0x102, battery);
  });

  // The control plane. The IPC methods and the scenario steps call the same
  // handlers, so a file and a live command drive one code path, not two.
  tcu::ipc::ServerOptions server_options;
  server_options.socket_path = socket_path;
  tcu::ipc::IpcServer control(server_options);

  auto idFromPayload = [](const std::string& payload) -> std::uint32_t {
    const auto at = payload.find("can_id");
    if (at == std::string::npos) {
      return 0;
    }
    const auto colon = payload.find(':', at);
    if (colon == std::string::npos) {
      return 0;
    }
    return static_cast<std::uint32_t>(std::stoul(payload.substr(colon + 1), nullptr, 0));
  };

  control.registerHandler("pause_id", [&](std::string payload) {
    state.pause(idFromPayload(payload));
    return tcu::ipc::IpcResponse{tcu::ipc::IpcError::None, state.statusJson(), {}};
  });
  control.registerHandler("resume_id", [&](std::string payload) {
    state.resume(idFromPayload(payload));
    return tcu::ipc::IpcResponse{tcu::ipc::IpcError::None, state.statusJson(), {}};
  });
  control.registerHandler("inject_invalid", [&](std::string payload) {
    state.corrupt(idFromPayload(payload));
    return tcu::ipc::IpcResponse{tcu::ipc::IpcError::None, state.statusJson(), {}};
  });
  control.registerHandler("set_signal", [&](std::string payload) {
    // {"signal": "speed_kmh", "value": 60}
    const auto name_at = payload.find("\"signal\"");
    const auto value_at = payload.find("\"value\"");
    if (name_at == std::string::npos || value_at == std::string::npos) {
      return tcu::ipc::IpcResponse{tcu::ipc::IpcError::ProtocolError, {}, "missing_field"};
    }
    const auto name_open = payload.find('"', payload.find(':', name_at));
    const auto name_close = payload.find('"', name_open + 1);
    const std::string signal = payload.substr(name_open + 1, name_close - name_open - 1);
    const double value = std::stod(payload.substr(payload.find(':', value_at) + 1));
    state.set(signal, value);
    return tcu::ipc::IpcResponse{tcu::ipc::IpcError::None, state.statusJson(), {}};
  });
  control.registerHandler("scenario_status", [&](std::string) {
    return tcu::ipc::IpcResponse{tcu::ipc::IpcError::None, state.statusJson(), {}};
  });

  if (!control.start()) {
    std::cerr << "warning: control socket " << socket_path
              << " is unavailable; the simulator runs without it\n";
  }

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  std::cout << "vehicle_simulator on " << config.interface_name << ", seed "
            << config.simulator.fixed_seed << ", control socket " << socket_path << "\n";
  if (!scenario.empty()) {
    std::cout << "scenario: " << scenario.size() << " steps\n";
  }

  // Scenario steps run on this thread against the real monotonic clock
  // (research R-21): at_ms is an offset from start, not a wall-clock time.
  const auto started = clock->now();
  std::size_t next_step = 0;

  while (g_stop_requested == 0) {
    while (next_step < scenario.size() && clock->now() - started >= scenario[next_step].at) {
      const auto& step = scenario[next_step];
      if (step.action == "set_signal") {
        state.set(step.signal, step.value);
      } else if (step.action == "pause_id") {
        state.pause(step.can_id);
      } else if (step.action == "resume_id") {
        state.resume(step.can_id);
      } else if (step.action == "inject_invalid") {
        state.corrupt(step.can_id);
      }
      std::cout << "step " << next_step << ": " << step.action << "\n";
      ++next_step;
    }
    std::this_thread::sleep_for(10ms);
  }

  std::cout << "\nstopping...\n";
  control.stop();
  scheduler.stop(tcu::scheduler::StopMode::Drain);
  return 0;
}
