// T027, T028, T054-T056, T067-T069: handshake gating, shutdown and reconnect.
// B-01, B-08, B-15, B-16, B-17, B-19, B-20, invariants C1, C2, C3, C5.

#include <gtest/gtest.h>

#include <dirent.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "support/raw_listener.hpp"
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
using tcu::ipc::test::RawConnection;
using tcu::ipc::test::RawListener;
using tcu::ipc::test::rawFrame;
using tcu::ipc::test::TempSocketDir;

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

bool waitConnected(const IpcClient& client, std::chrono::milliseconds budget = 3000ms) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (client.state() == ClientState::Connected) {
      return true;
    }
    std::this_thread::sleep_for(5ms);
  }
  return false;
}

// T027 / B-01 / invariant C1
TEST(ClientLifecycle, ConnectedOnlyAfterHelloAck) {
  TempSocketDir dir;
  RawListener listener;
  ASSERT_TRUE(listener.start(dir.socketPath()));

  ClientOptions options;
  options.socket_path = dir.socketPath();
  IpcClient client(options);
  ASSERT_TRUE(client.start());

  // The peer accepts but never answers ipc.hello.
  RawConnection peer = listener.acceptPeer(2000ms);
  ASSERT_TRUE(peer.valid());
  const std::string hello = peer.readAtLeast(18, 1000ms);
  EXPECT_NE(hello.find("ipc.hello"), std::string::npos);

  std::this_thread::sleep_for(500ms);
  EXPECT_NE(client.state(), ClientState::Connected);

  client.stop();
}

// T028 / B-19 / invariant C2
TEST(ClientLifecycle, VersionMismatchIsTerminal) {
  TempSocketDir dir;
  RawListener listener;
  ASSERT_TRUE(listener.start(dir.socketPath()));

  ClientOptions options;
  options.socket_path = dir.socketPath();
  IpcClient client(options);
  ASSERT_TRUE(client.start());

  RawConnection peer = listener.acceptPeer(2000ms);
  ASSERT_TRUE(peer.valid());
  const std::string hello = peer.readAtLeast(18, 1000ms);
  ASSERT_NE(hello.find("ipc.hello"), std::string::npos);

  const std::uint64_t id = tcu::ipc::test::frameRequestId(hello);
  ASSERT_TRUE(peer.writeBytes(
      rawFrame(4, id, "ipc.hello", "{\"code\":\"UNSUPPORTED_VERSION\",\"message\":\"v1 only\"}")));

  // A fixed configuration error must not spin: terminal, not Backoff.
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline && client.state() != ClientState::Stopping &&
         client.state() != ClientState::Stopped) {
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_TRUE(client.state() == ClientState::Stopping || client.state() == ClientState::Stopped);

  const std::uint64_t attempts_then = client.metrics().connect_attempts_total;
  std::this_thread::sleep_for(600ms);
  EXPECT_EQ(client.metrics().connect_attempts_total, attempts_then)
      << "a version mismatch must not drive a reconnect loop";

  client.stop();
}

// T054 / B-15
TEST(ClientLifecycle, StopIsIdempotentAndClosesEveryFdOnce) {
  const std::size_t before = openFdCount();
  {
    TempSocketDir dir;
    ServerOptions server_options;
    server_options.socket_path = dir.socketPath();
    IpcServer server(server_options);
    ASSERT_TRUE(server.start());

    ClientOptions client_options;
    client_options.socket_path = dir.socketPath();
    IpcClient client(client_options);
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(waitConnected(client));

    client.stop();
    client.stop();
    server.stop();
    server.stop();
    EXPECT_EQ(client.state(), ClientState::Stopped);
  }
  EXPECT_EQ(openFdCount(), before);
}

// T055 / B-17
TEST(ClientLifecycle, RequestAfterStopIsShuttingDown) {
  TempSocketDir dir;
  ServerOptions server_options;
  server_options.socket_path = dir.socketPath();
  IpcServer server(server_options);
  ASSERT_TRUE(server.registerHandler("vehicle.ok", [](std::string) { return IpcResponse{}; }));
  ASSERT_TRUE(server.start());

  ClientOptions client_options;
  client_options.socket_path = dir.socketPath();
  IpcClient client(client_options);
  ASSERT_TRUE(client.start());
  ASSERT_TRUE(waitConnected(client));
  client.stop();

  auto future = client.request("vehicle.ok", "{}", 1000ms);
  ASSERT_EQ(future.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(future.get().error, IpcError::ShuttingDown);

  server.stop();
}

// T056 / research R-09
TEST(ClientLifecycle, StopIsBoundedAndCancelsInflight) {
  TempSocketDir dir;
  ServerOptions server_options;
  server_options.socket_path = dir.socketPath();
  IpcServer server(server_options);
  ASSERT_TRUE(server.registerHandler("vehicle.slow", [](std::string) {
    std::this_thread::sleep_for(2000ms);
    return IpcResponse{};
  }));
  ASSERT_TRUE(server.start());

  ClientOptions client_options;
  client_options.socket_path = dir.socketPath();
  IpcClient client(client_options);
  ASSERT_TRUE(client.start());
  ASSERT_TRUE(waitConnected(client));

  std::vector<std::future<IpcResponse>> futures;
  for (int i = 0; i < 4; ++i) {
    futures.push_back(client.request("vehicle.slow", "{}", 10000ms));
  }
  std::this_thread::sleep_for(100ms);

  const auto before = std::chrono::steady_clock::now();
  client.stop();
  const auto elapsed = std::chrono::steady_clock::now() - before;
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 3000);

  for (auto& future : futures) {
    ASSERT_EQ(future.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(future.get().error, IpcError::Cancelled);
  }
  const auto m = client.metrics();
  EXPECT_EQ(m.requests_created_total,
            m.requests_completed_total + m.request_timeouts_total + m.requests_failed_total);
  EXPECT_EQ(m.tx_queue_bytes, 0u);

  server.stop();
}

// T068 / spec section 3.2
TEST(ClientLifecycle, BackoffGrowsRatherThanSpinning) {
  TempSocketDir dir;  // nothing is listening on this path
  ClientOptions options;
  options.socket_path = dir.socketPath();
  IpcClient client(options);
  ASSERT_TRUE(client.start());

  std::this_thread::sleep_for(2000ms);
  const std::uint64_t attempts = client.metrics().connect_attempts_total;
  client.stop();

  // 100 + 200 + 400 + 800 + 1600 ms covers 2 s in about five attempts. A tight
  // retry loop would be in the thousands.
  EXPECT_GE(attempts, 2u);
  EXPECT_LE(attempts, 12u) << "connect attempts look like a spin, not a backoff";
}

// T069 / B-16 (the TSan target; here the assertion is bounded termination)
TEST(ClientLifecycle, StopRacesReconnect) {
  TempSocketDir dir;  // nothing is listening, so the client keeps retrying
  ClientOptions options;
  options.socket_path = dir.socketPath();
  IpcClient client(options);
  ASSERT_TRUE(client.start());
  std::this_thread::sleep_for(150ms);

  std::atomic<bool> done{false};
  std::thread stopper([&client, &done] {
    client.stop();
    done.store(true);
  });

  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!done.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_TRUE(done.load()) << "stop() racing a reconnect must terminate in bounded time";
  stopper.join();
  EXPECT_EQ(client.state(), ClientState::Stopped);
}

// T067 / B-20
TEST(ClientLifecycle, NeverReplaysRequest) {
  TempSocketDir dir;
  std::atomic<int> handled{0};

  ClientOptions client_options;
  client_options.socket_path = dir.socketPath();
  IpcClient client(client_options);

  {
    ServerOptions server_options;
    server_options.socket_path = dir.socketPath();
    IpcServer server(server_options);
    ASSERT_TRUE(server.registerHandler("vehicle.command", [&handled](std::string) {
      handled.fetch_add(1);
      // Still running when the server goes away, so the request is inflight.
      std::this_thread::sleep_for(1500ms);
      return IpcResponse{};
    }));
    ASSERT_TRUE(server.start());

    ASSERT_TRUE(client.start());
    ASSERT_TRUE(waitConnected(client));

    auto future = client.request("vehicle.command", "{}", 10000ms);
    std::this_thread::sleep_for(200ms);
    ASSERT_EQ(handled.load(), 1);

    server.stop();  // the peer disappears with the request inflight

    ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
    EXPECT_EQ(future.get().error, IpcError::PeerClosed);
  }

  // A fresh service on the same path. The client reconnects, but the
  // side-effecting request must NOT be sent again (spec section 8.4, B-20).
  ServerOptions restarted_options;
  restarted_options.socket_path = dir.socketPath();
  IpcServer restarted(restarted_options);
  ASSERT_TRUE(restarted.registerHandler("vehicle.command", [&handled](std::string) {
    handled.fetch_add(1);
    return IpcResponse{};
  }));
  ASSERT_TRUE(restarted.start());

  ASSERT_TRUE(waitConnected(client, 8000ms));
  std::this_thread::sleep_for(300ms);
  EXPECT_EQ(handled.load(), 1) << "the inflight request was replayed after the reconnect";

  client.stop();
  restarted.stop();
}

}  // namespace
