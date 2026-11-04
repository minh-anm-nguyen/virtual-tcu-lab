// Integration 1 of spec section 12.2 / B-02: a client in this process and the
// real vehicle_service binary in another exchange request and response over a
// Unix domain socket.

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <string>
#include <thread>

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

TEST(IntegrationRequestResponse, StatusRequestCrossesTheProcessBoundary) {
  TempSocketDir dir;
  ServerProcess service;
  ASSERT_TRUE(service.start(dir.socketPath())) << "the service never started listening";

  ClientOptions options;
  options.socket_path = dir.socketPath();
  IpcClient client(options);
  ASSERT_TRUE(client.start());
  ASSERT_TRUE(waitConnected(client));

  auto future = client.request("vehicle.getStatus", "{}", 2000ms);
  ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
  const IpcResponse response = future.get();

  ASSERT_EQ(response.error, IpcError::None) << "detail=" << response.detail;
  // The three fields the demo handler promises (spec section 5.2).
  EXPECT_NE(response.payload_json.find("ignition"), std::string::npos);
  EXPECT_NE(response.payload_json.find("speed_kph"), std::string::npos);
  EXPECT_NE(response.payload_json.find("battery_pct"), std::string::npos);

  const auto m = client.metrics();
  EXPECT_EQ(m.requests_completed_total, 1u);
  EXPECT_EQ(m.requests_created_total,
            m.requests_completed_total + m.request_timeouts_total + m.requests_failed_total);

  client.stop();
  service.terminate();
}

TEST(IntegrationRequestResponse, UnknownMethodIsARemoteError) {
  TempSocketDir dir;
  ServerProcess service;
  ASSERT_TRUE(service.start(dir.socketPath()));

  ClientOptions options;
  options.socket_path = dir.socketPath();
  IpcClient client(options);
  ASSERT_TRUE(client.start());
  ASSERT_TRUE(waitConnected(client));

  auto future = client.request("vehicle.doesNotExist", "{}", 2000ms);
  ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
  const IpcResponse response = future.get();

  EXPECT_EQ(response.error, IpcError::RemoteError);
  EXPECT_EQ(response.detail, "UNKNOWN_METHOD");
  // A remote error still counts as completed: the peer answered.
  EXPECT_EQ(client.metrics().requests_completed_total, 1u);

  client.stop();
  service.terminate();
}

TEST(IntegrationRequestResponse, MalformedPayloadIsRejectedByTheHandler) {
  TempSocketDir dir;
  ServerProcess service;
  ASSERT_TRUE(service.start(dir.socketPath()));

  ClientOptions options;
  options.socket_path = dir.socketPath();
  IpcClient client(options);
  ASSERT_TRUE(client.start());
  ASSERT_TRUE(waitConnected(client));

  // B-18: the handler's depth-limited reader rejects it and the service lives.
  auto bad = client.request("vehicle.getStatus", "{\"a\":", 2000ms);
  ASSERT_EQ(bad.wait_for(3s), std::future_status::ready);
  const IpcResponse bad_response = bad.get();
  EXPECT_EQ(bad_response.error, IpcError::RemoteError);
  EXPECT_EQ(bad_response.detail, "INVALID_ARGUMENT");

  auto good = client.request("vehicle.getStatus", "{}", 2000ms);
  ASSERT_EQ(good.wait_for(3s), std::future_status::ready);
  EXPECT_EQ(good.get().error, IpcError::None);

  client.stop();
  service.terminate();
}

}  // namespace
