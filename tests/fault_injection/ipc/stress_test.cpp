// T093-T095: the "mass concurrency", "server delayed response" and "client
// shutdown mid-write" rows of the spec section 10 fault matrix.
//
// Duration: 8 clients x 10 requests/s for TCU_IPC_STRESS_SECONDS seconds,
// default 5. The 30-minute soak of spec section 12.3 is the same test with
// TCU_IPC_STRESS_SECONDS=1800 - the assertions are invariants, not timings, so
// they hold identically at either duration (research R-14).

#include <gtest/gtest.h>

#include <dirent.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "support/temp_socket_dir.hpp"
#include "tcu/ipc/ipc_client.hpp"
#include "tcu/ipc/ipc_server.hpp"

namespace {

using namespace std::chrono_literals;
using tcu::ipc::ClientOptions;
using tcu::ipc::ClientState;
using tcu::ipc::IpcClient;
using tcu::ipc::IpcError;
using tcu::ipc::IpcMetricsSnapshot;
using tcu::ipc::IpcResponse;
using tcu::ipc::IpcServer;
using tcu::ipc::ServerOptions;
using tcu::ipc::test::TempSocketDir;

constexpr int kClients = 8;
constexpr int kRequestsPerSecond = 10;

int stressSeconds() {
  const char* env = std::getenv("TCU_IPC_STRESS_SECONDS");
  if (env == nullptr) {
    return 5;
  }
  const int value = std::atoi(env);
  return value > 0 ? value : 5;
}

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

bool conserves(const IpcMetricsSnapshot& m) {
  return m.requests_created_total ==
         m.requests_completed_total + m.request_timeouts_total + m.requests_failed_total;
}

// T093 / fault row "mass concurrency"
TEST(FaultStress, EightClientsUnderLoadStayBoundedAndResolveEveryRequestOnce) {
  const int seconds = stressSeconds();
  const std::size_t fds_at_start = openFdCount();

  TempSocketDir dir;
  ServerOptions server_options;
  server_options.socket_path = dir.socketPath();
  IpcServer server(server_options);
  ASSERT_TRUE(server.registerHandler("vehicle.getStatus", [](std::string) {
    IpcResponse r;
    r.payload_json = "{\"ignition\":true,\"speed_kph\":0,\"battery_pct\":79}";
    return r;
  }));
  ASSERT_TRUE(server.start());

  ClientOptions client_options;
  client_options.socket_path = dir.socketPath();
  std::vector<std::unique_ptr<IpcClient>> clients;
  for (int i = 0; i < kClients; ++i) {
    client_options.client_id = "stress-" + std::to_string(i);
    clients.push_back(std::make_unique<IpcClient>(client_options));
    ASSERT_TRUE(clients.back()->start());
  }
  for (auto& client : clients) {
    ASSERT_TRUE(waitConnected(*client));
  }

  std::atomic<std::uint64_t> resolved{0};
  std::vector<std::thread> threads;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);

  for (int i = 0; i < kClients; ++i) {
    threads.emplace_back([&clients, &resolved, i, deadline] {
      std::vector<std::future<IpcResponse>> inflight;
      while (std::chrono::steady_clock::now() < deadline) {
        for (int r = 0; r < kRequestsPerSecond; ++r) {
          inflight.push_back(clients[i]->request("vehicle.getStatus", "{}", 5000ms));
        }
        // Drain what is ready, so the pending map does not grow without bound.
        for (auto it = inflight.begin(); it != inflight.end();) {
          if (it->wait_for(0ms) == std::future_status::ready) {
            it->get();
            resolved.fetch_add(1);
            it = inflight.erase(it);
          } else {
            ++it;
          }
        }
        std::this_thread::sleep_for(1s);
      }
      for (auto& future : inflight) {
        if (future.wait_for(10s) == std::future_status::ready) {
          future.get();
          resolved.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& t : threads) {
    t.join();
  }

  // No deadlock: every request resolved exactly once, which the conservation
  // equation detects whether one was resolved twice or never.
  std::uint64_t created = 0;
  for (auto& client : clients) {
    const IpcMetricsSnapshot m = client->metrics();
    EXPECT_TRUE(conserves(m)) << "equation E1 failed under load";
    created += m.requests_created_total;
  }
  EXPECT_EQ(resolved.load(), created);
  EXPECT_GT(created, 0u);

  for (auto& client : clients) {
    client->stop();
    EXPECT_EQ(client->metrics().tx_queue_bytes, 0u);
  }
  server.stop();
  EXPECT_EQ(server.metrics().connections_active, 0u);
  EXPECT_EQ(server.metrics().tx_queue_bytes, 0u);

  clients.clear();
  EXPECT_EQ(openFdCount(), fds_at_start) << "the load test leaked a descriptor";
}

// T094 / fault row "server delayed response"
TEST(FaultStress, SlowHandlerTimesOutWithoutBlockingAnotherClient) {
  TempSocketDir dir;
  ServerOptions server_options;
  server_options.socket_path = dir.socketPath();
  IpcServer server(server_options);
  ASSERT_TRUE(server.registerHandler("vehicle.slow", [](std::string) {
    std::this_thread::sleep_for(3000ms);  // the spec's 3 s against a 2 s deadline
    return IpcResponse{};
  }));
  ASSERT_TRUE(server.registerHandler("vehicle.fast", [](std::string) { return IpcResponse{}; }));
  ASSERT_TRUE(server.start());

  ClientOptions options;
  options.socket_path = dir.socketPath();
  IpcClient slow_caller(options);
  IpcClient other(options);
  ASSERT_TRUE(slow_caller.start());
  ASSERT_TRUE(other.start());
  ASSERT_TRUE(waitConnected(slow_caller));
  ASSERT_TRUE(waitConnected(other));

  auto slow = slow_caller.request("vehicle.slow", "{}", 2000ms);

  // The other client must be served normally while that handler sleeps.
  std::this_thread::sleep_for(200ms);
  const auto before = std::chrono::steady_clock::now();
  auto fast = other.request("vehicle.fast", "{}", 2000ms);
  ASSERT_EQ(fast.wait_for(3s), std::future_status::ready);
  EXPECT_EQ(fast.get().error, IpcError::None);
  const auto elapsed = std::chrono::steady_clock::now() - before;
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 1500)
      << "a slow handler blocked an unrelated client";

  ASSERT_EQ(slow.wait_for(4s), std::future_status::ready);
  EXPECT_EQ(slow.get().error, IpcError::Timeout);

  // The late answer is discarded and counted (B-12). Waited for as an event
  // with a deadline, not a fixed sleep (Principle IV).
  const auto wait_until = std::chrono::steady_clock::now() + 4s;
  while (slow_caller.metrics().late_responses_total == 0 &&
         std::chrono::steady_clock::now() < wait_until) {
    std::this_thread::sleep_for(10ms);
  }
  const IpcMetricsSnapshot m = slow_caller.metrics();
  EXPECT_EQ(m.request_timeouts_total, 1u);
  EXPECT_EQ(m.late_responses_total, 1u);
  EXPECT_TRUE(conserves(m));

  slow_caller.stop();
  other.stop();
  server.stop();
}

// T095 / fault row "client shutdown mid-write"
TEST(FaultStress, StopWithANonEmptyTxQueueTerminatesAndReleasesBuffers) {
  TempSocketDir dir;
  ServerOptions server_options;
  server_options.socket_path = dir.socketPath();
  IpcServer server(server_options);
  ASSERT_TRUE(server.registerHandler("vehicle.slow", [](std::string) {
    std::this_thread::sleep_for(1500ms);
    return IpcResponse{};
  }));
  ASSERT_TRUE(server.start());

  ClientOptions options;
  options.socket_path = dir.socketPath();
  options.max_pending_requests = 256;
  options.max_tx_messages = 256;
  IpcClient client(options);
  ASSERT_TRUE(client.start());
  ASSERT_TRUE(waitConnected(client));

  // Queue a burst, then stop immediately: some frames are certainly still
  // unsent or half written.
  std::vector<std::future<IpcResponse>> futures;
  for (int i = 0; i < 100; ++i) {
    futures.push_back(client.request("vehicle.slow", "{}", 20000ms));
  }

  const auto before = std::chrono::steady_clock::now();
  client.stop();
  const auto elapsed = std::chrono::steady_clock::now() - before;
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 3000)
      << "stop() with a non-empty TX queue was not bounded";

  for (auto& future : futures) {
    ASSERT_EQ(future.wait_for(1s), std::future_status::ready);
    future.get();
  }
  const IpcMetricsSnapshot m = client.metrics();
  EXPECT_TRUE(conserves(m));
  EXPECT_EQ(m.tx_queue_bytes, 0u) << "the TX buffers were not released";

  server.stop();
}

}  // namespace
