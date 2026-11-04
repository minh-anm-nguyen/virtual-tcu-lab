// Project 02 demo client. Scenarios of spec section 16:
//
//   status                 one vehicle.getStatus request and exit
//   status-and-subscribe   request, subscribe, then survive a kill/restart of
//                          the service and keep receiving notifications
//   slow-reader            connect, handshake, then stop reading, so the
//                          server's per-session TX queue must stay capped
//
// The lines printed here are the demo transcript of spec section 16.1. The
// authoritative record of a subscribe ACK is the library's own structured log
// on stderr (events subscription_active / subscription_restored), which the
// integration tests assert; stdout is for a human watching the demo.

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "tcu/ipc/ipc_client.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void onSignal(int) { g_stop = 1; }

struct Args {
  std::string socket_path{"/tmp/virtual-tcu/vehicle.sock"};
  std::string scenario{"status"};
  int timeout_ms{2000};
  int seconds{12};
  bool ok{true};
};

Args parseArgs(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    const bool has_value = i + 1 < argc;
    if (flag == "--socket" && has_value) {
      a.socket_path = argv[++i];
    } else if (flag == "--scenario" && has_value) {
      a.scenario = argv[++i];
    } else if (flag == "--timeout-ms" && has_value) {
      a.timeout_ms = std::atoi(argv[++i]);
    } else if (flag == "--seconds" && has_value) {
      a.seconds = std::atoi(argv[++i]);
    } else {
      std::cerr << "[client] unknown or incomplete flag: " << flag << "\n";
      a.ok = false;
      return a;
    }
  }
  return a;
}

const char* outcomeOf(tcu::ipc::IpcError error) {
  return error == tcu::ipc::IpcError::None ? "success" : tcu::ipc::toString(error);
}

bool waitForState(const tcu::ipc::IpcClient& client, tcu::ipc::ClientState wanted,
                  std::chrono::milliseconds budget) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (client.state() == wanted) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return client.state() == wanted;
}

int runStatus(const Args& args, tcu::ipc::IpcClient& client) {
  if (!waitForState(client, tcu::ipc::ClientState::Connected, std::chrono::seconds(5))) {
    std::cout << "[client] state=" << tcu::ipc::toString(client.state())
              << " reason=not_connected" << std::endl;
    return 1;
  }
  std::cout << "[client] state=Connected protocol=v1" << std::endl;

  auto future =
      client.request("vehicle.getStatus", "{}", std::chrono::milliseconds(args.timeout_ms));
  const tcu::ipc::IpcResponse response = future.get();
  std::cout << "[client] request_id=1 method=vehicle.getStatus outcome="
            << outcomeOf(response.error) << std::endl;
  if (response.error == tcu::ipc::IpcError::None) {
    std::cout << "[client] payload=" << response.payload_json << std::endl;
    return 0;
  }
  std::cout << "[client] detail=" << response.detail << std::endl;
  return 1;
}

int runStatusAndSubscribe(const Args& args, tcu::ipc::IpcClient& client) {
  const std::string topic = "vehicle.ignitionChanged";

  if (runStatus(args, client) != 0) {
    return 1;
  }

  std::mutex out_mutex;
  client.subscribe(topic, [&out_mutex, &topic](std::string payload) {
    std::lock_guard<std::mutex> lock(out_mutex);
    std::cout << "[client] notification topic=" << topic << " payload=" << payload << std::endl;
  });
  std::cout << "[client] subscription=" << topic << " outcome=active" << std::endl;

  // Watch the state machine for the rest of the run: the kill, the backoff and
  // the reconnect are what the demo exists to show.
  tcu::ipc::ClientState last = client.state();
  std::uint64_t last_reconnects = client.metrics().reconnect_total;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(args.seconds);

  while (g_stop == 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    const tcu::ipc::ClientState now = client.state();
    const tcu::ipc::IpcMetricsSnapshot m = client.metrics();
    if (now == last) {
      continue;
    }
    std::lock_guard<std::mutex> lock(out_mutex);
    if (now == tcu::ipc::ClientState::Backoff) {
      std::cout << "[client] state=Backoff reason=PeerClosed pending_failed="
                << m.requests_failed_total << std::endl;
    } else if (now == tcu::ipc::ClientState::Connected && m.reconnect_total > last_reconnects) {
      last_reconnects = m.reconnect_total;
      std::cout << "[client] state=Connected reconnect_count=" << m.reconnect_total << std::endl;
      std::cout << "[client] subscription=" << topic << " outcome=restored" << std::endl;
    }
    last = now;
  }
  return 0;
}

/// A peer that completes the handshake and then stops reading. Needed because
/// IpcClient always drains its socket, so a slow consumer cannot be simulated
/// with it (fault matrix row "slow client", B-13).
int runSlowReader(const Args& args) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    std::cerr << "[client] socket failed\n";
    return 1;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (args.socket_path.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    return 1;
  }
  std::memcpy(addr.sun_path, args.socket_path.c_str(), args.socket_path.size());
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::cerr << "[client] connect failed\n";
    ::close(fd);
    return 1;
  }

  // ipc.hello, then ipc.subscribe, hand-framed so this peer needs no client.
  auto frame = [](std::uint8_t type, std::uint64_t id, const std::string& name,
                  const std::string& payload) {
    std::string out;
    const std::uint32_t body = static_cast<std::uint32_t>(14 + name.size() + payload.size());
    for (int shift = 24; shift >= 0; shift -= 8) {
      out.push_back(static_cast<char>((body >> shift) & 0xFF));
    }
    out.push_back(static_cast<char>(1));  // version
    out.push_back(static_cast<char>(type));
    out.push_back(static_cast<char>(0));
    out.push_back(static_cast<char>(0));  // flags
    for (int shift = 56; shift >= 0; shift -= 8) {
      out.push_back(static_cast<char>((id >> shift) & 0xFF));
    }
    out.push_back(static_cast<char>((name.size() >> 8) & 0xFF));
    out.push_back(static_cast<char>(name.size() & 0xFF));
    out += name;
    out += payload;
    return out;
  };

  const std::string hello = frame(1, 1, "ipc.hello", "{\"version\":1,\"client_id\":\"slow\"}");
  const std::string sub = frame(1, 2, "ipc.subscribe", "{\"topic\":\"vehicle.ignitionChanged\"}");
  if (::send(fd, hello.data(), hello.size(), MSG_NOSIGNAL) < 0 ||
      ::send(fd, sub.data(), sub.size(), MSG_NOSIGNAL) < 0) {
    ::close(fd);
    return 1;
  }

  std::cout << "[client] scenario=slow-reader state=connected reading=off" << std::endl;
  std::cout << "[client] holding the connection open without reading for " << args.seconds
            << "s; the server TX queue must stay capped" << std::endl;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(args.seconds);
  while (g_stop == 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  ::close(fd);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = parseArgs(argc, argv);
  if (!args.ok) {
    std::cerr << "usage: tcu_client --socket PATH [--scenario status|status-and-subscribe|"
              << "slow-reader] [--timeout-ms N] [--seconds N]\n";
    return 2;
  }

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  if (args.scenario == "slow-reader") {
    return runSlowReader(args);
  }

  tcu::ipc::ClientOptions options;
  options.socket_path = args.socket_path;
  options.request_timeout = std::chrono::milliseconds(args.timeout_ms);
  options.client_id = "tcu_client";

  tcu::ipc::IpcClient client(options);
  if (!client.start()) {
    std::cerr << "[client] start failed\n";
    return 1;
  }

  int rc = 0;
  if (args.scenario == "status") {
    rc = runStatus(args, client);
  } else if (args.scenario == "status-and-subscribe") {
    rc = runStatusAndSubscribe(args, client);
  } else {
    std::cerr << "[client] unknown scenario: " << args.scenario << "\n";
    rc = 2;
  }

  client.stop();
  return rc;
}
