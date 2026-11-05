#include "tcu/config/configuration_service.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "file_poller.hpp"
#include "loader.hpp"
#include "metrics.hpp"
#include "snapshot_diff.hpp"
#include "validator.hpp"

namespace tcu::config {
namespace {

using namespace std::chrono_literals;
namespace log = tcu::logging;

enum class State { Created, Running, Failed, Stopped };

}  // namespace

struct ConfigurationService::Impl : std::enable_shared_from_this<Impl> {
  ServiceOptions options;
  tcu::scheduler::TimerScheduler& scheduler;
  log::ILogger& logger;
  internal::Metrics metrics;

  // Serializes the whole pipeline. Held across file IO, parse and validate, and
  // released BEFORE any observer runs (spec section 14, invariant S4).
  std::mutex reload_mutex;

  // The single handover point. Readers do one atomic_load and get a complete
  // snapshot or the previous one, never a mix (AS-03, invariant S2).
  ConfigPtr current;

  std::atomic<State> state{State::Created};

  std::mutex registry_mutex;
  struct Entry {
    SubscriptionId id{0};
    ConfigObserver observer;
    bool alive{true};
  };
  std::vector<Entry> registry;
  SubscriptionId next_id{1};

  // Latched when a notification could not be queued, cleared when the resync it
  // owes has been handed to the scheduler (research R-06).
  std::atomic<bool> resync_pending{false};

  std::unique_ptr<internal::FilePoller> poller;

  Impl(ServiceOptions opts,
       tcu::scheduler::TimerScheduler& sched,
       log::ILogger& log_ref)
      : options(std::move(opts)), scheduler(sched), logger(log_ref) {}

  std::uint64_t currentGeneration() const {
    const ConfigPtr snapshot = std::atomic_load(&current);
    return snapshot == nullptr ? 0 : snapshot->generation;
  }

  ReloadResult rejection(LoadError error, std::string reason) {
    metrics.addFailure(error);
    logger.log(log::Severity::Warn, "config", "reload_rejected", "",
               {log::f("reason", std::string(loadErrorName(error))),
                log::f("field", reason)});
    ReloadResult result;
    result.error = error;
    result.generation = currentGeneration();
    result.changed = false;
    result.publicReason = std::move(reason);
    return result;
  }

  // Publishes a validated candidate and returns the generation it got.
  std::uint64_t publish(ConfigSnapshot&& candidate) {
    const std::uint64_t generation = currentGeneration() + 1;
    candidate.generation = generation;
    candidate.loadedAt = std::chrono::system_clock::now();
    auto published = std::make_shared<const ConfigSnapshot>(std::move(candidate));
    std::atomic_store(&current, ConfigPtr(published));
    metrics.setGeneration(generation);
    metrics.setConfigValid(true);
    return generation;
  }

  // shortcut: no coalescing - one notification per published generation, as
  // spec section 9 rule 7 requires for v1, so a reload storm enqueues one task
  // per generation until the bounded queue is full. Upgrade when section 9
  // rule 7 changes; the overflow path already degrades safely and owes a
  // resync, so the failure mode is bounded either way (research R-06).
  void notify(ConfigPtr snapshot, ConfigPtr previous) {
    const bool resync = resync_pending.exchange(false);
    if (resync) {
      // A resync says "re-read everything" rather than "here is the delta", and
      // a null previous is already the shape of the first notification, so an
      // observer that handles startup handles resync with no new branch.
      previous = nullptr;
    }

    // The task co-owns this state only while it runs. If the service is gone
    // by then, the lock fails and the task does nothing.
    std::weak_ptr<Impl> weak = weak_from_this();
    const auto scheduled =
        scheduler.scheduleAfter(0ms, [weak, snapshot, previous, resync] {
          if (const std::shared_ptr<Impl> self = weak.lock()) {
            self->deliver(snapshot, previous, resync);
          }
        });

    if (!scheduled) {
      // The bounded notification queue is full. The commit is NOT rolled back:
      // a committed generation is committed whether or not anyone was told
      // (spec section 13 test-oracle note). Health degrades and a resync is
      // owed instead.
      metrics.addNotifyFailure();
      metrics.latchNotifyDegraded();
      resync_pending.store(true);
      logger.log(log::Severity::Warn, "config", "notify_overflow", "",
                 {log::f("generation",
                         static_cast<std::int64_t>(
                             snapshot == nullptr ? 0 : snapshot->generation))});
    }
    metrics.setQueueDepth(scheduler.metrics().ready_queue_depth);
  }

  void deliver(ConfigPtr snapshot, ConfigPtr previous, bool resync) {
    std::vector<Entry> live;
    {
      std::lock_guard<std::mutex> lock(registry_mutex);
      for (const Entry& entry : registry) {
        if (entry.alive) {
          live.push_back(entry);
        }
      }
    }

    for (const Entry& entry : live) {
      // Re-checked here, so no callback STARTS after unsubscribe returned, even
      // though this batch was copied before it did (invariant N3, BC-12).
      if (!isAlive(entry.id)) {
        continue;
      }
      try {
        entry.observer(snapshot, previous);
      } catch (...) {
        // Isolated and counted, never swallowed (BC-11, invariant N2).
        metrics.addNotifyFailure();
        logger.log(log::Severity::Warn, "config", "observer_failed", "",
                   {log::f("subscription_id", static_cast<std::int64_t>(entry.id))});
      }
    }

    metrics.addNotifyDelivered();
    if (resync) {
      metrics.addResync();
      logger.log(log::Severity::Info, "config", "resync", "",
                 {log::f("generation",
                         static_cast<std::int64_t>(
                             snapshot == nullptr ? 0 : snapshot->generation))});
    }
    metrics.setQueueDepth(scheduler.metrics().ready_queue_depth);

    // An owed resync is attempted here rather than waiting for the next reload:
    // the queue has just made room, and a consumer that missed a generation
    // should not have to wait for an unrelated change to learn about it.
    if (resync_pending.load() && state.load() == State::Running) {
      notify(std::atomic_load(&current), nullptr);
    }
  }

  ReloadResult reload();

  bool isAlive(SubscriptionId id) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    for (const Entry& entry : registry) {
      if (entry.id == id) {
        return entry.alive;
      }
    }
    return false;
  }
};

ConfigurationService::ConfigurationService(ServiceOptions options,
                                           tcu::scheduler::TimerScheduler& scheduler,
                                           log::ILogger& logger)
    : impl_(std::make_shared<Impl>(std::move(options), scheduler, logger)) {}

ConfigurationService::~ConfigurationService() { stop(); }

LoadError ConfigurationService::start() {
  if (impl_->state.load() != State::Created) {
    return LoadError::ServiceStopped;
  }

  std::lock_guard<std::mutex> lock(impl_->reload_mutex);

  internal::LoadOutcome loaded =
      internal::loadFile(impl_->options.path, impl_->options.maxFileBytes);
  if (loaded.error != LoadError::None) {
    impl_->state.store(State::Failed);
    impl_->metrics.addFailure(loaded.error);
    impl_->metrics.setConfigValid(false);
    impl_->logger.log(log::Severity::Error, "config", "startup_failed", "",
                      {log::f("reason", std::string(loadErrorName(loaded.error))),
                       log::f("field", loaded.reason)});
    return loaded.error;
  }

  internal::ValidateOutcome validated =
      internal::validate(loaded.document, impl_->options.path.string());
  if (validated.error != LoadError::None) {
    impl_->state.store(State::Failed);
    impl_->metrics.addFailure(validated.error);
    impl_->metrics.setConfigValid(false);
    impl_->logger.log(log::Severity::Error, "config", "startup_failed", "",
                      {log::f("reason", std::string(loadErrorName(validated.error))),
                       log::f("field", validated.reason)});
    return validated.error;
  }

  const std::uint64_t generation = impl_->publish(std::move(validated.snapshot));
  impl_->state.store(State::Running);
  impl_->logger.log(log::Severity::Info, "config", "startup_loaded", "",
                    {log::f("generation", static_cast<std::int64_t>(generation))});

  if (impl_->options.pollInterval.count() > 0) {
    // Same weak handle as the notifier: a probe that fires while this service
    // is being destroyed must do nothing rather than reload a dead object.
    std::weak_ptr<Impl> weak = impl_;
    impl_->poller = std::make_unique<internal::FilePoller>(
        impl_->scheduler, impl_->options.path, impl_->options.pollInterval,
        [weak] {
          if (const std::shared_ptr<Impl> self = weak.lock()) {
            self->reload();
          }
        });
    impl_->poller->start();
  }

  return LoadError::None;
}

ConfigPtr ConfigurationService::snapshot() const noexcept {
  // One atomic load. No mutex, no file IO, no allocation beyond the pointer
  // copy, so a reader never waits for a reload (invariant S7).
  return std::atomic_load(&impl_->current);
}

ReloadResult ConfigurationService::Impl::reload() {
  if (state.load() != State::Running) {
    ReloadResult result;
    result.error = LoadError::ServiceStopped;
    result.generation = currentGeneration();
    result.publicReason = "service stopped";
    return result;
  }

  metrics.addAttempt();
  std::lock_guard<std::mutex> lock(reload_mutex);

  // Re-checked under the lock: stop() may have run while this call waited, and
  // a stopped service publishes nothing (invariant S5).
  if (state.load() != State::Running) {
    ReloadResult result;
    result.error = LoadError::ServiceStopped;
    result.generation = currentGeneration();
    result.publicReason = "service stopped";
    return result;
  }

  internal::LoadOutcome loaded =
      internal::loadFile(options.path, options.maxFileBytes);
  if (loaded.error != LoadError::None) {
    return rejection(loaded.error, std::move(loaded.reason));
  }

  internal::ValidateOutcome validated =
      internal::validate(loaded.document, options.path.string());
  if (validated.error != LoadError::None) {
    return rejection(validated.error, std::move(validated.reason));
  }

  const ConfigPtr previous = std::atomic_load(&current);

  // Compared before published: a semantically identical file is a success that
  // changes nothing, so the generation counts effective changes rather than
  // reload attempts (BC-06, invariant S9).
  if (previous != nullptr && previous->contentEquals(validated.snapshot)) {
    metrics.addSuccessNoop();
    logger.log(log::Severity::Info, "config", "reload_noop", "",
                      {log::f("generation",
                              static_cast<std::int64_t>(previous->generation))});
    ReloadResult result;
    result.generation = previous->generation;
    result.changed = false;
    return result;
  }

  if (previous != nullptr) {
    const internal::DiffResult difference =
        internal::diff(*previous, validated.snapshot);
    if (difference.restartRequired) {
      // The whole candidate goes, including the hot keys that moved in the same
      // file: publishing half of it would leave a generation whose content
      // matches no file on disk (BC-08, research R-09).
      return rejection(
          LoadError::RestartRequired,
          "restart-required key changed: " +
              std::string(difference.firstRestartPointer));
    }
  }

  const std::uint64_t generation = publish(std::move(validated.snapshot));
  metrics.addSuccessChanged();
  logger.log(log::Severity::Info, "config", "reload_committed", "",
                    {log::f("generation", static_cast<std::int64_t>(generation))});

  const ConfigPtr published = std::atomic_load(&current);
  notify(published, previous);

  ReloadResult result;
  result.generation = generation;
  result.changed = true;
  return result;
}

ReloadResult ConfigurationService::reload() { return impl_->reload(); }

SubscriptionId ConfigurationService::subscribe(ConfigObserver observer) {
  if (!observer) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(impl_->registry_mutex);
  const SubscriptionId id = impl_->next_id++;
  impl_->registry.push_back(Impl::Entry{id, std::move(observer), true});
  return id;
}

bool ConfigurationService::unsubscribe(SubscriptionId id) {
  std::lock_guard<std::mutex> lock(impl_->registry_mutex);
  for (Impl::Entry& entry : impl_->registry) {
    if (entry.id == id && entry.alive) {
      entry.alive = false;
      return true;
    }
  }
  return false;
}

ConfigMetrics ConfigurationService::metrics() const {
  ConfigMetrics out = impl_->metrics.snapshot();
  out.notificationQueueDepth = impl_->scheduler.metrics().ready_queue_depth;
  return out;
}

void ConfigurationService::stop() {
  const State previous = impl_->state.exchange(State::Stopped);
  if (previous == State::Stopped) {
    return;  // idempotent (BC-30, invariant S6)
  }

  // Shutdown order of spec section 11: stop the trigger, let an in-flight
  // reload finish, then drop the observers so none is called again and no
  // captured object is referenced after this returns.
  if (impl_->poller != nullptr) {
    impl_->poller->stop();
  }
  {
    std::lock_guard<std::mutex> lock(impl_->reload_mutex);
  }
  {
    std::lock_guard<std::mutex> lock(impl_->registry_mutex);
    impl_->registry.clear();
  }
}

}  // namespace tcu::config
