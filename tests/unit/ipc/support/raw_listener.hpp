#pragma once

// A raw AF_UNIX listener for the cases where a real IpcServer cannot produce
// the behaviour under test: a peer that accepts but never answers ipc.hello
// (B-01), or one that answers it with UNSUPPORTED_VERSION (B-19).
//
// It owns its FDs and emits exactly the bytes a test chooses (research R-13).

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace tcu::ipc::test {

/// One accepted connection, readable and writable at byte level.
class RawConnection {
 public:
  RawConnection() = default;
  explicit RawConnection(int fd) noexcept : fd_(fd) {}
  ~RawConnection() { close(); }

  RawConnection(RawConnection&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  RawConnection& operator=(RawConnection&& other) noexcept {
    if (this != &other) {
      close();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }
  RawConnection(const RawConnection&) = delete;
  RawConnection& operator=(const RawConnection&) = delete;

  bool valid() const noexcept { return fd_ >= 0; }

  bool writeBytes(const std::string& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
      const ssize_t n = ::send(fd_, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
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

  /// Reads until at least `want` bytes are held or the deadline passes.
  std::string readAtLeast(std::size_t want, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string out;
    while (out.size() < want) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        break;
      }
      const auto left =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
      pollfd p{fd_, POLLIN, 0};
      if (::poll(&p, 1, static_cast<int>(left)) <= 0) {
        break;
      }
      char buf[4096];
      const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
      if (n <= 0) {
        break;
      }
      out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
  }

  void close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  int fd_{-1};
};

class RawListener {
 public:
  RawListener() = default;
  ~RawListener() { close(); }

  RawListener(const RawListener&) = delete;
  RawListener& operator=(const RawListener&) = delete;

  bool start(const std::string& socket_path) {
    close();
    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
      return false;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) {
      return false;
    }
    std::memcpy(addr.sun_path, socket_path.c_str(), socket_path.size());
    ::unlink(socket_path.c_str());
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      return false;
    }
    path_ = socket_path;
    return ::listen(fd_, 8) == 0;
  }

  /// Waits up to `timeout` for a connection. An invalid result means none came.
  RawConnection acceptPeer(std::chrono::milliseconds timeout) {
    pollfd p{fd_, POLLIN, 0};
    if (::poll(&p, 1, static_cast<int>(timeout.count())) <= 0) {
      return RawConnection();
    }
    const int fd = ::accept(fd_, nullptr, nullptr);
    return fd < 0 ? RawConnection() : RawConnection(fd);
  }

  void close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    if (!path_.empty()) {
      ::unlink(path_.c_str());
      path_.clear();
    }
  }

 private:
  int fd_{-1};
  std::string path_;
};

/// Builds a frame with chosen header fields, so a test can forge one without
/// depending on the library's encoder.
inline std::string rawFrame(std::uint8_t type, std::uint64_t request_id, const std::string& name,
                            const std::string& payload) {
  std::string out;
  const std::uint32_t body = static_cast<std::uint32_t>(14 + name.size() + payload.size());
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<char>((body >> shift) & 0xFF));
  }
  out.push_back(static_cast<char>(1));  // version
  out.push_back(static_cast<char>(type));
  out.push_back(static_cast<char>(0));
  out.push_back(static_cast<char>(0));  // flags
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<char>((request_id >> shift) & 0xFF));
  }
  out.push_back(static_cast<char>((name.size() >> 8) & 0xFF));
  out.push_back(static_cast<char>(name.size() & 0xFF));
  out += name;
  out += payload;
  return out;
}

/// request_id of a frame, read straight from the header bytes.
inline std::uint64_t frameRequestId(const std::string& frame) {
  if (frame.size() < 16) {
    return 0;
  }
  std::uint64_t id = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    id = (id << 8) | static_cast<std::uint8_t>(frame[8 + i]);
  }
  return id;
}

/// name field of a frame.
inline std::string frameName(const std::string& frame) {
  if (frame.size() < 18) {
    return {};
  }
  const std::size_t len = (static_cast<std::size_t>(static_cast<std::uint8_t>(frame[16])) << 8) |
                          static_cast<std::uint8_t>(frame[17]);
  if (frame.size() < 18 + len) {
    return {};
  }
  return frame.substr(18, len);
}

/// Total size of the frame at the front of `buffer`, or 0 when incomplete.
inline std::size_t frameSize(const std::string& buffer) {
  if (buffer.size() < 4) {
    return 0;
  }
  std::uint32_t body = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    body = (body << 8) | static_cast<std::uint8_t>(buffer[i]);
  }
  const std::size_t total = 4 + body;
  return buffer.size() >= total ? total : 0;
}

}  // namespace tcu::ipc::test
