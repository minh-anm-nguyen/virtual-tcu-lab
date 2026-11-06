// The service: two owned threads, project 05's transport, project 03's
// scheduler for everything periodic, and a cache whose events are published
// only after its lock is gone.
//
// The reload coordinator lives in this file rather than its own pair, because
// it and the worker are the only two users of the snapshot pointer and keeping
// one owner of that pointer is clearer than splitting it across a boundary.
// Recorded as a deviation from the planned layout in tasks.md.

#include "tcu/signal/decoder_service.hpp"

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#include "fault_hooks.hpp"
#include "rate_limited_log.hpp"
#include "rx_queue.hpp"
#include "tcu/signal/frame_adapter.hpp"

namespace tcu::signal {
namespace {

using tcu::logging::Severity;

std::string hex(std::uint32_t value) {
  static const char* digits = "0123456789ABCDEF";
  std::string out = "0x";
  bool leading = true;
  for (int shift = 28; shift >= 0; shift -= 4) {
    const char digit = digits[(value >> shift) & 0xF];
    if (digit != '0') leading = false;
    if (!leading || shift == 0) out.push_back(digit);
  }
  return out;
}

FrameResult resultOf(dbc::DecodeError error) {
  switch (error) {
    case dbc::DecodeError::None:           return FrameResult::Ok;
    case dbc::DecodeError::UnknownMessage: return FrameResult::UnknownId;
    case dbc::DecodeError::WrongLength:    return FrameResult::WrongLength;
    case dbc::DecodeError::WrongFormat:    return FrameResult::WrongFormat;
    case dbc::DecodeError::ErrorFrame:     return FrameResult::ErrorFrame;
    case dbc::DecodeError::RemoteFrame:    return FrameResult::RemoteFrame;
    default:                               return FrameResult::DecodeError;
  }
}

/// A frame-level rejection maps to the quality the cache should record for
/// every signal of that message, so last-good survives and the observation
/// axis still moves (invariant S-04, B-08).
dbc::SignalQuality qualityOf(dbc::DecodeError error) {
  switch (error) {
    case dbc::DecodeError::WrongLength:
    case dbc::DecodeError::WrongFormat:
    case dbc::DecodeError::ErrorFrame:
    case dbc::DecodeError::RemoteFrame:
      return dbc::SignalQuality::DecodeError;
    default:
      return dbc::SignalQuality::DecodeError;
  }
}

}  // namespace

const char* toString(ServiceState state) noexcept {
  switch (state) {
    case ServiceState::Created:  return "created";
    case ServiceState::Starting: return "starting";
    case ServiceState::Ready:    return "ready";
    case ServiceState::NotReady: return "not_ready";
    case ServiceState::Stopping: return "stopping";
    case ServiceState::Stopped:  return "stopped";
  }
  return "created";
}

struct DecoderService::Impl {
  Impl(DecoderConfig cfg, DecoderServiceDeps d)
      : config(std::move(cfg)),
        deps(std::move(d)),
        cache(config.freshness),
        queue(config.rxQueueCapacity),
        log(deps.logger, config.logRateLimitPerReasonPerMinute) {
    if (!deps.now) {
      deps.now = [] { return tcu::scheduler::Clock::now(); };
    }
    policy.length = dbc::PayloadLengthPolicy::Exact;
    policy.invalidRaw = &config.invalidRawCodes;
    policy.rangeEnforce = &config.rangeEnforcement;
  }

  DecoderConfig config;
  DecoderServiceDeps deps;
  DecoderMetrics metrics;
  SignalCache cache;
  detail::RxQueue queue;
  detail::RateLimitedLog log;
  dbc::DecodePolicy policy;

  std::shared_ptr<const dbc::CompiledDbc> snapshot;
  std::atomic<ServiceState> state{ServiceState::Created};
  std::atomic<bool> running{false};

  std::thread reader;
  std::thread worker;

  mutable std::mutex observerMutex;
  std::vector<std::pair<SubscriptionId, std::weak_ptr<ISignalObserver>>> observers;
  SubscriptionId nextSubscription{1};

  std::mutex reloadMutex;
  bool reloadPending{false};
  std::deque<SteadyTime> reloadAttempts;
  tcu::scheduler::TimerId freshnessTimer{};
  bool freshnessScheduled{false};

  std::shared_ptr<const dbc::CompiledDbc> pin() const { return std::atomic_load(&snapshot); }
  void publish(std::shared_ptr<const dbc::CompiledDbc> next) {
    std::atomic_store(&snapshot, std::move(next));
  }

  void readerLoop();
  void workerLoop();
  void handleFrame(const tcu::can::CanMessage& message);
  void dispatchEvents(const std::vector<QualityEvent>& events);
  void dispatchBatch(const dbc::DecodedBatch& batch);
  void runReload();
  void scheduleFreshness();
};

void DecoderService::Impl::readerLoop() {
  // The ONE thread that touches the transport, for its whole lifetime: project
  // 05's single-receiver rule. It never decodes and never runs a callback.
  while (running.load(std::memory_order_acquire)) {
    const auto result = deps.transport->receive(std::chrono::milliseconds{50});
    if (result.status == tcu::can::ReadStatus::Received && result.message.has_value()) {
      metrics.add(metrics.framesReceived);
      if (!queue.push(*result.message)) {
        log.log(Severity::Warn, "queue_overflow", "capacity",
                "dropped_total=" + std::to_string(queue.dropped()) +
                    " capacity=" + std::to_string(queue.capacity()),
                deps.now());
      }
      continue;
    }
    if (result.status == tcu::can::ReadStatus::Interrupted) {
      // Expected on shutdown, and on EINTR; the loop condition decides.
      continue;
    }
    if (result.status == tcu::can::ReadStatus::LinkError ||
        result.status == tcu::can::ReadStatus::FatalError) {
      log.log(Severity::Error, "transport_error", std::to_string(result.os_error),
              "the transport reported an error", deps.now());
    }
  }
}

void DecoderService::Impl::handleFrame(const tcu::can::CanMessage& message) {
  // Pinned ONCE per frame, so a batch cannot mix schema versions even if a
  // reload commits mid-decode (invariant P-13, research R-14).
  const std::shared_ptr<const dbc::CompiledDbc> db = pin();
  if (detail::testHooks().afterPinSnapshot) detail::testHooks().afterPinSnapshot();
  if (db == nullptr) return;

  const dbc::CanFrameView view = toFrameView(message, deps.busId);
  const dbc::SignalDecoder decoder(db, policy);
  const dbc::DecodeResult result = decoder.decode(view);

  metrics.countFrame(resultOf(result.error));

  if (result.error != dbc::DecodeError::None) {
    if (result.error == dbc::DecodeError::UnknownMessage) {
      log.log(Severity::Warn, "unknown_message", "unknown_id",
              "can_id=" + hex(view.arbitrationId) +
                  " format=" + (view.format == dbc::FrameFormat::Extended29 ? "EFF" : "STD"),
              deps.now());
      return;
    }
    // A known message that was refused: every one of its signals learns that
    // its observation failed, and every last-good value survives (B-06, B-08).
    const dbc::CompiledMessage* known =
        db->find(view.busId, view.format, view.arbitrationId);
    if (known != nullptr) {
      const auto events = cache.recordFrameError(*known, view.busId, qualityOf(result.error));
      log.log(Severity::Warn, "frame_rejected", dbc::toString(result.error),
              "can_id=" + hex(view.arbitrationId) +
                  " expected_len=" + std::to_string(known->expectedLength) +
                  " actual_len=" + std::to_string(view.size),
              deps.now());
      dispatchEvents(events);
    }
    return;
  }

  if (result.batch->muxUnresolved) {
    metrics.add(metrics.unknownMux);
    log.log(Severity::Warn, "unknown_mux_value", result.batch->messageName,
            "no branch matched the decoded selector", deps.now());
  }
  for (const auto& value : result.batch->signals) {
    metrics.countSignal(toLabel(value.quality));
  }

  const auto events = cache.commit(*result.batch);
  dispatchBatch(*result.batch);
  dispatchEvents(events);
}

void DecoderService::Impl::workerLoop() {
  while (true) {
    auto message = queue.pop();
    if (!message.has_value()) return;  // closed and drained
    handleFrame(*message);
  }
}

void DecoderService::Impl::dispatchEvents(const std::vector<QualityEvent>& events) {
  if (events.empty()) return;
  if (detail::testHooks().beforeEventPublish) detail::testHooks().beforeEventPublish();

  for (const auto& event : events) {
    if (event.to == dbc::SignalQuality::Stale) metrics.add(metrics.staleTransitions);
    if (event.kind == QualityEventKind::SignalRecovered) {
      metrics.add(metrics.recoveredTransitions);
    }
    if (event.kind == QualityEventKind::SchemaChanged) metrics.add(metrics.schemaTransitions);
    if (event.kind == QualityEventKind::SignalUpdated) continue;

    // Snapshot the registry, then call outside its lock: an observer must be
    // able to query the cache, subscribe or unsubscribe without deadlocking
    // (invariant S-07, spec section 12 rule 5).
    std::vector<std::pair<SubscriptionId, std::shared_ptr<ISignalObserver>>> targets;
    {
      std::lock_guard<std::mutex> lock(observerMutex);
      for (const auto& entry : observers) {
        if (auto observer = entry.second.lock()) targets.emplace_back(entry.first, observer);
      }
    }
    for (const auto& target : targets) {
      try {
        target.second->onQualityChanged(event.key, event.to);
      } catch (...) {
        // An observer's failure is isolated and counted, never fatal to the
        // worker (B-14, NFR-08, Constitution Principle V).
        metrics.add(metrics.observerFailures);
        log.log(Severity::Error, "observer_failed", std::to_string(target.first),
                "an observer threw from onQualityChanged", deps.now());
      }
    }
  }
}

void DecoderService::Impl::dispatchBatch(const dbc::DecodedBatch& batch) {
  std::vector<std::pair<SubscriptionId, std::shared_ptr<ISignalObserver>>> targets;
  {
    std::lock_guard<std::mutex> lock(observerMutex);
    for (const auto& entry : observers) {
      if (auto observer = entry.second.lock()) targets.emplace_back(entry.first, observer);
    }
  }
  if (targets.empty()) {
    metrics.add(metrics.batchesDroppedByPolicy);
    return;
  }
  metrics.add(metrics.batchesPublished);
  for (const auto& target : targets) {
    try {
      target.second->onBatch(batch);
    } catch (...) {
      metrics.add(metrics.observerFailures);
      log.log(Severity::Error, "observer_failed", std::to_string(target.first),
              "an observer threw from onBatch", deps.now());
    }
  }
}

void DecoderService::Impl::runReload() {
  // Runs on a scheduler worker, never on the decoder thread: parse, validate
  // and compile all happen off the hot path (invariant V-10, research R-15).
  const auto loaded = dbc::loadDbcFile(config.dbcPath, config.parseLimits());
  if (detail::testHooks().afterReloadParse) detail::testHooks().afterReloadParse();

  if (!loaded.ok()) {
    metrics.countLoad(loaded.firstWithCode(dbc::DbcErrorCode::DbcSource) != nullptr
                          ? LoadResultLabel::SourceError
                          : LoadResultLabel::Rejected);
    std::string reason = "unknown";
    std::uint32_t line = 0;
    for (const auto& diagnostic : loaded.diagnostics) {
      if (diagnostic.severity != dbc::Severity::Error) continue;
      reason = dbc::toString(diagnostic.code);
      line = diagnostic.line;
      break;
    }
    // The active snapshot, its version and every cache entry are untouched, and
    // the service stays Ready (invariant V-11, NFR-07, B-11).
    log.always(Severity::Warn, "reload_rejected",
               "reason=" + reason + " line=" + std::to_string(line));
    {
      std::lock_guard<std::mutex> lock(reloadMutex);
      reloadPending = false;
    }
    std::vector<std::pair<SubscriptionId, std::shared_ptr<ISignalObserver>>> targets;
    {
      std::lock_guard<std::mutex> lock(observerMutex);
      for (const auto& entry : observers) {
        if (auto observer = entry.second.lock()) targets.emplace_back(entry.first, observer);
      }
    }
    for (const auto& target : targets) {
      try {
        target.second->onSchemaChanged(0, std::string(), false);
      } catch (...) {
        metrics.add(metrics.observerFailures);
      }
    }
    return;
  }

  const std::shared_ptr<const dbc::CompiledDbc> previous = pin();
  const std::uint64_t nextVersion = (previous == nullptr ? 0 : previous->version()) + 1;

  // A new immutable database carrying the next version. CompiledDbc is
  // immutable, so the version is set by rebuilding rather than by mutation.
  auto next = std::make_shared<const dbc::CompiledDbc>(
      nextVersion, loaded.db->hash(), loaded.db->sourcePath(), loaded.db->messages(),
      loaded.db->stats());

  if (detail::testHooks().beforeReloadCommit) detail::testHooks().beforeReloadCommit();
  publish(next);  // the single commit point (research R-14)
  metrics.countLoad(LoadResultLabel::Ok);
  metrics.activeVersion.store(nextVersion, std::memory_order_relaxed);

  const auto events = cache.migrate(*next);
  log.always(Severity::Info, "reload_committed",
             "from_version=" + std::to_string(previous == nullptr ? 0 : previous->version()) +
                 " to_version=" + std::to_string(nextVersion) + " dbc_hash=" + next->hash() +
                 " signals_reset=" + std::to_string(events.size()));
  {
    std::lock_guard<std::mutex> lock(reloadMutex);
    reloadPending = false;
  }
  dispatchEvents(events);

  std::vector<std::pair<SubscriptionId, std::shared_ptr<ISignalObserver>>> targets;
  {
    std::lock_guard<std::mutex> lock(observerMutex);
    for (const auto& entry : observers) {
      if (auto observer = entry.second.lock()) targets.emplace_back(entry.first, observer);
    }
  }
  for (const auto& target : targets) {
    try {
      target.second->onSchemaChanged(nextVersion, next->hash(), true);
    } catch (...) {
      metrics.add(metrics.observerFailures);
    }
  }
}

void DecoderService::Impl::scheduleFreshness() {
  if (deps.scheduler == nullptr) return;
  const auto scheduled = deps.scheduler->scheduleEvery(
      config.scanPeriod, tcu::scheduler::PeriodicMode::FixedDelay, [this] {
        const auto events = cache.scanFreshness(deps.now());
        dispatchEvents(events);
      });
  if (scheduled.error == tcu::scheduler::Error::None) {
    freshnessTimer = scheduled.id;
    freshnessScheduled = true;
  }
}

DecoderService::DecoderService(DecoderConfig config, DecoderServiceDeps deps)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(deps))) {}

DecoderService::~DecoderService() { stop(); }

StartResult DecoderService::start() {
  StartResult result;
  if (impl_->state.load() != ServiceState::Created) return result;
  impl_->state.store(ServiceState::Starting);

  const auto loaded = dbc::loadDbcFile(impl_->config.dbcPath, impl_->config.parseLimits());
  if (!loaded.ok()) {
    result.diagnostics = loaded.diagnostics;
    impl_->metrics.countLoad(loaded.firstWithCode(dbc::DbcErrorCode::DbcSource) != nullptr
                                 ? LoadResultLabel::SourceError
                                 : LoadResultLabel::Rejected);
    // NotReady publishes nothing and answers every query ConfigUnavailable: a
    // decoder with no schema must never produce a valid signal (V-05, B-01).
    impl_->state.store(ServiceState::NotReady);
    impl_->log.always(Severity::Error, "decoder_not_ready",
                      "the DBC file was rejected; no signal will be published");
    return result;
  }

  impl_->publish(loaded.db);
  impl_->metrics.countLoad(LoadResultLabel::Ok);
  impl_->metrics.activeVersion.store(loaded.db->version(), std::memory_order_relaxed);
  impl_->cache.seed(*loaded.db);
  result.unmatchedPolicyKeys = unmatchedPolicyKeys(impl_->config, *loaded.db);
  for (const auto& key : result.unmatchedPolicyKeys) {
    impl_->log.always(Severity::Warn, "policy_key_unmatched",
                      "a configured policy names no signal in this database: " + key);
  }

  impl_->running.store(true, std::memory_order_release);
  impl_->worker = std::thread([this] { impl_->workerLoop(); });
  if (impl_->deps.transport != nullptr) {
    impl_->reader = std::thread([this] { impl_->readerLoop(); });
  }
  impl_->scheduleFreshness();

  impl_->state.store(ServiceState::Ready);
  result.ready = true;
  result.version = loaded.db->version();
  result.hash = loaded.db->hash();
  impl_->log.always(Severity::Info, "decoder_ready",
                    "dbc_version=" + std::to_string(result.version) + " dbc_hash=" + result.hash +
                        " messages=" + std::to_string(loaded.db->stats().messages) +
                        " signals=" + std::to_string(loaded.db->stats().signals));
  return result;
}

StopReport DecoderService::stop(std::chrono::milliseconds deadline) {
  StopReport report;
  const ServiceState current = impl_->state.load();
  if (current == ServiceState::Stopped || current == ServiceState::Created) {
    // Idempotent: calling stop() twice, or before start(), is not an error
    // (invariant V-07).
    impl_->state.store(ServiceState::Stopped);
    return report;
  }
  impl_->state.store(ServiceState::Stopping);
  if (detail::testHooks().onStopRequested) detail::testHooks().onStopRequested();

  // The fixed order of invariant V-06. requestStop only wakes the reader; the
  // reader owns its descriptors and closes nothing underneath itself, which is
  // project 05's rule.
  impl_->queue.stopAccepting();
  impl_->running.store(false, std::memory_order_release);
  if (impl_->deps.transport != nullptr) impl_->deps.transport->requestStop();
  if (impl_->reader.joinable()) impl_->reader.join();

  if (impl_->freshnessScheduled && impl_->deps.scheduler != nullptr) {
    impl_->deps.scheduler->cancel(impl_->freshnessTimer);
    impl_->freshnessScheduled = false;
  }

  report.drained = impl_->queue.depth();
  impl_->queue.close();

  // Bounded: a blocked observer must not hold shutdown open forever. The worker
  // is cooperative - there is no safe way to force-terminate a C++ thread, so
  // an overrun is reported rather than papered over (spec section 10.6).
  if (impl_->worker.joinable()) {
    const auto start = tcu::scheduler::Clock::now();
    while (tcu::scheduler::Clock::now() - start < deadline) {
      if (impl_->queue.depth() == 0) break;
      std::this_thread::yield();
    }
    impl_->worker.join();
  }
  report.discarded = impl_->queue.discarded();
  report.complete = true;

  {
    std::lock_guard<std::mutex> lock(impl_->observerMutex);
    impl_->observers.clear();
  }
  impl_->state.store(ServiceState::Stopped);
  impl_->log.always(Severity::Info, "decoder_stopped",
                    "drained=" + std::to_string(report.drained) +
                        " discarded=" + std::to_string(report.discarded) +
                        " forced=" + (report.complete ? "false" : "true"));
  return report;
}

ServiceState DecoderService::state() const { return impl_->state.load(); }

SchemaIdentity DecoderService::schema() const {
  SchemaIdentity identity;
  const auto db = impl_->pin();
  if (db != nullptr) {
    identity.version = db->version();
    identity.hash = db->hash();
  }
  return identity;
}

SubscriptionId DecoderService::addObserver(std::weak_ptr<ISignalObserver> observer) {
  std::lock_guard<std::mutex> lock(impl_->observerMutex);
  const SubscriptionId id = impl_->nextSubscription++;
  impl_->observers.emplace_back(id, std::move(observer));
  return id;
}

void DecoderService::removeObserver(SubscriptionId id) {
  std::lock_guard<std::mutex> lock(impl_->observerMutex);
  for (auto it = impl_->observers.begin(); it != impl_->observers.end(); ++it) {
    if (it->first != id) continue;
    impl_->observers.erase(it);
    return;
  }
}

std::optional<SignalSnapshot> DecoderService::query(const SignalKey& key) const {
  if (impl_->state.load() == ServiceState::NotReady) {
    // No schema means no answer that could be mistaken for a reading (V-05).
    SignalSnapshot unavailable;
    unavailable.lastObservationQuality = dbc::SignalQuality::ConfigUnavailable;
    return unavailable;
  }
  return impl_->cache.get(key, impl_->deps.now());
}

std::vector<std::pair<SignalKey, SignalSnapshot>> DecoderService::queryAll() const {
  if (impl_->state.load() == ServiceState::NotReady) return {};
  return impl_->cache.snapshot(impl_->deps.now());
}

ReloadRequest DecoderService::requestReload() {
  if (!impl_->config.reload.enabled) return ReloadRequest::Disabled;
  if (impl_->state.load() != ServiceState::Ready) return ReloadRequest::Disabled;

  {
    std::lock_guard<std::mutex> lock(impl_->reloadMutex);
    // At most one in flight: a second request is rejected, never queued, so a
    // burst cannot build a backlog of reloads (invariant V-09).
    if (impl_->reloadPending) {
      impl_->metrics.countLoad(LoadResultLabel::Busy);
      return ReloadRequest::Busy;
    }
    const SteadyTime now = impl_->deps.now();
    while (!impl_->reloadAttempts.empty() &&
           now - impl_->reloadAttempts.front() >= std::chrono::minutes{1}) {
      impl_->reloadAttempts.pop_front();
    }
    if (impl_->reloadAttempts.size() >= impl_->config.reload.maxAttemptsPerMinute) {
      impl_->metrics.countLoad(LoadResultLabel::RateLimited);
      impl_->log.log(Severity::Warn, "reload_rejected", "rate_limited",
                     "too many reload attempts in one minute", now);
      return ReloadRequest::RateLimited;
    }
    impl_->reloadAttempts.push_back(now);
    impl_->reloadPending = true;
  }

  impl_->log.always(Severity::Info, "reload_requested", "trigger=api");

  if (impl_->deps.scheduler == nullptr) {
    // No scheduler: run it here rather than silently doing nothing. Only the
    // tests that construct a service without one reach this.
    impl_->runReload();
    return ReloadRequest::Accepted;
  }
  const auto scheduled = impl_->deps.scheduler->scheduleAfter(impl_->config.reload.debounce,
                                                              [this] { impl_->runReload(); });
  if (scheduled.error != tcu::scheduler::Error::None) {
    std::lock_guard<std::mutex> lock(impl_->reloadMutex);
    impl_->reloadPending = false;
    return ReloadRequest::Busy;
  }
  return ReloadRequest::Accepted;
}

DecoderMetricsSnapshot DecoderService::metrics() const {
  auto snapshot = impl_->metrics.snapshot();
  snapshot.queueDepth = impl_->queue.depth();
  snapshot.queueDrops = impl_->queue.dropped();
  snapshot.queueDiscarded = impl_->queue.discarded();
  snapshot.outOfOrder = impl_->cache.outOfOrderCount();
  snapshot.logsSuppressed = impl_->log.suppressedTotal();
  return snapshot;
}

}  // namespace tcu::signal
