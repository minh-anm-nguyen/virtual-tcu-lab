#pragma once

// The two-axis quality cache.
//
// Spec section 9.2's matrix is the specification. The model is deliberately two
// separate views (research R-12, spec section 9.1):
//
//   lastObservationQuality  what the most recent decode of this signal said
//   lastGood + lastGoodRxTime  the most recent VALID value and when it arrived
//
// and freshness is derived from the last-good timestamp at query time, never
// stored. A single enum cannot say "invalid AND timed out", which is exactly
// what spec section 9.2's note requires us to be able to say.
//
// No method of this class invokes a caller-supplied callback: commit() and
// scanFreshness() RETURN the events they generated and the caller publishes
// them after the lock is gone. That is what makes "no callback under the cache
// mutex" structural rather than a review rule (invariant S-07).

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "tcu/dbc/dbc_database.hpp"
#include "tcu/dbc/signal_decoder.hpp"
#include "tcu/scheduler/scheduler_types.hpp"
#include "tcu/signal/signal_quality.hpp"

namespace tcu::signal {

/// The project clock, shared with projects 03 and 05 so one fake clock moves
/// every deadline together.
using SteadyTime = tcu::scheduler::Clock::time_point;

struct SignalSnapshot {
  dbc::SignalQuality lastObservationQuality{dbc::SignalQuality::NotReceived};
  /// Absent only while nothing valid was ever received. Present in every other
  /// state, including stale and invalid, holding the last good value.
  std::optional<dbc::SignalValue> lastGood;
  SteadyTime lastGoodRxTime{};
  /// Derived at read time: now >= lastGoodRxTime + timeout (invariant S-05).
  bool lastGoodIsStale{false};
  /// Increments on every last-good commit, never decreases (invariant S-03).
  /// The freshness scan compares it to detect a value that landed while the
  /// scan was deciding (research R-13).
  std::uint64_t epoch{0};
  /// The schema version the last good value was decoded against.
  std::uint64_t dbcVersion{0};
};

struct FreshnessConfig {
  std::chrono::milliseconds defaultTimeout{500};
  /// Keyed by "MESSAGE.Signal" (contracts/decoder_config.md section 2).
  std::map<std::string, std::chrono::milliseconds> overrides;
};

class SignalCache {
 public:
  explicit SignalCache(FreshnessConfig freshness);

  /// Applies one decoded batch. Returns the events it generated and publishes
  /// nothing (invariant S-08).
  std::vector<QualityEvent> commit(const dbc::DecodedBatch& batch);

  /// Records a frame-level rejection: the observed quality moves, last-good and
  /// its timestamp do not (invariant S-04, B-08).
  std::vector<QualityEvent> recordFrameError(const dbc::CompiledMessage& message,
                                             std::uint32_t busId, dbc::SignalQuality quality);

  std::optional<SignalSnapshot> get(const SignalKey& key, SteadyTime now) const;
  std::vector<std::pair<SignalKey, SignalSnapshot>> snapshot(SteadyTime now) const;

  /// Two-phase epoch recheck: sample the entries past their deadline, then emit
  /// only for those whose epoch has not moved since (research R-13, race B).
  std::vector<QualityEvent> scanFreshness(SteadyTime now);

  /// After a committed reload: a signal whose semantic hash is unchanged keeps
  /// its last-good and freshness; a changed one resets to NotReceived; one that
  /// no longer exists becomes ConfigUnavailable (research R-09, invariant V-12).
  std::vector<QualityEvent> migrate(const dbc::CompiledDbc& next);

  /// Every signal of the database, at NotReceived, so a query can tell
  /// "unknown signal" from "known but never seen" (B-02 of project 05's spec).
  void seed(const dbc::CompiledDbc& db);

  std::uint64_t outOfOrderCount() const;
  std::size_t size() const;

 private:
  struct State {
    dbc::SignalQuality observed{dbc::SignalQuality::NotReceived};
    std::optional<dbc::SignalValue> lastGood;
    SteadyTime lastGoodRxTime{};
    std::uint64_t epoch{0};
    std::uint64_t dbcVersion{0};
    std::uint64_t semanticHash{0};
    std::chrono::milliseconds timeout{500};
    /// The quality an event was last emitted for. Compared, never shown.
    dbc::SignalQuality reported{dbc::SignalQuality::NotReceived};
    bool reportedStale{false};
  };

  bool isStale(const State& state, SteadyTime now) const;
  std::chrono::milliseconds timeoutFor(const std::string& messageName,
                                       const std::string& signalName) const;

  mutable std::mutex mutex_;
  std::unordered_map<SignalKey, State, SignalKeyHash> states_;
  FreshnessConfig freshness_;
  std::uint64_t outOfOrder_{0};
};

}  // namespace tcu::signal
