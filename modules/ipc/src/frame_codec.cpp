#include "tcu/ipc/frame_codec.hpp"

namespace tcu::ipc {
namespace {

void appendBe16(std::string& out, std::uint16_t value) {
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
  out.push_back(static_cast<char>(value & 0xFF));
}

void appendBe32(std::string& out, std::uint32_t value) {
  out.push_back(static_cast<char>((value >> 24) & 0xFF));
  out.push_back(static_cast<char>((value >> 16) & 0xFF));
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
  out.push_back(static_cast<char>(value & 0xFF));
}

void appendBe64(std::string& out, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xFF));
  }
}

std::uint8_t readU8(std::string_view buffer, std::size_t offset) {
  return static_cast<std::uint8_t>(buffer[offset]);
}

std::uint16_t readBe16(std::string_view buffer, std::size_t offset) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(readU8(buffer, offset)) << 8) |
                                    readU8(buffer, offset + 1));
}

std::uint32_t readBe32(std::string_view buffer, std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    value = (value << 8) | readU8(buffer, offset + i);
  }
  return value;
}

std::uint64_t readBe64(std::string_view buffer, std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value = (value << 8) | readU8(buffer, offset + i);
  }
  return value;
}

bool isKnownType(std::uint8_t raw) noexcept {
  return raw >= static_cast<std::uint8_t>(FrameType::Request) &&
         raw <= static_cast<std::uint8_t>(FrameType::Error);
}

}  // namespace

bool isValidName(std::string_view name) noexcept {
  if (name.empty() || name.size() > kMaxNameLength) {
    return false;
  }
  for (const char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '_' || c == '.' || c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

std::string encode(const FrameHeader& header, std::string_view name, std::string_view payload) {
  if (name.size() > kMaxNameLength) {
    return {};
  }
  const std::size_t body = kFixedBodyFields + name.size() + payload.size();
  if (body > kMaxBodyLength) {
    return {};
  }

  std::string out;
  out.reserve(kHeaderPrefix + body);
  appendBe32(out, static_cast<std::uint32_t>(body));
  out.push_back(static_cast<char>(header.version));
  out.push_back(static_cast<char>(static_cast<std::uint8_t>(header.type)));
  appendBe16(out, header.flags);
  appendBe64(out, header.request_id);
  appendBe16(out, static_cast<std::uint16_t>(name.size()));
  out.append(name);
  out.append(payload);
  return out;
}

DecodeResult tryDecode(std::string_view buffer, Frame& out, std::size_t& consumed) {
  // Nothing is consumed unless a whole frame decodes (invariant W6).
  consumed = 0;

  if (buffer.size() < kHeaderPrefix) {
    return DecodeResult::Incomplete;
  }

  // The size bound is checked against a constant before anything is sized by
  // it, so a hostile length cannot drive an allocation (invariant W1).
  const std::uint32_t body_length = readBe32(buffer, 0);
  if (body_length > kMaxBodyLength) {
    return DecodeResult::TooLarge;
  }
  if (body_length < kMinBodyLength) {
    return DecodeResult::ProtocolError;
  }

  const std::size_t total = kHeaderPrefix + body_length;
  if (buffer.size() < total) {
    return DecodeResult::Incomplete;
  }

  FrameHeader header;
  header.body_length = body_length;
  header.version = readU8(buffer, kHeaderPrefix);
  if (header.version != kProtocolVersion) {
    return DecodeResult::UnsupportedVersion;
  }

  const std::uint8_t raw_type = readU8(buffer, kHeaderPrefix + 1);
  if (!isKnownType(raw_type)) {
    return DecodeResult::ProtocolError;
  }
  header.type = static_cast<FrameType>(raw_type);

  header.flags = readBe16(buffer, kHeaderPrefix + 2);
  if (header.flags != 0) {
    // v1 reserves every flag bit; rejecting a nonzero value is what keeps them
    // extensible in v2 without a second frame type.
    return DecodeResult::ProtocolError;
  }

  header.request_id = readBe64(buffer, kHeaderPrefix + 4);
  // request_id == 0 iff Notification (invariant W5).
  const bool is_notification = header.type == FrameType::Notification;
  if (is_notification != (header.request_id == 0)) {
    return DecodeResult::ProtocolError;
  }

  header.name_length = readBe16(buffer, kHeaderPrefix + 12);
  // The name must fit the body that is actually present; no substring is taken
  // from an unchecked length (invariants W2, W3).
  if (header.name_length == 0 ||
      static_cast<std::size_t>(header.name_length) > body_length - kFixedBodyFields) {
    return DecodeResult::ProtocolError;
  }

  const std::size_t name_offset = kHeaderPrefix + kFixedBodyFields;
  const std::string_view name = buffer.substr(name_offset, header.name_length);
  if (!isValidName(name)) {
    return DecodeResult::ProtocolError;
  }

  const std::size_t payload_offset = name_offset + header.name_length;
  const std::size_t payload_size = total - payload_offset;

  out.header = header;
  out.name.assign(name);
  out.payload.assign(buffer.substr(payload_offset, payload_size));
  consumed = total;
  return DecodeResult::Ok;
}

}  // namespace tcu::ipc
