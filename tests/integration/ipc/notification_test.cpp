// Integration 2 of spec section 12.2 / FR-05 / invariant B1: two subscribed
// clients both receive a notification, and a third that never subscribed does
// not.

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
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
using tcu::ipc::test::ServerProcess;
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

/// Counts notifications for one topic, with a condition variable so the test
/// waits on an event rather than on a fixed sleep.
class Counter {
 public:
  void add(std::string payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    payloads_.push_back(std::move(payload));
    cv_.notify_all();
  }

  bool waitFor(std::size_t want, std::chrono::milliseconds budget) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, budget, [this, want] { return payloads_.size() >= want; });
  }

  std::size_t count() {
    std::lock_guard<std::mutex> lock(mutex_);
    return payloads_.size();
  }

  std::string last() {
    std::lock_guard<std::mutex> lock(mutex_);
    return payloads_.empty() ? std::string() : payloads_.back();
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<std::string> payloads_;
};

TEST(IntegrationNotification, EverySubscriberReceivesAndNonSubscribersDoNot) {
  TempSocketDir dir;
  ServerProcess service;
  // A fast ignition timer, so the service publishes without the test needing a
  // way to trigger it from outside.
  ASSERT_TRUE(service.start(dir.socketPath(), 200));

  ClientOptions options;
  options.socket_path = dir.socketPath();

  IpcClient first(options);
  IpcClient second(options);
  IpcClient bystander(options);
  ASSERT_TRUE(first.start());
  ASSERT_TRUE(second.start());
  ASSERT_TRUE(bystander.start());
  ASSERT_TRUE(waitConnected(first));
  ASSERT_TRUE(waitConnected(second));
  ASSERT_TRUE(waitConnected(bystander));

  Counter first_counter;
  Counter second_counter;
  Counter bystander_counter;

  first.subscribe("vehicle.ignitionChanged",
                  [&first_counter](std::string p) { first_counter.add(std::move(p)); });
  second.subscribe("vehicle.ignitionChanged",
                   [&second_counter](std::string p) { second_counter.add(std::move(p)); });
  // The bystander deliberately subscribes to a different topic.
  bystander.subscribe("vehicle.somethingElse",
                      [&bystander_counter](std::string p) { bystander_counter.add(std::move(p)); });

  EXPECT_TRUE(first_counter.waitFor(2, 5s)) << "the first subscriber received nothing";
  EXPECT_TRUE(second_counter.waitFor(2, 5s)) << "the second subscriber received nothing";
  EXPECT_EQ(bystander_counter.count(), 0u) << "a notification reached a non-subscriber";

  EXPECT_NE(first_counter.last().find("ignition"), std::string::npos);

  first.stop();
  second.stop();
  bystander.stop();
  service.terminate();
}

TEST(IntegrationNotification, NotificationsCarryNoRequestId) {
  TempSocketDir dir;
  ServerProcess service;
  ASSERT_TRUE(service.start(dir.socketPath(), 150));

  ClientOptions options;
  options.socket_path = dir.socketPath();
  IpcClient client(options);
  ASSERT_TRUE(client.start());
  ASSERT_TRUE(waitConnected(client));

  Counter counter;
  client.subscribe("vehicle.ignitionChanged",
                   [&counter](std::string p) { counter.add(std::move(p)); });
  ASSERT_TRUE(counter.waitFor(1, 5s));

  // A notification is not a request, so it must not disturb the request
  // counters at all.
  const auto m = client.metrics();
  EXPECT_EQ(m.requests_created_total, 0u);
  EXPECT_EQ(m.requests_completed_total, 0u);
  EXPECT_EQ(m.late_responses_total, 0u);

  client.stop();
  service.terminate();
}

}  // namespace
