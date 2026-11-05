#pragma once

// The configuration service. Pre and postconditions for every member are in
// contracts/config_api.md section 3; the short version is here.

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>

#include "tcu/config/config_metrics.hpp"
#include "tcu/config/config_types.hpp"
#include "tcu/logging/logger.hpp"
#include "tcu/scheduler/timer_scheduler.hpp"

namespace tcu::config {

struct ServiceOptions {
  std::filesystem::path path;
  // Spec section 9 rule 1: oversize input is rejected before it is parsed.
  std::size_t maxFileBytes{1024 * 1024};
  // Change detection interval. 0 disables polling entirely, leaving explicit
  // reload() as the only trigger - which is what every unit test uses, and the
  // deterministic path spec section 23 prefers (research R-03).
  std::chrono::milliseconds pollInterval{500};
};

class IConfigurationService {
 public:
  virtual ~IConfigurationService() = default;

  // One atomic read. Never blocks on file IO, parsing, validation or a reload
  // mutex. Returns null only before the first successful publish.
  virtual ConfigPtr snapshot() const noexcept = 0;

  // Serialized by the reload mutex. Performs file IO on the calling thread.
  // Success means the snapshot is committed, NOT that observers have applied it
  // (spec section 13 test-oracle note).
  virtual ReloadResult reload() = 0;

  // The observer is called for every generation published AFTER this call, not
  // for the one already in force.
  virtual SubscriptionId subscribe(ConfigObserver observer) = 0;

  // After this returns, no callback for that id will start. One already running
  // may finish (BC-12).
  virtual bool unsubscribe(SubscriptionId id) = 0;
};

class ConfigurationService final : public IConfigurationService {
 public:
  // The caller owns the scheduler and the logger, and both must outlive the
  // service. The scheduler MUST be started before start(), MUST be configured
  // with workerThreads == 1, and MUST be stopped AFTER this service.
  //
  // The one-worker requirement is what makes notification delivery ordered: a
  // single worker draining a FIFO ready queue gives publication order for free
  // (research R-02, invariant N1). It is a contract, not a runtime check.
  ConfigurationService(ServiceOptions options,
                       tcu::scheduler::TimerScheduler& scheduler,
                       tcu::logging::ILogger& logger);
  ~ConfigurationService() override;

  ConfigurationService(const ConfigurationService&) = delete;
  ConfigurationService& operator=(const ConfigurationService&) = delete;

  // Loads, validates and publishes generation 1, or leaves the service Failed
  // with no snapshot. Never starts on implicit defaults (BC-02).
  LoadError start();

  // Cancels the poller, waits for an in-flight reload, clears the registry.
  // Idempotent; safe from any thread; safe from inside an observer callback.
  // Does not stop the scheduler or the logger - the caller owns both.
  void stop();

  ConfigMetrics metrics() const;

  ConfigPtr snapshot() const noexcept override;
  ReloadResult reload() override;
  SubscriptionId subscribe(ConfigObserver observer) override;
  bool unsubscribe(SubscriptionId id) override;

 private:
  struct Impl;
  // shared_ptr, not unique_ptr: a notification task or a poller probe already
  // handed to the scheduler can still be waiting to run when this object is
  // destroyed. Those tasks hold a weak reference and co-own the state only
  // while they execute, so a late task becomes inert instead of dangling -
  // which is what ASan caught when they captured a raw pointer.
  std::shared_ptr<Impl> impl_;
};

}  // namespace tcu::config
