#pragma once

// RAII ownership of exactly one AF_UNIX file descriptor.
//
// Spec section 7 "Ownership": every FD is owned by exactly one UnixSocket, so
// a double close is a type error rather than a review item. Move-only for the
// same reason.
//
// Linux only. Section 8.3 requires MSG_NOSIGNAL so a dead peer cannot kill the
// process with SIGPIPE, and EINTR retried while EAGAIN is reported to the
// caller so the poll loop can wait for readiness instead of spinning.

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include <cstddef>
#include <string>

namespace tcu::ipc::detail {

struct PeerCredentials {
  uid_t uid{0};
  gid_t gid{0};
  pid_t pid{0};
};

class UnixSocket {
 public:
  UnixSocket() = default;
  explicit UnixSocket(int fd) noexcept : fd_(fd) {}

  ~UnixSocket() { close(); }

  UnixSocket(UnixSocket&& other) noexcept : fd_(other.fd_), last_errno_(other.last_errno_) {
    other.fd_ = -1;
  }

  UnixSocket& operator=(UnixSocket&& other) noexcept {
    if (this != &other) {
      close();
      fd_ = other.fd_;
      last_errno_ = other.last_errno_;
      other.fd_ = -1;
    }
    return *this;
  }

  UnixSocket(const UnixSocket&) = delete;
  UnixSocket& operator=(const UnixSocket&) = delete;

  bool valid() const noexcept { return fd_ >= 0; }
  int fd() const noexcept { return fd_; }
  int lastErrno() const noexcept { return last_errno_; }

  /// Creates the socket. Returns false with lastErrno() set.
  bool open();

  /// 0 on success, -1 with lastErrno() set. The path must fit sun_path and must
  /// not be on a DrvFs mount (research R-00 note 2).
  int bindPath(const std::string& path);
  int connectPath(const std::string& path);
  int listen(int backlog);

  /// An invalid result means "nothing to accept"; check lastErrno() for EAGAIN.
  UnixSocket accept();

  bool setNonBlocking();

  /// A negative result with lastErrno() EAGAIN/EWOULDBLOCK means "not ready".
  /// 0 from recvSome means the peer performed an orderly shutdown.
  ssize_t recvSome(void* buffer, std::size_t length);
  ssize_t sendSome(const void* buffer, std::size_t length);

  /// SO_PEERCRED, the local trust boundary of spec section 8.8.
  bool peerCredentials(PeerCredentials& out);

  void close();

  /// Releases ownership without closing, for handing the FD to another owner.
  int release() noexcept {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }

 private:
  bool fillAddress(const std::string& path, sockaddr_un& addr);

  int fd_{-1};
  int last_errno_{0};
};

}  // namespace tcu::ipc::detail
