#include "tcu/can/signal_cache.hpp"

#include <utility>

#include "tcu/can/frame_catalog.hpp"

namespace tcu::can {

SignalCache::SignalCache(const std::map<std::string, SignalConfig>& configured_signals,
                         std::shared_ptr<tcu::scheduler::ClockAdapter> clock)
    : clock_(std::move(clock)) {
  // Seeded from the catalog, not from the configuration: which signals exist is
  // a property of the frame table, and the configuration only adjusts
  // deadlines. A configuration missing a signal therefore cannot make that
  // signal unreadable - and the loader rejects such a file anyway.
  for (const auto& definition : kSignals) {
    State state;
    state.timeout = definition.timeout;
    state.source_id = definition.source_id;

    const auto configured = configured_signals.find(std::string(definition.name));
    if (configured != configured_signals.end()) {
      state.timeout = configured->second.timeout;
    }

    states_.emplace(std::string(definition.name), state);
  }
}

SignalQuality SignalCache::computeQuality(const State& state, SteadyTime now) const {
  // The precedence order of data-model.md section 6.1. Each step answers a
  // different question, and the order is the order in which the answers matter
  // to a consumer deciding whether to use the value.

  // 1. The transport cannot deliver, so nothing we hold can be called fresh,
  //    whatever its age (spec section 13.6).
  if (!transport_available_) {
    return SignalQuality::LinkUnavailable;
  }

  // 2. Nothing was ever accepted. Distinct from a value of zero, and that
  //    distinction is the whole point of B-02 and UT-22.
  if (!state.last_good_value.has_value()) {
    return SignalQuality::NeverReceived;
  }

  // 3. The value predates a transport outage and nothing has arrived since.
  //    Reported Stale rather than Fresh: the value is real but unproven, and
  //    spec section 13.6 requires a new valid frame before freshness returns.
  //    Stale rather than LinkUnavailable because the transport IS up now, and
  //    rather than Invalid because nothing was malformed (B-20).
  if (state.unproven_since_link_loss) {
    return SignalQuality::Stale;
  }

  // 4. The deadline. STRICTLY greater: at exactly last_good_at + timeout the
  //    signal is still fresh (research R-12, B-06, B-07, UT-11).
  if (now > state.last_good_at + state.timeout) {
    return SignalQuality::Stale;
  }

  // 5. A decode error newer than the last good value. Checked after the
  //    deadline on purpose: a signal that is both stale and recently malformed
  //    is reported Stale, because the deadline is the stronger fact.
  if (state.last_error_reason.has_value() && state.last_error_at > state.last_good_at) {
    return SignalQuality::Invalid;
  }

  return SignalQuality::Fresh;
}

SignalSample SignalCache::buildSample(const std::string& name, const State& state,
                                      SteadyTime now) const {
  SignalSample sample;
  sample.signal_name = name;
  // Present in every state except NeverReceived, including Stale and Invalid:
  // spec section 6.4 - "last_good_value does not imply freshness". The quality
  // is what tells the reader how much to trust it (NFR-07).
  sample.value = state.last_good_value;
  sample.quality = computeQuality(state, now);
  sample.last_good_at = state.last_good_at;
  sample.source_id = state.source_id;
  sample.generation = state.generation;
  return sample;
}

bool SignalCache::apply(const SignalSample& update) {
  std::lock_guard<std::mutex> lock(mutex_);

  const auto it = states_.find(update.signal_name);
  if (it == states_.end() || !update.value.has_value()) {
    return false;
  }

  State& state = it->second;
  state.last_good_value = update.value;
  // The one place this advances (spec section 17.2). Everything that makes a
  // stale signal stale, and keeps a malformed frame from un-staling it, rests
  // on that remaining true.
  state.last_good_at = update.last_good_at;
  state.source_id = update.source_id;
  state.generation += 1;
  // A frame just arrived, so the value is proven again. This is the only thing
  // that clears it - which is what makes B-20 hold however brief the outage was.
  state.unproven_since_link_loss = false;
  // Cleared because a newer good value supersedes the error. The timestamp
  // comparison in computeQuality would reach the same conclusion; clearing
  // keeps the stored state honest rather than relying on that arithmetic.
  state.last_error_reason.reset();

  return true;
}

void SignalCache::recordError(std::string_view name, RejectReason reason, SteadyTime at) {
  std::lock_guard<std::mutex> lock(mutex_);

  const auto it = states_.find(name);
  if (it == states_.end()) {
    return;
  }

  // Deliberately touches nothing else: not last_good_value, not last_good_at,
  // not generation. Spec section 13.4 - "Do not reset last_good_at; a malformed
  // frame cannot refresh a stale signal."
  it->second.last_error_reason = reason;
  it->second.last_error_at = at;
}

std::optional<SignalSample> SignalCache::get(std::string_view name) const {
  std::lock_guard<std::mutex> lock(mutex_);

  const auto it = states_.find(name);
  if (it == states_.end()) {
    // Not a catalog signal at all. A known-but-never-received signal returns a
    // sample with quality NeverReceived instead, and a caller must be able to
    // distinguish the two.
    return std::nullopt;
  }
  return buildSample(it->first, it->second, clock_->now());
}

std::vector<SignalSample> SignalCache::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);

  // One clock reading for the whole snapshot, so two signals in the same
  // response cannot disagree about what time it is.
  const SteadyTime now = clock_->now();

  std::vector<SignalSample> out;
  out.reserve(states_.size());
  for (const auto& entry : states_) {
    out.push_back(buildSample(entry.first, entry.second, now));
  }
  return out;
}

std::vector<QualityTransition> SignalCache::recomputeAll(SteadyTime now) {
  std::vector<QualityTransition> transitions;

  for (auto& entry : states_) {
    State& state = entry.second;
    const SignalQuality computed = computeQuality(state, now);
    if (computed == state.published_quality) {
      // Nothing to say. This is what makes B-08 structural: a health tick that
      // finds the same answer as last time emits nothing, so a signal that has
      // been stale for an hour produced exactly one event.
      continue;
    }

    transitions.push_back(
        QualityTransition{entry.first, state.published_quality, computed, state.generation});
    state.published_quality = computed;
  }

  return transitions;
}

std::vector<QualityTransition> SignalCache::refresh(SteadyTime now) {
  std::lock_guard<std::mutex> lock(mutex_);
  return recomputeAll(now);
}

std::vector<QualityTransition> SignalCache::setTransportAvailable(bool available) {
  std::lock_guard<std::mutex> lock(mutex_);

  transport_available_ = available;

  if (!available) {
    // Marked on the way DOWN rather than on the way up, so a service that is
    // destroyed while degraded leaves no stale "proven" claim behind. A signal
    // with no value yet has nothing to doubt.
    for (auto& entry : states_) {
      if (entry.second.last_good_value.has_value()) {
        entry.second.unproven_since_link_loss = true;
      }
    }
  }
  // Recomputed at the current time rather than at some remembered instant: on
  // the way down every signal becomes LinkUnavailable regardless of age, and on
  // the way back up each one falls to rule 2 or 3 - so none can return to Fresh
  // until a new valid frame moves its last_good_at (B-20).
  return recomputeAll(clock_->now());
}

}  // namespace tcu::can
