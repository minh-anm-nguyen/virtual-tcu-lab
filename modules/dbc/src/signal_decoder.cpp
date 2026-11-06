#include "tcu/dbc/signal_decoder.hpp"

#include <cmath>
#include <utility>

#include "fault_hooks.hpp"
#include "tcu/dbc/bit_extractor.hpp"

namespace tcu::dbc {
namespace {

constexpr std::uint64_t kExactDoubleLimit = 1ull << 53;

bool contains(const std::vector<std::uint64_t>& values, std::uint64_t value) {
  for (const std::uint64_t candidate : values) {
    if (candidate == value) return true;
  }
  return false;
}

const std::vector<std::uint64_t>* invalidCodesFor(const DecodePolicy& policy,
                                                  const std::string& messageName,
                                                  const std::string& signalName) {
  // Only pay for the key when a policy exists at all.
  // shortcut: builds a "MESSAGE.Signal" string per signal when invalid_raw_codes
  // is non-empty; upgrade to indices resolved at load time when the benchmark
  // shows it matters (research R-07).
  if (policy.invalidRaw == nullptr || policy.invalidRaw->empty()) return nullptr;
  const auto it = policy.invalidRaw->find(messageName + "." + signalName);
  return it == policy.invalidRaw->end() ? nullptr : &it->second;
}

bool rangeEnforcedFor(const DecodePolicy& policy, const std::string& messageName,
                      const std::string& signalName) {
  if (policy.rangeEnforce == nullptr || policy.rangeEnforce->empty()) return false;
  const auto it = policy.rangeEnforce->find(messageName + "." + signalName);
  return it != policy.rangeEnforce->end() && it->second;
}

const std::string* labelFor(const SignalMeta& meta, std::uint64_t raw, bool isSigned,
                            std::optional<std::int64_t> signedRaw) {
  if (meta.valueDescriptions.empty()) return nullptr;
  const std::int64_t key =
      isSigned && signedRaw.has_value() ? *signedRaw : static_cast<std::int64_t>(raw);
  for (const auto& pair : meta.valueDescriptions) {
    if (pair.first == key) return &pair.second;
  }
  // A number with no label is not invalid: the numeric value is always
  // available (spec section 6.1, invariant D-11).
  return nullptr;
}

SignalValue decodeOne(const CanFrameView& frame, const CompiledMessage& message,
                      const BitPlan& plan, const DecodePolicy& policy) {
  const SignalMeta& meta = message.signals[plan.signalIndex];

  SignalValue value;
  value.signalName = meta.name;
  value.unit = meta.unit;
  value.isSigned = plan.isSigned;

  detail::testHooks().beforeBitRead();

  const auto raw = extractRaw(frame.data, frame.size, plan.startBit, plan.width, plan.byteOrder);
  if (!raw.has_value()) {
    // Cannot happen for a validated plan against a correct length, so it means
    // the frame length check was bypassed: report, never guess (P-01, P-02).
    value.quality = SignalQuality::DecodeError;
    return value;
  }
  value.raw = *raw;

  double numeric = 0.0;
  if (plan.isSigned) {
    const auto signedRaw = signExtend(*raw, plan.width);
    if (!signedRaw.has_value()) {
      value.quality = SignalQuality::DecodeError;
      return value;
    }
    value.signedRaw = *signedRaw;
    numeric = static_cast<double>(*signedRaw);
  } else {
    numeric = static_cast<double>(*raw);
    value.physicalPrecisionLoss = *raw >= kExactDoubleLimit;
  }

  value.physical = numeric * plan.factor + plan.offset;
  if (!std::isfinite(value.physical)) {
    value.physical = 0.0;
    value.quality = SignalQuality::DecodeError;
    return value;
  }

  const std::string* label = labelFor(meta, value.raw, plan.isSigned, value.signedRaw);
  if (label != nullptr) value.enumLabel = *label;

  const auto* invalidCodes = invalidCodesFor(policy, message.name, meta.name);
  if (invalidCodes != nullptr && contains(*invalidCodes, value.raw)) {
    value.quality = SignalQuality::InvalidRaw;
    return value;
  }

  if (rangeEnforcedFor(policy, message.name, meta.name) &&
      (value.physical < meta.minimum || value.physical > meta.maximum)) {
    // Never clamped: an out-of-range value is a quality, not a correction
    // (spec section 7.2, research R-04).
    value.quality = SignalQuality::OutOfRange;
    return value;
  }

  value.quality = SignalQuality::Valid;
  return value;
}

}  // namespace

const char* toString(SignalQuality quality) noexcept {
  switch (quality) {
    case SignalQuality::NotReceived:       return "not_received";
    case SignalQuality::Valid:             return "valid";
    case SignalQuality::InvalidRaw:        return "invalid_raw";
    case SignalQuality::OutOfRange:        return "out_of_range";
    case SignalQuality::Stale:             return "stale";
    case SignalQuality::DecodeError:       return "decode_error";
    case SignalQuality::NotActive:         return "not_active";
    case SignalQuality::ConfigUnavailable: return "config_unavailable";
  }
  return "not_received";
}

SignalDecoder::SignalDecoder(std::shared_ptr<const CompiledDbc> db, DecodePolicy policy)
    : db_(std::move(db)), policy_(policy) {}

DecodeResult SignalDecoder::decode(const CanFrameView& frame) const {
  DecodeResult result;
  if (db_ == nullptr) {
    result.error = DecodeError::InternalError;
    return result;
  }

  // Order fixed by contracts/dbc_api.md section 4: flags, then length, then
  // lookup. A refused frame never reads the payload (invariant P-11).
  if (frame.isError) {
    result.error = DecodeError::ErrorFrame;
    return result;
  }
  if (frame.isRemote) {
    result.error = DecodeError::RemoteFrame;
    return result;
  }
  if (frame.size > kMaxClassicalPayload) {
    result.error = DecodeError::WrongLength;
    return result;
  }

  const CompiledMessage* message = db_->find(frame.busId, frame.format, frame.arbitrationId);
  if (message == nullptr) {
    result.error = DecodeError::UnknownMessage;
    return result;
  }
  if (frame.size != message->expectedLength) {
    result.error = DecodeError::WrongLength;
    return result;
  }
  if (frame.size > 0 && frame.data == nullptr) {
    result.error = DecodeError::InternalError;
    return result;
  }

  DecodedBatch batch;
  batch.arbitrationId = frame.arbitrationId;
  batch.busId = frame.busId;
  batch.format = frame.format;
  batch.messageName = message->name;
  batch.dbcHash = db_->hash();
  batch.dbcVersion = db_->version();
  batch.rxTime = frame.rxTime;
  batch.signals.reserve(message->unconditional.size() + 1u);

  for (const BitPlan& plan : message->unconditional) {
    batch.signals.push_back(decodeOne(frame, *message, plan, policy_));
  }

  if (message->selector.has_value()) {
    const BitPlan& selectorPlan = *message->selector;
    SignalValue selector = decodeOne(frame, *message, selectorPlan, policy_);
    const bool usable = selector.quality == SignalQuality::Valid ||
                        selector.quality == SignalQuality::InvalidRaw ||
                        selector.quality == SignalQuality::OutOfRange;
    const std::uint64_t selectorRaw = selector.raw;
    batch.signals.push_back(std::move(selector));

    if (!usable) {
      // The selector itself could not be decoded: reject the branch half only,
      // and never choose a default branch (invariant P-15, spec section 10.3).
      batch.muxUnresolved = true;
    } else {
      const auto branch = message->branches.find(static_cast<std::uint32_t>(selectorRaw));
      if (branch == message->branches.end()) {
        batch.muxUnresolved = true;
      } else {
        for (const BitPlan& plan : branch->second) {
          batch.signals.push_back(decodeOne(frame, *message, plan, policy_));
        }
      }
    }
  }

  result.batch = std::move(batch);
  return result;
}

}  // namespace tcu::dbc
