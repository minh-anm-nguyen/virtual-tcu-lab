// Integration 6 of spec section 12.2 / equations E3, E4: the service is shut
// down while a client is stopping, and neither process may hang.

#include <gtest/gtest.h>

#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>

#include "helpers/server_process.hpp"
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
using tcu::ipc::test::ServerProcess;
using tcu::ipc::test::TempSocketDir;

bool waitConnected(const IpcClient& client, std::chrono::milliseconds budget = 8000ms) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (client.state() == ClientState::Connected) {
      return true;
    }
    std::this_thread::sleep_for(5ms);
  }
  return false;
}

bool pathExists(const std::string& path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0;
}

TEST(IntegrationServerLifecycle, ShutdownWhileAClientIsStoppingHangsNeither) {
  TempSocketDir dir;
  ServerProcess service;
  ASSERT_TRUE(service.start(dir.socketPath(), 100));

  ClientOptions options;
  options.socket_path = dir.socketPath();
  IpcClient client(options);
  ASSERT_TRUE(client.start());
  ASSERT_TRUE(waitConnected(client));
  client.subscribe("vehicle.ignitionChanged", [](std::string) {});

  auto inflight = client.request("vehicle.getStatus", "{}", 5000ms);

  // Stop both sides at the same time; neither may block the other.
  std::atomic<bool> client_stopped{false};
  std::thread stopper([&client, &client_stopped] {
    client.stop();
    client_stopped.store(true);
  });
  service.terminate();

  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!client_stopped.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_TRUE(client_stopped.load()) << "the client did not stop in bounded time";
  stopper.join();

  ASSERT_EQ(inflight.wait_for(2s), std::future_status::ready);
  const IpcError error = inflight.get().error;
  EXPECT_TRUE(error == IpcError::Cancelled || error == IpcError::PeerClosed ||
              error == IpcError::None)
      << "unexpected outcome: " << tcu::ipc::toString(error);

  const auto m = client.metrics();
  EXPECT_EQ(m.requests_created_total,
            m.requests_completed_total + m.request_timeouts_total + m.requests_failed_total);
  // Equation E3.
  EXPECT_EQ(m.tx_queue_bytes, 0u);
  EXPECT_EQ(client.state(), ClientState::Stopped);
}

// Equation E4, plus the socket-file postcondition of contracts/ipc_api.md.
TEST(IntegrationServerLifecycle, StopClosesEverySessionAndRemovesTheSocket) {
  TempSocketDir dir;
  ServerOptions server_options;
  server_options.socket_path = dir.socketPath();
  IpcServer server(server_options);
  ASSERT_TRUE(server.registerHandler("vehicle.ok", [](std::string) { return IpcResponse{}; }));
  ASSERT_TRUE(server.start());
  EXPECT_TRUE(pathExists(dir.socketPath()));

  ClientOptions client_options;
  client_options.socket_path = dir.socketPath();
  IpcClient a(client_options);
  IpcClient b(client_options);
  ASSERT_TRUE(a.start());
  ASSERT_TRUE(b.start());
  ASSERT_TRUE(waitConnected(a));
  ASSERT_TRUE(waitConnected(b));
  EXPECT_EQ(server.sessionCount(), 2u);

  server.stop();
  EXPECT_EQ(server.metrics().connections_active, 0u);
  EXPECT_EQ(server.metrics().tx_queue_bytes, 0u);
  // The server created the socket, so it unlinks it.
  EXPECT_FALSE(pathExists(dir.socketPath()));

  a.stop();
  b.stop();
}

}  // namespace
