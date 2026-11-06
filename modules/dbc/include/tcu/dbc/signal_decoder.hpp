#pragma once

// One frame to one batch of decoded signals.
//
// Pure: no file, no lock, no clock, no thread, and no exception for any frame
// content (contracts/dbc_api.md section 4). Spec section 8 fixes SignalValue,
// DecodedBatch, DecodeResult and ISignalDecoder; DecodePolicy carries the two
// configuration-driven rules of research R-04 and R-05.

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "tcu/dbc/dbc_database.hpp"
#include "tcu/dbc/frame_view.hpp"
#include "tcu/dbc/signal_quality.hpp"

namespace tcu::dbc {

struct SignalValue {
  std::string signalName;
  /// Always the exact integer read from the bits, whatever `physical` rounds
  /// to. Never routed through double or int64_t (invariant P-06).
  std::uint64_t raw{0};
  bool isSigned{false};
  std::optional<std::int64_t> signedRaw;
  double physical{0.0};
  std::string unit;
  std::optional<std::string> enumLabel;
  SignalQuality quality{SignalQuality::NotReceived};
  /// Set when the unsigned raw value exceeds 2^53, where a double can no
  /// longer represent every integer exactly (spec section 7.2).
  bool physicalPrecisionLoss{false};
};

struct DecodedBatch {
  std::uint32_t arbitrationId{0};
  std::uint32_t busId{0};
  FrameFormat format{FrameFormat::Standard11};
  std::string messageName;
  /// Exactly one schema identity per batch (invariant P-13, NFR-06).
  std::string dbcHash;
  std::uint64_t dbcVersion{0};
  std::chrono::steady_clock::time_point rxTime;
  /// Active signals only: the unconditional set plus the branch the selector
  /// chose. An inactive branch signal never appears (invariant P-14).
  std::vector<SignalValue> signals;
  /// True when the message is multiplexed and the selector value matched no
  /// branch: the unconditional signals are still here, and no branch was
  /// guessed (invariant P-15, B-20).
  bool muxUnresolved{false};
};

struct DecodeResult {
  DecodeError error{DecodeError::None};
  std::optional<DecodedBatch> batch;
};

enum class PayloadLengthPolicy {
  /// The only policy in v1: frame.size must equal the declared length (B-06).
  Exact,
};

/// Keyed by fully qualified signal name, "MESSAGE.Signal" (contracts/
/// decoder_config.md section 2). Raw, pre-scaling values (research R-05).
using InvalidRawTable = std::unordered_map<std::string, std::vector<std::uint64_t>>;
/// Range checking is opt-in per signal (research R-04).
using RangeEnforcement = std::unordered_map<std::string, bool>;

struct DecodePolicy {
  PayloadLengthPolicy length{PayloadLengthPolicy::Exact};
  /// Borrowed, may be null. Owned by the caller for the decoder's lifetime.
  const InvalidRawTable* invalidRaw{nullptr};
  const RangeEnforcement* rangeEnforce{nullptr};
};

class ISignalDecoder {
 public:
  virtual ~ISignalDecoder() = default;
  virtual DecodeResult decode(const CanFrameView& frame) const = 0;
};

class SignalDecoder final : public ISignalDecoder {
 public:
  SignalDecoder(std::shared_ptr<const CompiledDbc> db, DecodePolicy policy);

  /// const, and reads only its arguments plus the immutable database, so any
  /// number of threads may call it on one instance.
  DecodeResult decode(const CanFrameView& frame) const override;

  const CompiledDbc& database() const noexcept { return *db_; }

 private:
  std::shared_ptr<const CompiledDbc> db_;
  DecodePolicy policy_;
};

}  // namespace tcu::dbc
