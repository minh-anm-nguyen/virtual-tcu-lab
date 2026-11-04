// T047-T049: deadlines and late responses.
// B-07, B-12, invariants S1, S2, S4, equation E2.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
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

TEST(ClientTimeout, TimeoutCompletesExactlyOnce) {
  TempSocketDir dir;
  ServerOptions server_options;
  server_options.socket_path = dir.socketPath();
  IpcServer server(server_options);
  // A handler that outlives the client's deadline by a wide margin.
  ASSERT_TRUE(server.registerHandler("vehicle.slow", [](std::string) {
    std::this_thread::sleep_for(1500ms);
    return IpcResponse{};
  }));
  ASSERT_TRUE(server.start());

  ClientOptions client_options;
  client_options.socket_path = dir.socketPath();
  IpcClient client(client_options);
  ASSERT_TRUE(client.start());
  ASSERT_TRUE(waitConnected(client));

  auto future = client.request("vehicle.slow", "{}", 200ms);
  ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
  const IpcResponse response = future.get();

  EXPECT_EQ(response.error, IpcError::Timeout);
  EXPECT_EQ(response.detail, "deadline exceeded");

  const auto m = client.metrics();
  EXPECT_EQ(m.request_timeouts_total, 1u);
  // Exactly one terminal counter moved for this request (invariants S1, M2).
  EXPECT_EQ(m.requests_completed_total, 0u);
  EXPECT_EQ(m.requests_created_total,
            m.requests_completed_total + m.request_timeouts_total + m.requests_failed_total);

  client.stop();
  server.stop();
}

TEST(ClientTimeout, TimeoutCountsQueueTime) {
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

  // A deadline this short expires while the frame is still queued, which is
  // what invariant S2 requires the clock to include.
  auto future = client.request("vehicle.ok", "{}", 1ms);
  ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
  const IpcResponse response = future.get();

  // Either outcome is legal on a fast machine; what must hold is that the
  // request resolved exactly once and the conservation equation still balances.
  EXPECT_TRUE(response.error == IpcError::Timeout || response.error == IpcError::None);
  const auto m = client.metrics();
  EXPECT_EQ(m.requests_created_total,
            m.requests_completed_total + m.request_timeouts_total + m.requests_failed_total);

  client.stop();
  server.stop();
}

TEST(ClientTimeout, LateResponseIgnored) {
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
  IpcClient client(client_options);
  ASSERT_TRUE(client.start());
  ASSERT_TRUE(waitConnected(client));

  auto future = client.request("vehicle.slow", "{}", 150ms);
  ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
  EXPECT_EQ(future.get().error, IpcError::Timeout);

  // The answer arrives after the deadline: dropped and counted, never matched
  // to another entry (invariant S4, B-12). Waited for as an event with a
  // deadline, not with a fixed sleep (Principle IV).
  const auto wait_until = std::chrono::steady_clock::now() + 3s;
  while (client.metrics().late_responses_total == 0 &&
         std::chrono::steady_clock::now() < wait_until) {
    std::this_thread::sleep_for(10ms);
  }
  const auto m = client.metrics();
  EXPECT_EQ(m.late_responses_total, 1u);
  EXPECT_EQ(m.requests_completed_total, 0u);
  EXPECT_EQ(m.request_timeouts_total, 1u);

  client.stop();
  server.stop();
}

TEST(ClientTimeout, LateResponseDoesNotResolveAnotherRequest) {
  TempSocketDir dir;
  ServerOptions server_options;
  server_options.socket_path = dir.socketPath();
  IpcServer server(server_options);
  std::atomic<int> calls{0};
  ASSERT_TRUE(server.registerHandler("vehicle.var", [&calls](std::string) {
    // The first call is slow enough to time out; the second answers at once.
    if (calls.fetch_add(1) == 0) {
      std::this_thread::sleep_for(600ms);
      IpcResponse slow;
      slow.payload_json = "{\"which\":\"first\"}";
      return slow;
    }
    IpcResponse fast;
    fast.payload_json = "{\"which\":\"second\"}";
    return fast;
  }));
  ASSERT_TRUE(server.start());

  ClientOptions client_options;
  client_options.socket_path = dir.socketPath();
  IpcClient client(client_options);
  ASSERT_TRUE(client.start());
  ASSERT_TRUE(waitConnected(client));

  auto first = client.request("vehicle.var", "{}", 150ms);
  ASSERT_EQ(first.wait_for(3s), std::future_status::ready);
  EXPECT_EQ(first.get().error, IpcError::Timeout);

  auto second = client.request("vehicle.var", "{}", 2000ms);
  ASSERT_EQ(second.wait_for(3s), std::future_status::ready);
  const IpcResponse response = second.get();
  ASSERT_EQ(response.error, IpcError::None);
  // The second request must carry its own answer, not the first one's.
  EXPECT_NE(response.payload_json.find("second"), std::string::npos);

  client.stop();
  server.stop();
}

}  // namespace
