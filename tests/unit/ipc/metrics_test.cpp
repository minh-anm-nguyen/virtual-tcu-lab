// T082, T083: the conservation equations of contracts/metrics.md made
// executable. E1, E2, E3, E4, E5, invariants M1, M2, M3, M4.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <future>
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

bool waitConnected(const IpcClient& client, std::chrono::milliseconds budget = 5000ms) {
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

// T082 / E1 / E3 / E4
TEST(Metrics, ConservationAfterMixedOutcomes) {
  TempSocketDir dir;
  ServerOptions server_options;
  server_options.socket_path = dir.socketPath();
  IpcServer server(server_options);
  ASSERT_TRUE(server.registerHandler("vehicle.ok", [](std::string) {
    IpcResponse r;
    r.payload_json = "{\"ok\":true}";
    return r;
  }));
  ASSERT_TRUE(server.registerHandler("vehicle.slow", [](std::string) {
    std::this_thread::sleep_for(700ms);
    return IpcResponse{};
  }));
  ASSERT_TRUE(server.registerHandler("vehicle.bad", [](std::string) {
    IpcResponse r;
    r.error = IpcError::RemoteError;
    r.detail = "INVALID_ARGUMENT";
    return r;
  }));
  ASSERT_TRUE(server.start());

  ClientOptions client_options;
  client_options.socket_path = dir.socketPath();
  client_options.max_tx_messages = 2;  // so some requests are refused
  IpcClient client(client_options);
  ASSERT_TRUE(client.start());
  ASSERT_TRUE(waitConnected(client));

  std::vector<std::future<IpcResponse>> futures;

  // Successes.
  for (int i = 0; i < 5; ++i) {
    futures.push_back(client.request("vehicle.ok", "{}", 5000ms));
  }
  // A remote error, which counts as completed: the peer answered.
  futures.push_back(client.request("vehicle.bad", "{}", 5000ms));
  // A timeout.
  futures.push_back(client.request("vehicle.slow", "{}", 150ms));
  // A burst the TX cap must refuse at least partly.
  for (int i = 0; i < 30; ++i) {
    futures.push_back(client.request("vehicle.ok", "{}", 5000ms));
  }
  // Refused outright: a reserved name never reaches the wire.
  futures.push_back(client.request("ipc.hello", "{}", 1000ms));

  std::uint64_t completed = 0;
  std::uint64_t timeouts = 0;
  std::uint64_t failed = 0;
  for (auto& future : futures) {
    ASSERT_EQ(future.wait_for(10s), std::future_status::ready);
    const IpcError error = future.get().error;
    if (error == IpcError::None || error == IpcError::RemoteError) {
      ++completed;
    } else if (error == IpcError::Timeout) {
      ++timeouts;
    } else {
      ++failed;
    }
  }

  const IpcMetricsSnapshot before_stop = client.metrics();
  EXPECT_EQ(before_stop.requests_created_total, futures.size());
  EXPECT_TRUE(conserves(before_stop)) << "equation E1 failed";
  EXPECT_EQ(before_stop.requests_completed_total, completed);
  EXPECT_EQ(before_stop.request_timeouts_total, timeouts);
  EXPECT_EQ(before_stop.requests_failed_total, failed);
  EXPECT_GT(before_stop.requests_failed_total, 0u) << "the TX cap never refused anything";

  client.stop();
  const IpcMetricsSnapshot after_stop = client.metrics();
  EXPECT_TRUE(conserves(after_stop));
  // E3: a clean stop releases every queued byte.
  EXPECT_EQ(after_stop.tx_queue_bytes, 0u);

  server.stop();
  // E4 on the server side.
  EXPECT_EQ(server.metrics().connections_active, 0u);
  EXPECT_EQ(server.metrics().tx_queue_bytes, 0u);
}

// T083 / E2 / E5
TEST(Metrics, QueueRejectIsNeverSentAndLateResponseIsCounted) {
  TempSocketDir dir;
  ServerOptions server_options;
  server_options.socket_path = dir.socketPath();
  IpcServer server(server_options);
  ASSERT_TRUE(server.registerHandler("vehicle.slow", [](std::string) {
    std::this_thread::sleep_for(600ms);
    IpcResponse r;
    r.payload_json = "{\"late\":true}";
    return r;
  }));
  ASSERT_TRUE(server.start());

  ClientOptions client_options;
  client_options.socket_path = dir.socketPath();
  client_options.max_pending_requests = 1;
  IpcClient client(client_options);
  ASSERT_TRUE(client.start());
  ASSERT_TRUE(waitConnected(client));

  // One request is accepted and times out; the second is refused by the
  // pending cap and must never be sent (equation E2).
  auto accepted = client.request("vehicle.slow", "{}", 150ms);
  auto refused = client.request("vehicle.slow", "{}", 150ms);

  ASSERT_EQ(refused.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(refused.get().error, IpcError::QueueFull);
  ASSERT_EQ(accepted.wait_for(3s), std::future_status::ready);
  EXPECT_EQ(accepted.get().error, IpcError::Timeout);

  // The answer to the first request arrives after its deadline (equation E5).
  // Waited for as an event with a deadline, not a fixed sleep (Principle IV).
  const auto wait_until = std::chrono::steady_clock::now() + 3s;
  while (client.metrics().late_responses_total == 0 &&
         std::chrono::steady_clock::now() < wait_until) {
    std::this_thread::sleep_for(10ms);
  }
  const IpcMetricsSnapshot m = client.metrics();
  EXPECT_EQ(m.requests_created_total, 2u);
  EXPECT_TRUE(conserves(m));
  EXPECT_EQ(m.request_timeouts_total, 1u);
  EXPECT_EQ(m.requests_failed_total, 1u);
  EXPECT_EQ(m.requests_completed_total, 0u);
  EXPECT_EQ(m.late_responses_total, 1u);
  // Only the accepted request ever reached the socket.
  EXPECT_EQ(m.requests_sent_total, 1u);

  client.stop();
  server.stop();
}

TEST(Metrics, LatencyHistogramRecordsOnlyAnsweredRequests) {
  TempSocketDir dir;
  ServerOptions server_options;
  server_options.socket_path = dir.socketPath();
  IpcServer server(server_options);
  ASSERT_TRUE(server.registerHandler("vehicle.ok", [](std::string) { return IpcResponse{}; }));
  ASSERT_TRUE(server.registerHandler("vehicle.slow", [](std::string) {
    std::this_thread::sleep_for(500ms);
    return IpcResponse{};
  }));
  ASSERT_TRUE(server.start());

  ClientOptions client_options;
  client_options.socket_path = dir.socketPath();
  IpcClient client(client_options);
  ASSERT_TRUE(client.start());
  ASSERT_TRUE(waitConnected(client));

  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(client.request("vehicle.ok", "{}", 5000ms).get().error, IpcError::None);
  }
  EXPECT_EQ(client.request("vehicle.slow", "{}", 100ms).get().error, IpcError::Timeout);

  const IpcMetricsSnapshot m = client.metrics();
  // A timeout's latency is its deadline by definition, so it is not observed
  // (contracts/metrics.md, "Histogram").
  EXPECT_EQ(m.request_latency_ms.count, 3u);

  client.stop();
  server.stop();
}

}  // namespace
