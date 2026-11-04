// T030-T034, T072: routing, the reserved namespace, handler isolation and
// notification targeting. B-11, B-18, B-02, invariants R1, R2, R3, S13.

#include "tcu/ipc/ipc_server.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>

#include "support/raw_listener.hpp"
#include "support/raw_peer.hpp"
#include "support/temp_socket_dir.hpp"
#include "tcu/ipc/frame_codec.hpp"

namespace {

using namespace std::chrono_literals;
using tcu::ipc::IpcError;
using tcu::ipc::IpcResponse;
using tcu::ipc::IpcServer;
using tcu::ipc::ServerOptions;
using tcu::ipc::test::frameName;
using tcu::ipc::test::frameRequestId;
using tcu::ipc::test::RawPeer;
using tcu::ipc::test::rawFrame;
using tcu::ipc::test::TempSocketDir;

/// Reads one whole frame, or an empty string when none arrives in time.
std::string readOneFrame(RawPeer& peer, std::chrono::milliseconds budget = 2000ms) {
  std::string buffer = peer.readAtLeast(4, budget);
  if (buffer.size() < 4) {
    return {};
  }
  while (tcu::ipc::test::frameSize(buffer) == 0) {
    const std::string more = peer.readAtLeast(1, 200ms);
    if (more.empty()) {
      return {};
    }
    buffer += more;
  }
  return buffer;
}

std::uint8_t frameType(const std::string& frame) {
  return frame.size() > 5 ? static_cast<std::uint8_t>(frame[5]) : 0;
}

bool handshake(RawPeer& peer) {
  if (!peer.writeBytes(rawFrame(1, 1, "ipc.hello", "{\"version\":1,\"client_id\":\"t\"}"))) {
    return false;
  }
  const std::string reply = readOneFrame(peer);
  return !reply.empty() && frameType(reply) == 2 && frameName(reply) == "ipc.hello";
}

TEST(ServerRouter, RejectUnauthorizedMethod) {
  TempSocketDir dir;
  ServerOptions options;
  options.socket_path = dir.socketPath();
  IpcServer server(options);

  // Reserved namespace, invalid name, empty handler: all refused (invariant R1).
  EXPECT_FALSE(server.registerHandler("ipc.hello", [](std::string) { return IpcResponse{}; }));
  EXPECT_FALSE(server.registerHandler("ipc.anything", [](std::string) { return IpcResponse{}; }));
  EXPECT_FALSE(server.registerHandler("has space", [](std::string) { return IpcResponse{}; }));
  EXPECT_FALSE(server.registerHandler("vehicle.ok", nullptr));

  EXPECT_TRUE(server.registerHandler("vehicle.ok", [](std::string) { return IpcResponse{}; }));
  // A duplicate must not silently replace the first handler.
  EXPECT_FALSE(server.registerHandler("vehicle.ok", [](std::string) { return IpcResponse{}; }));

  ASSERT_TRUE(server.start());
  // After start() the routing table is frozen.
  EXPECT_FALSE(server.registerHandler("vehicle.late", [](std::string) { return IpcResponse{}; }));
  server.stop();
}

TEST(ServerRouter, UnknownMethodKeepsTheSessionOpen) {
  TempSocketDir dir;
  ServerOptions options;
  options.socket_path = dir.socketPath();
  IpcServer server(options);
  ASSERT_TRUE(server.registerHandler("vehicle.ok", [](std::string) {
    IpcResponse r;
    r.payload_json = "{\"ok\":true}";
    return r;
  }));
  ASSERT_TRUE(server.start());

  RawPeer peer;
  ASSERT_TRUE(peer.connect(dir.socketPath()));
  ASSERT_TRUE(handshake(peer));

  ASSERT_TRUE(peer.writeBytes(rawFrame(1, 2, "vehicle.nope", "{}")));
  const std::string error = readOneFrame(peer);
  ASSERT_FALSE(error.empty());
  EXPECT_EQ(frameType(error), 4);
  EXPECT_NE(error.find("UNKNOWN_METHOD"), std::string::npos);
  EXPECT_EQ(frameRequestId(error), 2u);

  // The session survives, so a following valid request still works.
  ASSERT_TRUE(peer.writeBytes(rawFrame(1, 3, "vehicle.ok", "{}")));
  const std::string ok = readOneFrame(peer);
  ASSERT_FALSE(ok.empty());
  EXPECT_EQ(frameType(ok), 2);
  EXPECT_EQ(frameRequestId(ok), 3u);
  server.stop();
}

TEST(ServerRouter, HandlerExceptionMappedToError) {
  TempSocketDir dir;
  ServerOptions options;
  options.socket_path = dir.socketPath();
  IpcServer server(options);
  ASSERT_TRUE(server.registerHandler(
      "vehicle.boom", [](std::string) -> IpcResponse { throw std::runtime_error("boom"); }));
  ASSERT_TRUE(server.registerHandler("vehicle.ok", [](std::string) { return IpcResponse{}; }));
  ASSERT_TRUE(server.start());

  RawPeer peer;
  ASSERT_TRUE(peer.connect(dir.socketPath()));
  ASSERT_TRUE(handshake(peer));

  ASSERT_TRUE(peer.writeBytes(rawFrame(1, 7, "vehicle.boom", "{}")));
  const std::string error = readOneFrame(peer);
  ASSERT_FALSE(error.empty());
  EXPECT_EQ(frameType(error), 4);
  EXPECT_NE(error.find("INTERNAL_ERROR"), std::string::npos);

  // B-11: the server is still alive after a throwing handler.
  ASSERT_TRUE(peer.writeBytes(rawFrame(1, 8, "vehicle.ok", "{}")));
  const std::string ok = readOneFrame(peer);
  ASSERT_FALSE(ok.empty());
  EXPECT_EQ(frameRequestId(ok), 8u);
  server.stop();
}

TEST(ServerRouter, HandlerRunsOffIoThread) {
  TempSocketDir dir;
  ServerOptions options;
  options.socket_path = dir.socketPath();
  IpcServer server(options);

  std::atomic<bool> seen{false};
  std::atomic<bool> same_as_main{true};
  const std::thread::id main_thread = std::this_thread::get_id();
  ASSERT_TRUE(server.registerHandler("vehicle.slow", [&](std::string) {
    same_as_main.store(std::this_thread::get_id() == main_thread);
    seen.store(true);
    // A handler that blocks must not stall the I/O loop (spec section 4.1).
    std::this_thread::sleep_for(400ms);
    return IpcResponse{};
  }));
  ASSERT_TRUE(server.start());

  RawPeer slow_peer;
  ASSERT_TRUE(slow_peer.connect(dir.socketPath()));
  ASSERT_TRUE(handshake(slow_peer));
  ASSERT_TRUE(slow_peer.writeBytes(rawFrame(1, 1, "vehicle.slow", "{}")));

  // While the handler sleeps, a second session must still be accepted and
  // served promptly - that is the proof the handler is not on the I/O loop.
  std::this_thread::sleep_for(100ms);
  const auto before = std::chrono::steady_clock::now();
  RawPeer fast_peer;
  ASSERT_TRUE(fast_peer.connect(dir.socketPath()));
  ASSERT_TRUE(handshake(fast_peer));
  const auto elapsed = std::chrono::steady_clock::now() - before;

  EXPECT_TRUE(seen.load());
  EXPECT_FALSE(same_as_main.load());
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 300);
  server.stop();
}

TEST(ServerRouter, MalformedPayloadIsInvalidArgument) {
  TempSocketDir dir;
  ServerOptions options;
  options.socket_path = dir.socketPath();
  IpcServer server(options);
  // A handler that validates its argument and rejects it, as the demo does.
  ASSERT_TRUE(server.registerHandler("vehicle.getStatus", [](std::string payload) {
    IpcResponse r;
    if (payload != "{}") {
      r.error = IpcError::RemoteError;
      r.detail = "INVALID_ARGUMENT";
    }
    return r;
  }));
  ASSERT_TRUE(server.start());

  RawPeer peer;
  ASSERT_TRUE(peer.connect(dir.socketPath()));
  ASSERT_TRUE(handshake(peer));

  ASSERT_TRUE(peer.writeBytes(rawFrame(1, 4, "vehicle.getStatus", "{broken")));
  const std::string error = readOneFrame(peer);
  ASSERT_FALSE(error.empty());
  EXPECT_EQ(frameType(error), 4);
  EXPECT_NE(error.find("INVALID_ARGUMENT"), std::string::npos);

  // B-18: the I/O loop is untouched by a rejected payload.
  ASSERT_TRUE(peer.writeBytes(rawFrame(1, 5, "vehicle.getStatus", "{}")));
  const std::string ok = readOneFrame(peer);
  ASSERT_FALSE(ok.empty());
  EXPECT_EQ(frameType(ok), 2);
  server.stop();
}

TEST(ServerRouter, UnauthenticatedBeforeHello) {
  TempSocketDir dir;
  ServerOptions options;
  options.socket_path = dir.socketPath();
  IpcServer server(options);
  ASSERT_TRUE(
      server.registerHandler("vehicle.getStatus", [](std::string) { return IpcResponse{}; }));
  ASSERT_TRUE(server.start());

  RawPeer peer;
  ASSERT_TRUE(peer.connect(dir.socketPath()));
  // The first frame is not ipc.hello (invariant S13).
  ASSERT_TRUE(peer.writeBytes(rawFrame(1, 1, "vehicle.getStatus", "{}")));

  const std::string error = readOneFrame(peer);
  ASSERT_FALSE(error.empty());
  EXPECT_EQ(frameType(error), 4);
  EXPECT_NE(error.find("UNAUTHENTICATED"), std::string::npos);

  // The session is closed, so the next read sees EOF rather than a reply.
  peer.readAvailable(500ms);
  EXPECT_TRUE(peer.isClosedByPeer());
  server.stop();
}

TEST(ServerRouter, NotificationOnlyToSubscribers) {
  TempSocketDir dir;
  ServerOptions options;
  options.socket_path = dir.socketPath();
  IpcServer server(options);
  ASSERT_TRUE(server.start());

  RawPeer subscriber;
  ASSERT_TRUE(subscriber.connect(dir.socketPath()));
  ASSERT_TRUE(handshake(subscriber));
  ASSERT_TRUE(subscriber.writeBytes(
      rawFrame(1, 2, "ipc.subscribe", "{\"topic\":\"vehicle.ignitionChanged\"}")));
  const std::string ack = readOneFrame(subscriber);
  ASSERT_FALSE(ack.empty());
  EXPECT_EQ(frameName(ack), "ipc.subscribe");

  RawPeer bystander;
  ASSERT_TRUE(bystander.connect(dir.socketPath()));
  ASSERT_TRUE(handshake(bystander));

  server.publishNotification("vehicle.ignitionChanged", "{\"ignition\":false}");

  const std::string note = readOneFrame(subscriber);
  ASSERT_FALSE(note.empty());
  EXPECT_EQ(frameType(note), 3);
  EXPECT_EQ(frameName(note), "vehicle.ignitionChanged");
  EXPECT_EQ(frameRequestId(note), 0u);

  // The session that never subscribed receives nothing (invariant B1).
  EXPECT_TRUE(bystander.readAvailable(300ms).empty());

  // A reserved topic is refused outright.
  server.publishNotification("ipc.sneaky", "{}");
  EXPECT_TRUE(subscriber.readAvailable(300ms).empty());
  server.stop();
}

}  // namespace
