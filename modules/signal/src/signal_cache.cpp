#include "tcu/signal/signal_cache.hpp"

#include <utility>

#include "fault_hooks.hpp"

namespace tcu::signal {
namespace {

std::string qualifiedName(const std::string& messageName, const std::string& signalName) {
  return messageName + "." + signalName;
}

}  // namespace

const char* toString(QualityEventKind kind) noexcept {
  switch (kind) {
    case QualityEventKind::SignalUpdated:        return "signal_updated";
    case QualityEventKind::SignalQualityChanged: return "signal_quality_changed";
    case QualityEventKind::SignalRecovered:      return "signal_recovered";
    case QualityEventKind::SchemaChanged:        return "schema_changed";
  }
  return "signal_quality_changed";
}

SignalCache::SignalCache(FreshnessConfig freshness) : freshness_(std::move(freshness)) {}

std::chrono::milliseconds SignalCache::timeoutFor(const std::string& messageName,
                                                  const std::string& signalName) const {
  const auto it = freshness_.overrides.find(qualifiedName(messageName, signalName));
  return it == freshness_.overrides.end() ? freshness_.defaultTimeout : it->second;
}

bool SignalCache::isStale(const State& state, SteadyTime now) const {
  if (!state.lastGood.has_value()) return false;
  // now >= deadline is stale: the boundary belongs to the stale side, and
  // isFresh() is exact whatever the timer's jitter was (B-09, invariant S-05).
  return now >= state.lastGoodRxTime + state.timeout;
}

std::vector<QualityEvent> SignalCache::commit(const dbc::DecodedBatch& batch) {
  std::vector<QualityEvent> events;
  detail::testHooks().beforeCacheCommit();

  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& value : batch.signals) {
      SignalKey key{batch.busId, batch.format == dbc::FrameFormat::Extended29,
                    batch.arbitrationId, value.signalName};
      State& state = states_[key];
      state.timeout = timeoutFor(batch.messageName, value.signalName);

      if (value.quality != dbc::SignalQuality::Valid) {
        // Invalid, out-of-range or a decode error: the observation moves, the
        // last good value and its timestamp do not (invariant S-04, B-08).
        state.observed = value.quality;
        if (state.reported != value.quality) {
          events.push_back(QualityEvent{QualityEventKind::SignalQualityChanged, key,
                                        state.reported, value.quality});
          state.reported = value.quality;
        }
        continue;
      }

      if (state.lastGood.has_value() && batch.rxTime < state.lastGoodRxTime) {
        // A frame older than what we already accepted: lastGoodRxTime must
        // never go backwards (invariant S-02, spec section 10.5).
        ++outOfOrder_;
        continue;
      }

      const dbc::SignalQuality previous = state.reported;
      const bool wasStale = state.reportedStale;

      state.lastGood = value;
      state.lastGoodRxTime = batch.rxTime;
      state.dbcVersion = batch.dbcVersion;
      ++state.epoch;
      state.observed = dbc::SignalQuality::Valid;
      state.reported = dbc::SignalQuality::Valid;
      state.reportedStale = false;

      events.push_back(QualityEvent{QualityEventKind::SignalUpdated, key, previous,
                                    dbc::SignalQuality::Valid});
      if (wasStale) {
        events.push_back(QualityEvent{QualityEventKind::SignalRecovered, key,
                                      dbc::SignalQuality::Stale, dbc::SignalQuality::Valid});
      } else if (previous != dbc::SignalQuality::Valid) {
        events.push_back(QualityEvent{QualityEventKind::SignalQualityChanged, key, previous,
                                      dbc::SignalQuality::Valid});
      }
    }
  }

  detail::testHooks().afterCacheCommit();
  return events;
}

std::vector<QualityEvent> SignalCache::recordFrameError(const dbc::CompiledMessage& message,
                                                        std::uint32_t busId,
                                                        dbc::SignalQuality quality) {
  std::vector<QualityEvent> events;
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& meta : message.signals) {
    SignalKey key{busId, message.key.isExtended, message.key.arbitrationId, meta.name};
    State& state = states_[key];
    state.timeout = timeoutFor(message.name, meta.name);
    state.observed = quality;
    if (state.reported != quality) {
      events.push_back(
          QualityEvent{QualityEventKind::SignalQualityChanged, key, state.reported, quality});
      state.reported = quality;
    }
  }
  return events;
}

std::optional<SignalSnapshot> SignalCache::get(const SignalKey& key, SteadyTime now) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = states_.find(key);
  if (it == states_.end()) return std::nullopt;

  const State& state = it->second;
  SignalSnapshot out;
  out.lastObservationQuality = state.observed;
  out.lastGood = state.lastGood;
  out.lastGoodRxTime = state.lastGoodRxTime;
  out.lastGoodIsStale = isStale(state, now);
  out.epoch = state.epoch;
  out.dbcVersion = state.dbcVersion;
  return out;
}

std::vector<std::pair<SignalKey, SignalSnapshot>> SignalCache::snapshot(SteadyTime now) const {
  std::vector<std::pair<SignalKey, SignalSnapshot>> out;
  std::lock_guard<std::mutex> lock(mutex_);
  out.reserve(states_.size());
  for (const auto& entry : states_) {
    SignalSnapshot value;
    value.lastObservationQuality = entry.second.observed;
    value.lastGood = entry.second.lastGood;
    value.lastGoodRxTime = entry.second.lastGoodRxTime;
    value.lastGoodIsStale = isStale(entry.second, now);
    value.epoch = entry.second.epoch;
    value.dbcVersion = entry.second.dbcVersion;
    out.emplace_back(entry.first, value);
  }
  return out;
}

std::vector<QualityEvent> SignalCache::scanFreshness(SteadyTime now) {
  // Phase 1: sample who is past their deadline, with the epoch we saw.
  std::vector<std::pair<SignalKey, std::uint64_t>> candidates;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : states_) {
      const State& state = entry.second;
      if (state.reportedStale) continue;
      if (!isStale(state, now)) continue;
      candidates.emplace_back(entry.first, state.epoch);
    }
  }

  // The seam race recipe B needs: a valid frame may commit right here, after we
  // decided a signal was stale but before we say so (research R-13).
  detail::testHooks().freshnessSampled();

  // Phase 2: emit only for entries whose epoch has not moved since phase 1. An
  // entry that took a valid value in between is fresh now, and a stale event
  // computed from older state must not overwrite it (invariant S-10).
  std::vector<QualityEvent> events;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& candidate : candidates) {
      const auto it = states_.find(candidate.first);
      if (it == states_.end()) continue;
      State& state = it->second;
      if (state.epoch != candidate.second) continue;
      if (state.reportedStale) continue;
      if (!isStale(state, now)) continue;

      const dbc::SignalQuality previous = state.reported;
      state.reportedStale = true;
      state.reported = dbc::SignalQuality::Stale;
      events.push_back(QualityEvent{QualityEventKind::SignalQualityChanged, candidate.first,
                                    previous, dbc::SignalQuality::Stale});
    }
  }
  return events;
}

void SignalCache::seed(const dbc::CompiledDbc& db) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& entry : db.messages()) {
    const dbc::CompiledMessage& message = entry.second;
    for (const auto& meta : message.signals) {
      SignalKey key{message.key.busId, message.key.isExtended, message.key.arbitrationId,
                    meta.name};
      State& state = states_[key];
      state.semanticHash = meta.semanticHash;
      state.timeout = timeoutFor(message.name, meta.name);
    }
  }
}

std::vector<QualityEvent> SignalCache::migrate(const dbc::CompiledDbc& next) {
  std::vector<QualityEvent> events;
  std::lock_guard<std::mutex> lock(mutex_);

  // What the new schema says about every signal it defines.
  std::unordered_map<SignalKey, std::pair<std::uint64_t, std::chrono::milliseconds>,
                     SignalKeyHash>
      incoming;
  for (const auto& entry : next.messages()) {
    const dbc::CompiledMessage& message = entry.second;
    for (const auto& meta : message.signals) {
      SignalKey key{message.key.busId, message.key.isExtended, message.key.arbitrationId,
                    meta.name};
      incoming.emplace(key, std::make_pair(meta.semanticHash,
                                           timeoutFor(message.name, meta.name)));
    }
  }

  for (auto& entry : states_) {
    State& state = entry.second;
    const auto found = incoming.find(entry.first);

    if (found == incoming.end()) {
      // The signal is gone. Its last-good is kept for diagnostics only and is
      // never reported as a valid reading again (spec section 9.3).
      if (state.reported != dbc::SignalQuality::ConfigUnavailable) {
        events.push_back(QualityEvent{QualityEventKind::SchemaChanged, entry.first,
                                      state.reported, dbc::SignalQuality::ConfigUnavailable});
      }
      state.observed = dbc::SignalQuality::ConfigUnavailable;
      state.reported = dbc::SignalQuality::ConfigUnavailable;
      state.reportedStale = false;
      continue;
    }

    state.timeout = found->second.second;
    if (state.semanticHash != 0 && state.semanticHash == found->second.first) {
      // Same bits, same factor, same unit: the cached number still means what
      // it meant, so last-good and freshness survive the swap (research R-09).
      continue;
    }

    // The quantity changed. Resetting to NotReceived is the conservative
    // choice spec section 9.3 recommends: keeping the old number as Valid is
    // exactly the silent semantics drift the spec forbids.
    if (state.reported != dbc::SignalQuality::NotReceived) {
      events.push_back(QualityEvent{QualityEventKind::SchemaChanged, entry.first, state.reported,
                                    dbc::SignalQuality::NotReceived});
    }
    state.semanticHash = found->second.first;
    state.lastGood.reset();
    state.lastGoodRxTime = SteadyTime{};
    state.observed = dbc::SignalQuality::NotReceived;
    state.reported = dbc::SignalQuality::NotReceived;
    state.reportedStale = false;
    ++state.epoch;
  }

  // Signals the new schema adds, so a query can tell them from unknown names.
  for (const auto& incomingEntry : incoming) {
    const auto it = states_.find(incomingEntry.first);
    if (it != states_.end()) continue;
    State fresh;
    fresh.semanticHash = incomingEntry.second.first;
    fresh.timeout = incomingEntry.second.second;
    states_.emplace(incomingEntry.first, fresh);
  }

  return events;
}

std::uint64_t SignalCache::outOfOrderCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return outOfOrder_;
}

std::size_t SignalCache::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return states_.size();
}

}  // namespace tcu::signal
