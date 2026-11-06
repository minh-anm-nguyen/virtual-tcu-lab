#pragma once

// contracts/metrics.md section 1. Relaxed atomics: metrics are observation,
// never synchronisation (spec section 12 rule 8).
//
// Label sets are closed enums, not strings, because a DBC is untrusted input
// with up to 10,000 signals - no DBC-supplied name may ever become a label
// (spec section 16.1 cardinality rule).

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "tcu/dbc/signal_quality.hpp"

namespace tcu::signal {

enum class FrameResult {
  Ok,
  UnknownId,
  WrongLength,
  WrongFormat,
  ErrorFrame,
  RemoteFrame,
  DecodeError,
  Count,
};

enum class LoadResultLabel { Ok, Rejected, Busy, RateLimited, SourceError, Count };

enum class QualityLabel {
  NotReceived,
  Valid,
  InvalidRaw,
  OutOfRange,
  Stale,
  DecodeError,
  NotActive,
  ConfigUnavailable,
  Count,
};

const char* toString(FrameResult result) noexcept;
const char* toString(LoadResultLabel result) noexcept;

/// A plain copy, for assertions and for the health surface.
struct DecoderMetricsSnapshot {
  std::array<std::uint64_t, static_cast<std::size_t>(FrameResult::Count)> frames{};
  std::array<std::uint64_t, static_cast<std::size_t>(LoadResultLabel::Count)> loadAttempts{};
  std::array<std::uint64_t, static_cast<std::size_t>(QualityLabel::Count)> signals{};
  std::uint64_t framesReceived{0};
  std::uint64_t queueDrops{0};
  std::uint64_t queueDiscarded{0};
  std::size_t queueDepth{0};
  std::uint64_t activeVersion{0};
  std::uint64_t batchesPublished{0};
  std::uint64_t batchesDroppedByPolicy{0};
  std::uint64_t staleTransitions{0};
  std::uint64_t recoveredTransitions{0};
  std::uint64_t unknownMux{0};
  std::uint64_t observerFailures{0};
  std::uint64_t outOfOrder{0};
  std::uint64_t eventPublishFailures{0};
  std::uint64_t schemaTransitions{0};
  std::uint64_t logsSuppressed{0};

  std::uint64_t totalFrames() const {
    std::uint64_t total = 0;
    for (const std::uint64_t value : frames) total += value;
    return total;
  }
};

class DecoderMetrics {
 public:
  void countFrame(FrameResult result) {
    frames_[static_cast<std::size_t>(result)].fetch_add(1, std::memory_order_relaxed);
  }
  void countLoad(LoadResultLabel result) {
    loadAttempts_[static_cast<std::size_t>(result)].fetch_add(1, std::memory_order_relaxed);
  }
  void countSignal(QualityLabel quality) {
    signals_[static_cast<std::size_t>(quality)].fetch_add(1, std::memory_order_relaxed);
  }
  void add(std::atomic<std::uint64_t>& counter, std::uint64_t delta = 1) {
    counter.fetch_add(delta, std::memory_order_relaxed);
  }

  std::atomic<std::uint64_t> framesReceived{0};
  std::atomic<std::uint64_t> batchesPublished{0};
  std::atomic<std::uint64_t> batchesDroppedByPolicy{0};
  std::atomic<std::uint64_t> staleTransitions{0};
  std::atomic<std::uint64_t> recoveredTransitions{0};
  std::atomic<std::uint64_t> unknownMux{0};
  std::atomic<std::uint64_t> observerFailures{0};
  std::atomic<std::uint64_t> eventPublishFailures{0};
  std::atomic<std::uint64_t> schemaTransitions{0};
  std::atomic<std::uint64_t> logsSuppressed{0};
  std::atomic<std::uint64_t> activeVersion{0};

  DecoderMetricsSnapshot snapshot() const;

 private:
  std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(FrameResult::Count)> frames_{};
  std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(LoadResultLabel::Count)>
      loadAttempts_{};
  std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(QualityLabel::Count)> signals_{};
};

QualityLabel toLabel(dbc::SignalQuality quality) noexcept;

}  // namespace tcu::signal
