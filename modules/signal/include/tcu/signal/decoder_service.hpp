#pragma once

// The facade: one object that owns the RX queue, the single decoder worker, the
// quality cache and the reload coordinator, and hands out a quality-tagged view
// of the bus.
//
// Threading, in full (spec section 12, research R-15):
//
//   decoder worker   the ONE thread this project owns. Reads from the
//                    transport, pins the DBC snapshot once per frame, decodes,
//                    commits to the cache, then publishes with no lock held.
//   scheduler worker runs the freshness scan and the reload job - project 03's
//                    TimerScheduler, so this project adds no timer thread.
//   caller thread    start, stop, query, requestReload, metrics.
//
// Rule: no observer callback and no event publish happens under the cache
// mutex, the observer-registry mutex or the lifecycle mutex. Every publish is
// driven by a value a cache call already returned, after its lock was released
// (invariant S-07, spec section 12 rule 5).

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tcu/can/can_transport.hpp"
#include "tcu/dbc/dbc_parser.hpp"
#include "tcu/dbc/signal_decoder.hpp"
#include "tcu/logging/logger.hpp"
#include "tcu/scheduler/clock_adapter.hpp"
#include "tcu/scheduler/timer_scheduler.hpp"
#include "tcu/signal/decoder_config.hpp"
#include "tcu/signal/decoder_metrics.hpp"
#include "tcu/signal/signal_cache.hpp"

namespace tcu::signal {

class ISignalObserver {
 public:
  virtual ~ISignalObserver() = default;
  /// Called from the decoder worker, after the cache commit, with no lock held.
  virtual void onBatch(const dbc::DecodedBatch& batch) = 0;
  /// Called from the decoder worker or a scheduler worker, never under a lock.
  virtual void onQualityChanged(const SignalKey& key, dbc::SignalQuality quality) = 0;
  /// A reload committed or was rejected. `version` is 0 on a rejection.
  virtual void onSchemaChanged(std::uint64_t version, const std::string& hash, bool committed) {
    (void)version;
    (void)hash;
    (void)committed;
  }
};

enum class ServiceState { Created, Starting, Ready, NotReady, Stopping, Stopped };

const char* toString(ServiceState state) noexcept;

struct StartResult {
  bool ready{false};
  std::uint64_t version{0};
  std::string hash;
  /// Why the cold start failed, when it did.
  std::vector<dbc::ParseDiagnostic> diagnostics;
  std::vector<ConfigError> configErrors;
  /// Policy keys naming no signal in the loaded database (invariant V-16).
  std::vector<std::string> unmatchedPolicyKeys;
};

enum class ReloadRequest { Accepted, Busy, Disabled, RateLimited };

struct SchemaIdentity {
  std::uint64_t version{0};
  std::string hash;
};

struct StopReport {
  bool complete{true};
  std::uint64_t drained{0};
  std::uint64_t discarded{0};
};

struct DecoderServiceDeps {
  /// Project 05's transport. A FakeCanTransport here is how every test runs
  /// without vcan; a SocketCanTransport is the vcan0 path (research R-22).
  std::unique_ptr<tcu::can::ICanTransport> transport;
  /// Required: the freshness scan and the reload job run on it (research R-15).
  std::shared_ptr<tcu::scheduler::TimerScheduler> scheduler;
  /// Null disables logging; nothing else changes.
  std::shared_ptr<tcu::logging::ILogger> logger;
  /// Defaults to steady_clock. A fake clock makes every deadline deterministic.
  std::function<SteadyTime()> now;
  /// Which bus identifier to stamp on decoded batches. v1 loads one database.
  std::uint32_t busId{0};
};

using SubscriptionId = std::uint64_t;

class DecoderService {
 public:
  DecoderService(DecoderConfig config, DecoderServiceDeps deps);
  /// Calls stop(). Safe when start() was never called or failed.
  ~DecoderService();

  DecoderService(const DecoderService&) = delete;
  DecoderService& operator=(const DecoderService&) = delete;

  /// Loads the DBC, seeds the cache, starts the worker and the freshness scan.
  /// Ready only with a valid database: in NotReady every query answers
  /// ConfigUnavailable and no batch is ever published (invariant V-05, B-01).
  StartResult start();

  /// Idempotent, ordered, bounded. Never runs a callback after it returns
  /// (invariants V-06, V-07).
  StopReport stop(std::chrono::milliseconds deadline = std::chrono::milliseconds{2000});

  ServiceState state() const;
  SchemaIdentity schema() const;

  /// Stored as a weak_ptr, so a destroyed observer is skipped and unregistered
  /// rather than called (FR-16).
  SubscriptionId addObserver(std::weak_ptr<ISignalObserver> observer);
  void removeObserver(SubscriptionId id);

  /// Never blocks on the worker, on file I/O or on a reload.
  std::optional<SignalSnapshot> query(const SignalKey& key) const;
  std::vector<std::pair<SignalKey, SignalSnapshot>> queryAll() const;

  ReloadRequest requestReload();

  DecoderMetricsSnapshot metrics() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tcu::signal
