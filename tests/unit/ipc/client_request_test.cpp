// T029, T050, T051, T070: correlation, backpressure and re-subscribe.
// B-03, B-10, invariants S3, S5, B2.

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <future>
#include <mutex>
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

// T029 / B-03
TEST(ClientRequest, ResponseOutOfOrderMatchesFuture) {
  TempSocketDir dir;
  ServerOptions server_options;
  server_options.socket_path = dir.socketPath();
  IpcServer server(server_options);
  // The slow method answers after the fast one, so the responses arrive in the
  // opposite order to the requests.
  ASSERT_TRUE(server.registerHandler("vehicle.slow", [](std::string) {
    std::this_thread::sleep_for(400ms);
    IpcResponse r;
    r.payload_json = "{\"which\":\"slow\"}";
    return r;
  }));
  ASSERT_TRUE(server.registerHandler("vehicle.fast", [](std::string) {
    IpcResponse r;
    r.payload_json = "{\"which\":\"fast\"}";
    return r;
  }));
  ASSERT_TRUE(server.start());

  ClientOptions client_options;
  client_options.socket_path = dir.socketPath();
  IpcClient client(client_options);
  ASSERT_TRUE(client.start());
  ASSERT_TRUE(waitConnected(client));

  auto slow = client.request("vehicle.slow", "{}", 5000ms);
  std::this_thread::sleep_for(50ms);
  auto fast = client.request("vehicle.fast", "{}", 5000ms);

  ASSERT_EQ(fast.wait_for(3s), std::future_status::ready);
  const IpcResponse fast_response = fast.get();
  ASSERT_EQ(slow.wait_for(3s), std::future_status::ready);
  const IpcResponse slow_response = slow.get();

  ASSERT_EQ(fast_response.error, IpcError::None);
  ASSERT_EQ(slow_response.error, IpcError::None);
  EXPECT_NE(fast_response.payload_json.find("fast"), std::string::npos);
  EXPECT_NE(slow_response.payload_json.find("slow"), std::string::npos);

  client.stop();
  server.stop();
}

// T051 / invariant S3
TEST(ClientRequest, PendingCapReturnsQueueFull) {
  TempSocketDir dir;
  ServerOptions server_options;
  server_options.socket_path = dir.socketPath();
  IpcServer server(server_options);
  ASSERT_TRUE(server.registerHandler("vehicle.slow", [](std::string) {
    std::this_thread::sleep_for(800ms);
    return IpcResponse{};
  }));
  ASSERT_TRUE(server.start());

  ClientOptions client_options;
  client_options.socket_path = dir.socketPath();
  client_options.max_pending_requests = 1;
  IpcClient client(client_options);
  ASSERT_TRUE(client.start());
  ASSERT_TRUE(waitConnected(client));

  auto first = client.request("vehicle.slow", "{}", 5000ms);
  auto second = client.request("vehicle.slow", "{}", 5000ms);

  // The second request is past the cap: refused at once, never registered.
  ASSERT_EQ(second.wait_for(500ms), std::future_status::ready);
  EXPECT_EQ(second.get().error, IpcError::QueueFull);

  ASSERT_EQ(first.wait_for(5s), std::future_status::ready);
  EXPECT_EQ(first.get().error, IpcError::None);

  const auto m = client.metrics();
  EXPECT_EQ(m.requests_created_total, 2u);
  EXPECT_EQ(m.requests_created_total,
            m.requests_completed_total + m.request_timeouts_total + m.requests_failed_total);

  client.stop();
  server.stop();
}

// T050 / B-10
TEST(ClientRequest, QueueFullReturnsErrorWithoutBlocking) {
  TempSocketDir dir;
  ServerOptions server_options;
  server_options.socket_path = dir.socketPath();
  IpcServer server(server_options);
  ASSERT_TRUE(server.registerHandler("vehicle.slow", [](std::string) {
    std::this_thread::sleep_for(300ms);
    return IpcResponse{};
  }));
  ASSERT_TRUE(server.start());

  ClientOptions client_options;
  client_options.socket_path = dir.socketPath();
  client_options.max_tx_messages = 1;  // the TX cap of spec section 3.2, lowered
  client_options.max_pending_requests = 128;
  IpcClient client(client_options);
  ASSERT_TRUE(client.start());
  ASSERT_TRUE(waitConnected(client));

  std::vector<std::future<IpcResponse>> futures;
  const auto before = std::chrono::steady_clock::now();
  for (int i = 0; i < 50; ++i) {
    futures.push_back(client.request("vehicle.slow", "{}", 5000ms));
  }
  const auto elapsed = std::chrono::steady_clock::now() - before;
  // Fail fast, never block the caller (spec section 8.6).
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 1000);

  int queue_full = 0;
  for (auto& future : futures) {
    ASSERT_EQ(future.wait_for(10s), std::future_status::ready);
    if (future.get().error == IpcError::QueueFull) {
      ++queue_full;
    }
  }
  EXPECT_GT(queue_full, 0) << "a one-message TX cap must reject at least one of 50 requests";

  const auto m = client.metrics();
  EXPECT_EQ(m.requests_created_total, 50u);
  EXPECT_EQ(m.requests_created_total,
            m.requests_completed_total + m.request_timeouts_total + m.requests_failed_total);

  client.stop();
  server.stop();
}

// T070 / invariant B2 / research R-07
TEST(ClientRequest, ResubscribeAfterReconnect) {
  TempSocketDir dir;
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<std::string> received;

  ClientOptions client_options;
  client_options.socket_path = dir.socketPath();
  IpcClient client(client_options);

  {
    ServerOptions server_options;
    server_options.socket_path = dir.socketPath();
    IpcServer first(server_options);
    ASSERT_TRUE(first.start());

    ASSERT_TRUE(client.start());
    ASSERT_TRUE(waitConnected(client));
    client.subscribe("vehicle.ignitionChanged", [&](std::string payload) {
      std::lock_guard<std::mutex> lock(mutex);
      received.push_back(std::move(payload));
      cv.notify_all();
    });

    // Prove the subscription is live before the restart.
    std::this_thread::sleep_for(200ms);
    first.publishNotification("vehicle.ignitionChanged", "{\"ignition\":true}");
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(cv.wait_for(lock, 3s, [&received] { return !received.empty(); }));
    lock.unlock();

    first.stop();
  }

  ServerOptions restarted_options;
  restarted_options.socket_path = dir.socketPath();
  IpcServer restarted(restarted_options);
  ASSERT_TRUE(restarted.start());
  ASSERT_TRUE(waitConnected(client, 8000ms));

  // The client must have re-sent ipc.subscribe and received its ACK before the
  // notification can reach it again.
  std::size_t before_count = 0;
  {
    std::lock_guard<std::mutex> lock(mutex);
    before_count = received.size();
  }
  std::this_thread::sleep_for(300ms);
  restarted.publishNotification("vehicle.ignitionChanged", "{\"ignition\":false}");

  std::unique_lock<std::mutex> lock(mutex);
  EXPECT_TRUE(cv.wait_for(lock, 5s, [&] { return received.size() > before_count; }))
      << "the subscription was not restored after the reconnect";
  lock.unlock();

  EXPECT_GE(client.metrics().reconnect_total, 1u);

  client.stop();
  restarted.stop();
}

}  // namespace
