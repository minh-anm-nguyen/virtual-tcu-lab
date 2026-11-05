#pragma once

// A sink is touched by the writer thread only (spec section 14), so an
// implementation needs no internal locking.

#include <chrono>
#include <cstddef>
#include <string_view>

namespace tcu::logging {

enum class SinkHealth { Healthy, Unhealthy };

// The closed set of sinks v1 ships. An enum rather than a string lookup,
// because every per-sink metric is an array indexed by it (invariant M4): a
// label cannot be invented at runtime.
enum class SinkKind { Console, File };

inline constexpr std::size_t kSinkCount = 2;

inline std::size_t sinkIndex(SinkKind kind) noexcept {
  return static_cast<std::size_t>(kind);
}

inline std::string_view sinkKindName(SinkKind kind) noexcept {
  return kind == SinkKind::Console ? "console" : "file";
}

class ILogSink {
 public:
  virtual ~ILogSink() = default;

  // One of the closed set: console, file.
  virtual std::string_view name() const noexcept = 0;

  // Index into the per-sink metric arrays.
  virtual SinkKind kind() const noexcept = 0;

  // Writes one complete JSONL line. The line carries no trailing newline; the
  // sink adds its own record separator. Returns false instead of throwing, so
  // the writer counts the failure and the application never sees it.
  virtual bool write(std::string_view line) = 0;

  virtual void flush() = 0;

  virtual SinkHealth health() const noexcept = 0;

  // Called by the writer thread whenever its wait expires, so a sink can do
  // bounded periodic work - the recovery probe of spec section 12 scenario F -
  // without a thread of its own (research R-14). Default: nothing.
  virtual void periodic() {}

  // When the sink wants to be called back sooner than the writer idle wait.
  // Zero means no preference.
  virtual std::chrono::steady_clock::duration nextPeriodicIn() const {
    return std::chrono::steady_clock::duration::zero();
  }
};

}  // namespace tcu::logging
