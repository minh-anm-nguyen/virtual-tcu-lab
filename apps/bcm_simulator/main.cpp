// bcm_simulator - a synthetic body control module.
//
// Listens for 0x200 lock commands, answers with 0x201, and publishes 0x103
// door and lock state on change and every second.
//
// The lab policy is explicit, which is what spec open question 3 asks for: a
// command is accepted regardless of ignition state (research R-20). A real
// vehicle would evaluate a safety policy here; this one deliberately does not
// pretend to, because pretending would teach the wrong lesson (ADR-07).
//
// SAFETY: 0x200 and 0x201 are invented identifiers. Nothing here may be pointed
// at a real body control module.

#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <variant>

#include "tcu/can/frame_catalog.hpp"
#include "tcu/can/socketcan_transport.hpp"
#include "tcu/scheduler/clock_adapter.hpp"

namespace {

using namespace std::chrono_literals;

volatile std::sig_atomic_t g_stop_requested = 0;

extern "C" void onSignal(int) { g_stop_requested = 1; }

/// The lab door state, as the five bits of 0x103 byte 0.
struct DoorState {
  bool locked{true};
  std::uint8_t open_mask{0};  // bits 0-3, one per door

  std::uint8_t encode() const {
    return static_cast<std::uint8_t>((open_mask & 0x0F) | (locked ? 0x10 : 0x00));
  }
};

tcu::can::CanMessage doorFrame(const DoorState& state, tcu::can::SteadyTime now) {
  tcu::can::CanMessage frame;
  frame.id = 0x103;
  frame.length = 8;
  // Every reserved bit zero: that is the V1 sender rule
  // (contracts/frame_catalog.md section 1).
  frame.data = {state.encode(), 0, 0, 0, 0, 0, 0, 0};
  frame.received_at = now;
  return frame;
}

tcu::can::CanMessage ackFrame(std::uint8_t sequence, std::uint8_t result,
                              tcu::can::SteadyTime now) {
  tcu::can::CanMessage frame;
  frame.id = 0x201;
  frame.length = 8;
  frame.data = {sequence, result, 0, 0, 0, 0, 0, 0};
  frame.received_at = now;
  return frame;
}

}  // namespace

int main(int argc, char** argv) {
  std::string interface_name = "vcan0";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      std::cout << "usage: bcm_simulator [--interface NAME]\n";
      return 0;
    }
    if (arg == "--interface" && i + 1 < argc) {
      interface_name = argv[++i];
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return 2;
    }
  }

  auto clock = std::make_shared<tcu::scheduler::SteadyClockAdapter>();

  tcu::can::SocketCanTransport::Options options;
  options.interface_name = interface_name;
  options.clock = clock;
  // Only the command identifier: the BCM has no business seeing vehicle
  // telemetry (contracts/frame_catalog.md section 4).
  options.rx_filter = {tcu::can::kBcmRxFilterId};

  auto opened = tcu::can::SocketCanTransport::open(options);
  if (std::holds_alternative<tcu::can::OpenError>(opened)) {
    const auto error = std::get<tcu::can::OpenError>(opened);
    std::cerr << "cannot open " << interface_name << ": failed at "
              << tcu::can::openErrorStepName(error.step) << " (errno " << error.os_error << ")\n"
              << "Is the interface up? Try scripts/setup_vcan.sh\n";
    return 1;
  }
  auto transport = std::move(std::get<std::unique_ptr<tcu::can::SocketCanTransport>>(opened));

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  DoorState doors;
  auto last_periodic = clock->now();

  std::cout << "bcm_simulator on " << interface_name << ", listening for 0x200\n";
  // The initial state, so a TCU that starts later still learns it within a
  // second rather than waiting for a command.
  transport->send(doorFrame(doors, clock->now()));

  while (g_stop_requested == 0) {
    auto result = transport->receive(100ms);

    if (result.status == tcu::can::ReadStatus::Received && result.message->id == 0x200) {
      const auto& frame = *result.message;
      const std::uint8_t command = frame.data[0];
      const std::uint8_t sequence = frame.data[1];

      std::uint8_t reply = tcu::can::kResultOk;
      if (command == tcu::can::kCommandLock) {
        doors.locked = true;
      } else if (command == tcu::can::kCommandUnlock) {
        doors.locked = false;
      } else {
        // An unrecognised command byte is INVALID, not REJECTED: the two mean
        // different things to the requester, and B-24 checks this one.
        reply = tcu::can::kResultInvalid;
      }

      transport->send(ackFrame(sequence, reply, clock->now()));
      std::cout << "0x200 command=" << static_cast<int>(command)
                << " sequence=" << static_cast<int>(sequence)
                << " -> 0x201 result=" << static_cast<int>(reply) << "\n";

      if (reply == tcu::can::kResultOk) {
        // State changed, so the door frame follows immediately rather than at
        // the next periodic slot (spec section 11.3 sequence diagram).
        transport->send(doorFrame(doors, clock->now()));
        last_periodic = clock->now();
      }
    } else if (result.status == tcu::can::ReadStatus::LinkError ||
               result.status == tcu::can::ReadStatus::FatalError) {
      std::cerr << "link problem (errno " << result.os_error << "); stopping\n";
      break;
    }

    // Periodic 0x103 every second, per the section 6.1 cycle column. Driven
    // from the poll loop rather than a timer thread: the loop already wakes
    // every 100 ms, so a second thread would buy nothing.
    const auto now = clock->now();
    if (now - last_periodic >= 1s) {
      transport->send(doorFrame(doors, now));
      last_periodic = now;
    }
  }

  std::cout << "\nbcm_simulator stopped\n";
  return 0;
}
