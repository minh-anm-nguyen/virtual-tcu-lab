// T091: four rows of the spec section 10 fault matrix, driven from C++ with a
// raw AF_UNIX peer rather than a Python helper (research R-13).
//
//   partial frame      a frame written one byte at a time still decodes
//   two in one write   two frames in one write both decode, in order
//   oversized frame    a 10 MiB prefix is rejected before any allocation
//   corrupt header     a bad version or type byte closes the session

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
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
    const std::string more = peer.readAtLeast(1, 300ms);
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

ServerOptions optionsFor(const TempSocketDir& dir) {
  ServerOptions o;
  o.socket_path = dir.socketPath();
  return o;
}

bool handshakeByteByByte(RawPeer& peer) {
  const std::string hello = rawFrame(1, 1, "ipc.hello", "{\"version\":1,\"client_id\":\"t\"}");
  if (!peer.writeByteByByte(hello)) {
    return false;
  }
  const std::string reply = readOneFrame(peer);
  return !reply.empty() && frameType(reply) == 2;
}

// Fault row: "partial frame - Python test gui tung byte"
TEST(FaultPartialFrame, FrameSplitIntoSingleBytesStillDecodes) {
  TempSocketDir dir;
  IpcServer server(optionsFor(dir));
  ASSERT_TRUE(server.registerHandler("vehicle.ok", [](std::string) {
    IpcResponse r;
    r.payload_json = "{\"ok\":true}";
    return r;
  }));
  ASSERT_TRUE(server.start());

  RawPeer peer;
  ASSERT_TRUE(peer.connect(dir.socketPath()));
  // Even the handshake arrives one byte at a time.
  ASSERT_TRUE(handshakeByteByByte(peer));

  const std::string request = rawFrame(1, 42, "vehicle.ok", "{\"a\":1}");
  ASSERT_TRUE(peer.writeByteByByte(request));

  const std::string reply = readOneFrame(peer, 3000ms);
  ASSERT_FALSE(reply.empty()) << "a byte-at-a-time frame was not decoded";
  EXPECT_EQ(frameType(reply), 2);
  EXPECT_EQ(frameRequestId(reply), 42u);
  EXPECT_EQ(server.metrics().protocol_errors_total, 0u);

  server.stop();
}

// Fault row: two frames delivered in one write
TEST(FaultPartialFrame, TwoFramesInOneWriteBothDecode) {
  TempSocketDir dir;
  IpcServer server(optionsFor(dir));
  ASSERT_TRUE(server.registerHandler("vehicle.ok", [](std::string) { return IpcResponse{}; }));
  ASSERT_TRUE(server.start());

  RawPeer peer;
  ASSERT_TRUE(peer.connect(dir.socketPath()));
  // The handshake and the first request arrive in a single write.
  const std::string both = rawFrame(1, 1, "ipc.hello", "{\"version\":1,\"client_id\":\"t\"}") +
                           rawFrame(1, 2, "vehicle.ok", "{}");
  ASSERT_TRUE(peer.writeBytes(both));

  const std::string first = readOneFrame(peer);
  ASSERT_FALSE(first.empty());
  EXPECT_EQ(frameRequestId(first), 1u);

  const std::string second = readOneFrame(peer);
  ASSERT_FALSE(second.empty()) << "the second frame of the same write was lost";
  EXPECT_EQ(frameRequestId(second), 2u);
  EXPECT_EQ(server.metrics().protocol_errors_total, 0u);

  server.stop();
}

// Fault row: "oversized frame - gui prefix 10 MiB"
TEST(FaultPartialFrame, OversizePrefixIsRejectedBeforeAllocation) {
  TempSocketDir dir;
  IpcServer server(optionsFor(dir));
  ASSERT_TRUE(server.start());

  RawPeer peer;
  ASSERT_TRUE(peer.connect(dir.socketPath()));
  ASSERT_TRUE(peer.writeBytes(rawFrame(1, 1, "ipc.hello", "{\"version\":1,\"client_id\":\"t\"}")));
  ASSERT_FALSE(readOneFrame(peer).empty());

  // Only the prefix is sent: if the server sized a buffer from it, this would
  // allocate 10 MiB for four bytes of input.
  const std::uint32_t huge = 10u * 1024u * 1024u;
  std::string prefix;
  for (int shift = 24; shift >= 0; shift -= 8) {
    prefix.push_back(static_cast<char>((huge >> shift) & 0xFF));
  }
  ASSERT_TRUE(peer.writeBytes(prefix));

  peer.readAvailable(1000ms);
  EXPECT_TRUE(peer.isClosedByPeer()) << "an oversize prefix must close the session";
  EXPECT_GT(server.metrics().protocol_errors_total, 0u);

  // The peer observes EOF the instant the descriptor closes, which is a few
  // instructions before connections_active is restored - so this is a wait,
  // not an immediate assertion. Visible only under a sanitizer's slowdown.
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (server.sessionCount() != 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_EQ(server.sessionCount(), 0u);

  server.stop();
}

// Fault row: "corrupt version/type - sua bytes trong test fixture"
TEST(FaultPartialFrame, CorruptVersionOrTypeClosesTheSession) {
  for (const std::size_t offset : {std::size_t{4}, std::size_t{5}}) {  // version, type
    TempSocketDir dir;
    IpcServer server(optionsFor(dir));
    ASSERT_TRUE(server.registerHandler("vehicle.ok", [](std::string) { return IpcResponse{}; }));
    ASSERT_TRUE(server.start());

    RawPeer peer;
    ASSERT_TRUE(peer.connect(dir.socketPath()));
    ASSERT_TRUE(
        peer.writeBytes(rawFrame(1, 1, "ipc.hello", "{\"version\":1,\"client_id\":\"t\"}")));
    ASSERT_FALSE(readOneFrame(peer).empty());

    std::string corrupt = rawFrame(1, 2, "vehicle.ok", "{}");
    corrupt[offset] = static_cast<char>(9);
    ASSERT_TRUE(peer.writeBytes(corrupt));

    peer.readAvailable(1000ms);
    EXPECT_TRUE(peer.isClosedByPeer()) << "corrupt byte at offset " << offset;
    EXPECT_GT(server.metrics().protocol_errors_total, 0u);

    server.stop();
  }
}

}  // namespace
