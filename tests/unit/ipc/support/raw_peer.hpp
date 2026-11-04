#pragma once

// A raw AF_UNIX peer that owns its FD and writes exactly the bytes a test
// chooses. It is how the fault cases of spec section 10 are driven without a
// Python helper (research R-13).

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>

namespace tcu::ipc::test {

class RawPeer {
 public:
  RawPeer() = default;

  ~RawPeer() { close(); }

  RawPeer(const RawPeer&) = delete;
  RawPeer& operator=(const RawPeer&) = delete;

  // Returns false with errno set, so a test can assert the failure mode.
  bool connect(const std::string& socket_path) {
    close();
    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
      return false;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) {
      errno = ENAMETOOLONG;
      return false;
    }
    std::memcpy(addr.sun_path, socket_path.c_str(), socket_path.size());
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      close();
      return false;
    }
    return true;
  }

  // Writes every byte, retrying short writes and EINTR.
  bool writeBytes(const std::string& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
      ssize_t n = ::send(fd_, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
      sent += static_cast<std::size_t>(n);
    }
    return true;
  }

  // One byte per send(), so the receiver sees a frame split as far as it can be.
  bool writeByteByByte(const std::string& bytes) {
    for (char c : bytes) {
      if (!writeBytes(std::string(1, c))) {
        return false;
      }
    }
    return true;
  }

  // Reads whatever has arrived within the deadline. An empty result means the
  // deadline passed; isClosed() distinguishes a peer that hung up.
  std::string readAvailable(std::chrono::milliseconds timeout) {
    std::string out;
    pollfd p{fd_, POLLIN, 0};
    int rc = ::poll(&p, 1, static_cast<int>(timeout.count()));
    if (rc <= 0) {
      return out;
    }
    char buf[4096];
    ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
    if (n == 0) {
      closed_by_peer_ = true;
      return out;
    }
    if (n > 0) {
      out.assign(buf, static_cast<std::size_t>(n));
    }
    return out;
  }

  // Reads until it holds at least want bytes or the deadline passes.
  std::string readAtLeast(std::size_t want, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string out;
    while (out.size() < want) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        break;
      }
      out += readAvailable(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
      if (closed_by_peer_) {
        break;
      }
    }
    return out;
  }

  bool isClosedByPeer() const noexcept { return closed_by_peer_; }

  int fd() const noexcept { return fd_; }

  void close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  int fd_{-1};
  bool closed_by_peer_{false};
};

}  // namespace tcu::ipc::test
