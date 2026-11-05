#pragma once

// The one Linux implementation of ICanTransport.
//
// This header declares it without naming a single Linux type: the UAPI lives
// behind a pimpl in src/socketcan_transport.cpp, which is what keeps every
// public header OS-neutral (NFR-10, research R-02).

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "tcu/can/can_transport.hpp"
#include "tcu/scheduler/clock_adapter.hpp"

namespace tcu::can {

/// Which step of the spec section 10.1 open sequence failed, and the errno it
/// failed with. Reported rather than thrown: a missing interface is an expected
/// condition the service answers with DEGRADED, not an exception.
struct OpenError {
  enum class Step { Validate, Socket, IfIndex, Filter, EventFd, Bind };
  Step step{Step::Validate};
  int os_error{0};
};

const char* openErrorStepName(OpenError::Step step) noexcept;

class SocketCanTransport final : public ICanTransport {
 public:
  struct Options {
    /// Must match vcan[0-9]* while the configuration is in lab_only mode; the
    /// loader has already enforced that before this is reached (research R-16).
    std::string interface_name;
    /// Identifiers to install as CAN_RAW_FILTER. Empty means no filter at all,
    /// which is the --broad-filter test mode B-12 needs.
    std::vector<std::uint32_t> rx_filter;
    /// The project clock. Stamps received_at, so a fake clock makes the
    /// freshness tests deterministic (research R-03).
    std::shared_ptr<tcu::scheduler::ClockAdapter> clock;
  };

  /// A factory, not a throwing constructor (spec section 9.2). Performs the
  /// section 10.1 sequence in order and leaks no descriptor on a partial
  /// failure.
  static std::variant<std::unique_ptr<SocketCanTransport>, OpenError> open(Options options);

  ~SocketCanTransport() override;

  SocketCanTransport(const SocketCanTransport&) = delete;
  SocketCanTransport& operator=(const SocketCanTransport&) = delete;

  ReadResult receive(std::chrono::milliseconds timeout) override;
  WriteStatus send(const CanMessage& msg) override;
  void requestStop() noexcept override;

  /// The interface this transport is bound to, for logs and diagnostics.
  const std::string& interfaceName() const noexcept;

 private:
  struct Impl;
  explicit SocketCanTransport(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace tcu::can
