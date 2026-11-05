#pragma once

// The facade: one object that owns the transport, the queue, the two worker
// threads, the cache, the correlator and the metrics, and hands out a
// quality-tagged view of the vehicle.
//
// Threading, in full (plan.md threading table, spec section 17.1):
//
//   RX reader thread     owns the socket and the eventfd. Reads, stamps,
//                        enqueues, and performs the degrade-and-retry backoff.
//                        Never decodes, never invokes a callback.
//   Decode worker thread pops, validates, decodes, commits to the cache and
//                        publishes. Never touches a descriptor.
//   Scheduler worker     runs the freshness tick (project 03, FixedDelay).
//   Caller thread        start, stop, reads, sendDemoCommand, metrics.
//
// Rule 7 of section 17.1 is absolute: no callback and no event publish happens
// under the socket, queue, cache or lifecycle mutex. Every publish here is
// driven by a value a cache call already returned, after its lock was released.

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tcu/can/can_config.hpp"
#include "tcu/can/can_metrics.hpp"
#include "tcu/can/can_transport.hpp"
#include "tcu/can/can_types.hpp"
#include "tcu/eventbus/event_bus.hpp"
#include "tcu/logging/logger.hpp"
#include "tcu/scheduler/clock_adapter.hpp"
#include "tcu/scheduler/timer_scheduler.hpp"

namespace tcu::can {

struct CanServiceDeps {
  /// Defaults to a SteadyClockAdapter. Pass a FakeClockAdapter to make every
  /// deadline in the service deterministic (research R-03).
  std::shared_ptr<tcu::scheduler::ClockAdapter> clock;
  /// Null disables logging. Nothing else changes.
  std::shared_ptr<tcu::logging::ILogger> logger;
  /// Null disables event publishing (research R-07).
  std::shared_ptr<tcu::eventbus::EventBus> bus;
  /// Null means "open a SocketCanTransport for the configured interface".
  /// A FakeCanTransport here is how every unit test runs without vcan.
  std::unique_ptr<ICanTransport> transport;
  /// Null means the service creates and owns one.
  std::shared_ptr<tcu::scheduler::TimerScheduler> scheduler;
  /// Installs no CAN_RAW_FILTER, so unknown identifiers reach the application.
  /// Exists only for B-12; the filtered path is the default (FR-04).
  bool broad_filter{false};
  /// How long a 0x200 waits for its 0x201 before the request is abandoned.
  std::chrono::milliseconds command_timeout{1000};
};

class CanService {
 public:
  explicit CanService(CanServiceDeps deps);
  /// Calls stop(). Safe when start() was never called or already failed.
  ~CanService();

  CanService(const CanService&) = delete;
  CanService& operator=(const CanService&) = delete;

  /// Validates the configuration BEFORE creating any socket or thread, so an
  /// invalid file leaves nothing behind (spec section 13.1 step 2).
  ///
  /// true  - Running, or Degraded and retrying a transient open failure.
  /// false - Stopped: the configuration was rejected, or the service was
  ///         already Running (B-27: V1 rejects rather than being idempotent).
  bool start(const CanLabConfig& config);

  /// Idempotent, noexcept. Must NOT be called from inside a callback this
  /// service invoked - a debug assert catches it (spec section 9.4).
  void stop() noexcept;

  ServiceState state() const noexcept;

  /// nullopt for a name outside the catalog. A known signal that has never been
  /// received returns a sample with quality NeverReceived and no value, and the
  /// two answers are deliberately different (B-02).
  std::optional<SignalSample> readSignal(std::string_view name) const;
  std::vector<SignalSample> readAll() const;

  /// Sends a synthetic 0x200. `Sent` means the local socket accepted the frame
  /// - never that the BCM acted on it (spec section 10.2). Only a matching
  /// 0x201 proves that.
  WriteStatus sendDemoCommand(std::uint8_t command, std::uint8_t sequence);

  CanMetricsSnapshot metrics() const noexcept;

  /// The spec section 16.2 surface. A presentation, not a guaranteed schema,
  /// and it never contains raw frame bytes.
  std::string healthJson() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tcu::can
