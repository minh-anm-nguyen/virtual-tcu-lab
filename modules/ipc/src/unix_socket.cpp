#include "unix_socket.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace tcu::ipc::detail {

bool UnixSocket::open() {
  close();
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    last_errno_ = errno;
    return false;
  }
  fd_ = fd;
  last_errno_ = 0;
  return true;
}

bool UnixSocket::fillAddress(const std::string& path, sockaddr_un& addr) {
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (path.empty() || path.size() >= sizeof(addr.sun_path)) {
    last_errno_ = ENAMETOOLONG;
    return false;
  }
  std::memcpy(addr.sun_path, path.c_str(), path.size());
  return true;
}

int UnixSocket::bindPath(const std::string& path) {
  sockaddr_un addr{};
  if (!fillAddress(path, addr)) {
    return -1;
  }
  if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    last_errno_ = errno;
    return -1;
  }
  last_errno_ = 0;
  return 0;
}

int UnixSocket::connectPath(const std::string& path) {
  sockaddr_un addr{};
  if (!fillAddress(path, addr)) {
    return -1;
  }
  for (;;) {
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
      last_errno_ = 0;
      return 0;
    }
    if (errno == EINTR) {
      continue;
    }
    last_errno_ = errno;
    return -1;
  }
}

int UnixSocket::listen(int backlog) {
  if (::listen(fd_, backlog) != 0) {
    last_errno_ = errno;
    return -1;
  }
  last_errno_ = 0;
  return 0;
}

UnixSocket UnixSocket::accept() {
  for (;;) {
    const int fd = ::accept(fd_, nullptr, nullptr);
    if (fd >= 0) {
      last_errno_ = 0;
      return UnixSocket(fd);
    }
    if (errno == EINTR) {
      continue;
    }
    last_errno_ = errno;
    return UnixSocket();
  }
}

bool UnixSocket::setNonBlocking() {
  const int flags = ::fcntl(fd_, F_GETFL, 0);
  if (flags < 0) {
    last_errno_ = errno;
    return false;
  }
  if (::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) != 0) {
    last_errno_ = errno;
    return false;
  }
  last_errno_ = 0;
  return true;
}

ssize_t UnixSocket::recvSome(void* buffer, std::size_t length) {
  for (;;) {
    const ssize_t n = ::recv(fd_, buffer, length, 0);
    if (n >= 0) {
      last_errno_ = 0;
      return n;
    }
    if (errno == EINTR) {
      continue;
    }
    // EAGAIN/EWOULDBLOCK is "wait for readiness", not an error: the caller
    // returns to poll() instead of spinning (spec section 8.3).
    last_errno_ = errno;
    return -1;
  }
}

ssize_t UnixSocket::sendSome(const void* buffer, std::size_t length) {
  for (;;) {
    // MSG_NOSIGNAL: a dead peer must not raise SIGPIPE and kill the process
    // (spec section 8.3).
    const ssize_t n = ::send(fd_, buffer, length, MSG_NOSIGNAL);
    if (n >= 0) {
      last_errno_ = 0;
      return n;
    }
    if (errno == EINTR) {
      continue;
    }
    last_errno_ = errno;
    return -1;
  }
}

bool UnixSocket::peerCredentials(PeerCredentials& out) {
  ucred cred{};
  socklen_t len = sizeof(cred);
  if (::getsockopt(fd_, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
    last_errno_ = errno;
    return false;
  }
  out.uid = cred.uid;
  out.gid = cred.gid;
  out.pid = cred.pid;
  last_errno_ = 0;
  return true;
}

void UnixSocket::close() {
  if (fd_ >= 0) {
    // Retrying close() on EINTR can close a reused descriptor on Linux, so the
    // result is deliberately ignored: the FD is gone either way.
    ::close(fd_);
    fd_ = -1;
  }
}

}  // namespace tcu::ipc::detail
