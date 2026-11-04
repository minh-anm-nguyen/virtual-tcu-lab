// Integration 5 of spec section 12.2 / FR-10: eight clients pipeline ten
// requests each against one service, and every future must carry its own
// answer.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "helpers/server_process.hpp"
#include "support/temp_socket_dir.hpp"
#include "tcu/ipc/ipc_client.hpp"

namespace {

using namespace std::chrono_literals;
using tcu::ipc::ClientOptions;
using tcu::ipc::ClientState;
using tcu::ipc::IpcClient;
using tcu::ipc::IpcError;
using tcu::ipc::IpcResponse;
using tcu::ipc::test::ServerProcess;
using tcu::ipc::test::TempSocketDir;

constexpr int kClients = 8;  // the figure spec section 3.2 names
constexpr int kRequests = 10;

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

TEST(IntegrationMultiClient, EightClientsPipelineCorrectly) {
  TempSocketDir dir;
  ServerProcess service;
  ASSERT_TRUE(service.start(dir.socketPath()));

  ClientOptions options;
  options.socket_path = dir.socketPath();

  std::vector<std::unique_ptr<IpcClient>> clients;
  for (int i = 0; i < kClients; ++i) {
    options.client_id = "client-" + std::to_string(i);
    clients.push_back(std::make_unique<IpcClient>(options));
    ASSERT_TRUE(clients.back()->start());
  }
  for (auto& client : clients) {
    ASSERT_TRUE(waitConnected(*client));
  }

  // Each client fires its requests from its own thread, so the service sees
  // genuinely concurrent traffic.
  std::vector<std::thread> threads;
  std::vector<int> successes(kClients, 0);
  for (int i = 0; i < kClients; ++i) {
    threads.emplace_back([&clients, &successes, i] {
      std::vector<std::future<IpcResponse>> futures;
      futures.reserve(kRequests);
      for (int r = 0; r < kRequests; ++r) {
        futures.push_back(clients[i]->request("vehicle.getStatus", "{}", 8000ms));
      }
      for (auto& future : futures) {
        if (future.wait_for(10s) != std::future_status::ready) {
          continue;
        }
        const IpcResponse response = future.get();
        if (response.error == IpcError::None &&
            response.payload_json.find("battery_pct") != std::string::npos) {
          ++successes[i];
        }
      }
    });
  }
  for (std::thread& t : threads) {
    t.join();
  }

  for (int i = 0; i < kClients; ++i) {
    EXPECT_EQ(successes[i], kRequests) << "client " << i << " lost answers";
    const auto m = clients[i]->metrics();
    // Equation E1 holds per client once nothing is outstanding.
    EXPECT_EQ(m.requests_created_total,
              m.requests_completed_total + m.request_timeouts_total + m.requests_failed_total);
    EXPECT_EQ(m.requests_created_total, static_cast<std::uint64_t>(kRequests));
  }

  for (auto& client : clients) {
    client->stop();
    EXPECT_EQ(client->metrics().tx_queue_bytes, 0u);
  }
  service.terminate();
}

}  // namespace
