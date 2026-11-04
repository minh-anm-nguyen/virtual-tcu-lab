#pragma once

// Forks and execs the real vehicle_service binary, so the integration suite
// exercises a genuine process boundary rather than two objects in one process
// (spec section 12.2).
//
// Readiness is detected by connecting to the socket with a deadline, never by
// sleeping for a fixed time (Principle IV: deterministic tests).

#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace tcu::ipc::test {

class ServerProcess {
 public:
  ServerProcess() = default;

  ~ServerProcess() { kill(); }

  ServerProcess(const ServerProcess&) = delete;
  ServerProcess& operator=(const ServerProcess&) = delete;

  /// Starts the service on `socket_path`. `ignition_period_ms <= 0` switches
  /// the notification timer off, which most tests want.
  bool start(const std::string& socket_path, int ignition_period_ms = 0,
             const std::vector<std::string>& extra = {}) {
    socket_path_ = socket_path;
    ignition_period_ms_ = ignition_period_ms;
    extra_ = extra;
    return spawn();
  }

  bool restart() { return spawn(); }

  /// SIGKILL, as the demo of spec section 16 does, then reap.
  void kill() {
    if (pid_ <= 0) {
      return;
    }
    ::kill(pid_, SIGKILL);
    int status = 0;
    ::waitpid(pid_, &status, 0);
    pid_ = -1;
  }

  /// SIGTERM, for the graceful-shutdown case.
  void terminate() {
    if (pid_ <= 0) {
      return;
    }
    ::kill(pid_, SIGTERM);
    int status = 0;
    ::waitpid(pid_, &status, 0);
    pid_ = -1;
  }

  pid_t pid() const noexcept { return pid_; }
  bool running() const noexcept { return pid_ > 0; }
  const std::string& socketPath() const noexcept { return socket_path_; }

  /// True once a connect() to the socket succeeds.
  static bool waitUntilListening(const std::string& path, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
      if (canConnect(path)) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
  }

 private:
  static bool canConnect(const std::string& path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
      return false;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
      ::close(fd);
      return false;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size());
    const bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    return ok;
  }

  bool spawn() {
    const std::string period = std::to_string(ignition_period_ms_);
    std::vector<std::string> args = {TCU_VEHICLE_SERVICE_BIN, "--socket", socket_path_,
                                     "--ignition-period-ms", period};
    args.insert(args.end(), extra_.begin(), extra_.end());

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (std::string& arg : args) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
      return false;
    }
    if (pid == 0) {
      ::execv(TCU_VEHICLE_SERVICE_BIN, argv.data());
      ::_exit(127);
    }
    pid_ = pid;
    if (!waitUntilListening(socket_path_, std::chrono::seconds(5))) {
      kill();
      return false;
    }
    return true;
  }

  pid_t pid_{-1};
  std::string socket_path_;
  int ignition_period_ms_{0};
  std::vector<std::string> extra_;
};

}  // namespace tcu::ipc::test
