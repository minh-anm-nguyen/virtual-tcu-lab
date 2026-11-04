// T052, T053, T057: per-session TX caps, isolation between sessions, and the
// protocol-error path. B-13, invariants S11, S12, S15, research R-08.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "support/raw_listener.hpp"
#include "support/raw_peer.hpp"
#include "support/temp_socket_dir.hpp"
#include "tcu/ipc/frame_codec.hpp"
#include "tcu/ipc/ipc_server.hpp"

namespace {

using namespace std::chrono_literals;
using tcu::ipc::IpcResponse;
using tcu::ipc::IpcServer;
using tcu::ipc::ServerOptions;
using tcu::ipc::test::frameRequestId;
using tcu::ipc::test::RawPeer;
using tcu::ipc::test::rawFrame;
using tcu::ipc::test::TempSocketDir;

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
  return !reply.empty() && frameType(reply) == 2;
}

// T052 / invariant S11 / research R-08
TEST(Session, SlowConsumerDoesNotGrowMemory) {
  TempSocketDir dir;
  ServerOptions options;
  options.socket_path = dir.socketPath();
  // Caps lowered so the test reaches them quickly; the shape of the rule is
  // the same at the spec defaults of 256 messages / 1 MiB.
  options.max_tx_messages = 4;
  options.max_tx_bytes = tcu::ipc::kMaxFrameBytes;
  IpcServer server(options);
  ASSERT_TRUE(server.start());

  RawPeer slow;
  ASSERT_TRUE(slow.connect(dir.socketPath()));
  ASSERT_TRUE(handshake(slow));
  ASSERT_TRUE(
      slow.writeBytes(rawFrame(1, 2, "ipc.subscribe", "{\"topic\":\"vehicle.ignitionChanged\"}")));
  ASSERT_FALSE(readOneFrame(slow).empty());

  // From here the peer never reads again. Large payloads fill the kernel socket
  // buffer quickly, after which the session queue must refuse rather than grow.
  const std::string big_payload(60000, 'x');
  for (int i = 0; i < 200; ++i) {
    server.publishNotification("vehicle.ignitionChanged", big_payload);
    if (i % 20 == 0) {
      std::this_thread::sleep_for(10ms);
    }
  }
  std::this_thread::sleep_for(500ms);

  const auto m = server.metrics();
  EXPECT_GT(m.queue_rejects_total, 0u) << "the TX cap never engaged";
  EXPECT_LE(m.tx_queue_bytes, options.max_tx_bytes) << "the session queue grew past its cap";
  // The session is counted, not closed (research R-08).
  EXPECT_EQ(server.sessionCount(), 1u);

  server.stop();
}

// T053 / B-13
TEST(Session, OneSlowPeerDoesNotStarveAnother) {
  TempSocketDir dir;
  ServerOptions options;
  options.socket_path = dir.socketPath();
  options.max_tx_messages = 4;
  options.max_tx_bytes = tcu::ipc::kMaxFrameBytes;
  IpcServer server(options);
  ASSERT_TRUE(server.registerHandler("vehicle.ok", [](std::string) {
    IpcResponse r;
    r.payload_json = "{\"ok\":true}";
    return r;
  }));
  ASSERT_TRUE(server.start());

  RawPeer slow;
  ASSERT_TRUE(slow.connect(dir.socketPath()));
  ASSERT_TRUE(handshake(slow));
  ASSERT_TRUE(
      slow.writeBytes(rawFrame(1, 2, "ipc.subscribe", "{\"topic\":\"vehicle.ignitionChanged\"}")));
  ASSERT_FALSE(readOneFrame(slow).empty());

  RawPeer healthy;
  ASSERT_TRUE(healthy.connect(dir.socketPath()));
  ASSERT_TRUE(handshake(healthy));

  // Flood the slow peer, which is not reading.
  const std::string big_payload(60000, 'x');
  for (int i = 0; i < 100; ++i) {
    server.publishNotification("vehicle.ignitionChanged", big_payload);
  }

  // The healthy session must still be served.
  ASSERT_TRUE(healthy.writeBytes(rawFrame(1, 9, "vehicle.ok", "{}")));
  const std::string reply = readOneFrame(healthy, 3000ms);
  ASSERT_FALSE(reply.empty()) << "a slow peer starved an unrelated session";
  EXPECT_EQ(frameType(reply), 2);
  EXPECT_EQ(frameRequestId(reply), 9u);

  server.stop();
}

// T057 / invariant S12
TEST(Session, ProtocolErrorClosesSessionAndSparesOthers) {
  TempSocketDir dir;
  ServerOptions options;
  options.socket_path = dir.socketPath();
  IpcServer server(options);
  ASSERT_TRUE(server.registerHandler("vehicle.ok", [](std::string) { return IpcResponse{}; }));
  ASSERT_TRUE(server.start());

  RawPeer healthy;
  ASSERT_TRUE(healthy.connect(dir.socketPath()));
  ASSERT_TRUE(handshake(healthy));

  RawPeer corrupt;
  ASSERT_TRUE(corrupt.connect(dir.socketPath()));
  ASSERT_TRUE(handshake(corrupt));
  // A frame whose flags field is nonzero: reserved in v1, so a protocol error.
  std::string bad = rawFrame(1, 5, "vehicle.ok", "{}");
  bad[6] = static_cast<char>(0);
  bad[7] = static_cast<char>(1);
  ASSERT_TRUE(corrupt.writeBytes(bad));

  corrupt.readAvailable(1000ms);
  EXPECT_TRUE(corrupt.isClosedByPeer()) << "a protocol error must close that session";

  const auto m = server.metrics();
  EXPECT_GT(m.protocol_errors_total, 0u);

  // The other session is untouched (invariant S15).
  ASSERT_TRUE(healthy.writeBytes(rawFrame(1, 6, "vehicle.ok", "{}")));
  const std::string reply = readOneFrame(healthy);
  ASSERT_FALSE(reply.empty());
  EXPECT_EQ(frameRequestId(reply), 6u);

  server.stop();
}

// Invariant S12: an oversize length prefix is rejected before allocation and
// closes the session (B-06 at the session level rather than the codec level).
TEST(Session, OversizeFramePrefixClosesSession) {
  TempSocketDir dir;
  ServerOptions options;
  options.socket_path = dir.socketPath();
  IpcServer server(options);
  ASSERT_TRUE(server.start());

  RawPeer peer;
  ASSERT_TRUE(peer.connect(dir.socketPath()));
  ASSERT_TRUE(handshake(peer));

  std::string prefix(4, static_cast<char>(0));
  const std::uint32_t huge = 10u * 1024u * 1024u;
  prefix[0] = static_cast<char>((huge >> 24) & 0xFF);
  prefix[1] = static_cast<char>((huge >> 16) & 0xFF);
  prefix[2] = static_cast<char>((huge >> 8) & 0xFF);
  prefix[3] = static_cast<char>(huge & 0xFF);
  ASSERT_TRUE(peer.writeBytes(prefix));

  peer.readAvailable(1000ms);
  EXPECT_TRUE(peer.isClosedByPeer());
  EXPECT_GT(server.metrics().protocol_errors_total, 0u);

  server.stop();
}

}  // namespace
