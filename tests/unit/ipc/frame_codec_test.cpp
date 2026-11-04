// T016-T023: the wire codec. contracts/wire_protocol.md sections 1 and 4,
// invariants W1-W7 of data-model.md, BDD B-04, B-05, B-06.

#include "tcu/ipc/frame_codec.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace {

using tcu::ipc::DecodeResult;
using tcu::ipc::Frame;
using tcu::ipc::FrameHeader;
using tcu::ipc::FrameType;
using tcu::ipc::kFixedBodyFields;
using tcu::ipc::kHeaderPrefix;
using tcu::ipc::kMaxBodyLength;

std::string makeRequest(std::uint64_t id, const std::string& name, const std::string& payload) {
  FrameHeader h;
  h.type = FrameType::Request;
  h.request_id = id;
  return tcu::ipc::encode(h, name, payload);
}

void putBe32(std::string& buf, std::size_t offset, std::uint32_t value) {
  buf[offset + 0] = static_cast<char>((value >> 24) & 0xFF);
  buf[offset + 1] = static_cast<char>((value >> 16) & 0xFF);
  buf[offset + 2] = static_cast<char>((value >> 8) & 0xFF);
  buf[offset + 3] = static_cast<char>(value & 0xFF);
}

void putBe16(std::string& buf, std::size_t offset, std::uint16_t value) {
  buf[offset + 0] = static_cast<char>((value >> 8) & 0xFF);
  buf[offset + 1] = static_cast<char>(value & 0xFF);
}

// T016 / B-06 / invariant W1
TEST(FrameCodec, RejectOversizeBeforeAlloc) {
  std::string buf(kHeaderPrefix, static_cast<char>(0));
  putBe32(buf, 0, 10u * 1024u * 1024u);  // 10 MiB declared, 4 bytes present
  const std::string untouched = buf;

  Frame out;
  std::size_t consumed = 99;
  EXPECT_EQ(tcu::ipc::tryDecode(buf, out, consumed), DecodeResult::TooLarge);
  EXPECT_EQ(consumed, 0u);
  EXPECT_EQ(buf, untouched);
}

TEST(FrameCodec, RejectBodyLengthBelowMinimum) {
  // 14 fixed body bytes plus at least one name byte: 14 alone is too short.
  std::string buf(kHeaderPrefix, static_cast<char>(0));
  putBe32(buf, 0, static_cast<std::uint32_t>(kFixedBodyFields));
  Frame out;
  std::size_t consumed = 0;
  EXPECT_EQ(tcu::ipc::tryDecode(buf, out, consumed), DecodeResult::ProtocolError);
}

// T017 / B-04 / invariant W6
TEST(FrameCodec, ReadHeaderByteByByte) {
  const std::string frame = makeRequest(7, "vehicle.getStatus", "{}");
  ASSERT_FALSE(frame.empty());

  std::string fed;
  for (std::size_t i = 0; i + 1 < frame.size(); ++i) {
    fed.push_back(frame[i]);
    Frame out;
    std::size_t consumed = 123;
    const std::string before = fed;
    EXPECT_EQ(tcu::ipc::tryDecode(fed, out, consumed), DecodeResult::Incomplete) << "at byte " << i;
    EXPECT_EQ(consumed, 0u);
    EXPECT_EQ(fed, before) << "Incomplete must leave the buffer byte-identical";
  }

  fed.push_back(frame.back());
  Frame out;
  std::size_t consumed = 0;
  ASSERT_EQ(tcu::ipc::tryDecode(fed, out, consumed), DecodeResult::Ok);
  EXPECT_EQ(consumed, frame.size());
  EXPECT_EQ(out.name, "vehicle.getStatus");
  EXPECT_EQ(out.payload, "{}");
  EXPECT_EQ(out.header.request_id, 7u);
  EXPECT_EQ(out.header.type, FrameType::Request);
}

// T018 / B-05
TEST(FrameCodec, TwoFramesOneBuffer) {
  const std::string a = makeRequest(1, "a.one", "{}");
  const std::string b = makeRequest(2, "b.two", "[]");
  std::string buf = a + b;

  Frame out;
  std::size_t consumed = 0;
  ASSERT_EQ(tcu::ipc::tryDecode(buf, out, consumed), DecodeResult::Ok);
  EXPECT_EQ(consumed, a.size());
  EXPECT_EQ(out.header.request_id, 1u);
  EXPECT_EQ(out.name, "a.one");

  buf.erase(0, consumed);
  ASSERT_EQ(tcu::ipc::tryDecode(buf, out, consumed), DecodeResult::Ok);
  EXPECT_EQ(consumed, b.size());
  EXPECT_EQ(out.header.request_id, 2u);
  EXPECT_EQ(out.name, "b.two");

  buf.erase(0, consumed);
  EXPECT_EQ(tcu::ipc::tryDecode(buf, out, consumed), DecodeResult::Incomplete);
}

// T019 / invariants W2, W3
TEST(FrameCodec, RejectInvalidNameLength) {
  {
    // name_length larger than the remaining body
    std::string frame = makeRequest(5, "abc", "{}");
    putBe16(frame, kHeaderPrefix + 12, 9999);
    Frame out;
    std::size_t consumed = 0;
    EXPECT_EQ(tcu::ipc::tryDecode(frame, out, consumed), DecodeResult::ProtocolError);
  }
  {
    // A shorter name leaves a longer payload, which is still self-consistent,
    // so the frame decodes - what must never happen is reading past the body.
    std::string frame = makeRequest(5, "abc", "{}");
    putBe16(frame, kHeaderPrefix + 12, 1);
    Frame out;
    std::size_t consumed = 0;
    ASSERT_EQ(tcu::ipc::tryDecode(frame, out, consumed), DecodeResult::Ok);
    EXPECT_EQ(out.name.size(), 1u);
    EXPECT_EQ(out.name.size() + out.payload.size() + kFixedBodyFields, out.header.body_length);
  }
  {
    // zero-length name is invalid for every frame type
    std::string frame = makeRequest(5, "abc", "{}");
    putBe16(frame, kHeaderPrefix + 12, 0);
    Frame out;
    std::size_t consumed = 0;
    EXPECT_EQ(tcu::ipc::tryDecode(frame, out, consumed), DecodeResult::ProtocolError);
  }
}

// T020 / invariant W4
TEST(FrameCodec, RejectUnsupportedVersion) {
  {
    std::string frame = makeRequest(5, "abc", "{}");
    frame[kHeaderPrefix] = static_cast<char>(2);  // version
    Frame out;
    std::size_t consumed = 0;
    EXPECT_EQ(tcu::ipc::tryDecode(frame, out, consumed), DecodeResult::UnsupportedVersion);
  }
  {
    std::string frame = makeRequest(5, "abc", "{}");
    frame[kHeaderPrefix + 1] = static_cast<char>(9);  // type outside 1..4
    Frame out;
    std::size_t consumed = 0;
    EXPECT_EQ(tcu::ipc::tryDecode(frame, out, consumed), DecodeResult::ProtocolError);
  }
  {
    std::string frame = makeRequest(5, "abc", "{}");
    putBe16(frame, kHeaderPrefix + 2, 1);  // flags must be 0 in v1
    Frame out;
    std::size_t consumed = 0;
    EXPECT_EQ(tcu::ipc::tryDecode(frame, out, consumed), DecodeResult::ProtocolError);
  }
}

// T021 / invariant W5
TEST(FrameCodec, RequestIdRules) {
  {
    // request_id 0 is reserved for notifications
    const std::string frame = makeRequest(0, "abc", "{}");
    Frame out;
    std::size_t consumed = 0;
    EXPECT_EQ(tcu::ipc::tryDecode(frame, out, consumed), DecodeResult::ProtocolError);
  }
  {
    FrameHeader h;
    h.type = FrameType::Notification;
    h.request_id = 0;
    const std::string frame = tcu::ipc::encode(h, "vehicle.ignitionChanged", "{}");
    Frame out;
    std::size_t consumed = 0;
    ASSERT_EQ(tcu::ipc::tryDecode(frame, out, consumed), DecodeResult::Ok);
    EXPECT_EQ(out.header.type, FrameType::Notification);
    EXPECT_EQ(out.header.request_id, 0u);
  }
  {
    // a notification carrying a nonzero id is a protocol error
    FrameHeader h;
    h.type = FrameType::Notification;
    h.request_id = 4;
    const std::string frame = tcu::ipc::encode(h, "t.topic", "{}");
    Frame out;
    std::size_t consumed = 0;
    EXPECT_EQ(tcu::ipc::tryDecode(frame, out, consumed), DecodeResult::ProtocolError);
  }
}

// T022 / invariant W7
TEST(FrameCodec, RejectInvalidName) {
  const char* bad[] = {"has space", "has/slash", "has:colon", "paren()"};
  for (const char* name : bad) {
    FrameHeader h;
    h.type = FrameType::Request;
    h.request_id = 1;
    const std::string frame = tcu::ipc::encode(h, name, "{}");
    Frame out;
    std::size_t consumed = 0;
    EXPECT_EQ(tcu::ipc::tryDecode(frame, out, consumed), DecodeResult::ProtocolError)
        << "name=" << name;
  }
  {
    // an embedded NUL must not survive into a map key
    std::string name = "ab";
    name.push_back(static_cast<char>(0));
    name += "cd";
    FrameHeader h;
    h.type = FrameType::Request;
    h.request_id = 1;
    const std::string frame = tcu::ipc::encode(h, name, "{}");
    Frame out;
    std::size_t consumed = 0;
    EXPECT_EQ(tcu::ipc::tryDecode(frame, out, consumed), DecodeResult::ProtocolError);
  }
  {
    // the allowed set still passes
    FrameHeader h;
    h.type = FrameType::Request;
    h.request_id = 1;
    const std::string frame = tcu::ipc::encode(h, "a.b_c-D9", "{}");
    Frame out;
    std::size_t consumed = 0;
    EXPECT_EQ(tcu::ipc::tryDecode(frame, out, consumed), DecodeResult::Ok);
  }
}

// T023
TEST(FrameCodec, RoundTrip) {
  const std::string name = "vehicle.getStatus";
  const std::size_t max_payload = kMaxBodyLength - kFixedBodyFields - name.size();
  const std::string payloads[] = {"", "x", std::string(max_payload, 'p')};

  for (const std::string& payload : payloads) {
    FrameHeader h;
    h.type = FrameType::Response;
    h.request_id = 123456789012345ULL;
    const std::string frame = tcu::ipc::encode(h, name, payload);
    ASSERT_EQ(frame.size(), kHeaderPrefix + kFixedBodyFields + name.size() + payload.size());

    Frame out;
    std::size_t consumed = 0;
    ASSERT_EQ(tcu::ipc::tryDecode(frame, out, consumed), DecodeResult::Ok)
        << "payload size " << payload.size();
    EXPECT_EQ(consumed, frame.size());
    EXPECT_EQ(out.header.version, 1);
    EXPECT_EQ(out.header.type, FrameType::Response);
    EXPECT_EQ(out.header.flags, 0);
    EXPECT_EQ(out.header.request_id, 123456789012345ULL);
    EXPECT_EQ(out.name, name);
    EXPECT_EQ(out.payload, payload);
  }
}

TEST(FrameCodec, EncodeRefusesAnOversizeBody) {
  FrameHeader h;
  h.type = FrameType::Request;
  h.request_id = 1;
  const std::string too_big(kMaxBodyLength, 'x');
  EXPECT_TRUE(tcu::ipc::encode(h, "a.b", too_big).empty());
}

}  // namespace
