#pragma once

// Public vocabulary of the CAN laboratory.
//
// These declarations are fixed by spec section 9 and
// contracts/can_api.md section 1. Adding an enum value here is a spec change,
// not an implementation detail (Constitution Principle I).
//
// NOTE: no Linux header may ever be included from this file or any other
// header under include/tcu/can/. The UAPI lives in src/socketcan_fd.hpp and
// src/socketcan_transport.cpp only. That is how NFR-10 ("public domain model
// is not tied to Linux") is satisfied - research R-02.

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "tcu/scheduler/scheduler_types.hpp"

namespace tcu::can {

/// One clock for the whole project: the RX stamp, the cache's last_good_at and
/// the freshness monitor all read it, so advancing a FakeClockAdapter moves all
/// three together (research R-03). tcu::scheduler::Clock is steady_clock, which
/// is what spec section 9.1 names.
using SteadyTime = tcu::scheduler::Clock::time_point;

enum class FrameFormat {
  Standard11,
  /// V1 never accepts one. The value exists so a rejection can name what it
  /// rejected rather than silently aliasing it onto a standard ID (B-14).
  Extended29,
};

/// Spec section 6.2 requires V1 to "reject frames with unsupported flags
/// (EFF/RTR/ERR), rather than quietly interpreting them as data". FrameFormat
/// carries EFF; this carries RTR and ERR. The spec's illustrative section 9.1
/// struct has no field for either, but UT-07, B-13 and B-15 require
/// FrameValidator - not just the socket filter - to reject and CLASSIFY them,
/// and FakeCanTransport::pushRaw has to be able to construct one. Recorded in
/// plan.md Complexity Tracking.
enum class FrameKind { Data, Remote, Error };

/// Computed on every read from four stored fields, never stored as itself -
/// see SignalCache and data-model.md section 6.1 (research R-13).
enum class SignalQuality {
  NeverReceived,
  Fresh,
  Stale,
  Invalid,
  LinkUnavailable,
};

enum class ReadStatus {
  Received,
  /// Normal: nothing arrived within the timeout.
  Timeout,
  /// Expected on shutdown, or EINTR.
  Interrupted,
  /// Retryable: interface down, deleted, ENETDOWN/ENODEV.
  LinkError,
  /// Not retryable at transport level; service policy decides.
  FatalError,
};

enum class WriteStatus {
  /// The local socket accepted the full frame. This is NOT proof that a peer
  /// processed it (spec section 10.2); for the BCM demo only a matching 0x201
  /// proves delivery.
  Sent,
  InvalidFrame,
  LinkUnavailable,
  SystemError,
};

enum class ServiceState {
  Stopped,
  Starting,
  Running,
  Degraded,
  Stopping,
};

/// Closed set, never a string built from rejected input: a reason goes into a
/// log line and a metric label, and a log line must not carry unvalidated
/// input (Constitution Principle V, spec section 16.1).
enum class RejectReason {
  None,
  Flags,
  Length,
  UnknownId,
  Range,
  ReservedNonzero,
};

/// One frame, as received. Classical CAN, 8 data bytes in V1.
struct CanMessage {
  /// Numeric identifier with the Linux flag bits already stripped: spec
  /// section 6.2 - "CAN ID is not part of the data payload... mask them before
  /// using a numeric identifier".
  std::uint32_t id{0};
  /// Taken from the EFF flag before masking, so a rejection can say why.
  FrameFormat format{FrameFormat::Standard11};
  /// Taken from the RTR and ERR flags, for the same reason.
  FrameKind kind{FrameKind::Data};
  /// Fixed size: no allocation on the RX path.
  std::array<std::uint8_t, 8> data{};
  /// 0..8. Payload byte length, NOT the wire-level DLC encoding for CAN FD
  /// (spec section 6.2).
  std::uint8_t length{0};
  /// Stamped by the RX worker from the injected clock the moment read()
  /// returned. Never a sender-supplied time (spec section 9.1).
  SteadyTime received_at{};
};

/// The public read model. A value, never a handle into the cache.
struct SignalSample {
  std::string signal_name;
  /// Absent ONLY when nothing was ever accepted. Present in Stale, Invalid and
  /// LinkUnavailable, holding the last good value - spec section 6.4: "value is
  /// optional; last_good_value does not imply freshness". A consumer is never
  /// handed a stale speed as 0.0 or a missing ignition as false (NFR-07).
  std::optional<double> value;
  SignalQuality quality{SignalQuality::NeverReceived};
  /// Zero when quality is NeverReceived.
  SteadyTime last_good_at{};
  std::uint32_t source_id{0};
  /// Strictly increasing per accepted update; 0 means never updated. Lets a
  /// consumer detect "same value, new frame" (B-05).
  std::uint64_t generation{0};
};

}  // namespace tcu::can
