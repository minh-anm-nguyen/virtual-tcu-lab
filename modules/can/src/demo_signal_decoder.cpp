#include "tcu/can/demo_signal_decoder.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <utility>

#include "tcu/can/frame_catalog.hpp"

namespace tcu::can {
namespace {

/// Extracts one signal from the payload according to its catalog row. Every
/// read is bounds-checked by Catalog.DecodeDescriptorsStayInsideThePayload at
/// test time, so there is no runtime index check here.
double extract(const SignalDefinition& signal, const std::array<std::uint8_t, 8>& data) {
  switch (signal.decode) {
    case Decode::Bit:
      return static_cast<double>((data[signal.byte_offset] >> signal.bit_offset) & 0x1u);

    case Decode::Byte:
      return static_cast<double>(data[signal.byte_offset]);

    case Decode::BitMask: {
      const std::uint32_t mask = (1u << signal.bit_width) - 1u;
      return static_cast<double>((data[signal.byte_offset] >> signal.bit_offset) & mask);
    }

    case Decode::U16LeScaled: {
      // Little-endian, as spec section 6.2 fixes for every multi-byte field.
      const std::uint32_t raw = static_cast<std::uint32_t>(data[signal.byte_offset]) |
                                (static_cast<std::uint32_t>(data[signal.byte_offset + 1]) << 8);
      return static_cast<double>(raw) / signal.scale_divisor;
    }
  }
  return 0.0;
}

}  // namespace

DecodeResult DemoSignalDecoder::decode(const CanMessage& frame) {
  // The validator normally runs first, but the decoder repeats the three
  // structural checks rather than trusting its caller: a decoder that produced
  // a signal from a remote frame would be a silent data-integrity failure, and
  // the cost is three comparisons.
  if (frame.format != FrameFormat::Standard11 || frame.kind != FrameKind::Data) {
    return {false, RejectReason::Flags, {}};
  }

  const FrameDefinition* definition = findFrame(frame.id);
  if (definition == nullptr) {
    return {false, RejectReason::UnknownId, {}};
  }
  if (frame.length != definition->expected_length) {
    return {false, RejectReason::Length, {}};
  }

  // Decode every signal the frame carries, then commit none of them if any is
  // out of range. 0x102 carries voltage and SOC; 0x110 carries the heartbeat
  // counter and the ECU status. A frame is one sender's snapshot, so accepting
  // half of it would publish a state the ECU never reported (research R-14,
  // answering spec open question 5).
  std::vector<SignalSample> updates;
  for (const auto& signal : kSignals) {
    if (signal.source_id != frame.id) {
      continue;
    }

    const double value = extract(signal, frame.data);
    // Inclusive bounds: 250.00 km/h and 6.00 V are accepted values, not
    // rejected ones (spec section 6.3).
    if (value < signal.min || value > signal.max) {
      return {false, RejectReason::Range, {}};
    }

    SignalSample sample;
    sample.signal_name = std::string(signal.name);
    sample.value = value;
    // Fresh by construction: the value was just accepted. The cache recomputes
    // quality on read anyway, so this records what the decoder observed rather
    // than a claim that outlives the frame (research R-13).
    sample.quality = SignalQuality::Fresh;
    sample.last_good_at = frame.received_at;
    sample.source_id = frame.id;
    updates.push_back(std::move(sample));
  }

  // 0x200 and 0x201 are in the frame table but carry no signals: they are the
  // command and its acknowledgment. The service routes 0x201 to the correlator
  // before the decoder is reached, so this is a defensive answer rather than a
  // path the service takes.
  if (updates.empty()) {
    return {false, RejectReason::UnknownId, {}};
  }

  return {true, RejectReason::None, std::move(updates)};
}

}  // namespace tcu::can
