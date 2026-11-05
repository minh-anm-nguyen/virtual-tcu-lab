#pragma once

// The size-rotating file sink of spec section 10.4.
//
// shortcut: rotation is size-based only - no time-based retention, no
// compression - and the rename chain runs on the writer thread, so a rotation
// briefly stalls writes. Producers never block, they enqueue, so the stall
// shows up as queue depth, which is already a metric. Upgrade when spec
// section 10.4 moves its v1 boundary (research R-13).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

#include "tcu/logging/log_sink.hpp"

namespace tcu::logging {

// The seam that lets a test inject ENOSPC, EACCES, a short write or EINTR
// without a real disk (research R-14, Constitution Principle IV). Only the
// three operations that can fail in an interesting way are virtual; rotation
// also removes and stats files, and those go straight to <filesystem> because
// no test needs them to fail.
class FileOps {
 public:
  virtual ~FileOps() = default;

  // Opens for append, creating the file if needed, and reports its size.
  virtual bool open(const std::filesystem::path& path,
                    std::uint64_t& size_out,
                    int& error_out) = 0;
  virtual bool write(std::string_view bytes, int& error_out) = 0;
  virtual void close() = 0;
  virtual bool rename(const std::filesystem::path& from,
                      const std::filesystem::path& to,
                      int& error_out) = 0;
};

std::unique_ptr<FileOps> makeDefaultFileOps();

struct FileSinkOptions {
  std::filesystem::path path{"logs/tcu.log"};
  std::size_t fileMaxBytes{10 * 1024 * 1024};
  std::size_t fileCount{5};
  // An unhealthy file sink still has somewhere to put the record. Section 10.4
  // calls this the configured fallback policy.
  bool fallbackToStderr{true};
  std::chrono::seconds recoveryProbeInterval{30};
};

class FileSink final : public ILogSink {
 public:
  explicit FileSink(FileSinkOptions options,
                    std::unique_ptr<FileOps> ops = nullptr);
  ~FileSink() override;

  // Opens the file and sets owner-only permissions. Returns false and leaves
  // the sink Unhealthy when the path cannot be used - which does NOT fail the
  // logger (BC-27, invariant F5).
  bool open();

  std::string_view name() const noexcept override {
    return sinkKindName(SinkKind::File);
  }
  SinkKind kind() const noexcept override { return SinkKind::File; }

  bool write(std::string_view line) override;
  void flush() override;
  SinkHealth health() const noexcept override;

  void periodic() override;
  std::chrono::steady_clock::duration nextPeriodicIn() const override;

  // Observability for the tests and for the rotations metric.
  std::uint64_t rotations() const;
  std::uint64_t currentSize() const;
  int lastError() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tcu::logging
