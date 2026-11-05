#pragma once

// The latest-value store, and the one place signal quality is decided.
//
// THE CENTRAL IDEA (research R-13, spec section 12.2): quality is COMPUTED, not
// stored. Four independent facts are kept per signal - last good value, when it
// arrived, the last decode error, and whether the transport is up - and
// NeverReceived / Fresh / Stale / Invalid / LinkUnavailable is derived from them
// on every read.
//
// That choice is what makes several of the spec's hard requirements structural
// rather than conventional:
//   * exactly one stale transition per deadline crossing (B-08), because a
//     recomputation that matches the last published value reports nothing;
//   * a malformed frame cannot refresh a stale signal (B-10, B-11), because
//     only apply() may move last_good_at;
//   * a recovered link does not fabricate freshness (B-20), because transport
//     health is a separate field from the deadline.
//
// Spec section 12.2 asks for exactly this: "Implementation should track
// last_good, last_error, transport_health, and freshness as separate fields so
// information is not lost."

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tcu/can/can_config.hpp"
#include "tcu/can/can_types.hpp"
#include "tcu/scheduler/clock_adapter.hpp"

namespace tcu::can {

struct QualityTransition {
  std::string signal_name;
  SignalQuality from;
  SignalQuality to;
  std::uint64_t generation;
};

class ISignalCache {
 public:
  virtual ~ISignalCache() = default;

  /// Commits an accepted, in-range value. The ONLY method that may advance
  /// last_good_at (spec section 17.2). Returns false for an unknown signal
  /// name, having changed nothing.
  virtual bool apply(const SignalSample& update) = 0;

  /// Records a decode or validation failure against a signal. Leaves the last
  /// good value, its timestamp and the generation untouched: a malformed frame
  /// cannot refresh a stale signal (spec section 13.4).
  virtual void recordError(std::string_view name, RejectReason reason, SteadyTime at) = 0;

  /// nullopt for a name that is not in the catalog - which is NOT the same as a
  /// known signal that was never received, and a caller must be able to tell
  /// them apart (B-02).
  virtual std::optional<SignalSample> get(std::string_view name) const = 0;

  virtual std::vector<SignalSample> snapshot() const = 0;

  /// Recomputes every signal's quality and returns ONLY those whose quality
  /// differs from the last value an event was published for, updating that
  /// record as it goes.
  ///
  /// The service calls this after apply() as well as from the health timer, so
  /// a recovery is reported the moment the frame arrives rather than at the next
  /// tick - one mechanism for every transition, which is what keeps B-08 and
  /// B-09 from needing separate bookkeeping.
  virtual std::vector<QualityTransition> refresh(SteadyTime now) = 0;

  /// Mirrors the service state. After the transport is restored no signal
  /// becomes Fresh until a new valid frame advances its last_good_at (B-20).
  virtual std::vector<QualityTransition> setTransportAvailable(bool available) = 0;
};

class SignalCache final : public ISignalCache {
 public:
  /// Deadlines come from the configuration; a signal the configuration does not
  /// mention keeps its frame_catalog.hpp default. The clock is the project
  /// clock, so get() can compute staleness without the caller passing a time.
  SignalCache(const std::map<std::string, SignalConfig>& configured_signals,
              std::shared_ptr<tcu::scheduler::ClockAdapter> clock);

  bool apply(const SignalSample& update) override;
  void recordError(std::string_view name, RejectReason reason, SteadyTime at) override;
  std::optional<SignalSample> get(std::string_view name) const override;
  std::vector<SignalSample> snapshot() const override;
  std::vector<QualityTransition> refresh(SteadyTime now) override;
  std::vector<QualityTransition> setTransportAvailable(bool available) override;

 private:
  struct State {
    std::optional<double> last_good_value;
    SteadyTime last_good_at{};
    std::optional<RejectReason> last_error_reason;
    SteadyTime last_error_at{};
    std::uint64_t generation{0};
    /// True from the moment the transport goes down until a new valid frame
    /// arrives. Without it, an outage shorter than the signal's deadline would
    /// leave the value reading Fresh the instant the link returned - but spec
    /// section 13.6 says to "wait for new valid frames to restore FRESH status",
    /// and section 12.2 only allows LINK_UNAVAILABLE -> FRESH on "transport
    /// restored + valid update". The deadline alone cannot express that, because
    /// it measures age and this is about provenance (B-20).
    bool unproven_since_link_loss{false};
    /// The last quality an event was emitted for. Compared, never shown.
    SignalQuality published_quality{SignalQuality::NeverReceived};
    std::uint32_t source_id{0};
    std::chrono::milliseconds timeout{0};
  };

  SignalQuality computeQuality(const State& state, SteadyTime now) const;
  SignalSample buildSample(const std::string& name, const State& state, SteadyTime now) const;
  std::vector<QualityTransition> recomputeAll(SteadyTime now);

  mutable std::mutex mutex_;
  /// std::less<> so a string_view can look up without allocating.
  std::map<std::string, State, std::less<>> states_;
  bool transport_available_{true};
  std::shared_ptr<tcu::scheduler::ClockAdapter> clock_;
};

}  // namespace tcu::can
