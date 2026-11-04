// T092: the "rapid restart" row of the spec section 10 fault matrix, and the
// acceptance criterion "khong FD leaks ro rang trong test" (section 13) made
// executable by comparing /proc/self/fd before and after.
//
// The service is recreated in-process rather than forked here on purpose: the
// FD count that matters for a leak is this process's, and a forked child's
// descriptors are not in it. The SIGKILL path across a real process boundary
// is covered by tests/integration/ipc/reconnect_test.cpp.

#include <gtest/gtest.h>

#include <dirent.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "support/temp_socket_dir.hpp"
#include "tcu/ipc/ipc_client.hpp"
#include "tcu/ipc/ipc_server.hpp"

namespace {

using namespace std::chrono_literals;
using tcu::ipc::ClientOptions;
using tcu::ipc::ClientState;
using tcu::ipc::IpcClient;
using tcu::ipc::IpcError;
using tcu::ipc::IpcResponse;
using tcu::ipc::IpcServer;
using tcu::ipc::ServerOptions;
using tcu::ipc::test::TempSocketDir;

constexpr int kCycles = 20;

std::size_t openFdCount() {
  std::size_t n = 0;
  DIR* d = ::opendir("/proc/self/fd");
  if (d == nullptr) {
    return 0;
  }
  while (::readdir(d) != nullptr) {
    ++n;
  }
  ::closedir(d);
  return n;
}

bool waitState(const IpcClient& client, ClientState wanted,
               std::chrono::milliseconds budget = 8000ms) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (client.state() == wanted) {
      return true;
    }
    std::this_thread::sleep_for(5ms);
  }
  return false;
}

std::unique_ptr<IpcServer> startService(const std::string& socket_path) {
  ServerOptions options;
  options.socket_path = socket_path;
  auto server = std::make_unique<IpcServer>(options);
  const bool registered = server->registerHandler("vehicle.getStatus", [](std::string) {
    IpcResponse r;
    r.payload_json = "{\"ignition\":true,\"speed_kph\":0,\"battery_pct\":79}";
    return r;
  });
  if (!registered || !server->start()) {
    return nullptr;
  }
  return server;
}

TEST(FaultRapidRestart, TwentyCyclesReconnectWithoutLeakingDescriptors) {
  TempSocketDir dir;
  const std::size_t fds_at_start = openFdCount();

  ClientOptions client_options;
  client_options.socket_path = dir.socketPath();
  IpcClient client(client_options);

  {
    std::unique_ptr<IpcServer> service = startService(dir.socketPath());
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(waitState(client, ClientState::Connected));

    int reconnects_seen = 0;
    for (int cycle = 0; cycle < kCycles; ++cycle) {
      service->stop();
      service.reset();

      // The client must notice and back off, never spin.
      ASSERT_TRUE(waitState(client, ClientState::Backoff, 3000ms))
          << "cycle " << cycle << ": the client did not enter Backoff";

      service = startService(dir.socketPath());
      ASSERT_NE(service, nullptr) << "cycle " << cycle << ": the service did not restart";
      ASSERT_TRUE(waitState(client, ClientState::Connected, 10000ms))
          << "cycle " << cycle << ": the client did not reconnect";
      ++reconnects_seen;

      // The connection is usable again after every cycle.
      auto future = client.request("vehicle.getStatus", "{}", 5000ms);
      ASSERT_EQ(future.wait_for(6s), std::future_status::ready) << "cycle " << cycle;
      EXPECT_EQ(future.get().error, IpcError::None) << "cycle " << cycle;
    }

    EXPECT_EQ(reconnects_seen, kCycles);
    EXPECT_GE(client.metrics().reconnect_total, static_cast<std::uint64_t>(kCycles));

    const auto m = client.metrics();
    EXPECT_EQ(m.requests_created_total,
              m.requests_completed_total + m.request_timeouts_total + m.requests_failed_total);

    client.stop();
    service->stop();
  }

  // Acceptance criterion 8: no descriptor was leaked across 20 kill/restart
  // cycles. This catches a double-open as well as a missing close.
  EXPECT_EQ(openFdCount(), fds_at_start);
}

}  // namespace
