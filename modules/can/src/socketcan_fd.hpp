#pragma once

// A move-only owner for one file descriptor.
//
// NFR-04 asks for RAII over every descriptor and NFR-11 forbids a double
// close. Both come from one place here, so no error path in the transport has
// to remember to close - and spec section 24 lists "close/read concurrency on
// FDs" as a crash risk precisely because that remembering is where it goes
// wrong.

#include <unistd.h>

#include <utility>

namespace tcu::can {

class SocketFd {
 public:
  SocketFd() = default;
  explicit SocketFd(int fd) : fd_(fd) {}

  ~SocketFd() { reset(); }

  SocketFd(const SocketFd&) = delete;
  SocketFd& operator=(const SocketFd&) = delete;

  SocketFd(SocketFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

  SocketFd& operator=(SocketFd&& other) noexcept {
    if (this != &other) {
      reset();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  int get() const noexcept { return fd_; }
  bool valid() const noexcept { return fd_ >= 0; }

  void reset() noexcept {
    if (fd_ >= 0) {
      // The return value is deliberately ignored: a failing close on a
      // descriptor being destroyed leaves nothing to do, and retrying risks
      // closing a descriptor another thread has already been handed.
      ::close(fd_);
      fd_ = -1;
    }
  }

  /// Hands ownership out. Used only where a descriptor must outlive this
  /// object, so a leak here is a visible transfer rather than a missing close.
  int release() noexcept { return std::exchange(fd_, -1); }

 private:
  int fd_{-1};
};

}  // namespace tcu::can
