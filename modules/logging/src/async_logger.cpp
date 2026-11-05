#include "tcu/logging/async_logger.hpp"

#include <atomic>
#include <cstdio>
#include <deque>
#include <thread>
#include <utility>

#include "json_formatter.hpp"
#include "log_queue.hpp"
#include "metrics.hpp"
#include "redaction.hpp"

namespace tcu::logging {
namespace {

// Set only on the writer thread. This is what makes a sink that logs take the
// emergency path instead of re-entering the queue it is draining (BC-26, Q4),
// and what makes flush() and stop() from inside a sink callback safe (L6).
thread_local bool t_in_writer = false;

constexpr std::chrono::milliseconds kIdleWait{50};

}  // namespace

struct AsyncLogger::Impl {
  LoggerOptions options;
  std::vector<std::shared_ptr<ILogSink>> sinks;
  internal::LogQueue queue;
  internal::Metrics metrics;

  std::atomic<Severity> min_level;
  std::atomic<bool> console_enabled;
  std::atomic<bool> started{false};
  std::atomic<bool> stopping{false};
  std::atomic<bool> stopped{false};
  std::thread writer;

  Impl(LoggerOptions opts, std::vector<std::shared_ptr<ILogSink>> s)
      : options(std::move(opts)),
        sinks(std::move(s)),
        queue(options.queueCapacity),
        min_level(options.minLevel),
        console_enabled(options.consoleEnabled) {
    if (!options.clock) {
      options.clock = [] { return std::chrono::system_clock::now(); };
    }
  }

  // Pre-opened stderr, best effort, no queue: the path for FATAL and for a
  // reentrant call (spec section 10.3).
  void emergencyWrite(const LogRecord& record) {
    const internal::FormatResult formatted =
        internal::formatRecord(record, options.maxRecordBytes);
    std::fwrite(formatted.line.data(), 1, formatted.line.size(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
  }

  void writeBatch(std::deque<LogRecord>& batch) {
    for (const LogRecord& record : batch) {
      const internal::FormatResult formatted =
          internal::formatRecord(record, options.maxRecordBytes);
      if (formatted.truncated) {
        metrics.addTruncated();
      }
      if (formatted.failed) {
        metrics.addFormatFailure();
        continue;
      }

      for (const std::shared_ptr<ILogSink>& sink : sinks) {
        if (sink == nullptr) {
          continue;
        }
        const SinkKind kind = sink->kind();
        if (kind == SinkKind::Console &&
            !console_enabled.load(std::memory_order_relaxed)) {
          continue;
        }

        const auto started_at = std::chrono::steady_clock::now();
        bool ok = false;
        try {
          ok = sink->write(formatted.line);
        } catch (...) {
          // A sink that throws is a sink error, never an exception travelling
          // into a business thread (invariant L9).
          ok = false;
        }
        metrics.recordWriteLatency(std::chrono::steady_clock::now() - started_at);
        if (ok) {
          metrics.addWritten(kind);
        } else {
          metrics.addSinkError(kind);
        }
        metrics.setSinkHealth(kind, sink->health());
      }
    }
    queue.markProcessed(batch.size());
    metrics.setQueueDepth(queue.depth());
  }

  // The wait is shortened to the earliest sink that wants a periodic slot, so
  // recovery probing costs no thread and no spin (research R-14, invariant F6).
  std::chrono::milliseconds waitBudget() const {
    std::chrono::milliseconds budget = kIdleWait;
    for (const std::shared_ptr<ILogSink>& sink : sinks) {
      if (sink == nullptr) {
        continue;
      }
      const auto wanted = sink->nextPeriodicIn();
      if (wanted <= std::chrono::steady_clock::duration::zero()) {
        continue;
      }
      const auto wanted_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(wanted);
      if (wanted_ms < budget) {
        budget = wanted_ms;
      }
    }
    return budget;
  }

  void runSinkPeriodic() {
    for (const std::shared_ptr<ILogSink>& sink : sinks) {
      if (sink == nullptr) {
        continue;
      }
      try {
        sink->periodic();
      } catch (...) {
        metrics.addSinkError(sink->kind());
      }
      metrics.setSinkHealth(sink->kind(), sink->health());
    }
  }

  void writerLoop() {
    t_in_writer = true;

    while (true) {
      std::deque<LogRecord> batch = queue.drain(waitBudget());
      if (!batch.empty()) {
        writeBatch(batch);
      }
      runSinkPeriodic();

      if (stopping.load(std::memory_order_acquire)) {
        // Bounded shutdown drain (BC-23, invariant L7): keep writing what is
        // already queued until it is gone or the deadline passes. Whatever is
        // left is reported, never silently lost.
        const auto until =
            std::chrono::steady_clock::now() + options.shutdownDeadline;
        while (std::chrono::steady_clock::now() < until) {
          std::deque<LogRecord> rest = queue.drainNow();
          if (rest.empty()) {
            break;
          }
          writeBatch(rest);
        }
        break;
      }
    }

    for (const std::shared_ptr<ILogSink>& sink : sinks) {
      if (sink != nullptr) {
        try {
          sink->flush();
        } catch (...) {
          metrics.addSinkError(sink->kind());
        }
      }
    }
  }

  void submit(Severity level,
              std::string_view component,
              std::string_view event,
              std::string_view message,
              std::vector<Field> fields) {
    metrics.addSubmitted();

    if (stopped.load(std::memory_order_acquire)) {
      metrics.addRejectedAfterStop();
      return;
    }

    // Step one: the level check, before anything is copied or formatted
    // (BC-16). One relaxed atomic load is the whole cost of a filtered call.
    if (level < min_level.load(std::memory_order_relaxed)) {
      metrics.addFiltered();
      return;
    }

    // Step two: redact, then take owning copies. The record owns every byte it
    // carries, so nothing points into a caller buffer (invariant L1).
    internal::redactInPlace(fields);
    LogRecord record;
    record.timestampUtc = options.clock();
    record.timestampMono = std::chrono::steady_clock::now();
    record.severity = level;
    record.component.assign(component);
    record.eventName.assign(event);
    record.message.assign(message);
    record.fields = std::move(fields);

    // Step three: the reentrancy guard. A sink that logs lands here.
    if (t_in_writer) {
      metrics.addReentrantEmergency();
      emergencyWrite(record);
      return;
    }

    // FATAL gets a best-effort synchronous line first, because the process may
    // not live long enough for the writer thread to reach it.
    if (level == Severity::Fatal) {
      metrics.addFatalEmergency();
      emergencyWrite(record);
    }

    const std::chrono::milliseconds budget =
        (level >= Severity::Warn) ? options.highPriorityWaitMs
                                  : std::chrono::milliseconds{0};
    if (queue.push(std::move(record), budget)) {
      metrics.addEnqueued();
      metrics.setQueueDepth(queue.depth());
    } else {
      metrics.addDropped(level);
      if (level >= Severity::Warn) {
        metrics.addHighPriorityDrop();
      }
    }
  }
};

AsyncLogger::AsyncLogger(LoggerOptions options,
                         std::vector<std::shared_ptr<ILogSink>> sinks)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(sinks))) {}

AsyncLogger::~AsyncLogger() { stop(); }

bool AsyncLogger::start() {
  if (impl_->started.load() || impl_->stopped.load()) {
    return false;
  }
  impl_->started.store(true, std::memory_order_release);
  impl_->writer = std::thread([this] { impl_->writerLoop(); });
  return true;
}

void AsyncLogger::setLevel(Severity level) {
  impl_->min_level.store(level, std::memory_order_relaxed);
}

void AsyncLogger::setConsoleEnabled(bool enabled) {
  impl_->console_enabled.store(enabled, std::memory_order_relaxed);
}

LoggerMetrics AsyncLogger::metrics() const {
  LoggerMetrics out = impl_->metrics.snapshot();
  out.queueDepth = impl_->queue.depth();
  return out;
}

void AsyncLogger::log(Severity level,
                      std::string_view component,
                      std::string_view event,
                      std::string_view message) noexcept {
  try {
    impl_->submit(level, component, event, message, {});
  } catch (...) {
    // Allocation failure while building the record. Counted, never thrown.
    impl_->metrics.addFormatFailure();
  }
}

void AsyncLogger::log(Severity level,
                      std::string_view component,
                      std::string_view event,
                      std::string_view message,
                      std::vector<Field> fields) noexcept {
  try {
    impl_->submit(level, component, event, message, std::move(fields));
  } catch (...) {
    impl_->metrics.addFormatFailure();
  }
}

bool AsyncLogger::enabled(Severity level) const noexcept {
  return level >= impl_->min_level.load(std::memory_order_relaxed);
}

void AsyncLogger::flush() {
  if (t_in_writer) {
    return;  // waiting for itself would never finish
  }
  if (!impl_->started.load(std::memory_order_acquire) ||
      impl_->stopped.load(std::memory_order_acquire)) {
    return;
  }
  const std::uint64_t target = impl_->queue.enqueuedCount();
  impl_->queue.wake();
  impl_->queue.waitProcessed(target, impl_->options.shutdownDeadline);
  for (const std::shared_ptr<ILogSink>& sink : impl_->sinks) {
    if (sink != nullptr) {
      try {
        sink->flush();
      } catch (...) {
        impl_->metrics.addSinkError(sink->kind());
      }
    }
  }
}

void AsyncLogger::stop() {
  if (t_in_writer) {
    // Called from inside a sink callback. Closing admission is safe and useful;
    // joining would be a self-join, so the writer finishes its own loop and the
    // real join happens in whichever thread called stop() from outside.
    impl_->stopping.store(true, std::memory_order_release);
    impl_->queue.closeAdmission();
    return;
  }
  if (impl_->stopped.exchange(true)) {
    return;  // idempotent (BC-30)
  }
  impl_->stopping.store(true, std::memory_order_release);
  impl_->queue.closeAdmission();
  impl_->queue.wake();
  if (impl_->writer.joinable()) {
    impl_->writer.join();
  }
  const std::size_t left = impl_->queue.depth();
  if (left > 0) {
    impl_->metrics.addNotDrainedAtStop(static_cast<std::uint64_t>(left));
  }
  impl_->metrics.setQueueDepth(impl_->queue.depth());
}

}  // namespace tcu::logging
