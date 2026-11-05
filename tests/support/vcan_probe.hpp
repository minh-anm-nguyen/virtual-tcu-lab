#pragma once

// Is a usable vcan0 present on this host?
//
// Project 05 integration tests SKIP when it is absent rather than fail
// (specs/005-virtual-can-bus-laboratory/research.md R-01). The reason is
// concrete: the development host for this repository is Windows with WSL2,
// whose stock kernel ships no vcan module at all, and creating the interface
// also needs CAP_NET_ADMIN. A suite that fails on every developer machine
// teaches people to ignore red builds.
//
// The skip is always printed. An invisible skip is indistinguishable from a
// pass, and spec section 18.2's test note forbids claiming an untested path
// passed.

#include <linux/can.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>

namespace tcu::test {

struct VcanProbe {
  bool available{false};
  /// Empty when available; otherwise why not, for the skip message.
  std::string reason;
};

/// Probed once per process: the answer cannot change while a test binary runs,
/// and a per-test socket() would be noise in a strace.
inline const VcanProbe& probeVcan(const char* interface_name = "vcan0") {
  static const VcanProbe result = [interface_name] {
    VcanProbe probe;

    const unsigned int ifindex = ::if_nametoindex(interface_name);
    if (ifindex == 0) {
      probe.reason = std::string("interface ") + interface_name +
                     " does not exist (load the vcan module and create it: "
                     "scripts/setup_vcan.sh)";
      return probe;
    }

    const int fd = ::socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC, CAN_RAW);
    if (fd < 0) {
      probe.reason = "PF_CAN sockets are unavailable on this kernel";
      return probe;
    }

    sockaddr_can addr{};
    addr.can_family = AF_CAN;
    addr.can_ifindex = static_cast<int>(ifindex);
    const bool bound = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);

    if (!bound) {
      probe.reason = std::string("cannot bind to ") + interface_name +
                     " (is it up? does this process have CAP_NET_ADMIN?)";
      return probe;
    }

    probe.available = true;
    return probe;
  }();
  return result;
}

}  // namespace tcu::test

/// Put this first in any test that needs a real bus.
#define TCU_SKIP_WITHOUT_VCAN()                                              \
  do {                                                                       \
    const auto& probe = ::tcu::test::probeVcan();                            \
    if (!probe.available) {                                                  \
      GTEST_SKIP() << "vcan unavailable: " << probe.reason;                  \
    }                                                                        \
  } while (false)
