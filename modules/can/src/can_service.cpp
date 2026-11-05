#include "tcu/can/can_service.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <thread>
#include <utility>
#include <variant>

#include "bounded_frame_queue.hpp"
#include "can_metrics_store.hpp"
#include "command_correlator.hpp"
#include "event_bus_adapter.hpp"
#include "tcu/can/demo_signal_decoder.hpp"
#include "tcu/can/frame_catalog.hpp"
#include "tcu/can/frame_validator.hpp"
#include "tcu/can/signal_cache.hpp"
#include "tcu/can/socketcan_transport.hpp"

namespace tcu::can {
namespace {

using namespace std::chrono_literals;

constexpr auto kReceivePollTimeout = 100ms;
constexpr auto kLogRateLimit = 1s;
constexpr const char* kComponent = "can_service";

const char* qualityName(SignalQuality quality) {
  switch (quality) {
    case SignalQuality::NeverReceived:   return "NEVER_RECEIVED";
    case SignalQuality::Fresh:           return "FRESH";
    case SignalQuality::Stale:           return "STALE";
    case SignalQuality::Invalid:         return "INVALID";
    case SignalQuality::LinkUnavailable: return "LINK_UNAVAILABLE";
  }
  return "NEVER_RECEIVED";
}

const char* stateName(ServiceState state) {
  switch (state) {
    case ServiceState::Stopped:  return "STOPPED";
    case ServiceState::Starting: return "STARTING";
    case ServiceState::Running:  return "RUNNING";
    case ServiceState::Degraded: return "DEGRADED";
    case ServiceState::Stopping: return "STOPPING";
  }
  return "STOPPED";
}

const char* reasonName(RejectReason reason) {
  switch (reason) {
    case RejectReason::None:            return "none";
    case RejectReason::Flags:           return "flags";
    case RejectReason::Length:          return "length";
    case RejectReason::UnknownId:       return "unknown_id";
    case RejectReason::Range:           return "range";
    case RejectReason::ReservedNonzero: return "reserved_nonzero";
  }
  return "none";
}

std::string hexId(std::uint32_t id) {
  std::ostringstream out;
  out << "0x" << std::hex << id;
  return out.str();
}

}  // namespace

struct CanService::Impl {
  explicit Impl(CanServiceDeps incoming) : deps(std::move(incoming)) {
    if (!deps.clock) {
      deps.clock = std::make_shared<tcu::scheduler::SteadyClockAdapter>();
    }
  }

  CanServiceDeps deps;
  CanLabConfig config;

  CanMetricsStore metrics;

  std::unique_ptr<ICanTransport> transport;
  /// True when the service opened the socket itself and can therefore reopen
  /// it. False for an injected transport (a FakeCanTransport in tests), which
  /// the service must not try to recreate.
  bool owns_socket{false};

  std::unique_ptr<BoundedFrameQueue> queue;
  std::unique_ptr<SignalCache> cache;
  std::unique_ptr<EventBusAdapter> events;
  FrameValidator validator;
  DemoSignalDecoder decoder;
  CommandCorrelator correlator;

  std::shared_ptr<tcu::scheduler::TimerScheduler> scheduler;
  bool owns_scheduler{false};
  tcu::scheduler::TimerId health_timer{0};

  std::thread rx_thread;
  std::thread decode_thread;

  mutable std::mutex lifecycle;
  std::atomic<ServiceState> state{ServiceState::Stopped};

  /// The stop signal the RX thread waits on while degraded. The transport's
  /// eventfd cannot serve there: it belongs to a socket that is being replaced,
  /// and polling a broken socket spins (research R-18).
  std::mutex stop_mutex;
  std::condition_variable stop_cv;
  bool stop_requested{false};

  std::atomic<std::thread::id> rx_id{};
  std::atomic<std::thread::id> decode_id{};

  // Rate limiters. An overload condition must not turn the logger into the
  // bottleneck (spec sections 10.3 and 13.7), so the counter rises per event
  // while the log line is emitted at most once per second per cause.
  //
  // "Has it fired yet" is a separate flag rather than a sentinel timestamp: a
  // FakeClockAdapter starts at the epoch, so SteadyTime{} is a perfectly real
  // reading and using it as "never" made the limiter pass everything through.
  // Caught by Service.DropLogIsRateLimited.
  struct RateLimit {
    bool fired{false};
    SteadyTime at{};
  };
  std::mutex log_mutex;
  RateLimit last_drop_log;
  RateLimit last_reject_log;
  RateLimit last_reserved_log;

  std::mt19937 jitter_rng{std::random_device{}()};

  SteadyTime now() const { return deps.clock->now(); }

  bool stopping() const {
    const auto current = state.load();
    return current == ServiceState::Stopping || current == ServiceState::Stopped;
  }

  void log(tcu::logging::Severity level, const char* event, const char* message,
           std::vector<tcu::logging::Field> fields = {}) {
    if (!deps.logger || !deps.logger->enabled(level)) {
      return;
    }
    deps.logger->log(level, kComponent, event, message, std::move(fields));
  }

  /// True when the given limiter has not fired within the last second. Takes
  /// the log mutex only; never called with any other lock held.
  bool rateLimit(RateLimit& last) {
    const SteadyTime stamp = now();
    std::lock_guard<std::mutex> lock(log_mutex);
    if (last.fired && stamp - last.at < kLogRateLimit) {
      return false;
    }
    last.fired = true;
    last.at = stamp;
    return true;
  }

  void publishTransitionsAndCount(const std::vector<QualityTransition>& transitions) {
    for (const auto& transition : transitions) {
      switch (transition.to) {
        case SignalQuality::Stale:
          CanMetricsStore::bump(metrics.signal_stale_transitions_total);
          log(tcu::logging::Severity::Warn, "signal.stale", "signal deadline exceeded",
              {tcu::logging::f("name", transition.signal_name),
               tcu::logging::f("quality", qualityName(transition.to))});
          break;
        case SignalQuality::Fresh:
          // Only a transition INTO Fresh from something else reaches here: a
          // fresh-to-fresh update is not a transition at all.
          CanMetricsStore::bump(metrics.signal_recovered_total);
          log(tcu::logging::Severity::Info, "signal.recovered", "signal is fresh again",
              {tcu::logging::f("name", transition.signal_name),
               tcu::logging::f("generation", static_cast<std::int64_t>(transition.generation))});
          break;
        case SignalQuality::Invalid:
          CanMetricsStore::bump(metrics.signal_invalid_transitions_total);
          break;
        case SignalQuality::NeverReceived:
        case SignalQuality::LinkUnavailable:
          break;
      }
    }
    // After the cache lock was released, never under it (spec section 17.1
    // rule 7). The transitions are already a plain value by this point.
    if (events) {
      events->publishTransitions(transitions);
    }
  }

  void countReject(RejectReason reason, std::uint32_t can_id) {
    switch (reason) {
      case RejectReason::Flags:
        CanMetricsStore::bump(metrics.reject_flags_total);
        CanMetricsStore::bump(metrics.rx_invalid_total);
        break;
      case RejectReason::Length:
        CanMetricsStore::bump(metrics.reject_length_total);
        CanMetricsStore::bump(metrics.rx_invalid_total);
        break;
      case RejectReason::Range:
        CanMetricsStore::bump(metrics.reject_range_total);
        CanMetricsStore::bump(metrics.rx_invalid_total);
        break;
      case RejectReason::UnknownId:
        // Deliberately NOT an invalid frame: an unknown identifier is traffic
        // the filter let through, not a malformed frame, and an operator needs
        // to tell those apart (contracts/metrics.md, B-12).
        CanMetricsStore::bump(metrics.unknown_id_total);
        break;
      case RejectReason::ReservedNonzero:
      case RejectReason::None:
        break;
    }

    if (reason != RejectReason::None && rateLimit(last_reject_log)) {
      log(tcu::logging::Severity::Warn, "frame.rejected", "frame rejected before decode",
          {tcu::logging::f("can_id", hexId(can_id)),
           tcu::logging::f("reason", reasonName(reason))});
    }
  }

  /// Records the rejection against every signal the identifier carries, so each
  /// one reads back Invalid while keeping its last good value (spec 13.4).
  void recordRejectAgainstSignals(std::uint32_t can_id, SteadyTime at) {
    for (const auto& signal : kSignals) {
      if (signal.source_id == can_id) {
        cache->recordError(signal.name, RejectReason::Range, at);
      }
    }
  }

  void handleAck(const CanMessage& frame) {
    const std::uint8_t sequence = frame.data[0];
    const std::uint8_t result = frame.data[1];

    if (correlator.onAck(sequence, result) == AckOutcome::Matched) {
      CanMetricsStore::bump(metrics.ack_matched_total);
      log(tcu::logging::Severity::Info, "command.acked", "command acknowledged",
          {tcu::logging::f("sequence", static_cast<std::int64_t>(sequence)),
           tcu::logging::f("result", static_cast<std::int64_t>(result))});
    } else {
      // An acknowledgment for a sequence nobody awaits: a duplicate, or another
      // requester on the bus. Counted and ignored, and any pending request is
      // left pending (B-23).
      CanMetricsStore::bump(metrics.ack_unmatched_total);
    }
  }

  void processFrame(const CanMessage& frame) {
    const auto validation = validator.validate(frame);

    if (validation.reserved_nonzero) {
      CanMetricsStore::bump(metrics.reserved_nonzero_total);
      if (rateLimit(last_reserved_log)) {
        log(tcu::logging::Severity::Warn, "frame.reserved_nonzero",
            "reserved bytes were not zero", {tcu::logging::f("can_id", hexId(frame.id))});
      }
    }

    if (!validation.ok) {
      countReject(validation.reason, frame.id);
      if (validation.reason == RejectReason::Length) {
        recordRejectAgainstSignals(frame.id, frame.received_at);
        publishTransitionsAndCount(cache->refresh(now()));
      }
      return;
    }

    // The acknowledgment is correlator business, not cache business: it carries
    // no signal, so the decoder would only reject it (research R-20).
    if (frame.id == 0x201) {
      handleAck(frame);
      return;
    }

    const auto decoded = decoder.decode(frame);
    if (!decoded.accepted) {
      countReject(decoded.reason, frame.id);
      if (decoded.reason == RejectReason::Range) {
        recordRejectAgainstSignals(frame.id, frame.received_at);
        publishTransitionsAndCount(cache->refresh(now()));
      }
      return;
    }

    CanMetricsStore::bump(metrics.decoded_total);

    for (const auto& update : decoded.updates) {
      if (!cache->apply(update)) {
        continue;
      }
      CanMetricsStore::bump(metrics.signal_update_total);

      // Read back rather than echoing the decoder's sample: the cache owns the
      // generation and the computed quality, and a consumer must see those
      // rather than what the decoder guessed.
      const auto committed = cache->get(update.signal_name);
      if (!committed || !committed->value.has_value()) {
        continue;
      }
      if (events) {
        events->publishValue(*committed);
      }
      if (deps.logger && deps.logger->enabled(tcu::logging::Severity::Info)) {
        deps.logger->log(
            tcu::logging::Severity::Info, kComponent, "signal.updated", "signal updated",
            {tcu::logging::f("name", committed->signal_name),
             tcu::logging::f("can_id", hexId(committed->source_id)),
             tcu::logging::f("quality", qualityName(committed->quality)),
             tcu::logging::f("generation", static_cast<std::int64_t>(committed->generation)),
             // The physical value is vehicle data, so it carries its class and
             // project 04's formatter decides whether to write it (research
             // R-05, Constitution Principle V).
             tcu::logging::Field{"value", *committed->value,
                                 tcu::logging::DataClass::VehicleSensitive}});
      }
    }

    // One refresh for the whole frame, so a two-signal frame cannot report the
    // same transition twice.
    publishTransitionsAndCount(cache->refresh(now()));

    metrics.recordDecodeLatency(now() - frame.received_at);
  }

  void decodeLoop() {
    decode_id.store(std::this_thread::get_id());
    while (const auto frame = queue->pop()) {
      processFrame(*frame);
    }
  }

  /// Waits out a backoff interval, returning early the moment stop() is called.
  /// Not a busy loop, which is what spec section 13.6 actually requires.
  void waitBackoff(std::chrono::milliseconds interval) {
    std::unique_lock<std::mutex> lock(stop_mutex);
    stop_cv.wait_for(lock, interval, [this] { return stop_requested; });
  }

  std::chrono::milliseconds withJitter(std::chrono::milliseconds base) {
    if (config.reconnect_jitter_pct <= 0) {
      return base;
    }
    const auto span = base.count() * config.reconnect_jitter_pct / 100;
    if (span <= 0) {
      return base;
    }
    std::uniform_int_distribution<std::int64_t> dist(-span, span);
    const auto jittered = base.count() + dist(jitter_rng);
    return std::chrono::milliseconds{jittered < 1 ? 1 : jittered};
  }

  /// Tears the socket down and builds a new one. Only meaningful when the
  /// service opened it: an injected transport belongs to the test, and
  /// recreating it would throw away the script under test.
  bool reopenTransport() {
    if (!owns_socket) {
      return true;
    }

    SocketCanTransport::Options options;
    options.interface_name = config.interface_name;
    options.clock = deps.clock;
    if (!deps.broad_filter) {
      options.rx_filter.assign(kTcuRxFilterIds.begin(), kTcuRxFilterIds.end());
    }

    auto opened = SocketCanTransport::open(std::move(options));
    if (std::holds_alternative<OpenError>(opened)) {
      return false;
    }
    // The old transport is destroyed only once the new one exists, and only on
    // this thread - the single owner of the descriptors (spec 17.1 rule 1).
    transport = std::move(std::get<std::unique_ptr<SocketCanTransport>>(opened));
    return true;
  }

  void enterDegraded(int os_error) {
    state.store(ServiceState::Degraded);
    CanMetricsStore::bump(metrics.link_error_total);
    publishTransitionsAndCount(cache->setTransportAvailable(false));
    if (events) {
      events->publishLinkStatus(false, ServiceState::Degraded);
    }
    log(tcu::logging::Severity::Warn, "can_service.link_unavailable",
        "transport is unavailable",
        {tcu::logging::f("interface", config.interface_name),
         tcu::logging::f("state", stateName(ServiceState::Degraded)),
         tcu::logging::f("os_error", static_cast<std::int64_t>(os_error))});
  }

  void enterRunningAfterRecovery(std::chrono::milliseconds backoff) {
    // Incremented after a successful reopen AND bind, never after an attempt,
    // so a retry storm cannot look like a string of recoveries
    // (contracts/metrics.md).
    CanMetricsStore::bump(metrics.link_reconnect_total);
    state.store(ServiceState::Running);
    // Signals stay non-fresh until a new valid frame arrives: this only clears
    // the LinkUnavailable override (B-20).
    publishTransitionsAndCount(cache->setTransportAvailable(true));
    if (events) {
      events->publishLinkStatus(true, ServiceState::Running);
    }
    log(tcu::logging::Severity::Info, "can_service.link_restored", "transport is back",
        {tcu::logging::f("interface", config.interface_name),
         tcu::logging::f("state", stateName(ServiceState::Running)),
         tcu::logging::f("backoff_ms", static_cast<std::int64_t>(backoff.count()))});
  }

  /// The degrade-and-retry loop. Lives on the RX thread because spec section
  /// 17.1 rule 1 makes the adapter the sole owner of the descriptors: handing a
  /// reopen to a scheduler worker would create the close/use race section 24
  /// lists as a crash risk (research R-18).
  void degradeAndRetry(int os_error) {
    enterDegraded(os_error);

    auto backoff = config.reconnect_initial;
    while (!stopping()) {
      waitBackoff(withJitter(backoff));
      if (stopping()) {
        return;
      }

      if (reopenTransport()) {
        enterRunningAfterRecovery(backoff);
        return;
      }

      backoff = std::min(backoff * 2, config.reconnect_max);
    }
  }

  void rxLoop() {
    rx_id.store(std::this_thread::get_id());

    while (!stopping()) {
      auto result = transport->receive(kReceivePollTimeout);

      switch (result.status) {
        case ReadStatus::Received: {
          CanMetricsStore::bump(metrics.rx_read_total);
          if (queue->push(*result.message)) {
            CanMetricsStore::bump(metrics.rx_enqueued_total);
          } else {
            // DropNewest. The reader never blocks, and every intentional drop
            // is counted (ADR-04, NFR-08).
            CanMetricsStore::bump(metrics.rx_queue_drop_total);
            if (rateLimit(last_drop_log)) {
              log(tcu::logging::Severity::Warn, "queue.dropped",
                  "ingress queue full, newest frame dropped",
                  {tcu::logging::f("can_id", hexId(result.message->id)),
                   tcu::logging::f("depth", static_cast<std::int64_t>(queue->depth())),
                   tcu::logging::f("capacity", static_cast<std::int64_t>(queue->capacity()))});
            }
          }
          break;
        }

        case ReadStatus::Timeout:
          // An empty bus is normal, not an error.
          break;

        case ReadStatus::Interrupted:
          // Expected on shutdown; on a signal it is simply a retry.
          break;

        case ReadStatus::LinkError:
        case ReadStatus::FatalError:
          degradeAndRetry(result.os_error);
          break;
      }
    }
  }

  void onHealthTick() {
    // The freshness sweep. FixedDelay means this callback can never overlap
    // itself, which is what spec section 25 asks of exactly this job.
    publishTransitionsAndCount(cache->refresh(now()));

    if (correlator.expireIfTimedOut(now(), deps.command_timeout)) {
      CanMetricsStore::bump(metrics.ack_timeout_total);
    }
  }
};

CanService::CanService(CanServiceDeps deps) : impl_(std::make_unique<Impl>(std::move(deps))) {}

CanService::~CanService() { stop(); }

bool CanService::start(const CanLabConfig& config) {
  std::lock_guard<std::mutex> lifecycle(impl_->lifecycle);

  if (impl_->state.load() != ServiceState::Stopped) {
    // B-27: V1 rejects rather than being idempotent, so a second start cannot
    // create a second pair of threads on one socket.
    return false;
  }

  // Validated before anything exists, so a bad configuration leaves no socket
  // and no thread behind (spec section 13.1 step 2). The loader has normally
  // checked all of this; the service does not assume it was used.
  if (config.rx_queue_capacity == 0 || config.health_check_period <= 0ms ||
      config.interface_name.empty() || config.reconnect_initial <= 0ms ||
      config.reconnect_initial > config.reconnect_max) {
    return false;
  }

  impl_->config = config;
  impl_->state.store(ServiceState::Starting);
  {
    std::lock_guard<std::mutex> lock(impl_->stop_mutex);
    impl_->stop_requested = false;
  }

  impl_->cache = std::make_unique<SignalCache>(config.signals, impl_->deps.clock);
  impl_->queue = std::make_unique<BoundedFrameQueue>(config.rx_queue_capacity);
  impl_->events =
      std::make_unique<EventBusAdapter>(impl_->deps.bus, impl_->deps.clock, impl_->metrics);

  impl_->transport = std::move(impl_->deps.transport);
  impl_->owns_socket = impl_->transport == nullptr;

  bool degraded_at_start = false;
  if (impl_->owns_socket && !impl_->reopenTransport()) {
    // A missing interface is a transient condition, not a failure to start: the
    // retry loop picks it up when the interface appears (spec 12.1, ADR-06).
    degraded_at_start = true;
  }

  impl_->scheduler = impl_->deps.scheduler;
  if (!impl_->scheduler) {
    tcu::scheduler::Config scheduler_config;
    scheduler_config.workerThreads = 1;
    impl_->scheduler =
        std::make_shared<tcu::scheduler::TimerScheduler>(scheduler_config, impl_->deps.clock);
    impl_->owns_scheduler = true;
    if (impl_->scheduler->start() != tcu::scheduler::Error::None) {
      impl_->scheduler.reset();
      impl_->transport.reset();
      impl_->state.store(ServiceState::Stopped);
      return false;
    }
  }

  // FixedDelay, not FixedRate: its contract is "next deadline = completion time
  // + interval. Drifts, never overlaps", and spec section 25 requires that this
  // job never overlap itself (research R-04).
  const auto scheduled =
      impl_->scheduler->scheduleEvery(config.health_check_period,
                                      tcu::scheduler::PeriodicMode::FixedDelay,
                                      [impl = impl_.get()] { impl->onHealthTick(); });
  if (!scheduled) {
    if (impl_->owns_scheduler) {
      impl_->scheduler->stop(tcu::scheduler::StopMode::Discard);
      impl_->owns_scheduler = false;
    }
    impl_->scheduler.reset();
    impl_->transport.reset();
    impl_->state.store(ServiceState::Stopped);
    return false;
  }
  impl_->health_timer = scheduled.id;

  impl_->state.store(ServiceState::Running);

  impl_->decode_thread = std::thread([impl = impl_.get()] { impl->decodeLoop(); });
  if (impl_->transport) {
    impl_->rx_thread = std::thread([impl = impl_.get()] { impl->rxLoop(); });
  }

  if (degraded_at_start) {
    impl_->enterDegraded(0);
  }

  impl_->log(tcu::logging::Severity::Info, "can_service.started", "service started",
             {tcu::logging::f("interface", config.interface_name),
              tcu::logging::f("state", stateName(impl_->state.load())),
              tcu::logging::f("rx_queue_capacity",
                              static_cast<std::int64_t>(config.rx_queue_capacity))});

  // Every signal reads NeverReceived with no value at this point: startup must
  // not report ignition OFF or speed zero before a valid frame (section 13.1).
  return true;
}

void CanService::stop() noexcept {
  // Calling stop() from inside a callback this service invoked would self-join.
  // Prohibited by spec section 9.4; asserted here so a debug build catches it
  // rather than hanging.
  assert(std::this_thread::get_id() != impl_->rx_id.load() &&
         std::this_thread::get_id() != impl_->decode_id.load() &&
         "stop() must not be called from a service worker thread");

  std::lock_guard<std::mutex> lifecycle(impl_->lifecycle);

  if (impl_->state.load() == ServiceState::Stopped) {
    return;  // Idempotent, and no descriptor is closed twice (B-26).
  }

  // 1. Refuse new work.
  impl_->state.store(ServiceState::Stopping);

  // 2. Wake the reader, whether it is in poll() or in a backoff wait.
  {
    std::lock_guard<std::mutex> lock(impl_->stop_mutex);
    impl_->stop_requested = true;
  }
  impl_->stop_cv.notify_all();
  if (impl_->transport) {
    impl_->transport->requestStop();
  }

  // 3. Join the reader: after this nothing can enqueue.
  if (impl_->rx_thread.joinable()) {
    impl_->rx_thread.join();
  }

  // 4. Drain what is queued, up to the deadline, and count what is left. A
  //    callback must never be able to block stop() forever, which a deadline is
  //    the only way to guarantee (spec section 13.8 step 4, research R-19).
  if (impl_->queue) {
    const SteadyTime deadline = impl_->now() + impl_->config.drain_deadline;
    while (impl_->now() < deadline) {
      const auto frame = impl_->queue->tryPop();
      if (!frame) {
        break;
      }
      impl_->processFrame(*frame);
    }

    std::uint64_t discarded = 0;
    while (impl_->queue->tryPop()) {
      ++discarded;
    }
    if (discarded > 0) {
      CanMetricsStore::bump(impl_->metrics.shutdown_discarded_total, discarded);
    }

    // 5. Close the queue so the decode worker's pop() returns and it exits.
    impl_->queue->close();
  }

  if (impl_->decode_thread.joinable()) {
    impl_->decode_thread.join();
  }

  // 6. Cancel the timer, then stop the scheduler if we own it. Drain so a tick
  //    already running finishes rather than being abandoned mid-callback.
  if (impl_->scheduler) {
    if (impl_->health_timer != 0) {
      impl_->scheduler->cancel(impl_->health_timer);
      impl_->health_timer = 0;
    }
    if (impl_->owns_scheduler) {
      impl_->scheduler->stop(tcu::scheduler::StopMode::Drain);
    }
    impl_->scheduler.reset();
    impl_->owns_scheduler = false;
  }

  // 7. Release in ownership order: the event adapter first, because a publish
  //    must not outlive the bus handle; then the queue; then the transport,
  //    whose eventfd has now outlived the thread that polled it (17.1 rule 6).
  const auto discarded = impl_->metrics.shutdown_discarded_total.load();
  impl_->events.reset();
  impl_->queue.reset();
  if (impl_->owns_socket) {
    // Our socket, our descriptors to close.
    impl_->transport.reset();
  } else {
    // An injected transport belongs to the caller's lifetime, not to one
    // start/stop cycle, so it is handed back rather than destroyed - otherwise
    // a restart would silently fall through to opening a real socket, and a
    // test would be exercising something other than what it injected.
    impl_->deps.transport = std::move(impl_->transport);
  }
  impl_->owns_socket = false;

  impl_->state.store(ServiceState::Stopped);
  impl_->log(tcu::logging::Severity::Info, "can_service.stopped", "service stopped",
             {tcu::logging::f("state", stateName(ServiceState::Stopped)),
              tcu::logging::f("discarded_frames", static_cast<std::int64_t>(discarded))});
}

ServiceState CanService::state() const noexcept { return impl_->state.load(); }

std::optional<SignalSample> CanService::readSignal(std::string_view name) const {
  if (!impl_->cache) {
    return std::nullopt;
  }
  return impl_->cache->get(name);
}

std::vector<SignalSample> CanService::readAll() const {
  if (!impl_->cache) {
    return {};
  }
  return impl_->cache->snapshot();
}

WriteStatus CanService::sendDemoCommand(std::uint8_t command, std::uint8_t sequence) {
  if (command != kCommandLock && command != kCommandUnlock) {
    return WriteStatus::InvalidFrame;
  }
  if (impl_->state.load() != ServiceState::Running || !impl_->transport) {
    // B-21. Degraded counts as unavailable: there is no bound socket to write
    // to, and queueing the command would make it arrive at an unknown time.
    return WriteStatus::LinkUnavailable;
  }
  if (!impl_->correlator.beginCommand(command, sequence, impl_->now())) {
    // V1 allows one command in flight, which is what removes the 8-bit sequence
    // wrap ambiguity entirely (research R-20).
    return WriteStatus::InvalidFrame;
  }

  CanMessage frame;
  frame.id = 0x200;
  frame.length = 8;
  frame.data = {command, sequence, 0, 0, 0, 0, 0, 0};

  CanMetricsStore::bump(impl_->metrics.tx_attempt_total);
  const auto status = impl_->transport->send(frame);
  if (status == WriteStatus::Sent) {
    CanMetricsStore::bump(impl_->metrics.tx_success_total);
    impl_->log(tcu::logging::Severity::Info, "command.sent", "command sent",
               {tcu::logging::f("command", static_cast<std::int64_t>(command)),
                tcu::logging::f("sequence", static_cast<std::int64_t>(sequence))});
  } else {
    // The frame never reached the bus, so the slot must not stay reserved.
    impl_->correlator.abandonCommand();
  }
  return status;
}

CanMetricsSnapshot CanService::metrics() const noexcept {
  auto snapshot = impl_->metrics.snapshot();
  if (impl_->queue) {
    snapshot.can_queue_depth = impl_->queue->depth();
    snapshot.can_queue_capacity = impl_->queue->capacity();
  }
  return snapshot;
}

std::string CanService::healthJson() const {
  // Hand-built rather than routed through nlohmann: this is a presentation of a
  // handful of fields (spec section 16.2 calls it "an example presentation, not
  // a schema guaranteed by an existing implementation"), and keeping the writer
  // here lets the JSON library stay a private detail of the config loader.
  const auto snapshot = readAll();
  const SteadyTime stamp = impl_->now();

  std::ostringstream out;
  out << "{\n";
  out << "  \"service_state\": \"" << stateName(impl_->state.load()) << "\",\n";
  out << "  \"interface\": \"" << impl_->config.interface_name << "\",\n";
  out << "  \"rx_queue_depth\": " << (impl_->queue ? impl_->queue->depth() : 0) << ",\n";
  out << "  \"rx_queue_capacity\": " << (impl_->queue ? impl_->queue->capacity() : 0) << ",\n";
  out << "  \"signals\": {";

  bool first = true;
  for (const auto& sample : snapshot) {
    out << (first ? "\n" : ",\n");
    first = false;
    out << "    \"" << sample.signal_name << "\": { ";
    // null exactly when nothing was ever accepted - never a zero standing in
    // for a missing value (spec section 6.4, NFR-07).
    if (sample.value.has_value()) {
      out << "\"value\": " << *sample.value << ", ";
    } else {
      out << "\"value\": null, ";
    }
    out << "\"quality\": \"" << qualityName(sample.quality) << "\"";
    if (sample.value.has_value()) {
      const auto age =
          std::chrono::duration_cast<std::chrono::milliseconds>(stamp - sample.last_good_at)
              .count();
      out << ", \"source_can_id\": \"" << hexId(sample.source_id) << "\"";
      out << ", \"age_ms\": " << age;
    }
    out << " }";
  }

  out << (first ? "" : "\n") << "  }\n}";
  return out.str();
}

}  // namespace tcu::can
