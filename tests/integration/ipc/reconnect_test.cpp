// Integration 3 and 4 of spec section 12.2 / B-08, B-09: the service is killed
// with a request inflight, and the client must fail that request exactly once,
// reconnect on its own, re-subscribe, and keep receiving notifications.
//
// This is the demo goal stated in spec section 1, asserted rather than watched.

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <future>
#include <mutex>
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

class Counter {
 public:
  void add(std::string payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    payloads_.push_back(std::move(payload));
    cv_.notify_all();
  }

  bool waitForMoreThan(std::size_t baseline, std::chrono::milliseconds budget) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, budget, [this, baseline] { return payloads_.size() > baseline; });
  }

  std::size_t count() {
    std::lock_guard<std::mutex> lock(mutex_);
    return payloads_.size();
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<std::string> payloads_;
};

// Integration 3 / B-08
TEST(IntegrationReconnect, KillWithRequestInflightFailsItOnceThenReconnects) {
  TempSocketDir dir;
  ServerProcess service;
  // A slow handler, so a request is genuinely inflight when the service dies.
  ASSERT_TRUE(service.start(dir.socketPath(), 0, {"--slow-ms", "3000"}));

  ClientOptions options;
  options.socket_path = dir.socketPath();
  IpcClient client(options);
  ASSERT_TRUE(client.start());
  ASSERT_TRUE(waitState(client, ClientState::Connected));

  auto future = client.request("vehicle.getStatus", "{}", 10000ms);
  std::this_thread::sleep_for(200ms);

  service.kill();  // SIGKILL, exactly as the demo does

  ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
  const IpcResponse response = future.get();
  EXPECT_EQ(response.error, IpcError::PeerClosed);
  EXPECT_EQ(response.detail, "peer closed");

  // Resolved exactly once (invariant C3) and counted exactly once.
  const auto after_kill = client.metrics();
  EXPECT_EQ(after_kill.requests_created_total, after_kill.requests_completed_total +
                                                   after_kill.request_timeouts_total +
                                                   after_kill.requests_failed_total);
  EXPECT_EQ(after_kill.requests_failed_total, 1u);

  // The client backs off rather than spinning, then reconnects by itself.
  EXPECT_TRUE(waitState(client, ClientState::Backoff, 2000ms));
  ASSERT_TRUE(service.restart());
  ASSERT_TRUE(waitState(client, ClientState::Connected, 10000ms));
  EXPECT_GE(client.metrics().reconnect_total, 1u);

  // The connection works again afterwards. The restarted service still has
  // --slow-ms 3000, so the deadline here allows for it.
  auto again = client.request("vehicle.getStatus", "{}", 6000ms);
  ASSERT_EQ(again.wait_for(8s), std::future_status::ready);
  EXPECT_EQ(again.get().error, IpcError::None);

  client.stop();
  service.terminate();
}

// Integration 4 / B-09 / invariant B2
TEST(IntegrationReconnect, SubscriptionIsRestoredAfterRestart) {
  TempSocketDir dir;
  ServerProcess service;
  ASSERT_TRUE(service.start(dir.socketPath(), 200));

  ClientOptions options;
  options.socket_path = dir.socketPath();
  IpcClient client(options);
  ASSERT_TRUE(client.start());
  ASSERT_TRUE(waitState(client, ClientState::Connected));

  Counter counter;
  client.subscribe("vehicle.ignitionChanged",
                   [&counter](std::string p) { counter.add(std::move(p)); });
  ASSERT_TRUE(counter.waitForMoreThan(0, 5s)) << "no notification before the restart";

  service.kill();
  EXPECT_TRUE(waitState(client, ClientState::Backoff, 3000ms));

  const std::size_t before_restart = counter.count();
  ASSERT_TRUE(service.restart());
  ASSERT_TRUE(waitState(client, ClientState::Connected, 10000ms));

  // The client must have re-sent ipc.subscribe and had it acknowledged before
  // any further notification can arrive.
  EXPECT_TRUE(counter.waitForMoreThan(before_restart, 6s))
      << "the subscription was not restored after the restart";

  client.stop();
  service.terminate();
}

TEST(IntegrationReconnect, BackoffSurvivesAServiceThatStaysDown) {
  TempSocketDir dir;
  ServerProcess service;
  ASSERT_TRUE(service.start(dir.socketPath()));

  ClientOptions options;
  options.socket_path = dir.socketPath();
  IpcClient client(options);
  ASSERT_TRUE(client.start());
  ASSERT_TRUE(waitState(client, ClientState::Connected));

  service.kill();
  EXPECT_TRUE(waitState(client, ClientState::Backoff, 3000ms));

  // With nothing listening, a request must fail fast rather than hang.
  auto future = client.request("vehicle.getStatus", "{}", 1000ms);
  ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(future.get().error, IpcError::NotConnected);

  // And the retry rate stays bounded (spec section 3.2's 5 s cap).
  const auto attempts_before = client.metrics().connect_attempts_total;
  std::this_thread::sleep_for(2000ms);
  const auto attempts_after = client.metrics().connect_attempts_total;
  EXPECT_LE(attempts_after - attempts_before, 12u) << "retries look like a spin";

  client.stop();
}

}  // namespace
