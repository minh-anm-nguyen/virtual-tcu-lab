#pragma once

// The interface every consumer depends on. Keeping it abstract is what lets
// tcu_config take a logger by reference without knowing about queues, sinks or
// threads, and what lets a test pass a recording double.

#include <string_view>
#include <vector>

#include "tcu/logging/log_types.hpp"

namespace tcu::logging {

class ILogger {
 public:
  virtual ~ILogger() = default;

  // Never throws, never blocks unboundedly, never fails the caller. A logging
  // failure is counted, not reported: spec section 3 states that failure of
  // optional logging cannot crash business logic.
  virtual void log(Severity level,
                   std::string_view component,
                   std::string_view event,
                   std::string_view message) noexcept = 0;

  // Same contract. Fields are taken by value because the record must own every
  // byte it carries (invariant L1).
  virtual void log(Severity level,
                   std::string_view component,
                   std::string_view event,
                   std::string_view message,
                   std::vector<Field> fields) noexcept = 0;

  // True when a record at this level would be admitted. Guard any field vector
  // that costs something to build: spec section 10.2 asks for exactly this.
  virtual bool enabled(Severity level) const noexcept = 0;

  // Blocks until records queued at the time of the call have reached the
  // sinks. Must not be called from a sink callback or the writer thread; doing
  // so returns immediately rather than self-deadlocking.
  virtual void flush() = 0;

  // Stops admission, drains within the configured deadline, closes the sinks
  // and joins the writer thread. Idempotent.
  virtual void stop() = 0;
};

}  // namespace tcu::logging
