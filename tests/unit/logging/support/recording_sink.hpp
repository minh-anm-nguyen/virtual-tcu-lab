#pragma once

// A sink that keeps every line it was given, so a test can assert on output
// without touching a filesystem. It can also
//   - block inside write() until a TestGate opens, which is how a test fills a
//     bounded queue deterministically instead of racing the writer, and
//   - fail from the Nth write onward, which is how write-error accounting is
//     tested without a real disk (Constitution Principle IV).

#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "support/test_gate.hpp"
#include "tcu/logging/log_sink.hpp"

namespace tcu::logging::test {

class RecordingSink final : public ILogSink {
 public:
  explicit RecordingSink(SinkKind kind = SinkKind::Console) : kind_(kind) {}

  std::string_view name() const noexcept override { return sinkKindName(kind_); }
  SinkKind kind() const noexcept override { return kind_; }

  bool write(std::string_view line) override {
    tcu::test::TestGate* gate = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++write_calls_;
      if (fail_from_ != 0 && write_calls_ >= fail_from_) {
        health_ = SinkHealth::Unhealthy;
        return false;
      }
      lines_.emplace_back(line);
      gate = gate_;
    }
    // Outside the lock on purpose: a test that blocks the writer must still be
    // able to read lineCount() while it is blocked.
    if (gate != nullptr) {
      gate->wait_until_opened();
    }
    return true;
  }

  void flush() override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++flush_calls_;
  }

  SinkHealth health() const noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    return health_;
  }

  // Test controls.
  void blockOn(tcu::test::TestGate* gate) {
    std::lock_guard<std::mutex> lock(mutex_);
    gate_ = gate;
  }

  void failFromWrite(std::size_t n) {
    std::lock_guard<std::mutex> lock(mutex_);
    fail_from_ = n;
  }

  std::vector<std::string> lines() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lines_;
  }

  std::size_t lineCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lines_.size();
  }

  std::size_t writeCalls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return write_calls_;
  }

  std::size_t flushCalls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return flush_calls_;
  }

  bool containsSubstring(std::string_view needle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const std::string& line : lines_) {
      if (line.find(needle) != std::string::npos) {
        return true;
      }
    }
    return false;
  }

 private:
  mutable std::mutex mutex_;
  SinkKind kind_;
  std::vector<std::string> lines_;
  std::size_t write_calls_ = 0;
  std::size_t flush_calls_ = 0;
  std::size_t fail_from_ = 0;
  tcu::test::TestGate* gate_ = nullptr;
  SinkHealth health_ = SinkHealth::Healthy;
};

}  // namespace tcu::logging::test
