#pragma once

// Per-reason log rate limiting.
//
// Spec sections 10.3 and 16.1 require sampled logs for unknown identifiers and
// repeated parse errors: a poisoned bus can otherwise turn one malformed frame
// per millisecond into a disk-filling log. The bucket lives here rather than
// inside tcu_logging because this is a decoder policy, not a logger feature -
// adding it to a finished library for one consumer would be the wrong place
// (research R-17).
//
// Nothing disappears silently: a line emitted after suppression carries how
// many were dropped since the last one.

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "tcu/logging/logger.hpp"
#include "tcu/signal/signal_cache.hpp"

namespace tcu::signal::detail {

class RateLimitedLog {
 public:
  RateLimitedLog(std::shared_ptr<tcu::logging::ILogger> logger, std::uint32_t perReasonPerMinute)
      : logger_(std::move(logger)), limit_(perReasonPerMinute) {}

  /// Emits at most `limit_` lines per (event, reason) per minute. Returns true
  /// when the line was emitted, false when it was suppressed and counted.
  bool log(tcu::logging::Severity severity, const char* event, const std::string& reason,
           const std::string& message, SteadyTime now) {
    if (logger_ == nullptr) return false;

    std::uint64_t suppressed = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      Bucket& bucket = buckets_[std::string(event) + "/" + reason];
      if (now - bucket.windowStart >= std::chrono::minutes{1}) {
        bucket.windowStart = now;
        bucket.emitted = 0;
      }
      if (limit_ != 0 && bucket.emitted >= limit_) {
        ++bucket.suppressed;
        ++suppressedTotal_;
        return false;
      }
      ++bucket.emitted;
      suppressed = bucket.suppressed;
      bucket.suppressed = 0;
    }

    std::string text = message;
    if (suppressed > 0) {
      text += " (suppressed_since_last=" + std::to_string(suppressed) + ")";
    }
    logger_->log(severity, "dbc_decoder", event, text);
    return true;
  }

  /// Not rate limited: lifecycle and reload outcomes are rare and must always
  /// be visible.
  void always(tcu::logging::Severity severity, const char* event, const std::string& message) {
    if (logger_ == nullptr) return;
    logger_->log(severity, "dbc_decoder", event, message);
  }

  std::uint64_t suppressedTotal() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return suppressedTotal_;
  }

 private:
  struct Bucket {
    SteadyTime windowStart{};
    std::uint32_t emitted{0};
    std::uint64_t suppressed{0};
  };

  mutable std::mutex mutex_;
  std::shared_ptr<tcu::logging::ILogger> logger_;
  std::uint32_t limit_;
  std::map<std::string, Bucket> buckets_;
  std::uint64_t suppressedTotal_{0};
};

}  // namespace tcu::signal::detail
