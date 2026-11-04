#pragma once

// Transport protocol v1: length-prefixed framing over a byte stream.
//
// Authoritative contract:
//   specs/002-ipc-service-framework/contracts/wire_protocol.md
//
//  0        4    5    6        8                16        18
//  +--------+----+----+--------+----------------+--------+------+---------+
//  | body_  | ve | ty | flags  |   request_id   | name_  | name | payload |
//  | length | rs | pe | uint16 |     uint64     | length |      |         |
//  +--------+----+----+--------+----------------+--------+------+---------+
//  |<- not counted ->|<------------ body_length = 14 + N + M ------------>|
//
// Every multi-byte field is big-endian. The codec is pure: no I/O, no global
// state, and the payload is never parsed (research R-01).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace tcu::ipc {

enum class FrameType : std::uint8_t { Request = 1, Response = 2, Notification = 3, Error = 4 };

inline constexpr std::uint8_t kProtocolVersion = 1;
/// Bytes of the body_length prefix itself, which body_length does not count.
inline constexpr std::size_t kHeaderPrefix = 4;
/// version + type + flags + request_id + name_length.
inline constexpr std::size_t kFixedBodyFields = 14;
inline constexpr std::size_t kFrameOverhead = kHeaderPrefix + kFixedBodyFields;  // 18
inline constexpr std::size_t kMaxBodyLength = 65536;
inline constexpr std::size_t kMaxFrameBytes = kHeaderPrefix + kMaxBodyLength;  // 65540
/// 14 fixed body bytes plus at least one name byte.
inline constexpr std::size_t kMinBodyLength = kFixedBodyFields + 1;
inline constexpr std::size_t kMaxNameLength = 255;

struct FrameHeader {
  std::uint32_t body_length{0};
  std::uint8_t version{kProtocolVersion};
  FrameType type{FrameType::Request};
  std::uint16_t flags{0};
  std::uint64_t request_id{0};
  std::uint16_t name_length{0};
};

struct Frame {
  FrameHeader header{};
  /// Method for a Request/Response/Error, topic for a Notification.
  std::string name;
  /// Opaque application bytes.
  std::string payload;
};

enum class DecodeResult {
  Ok,
  /// More bytes are needed; the buffer was left untouched (invariant W6).
  Incomplete,
  /// body_length exceeds kMaxBodyLength. Nothing was allocated (invariant W1).
  TooLarge,
  UnsupportedVersion,
  ProtocolError
};

/// Serialises one frame. `header.version`, `type`, `flags` and `request_id` are
/// used verbatim so a test can forge an invalid frame; `body_length` and
/// `name_length` are always recomputed, so a *valid* frame cannot be encoded
/// inconsistently.
///
/// Returns an empty string when the body would exceed kMaxBodyLength or the
/// name would exceed kMaxNameLength - the caller must not put an oversize
/// payload on the wire.
std::string encode(const FrameHeader& header, std::string_view name, std::string_view payload);

/// Decodes at most one frame from the front of `buffer`.
///
/// On `Ok`, `consumed` is the number of bytes the frame occupied, which the
/// caller removes from its buffer. On every other result `consumed` is 0 and
/// the buffer is unchanged, so feeding one byte at a time decodes the same
/// frame as feeding it whole.
DecodeResult tryDecode(std::string_view buffer, Frame& out, std::size_t& consumed);

/// True when `name` matches [A-Za-z0-9_.-]{1,255} and holds no NUL
/// (invariant W7). Exposed because the router validates a name before using it
/// as a map key.
bool isValidName(std::string_view name) noexcept;

}  // namespace tcu::ipc
