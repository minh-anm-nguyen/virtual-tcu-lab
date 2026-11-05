#pragma once

// The fault seam of research R-14: ENOSPC, EACCES, a short write and EINTR, on
// demand, with no real disk involved (Constitution Principle IV). When no fault
// is armed every call is delegated to the real implementation, so rotation and
// permission behaviour are exercised against actual files.

#include <cerrno>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "tcu/logging/file_sink.hpp"

namespace tcu::logging::test {

class FaultyFileOps final : public FileOps {
 public:
  FaultyFileOps() : inner_(makeDefaultFileOps()) {}

  bool open(const std::filesystem::path& path,
            std::uint64_t& size_out,
            int& error_out) override {
    std::size_t remaining_failures = 0;
    int code = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++open_calls_;
      remaining_failures = failing_opens_;
      code = open_error_;
      if (failing_opens_ > 0) {
        --failing_opens_;
      }
    }
    if (remaining_failures > 0) {
      error_out = code;
      return false;
    }
    return inner_->open(path, size_out, error_out);
  }

  bool write(std::string_view bytes, int& error_out) override {
    int code = 0;
    bool fail = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++write_calls_;
      if (eintr_remaining_ > 0) {
        --eintr_remaining_;
        error_out = EINTR;
        return false;
      }
      if (fail_write_from_ != 0 && write_calls_ >= fail_write_from_) {
        fail = true;
        code = write_error_;
      }
    }
    if (fail) {
      error_out = code;
      return false;
    }
    return inner_->write(bytes, error_out);
  }

  void close() override { inner_->close(); }

  bool rename(const std::filesystem::path& from,
              const std::filesystem::path& to,
              int& error_out) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++rename_calls_;
      if (fail_rename_) {
        error_out = EACCES;
        return false;
      }
    }
    return inner_->rename(from, to, error_out);
  }

  // Controls.
  void failNextOpens(std::size_t count, int error) {
    std::lock_guard<std::mutex> lock(mutex_);
    failing_opens_ = count;
    open_error_ = error;
  }
  void failWriteFrom(std::size_t nth_call, int error) {
    std::lock_guard<std::mutex> lock(mutex_);
    fail_write_from_ = nth_call;
    write_error_ = error;
  }
  void interruptNextWrites(std::size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    eintr_remaining_ = count;
  }
  void failRenames(bool fail) {
    std::lock_guard<std::mutex> lock(mutex_);
    fail_rename_ = fail;
  }

  std::size_t openCalls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return open_calls_;
  }
  std::size_t writeCalls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return write_calls_;
  }
  std::size_t renameCalls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rename_calls_;
  }

 private:
  mutable std::mutex mutex_;
  std::unique_ptr<FileOps> inner_;
  std::size_t open_calls_ = 0;
  std::size_t write_calls_ = 0;
  std::size_t rename_calls_ = 0;
  std::size_t failing_opens_ = 0;
  std::size_t fail_write_from_ = 0;
  std::size_t eintr_remaining_ = 0;
  bool fail_rename_ = false;
  int open_error_ = EACCES;
  int write_error_ = ENOSPC;
};

}  // namespace tcu::logging::test
