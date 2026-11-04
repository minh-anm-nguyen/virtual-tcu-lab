#pragma once

// A unique 0700 directory under /tmp for one test's socket.
//
// It must be under /tmp, not /mnt: binding an AF_UNIX socket on a DrvFs path
// fails EOPNOTSUPP under WSL2 (research R-00, note 2).

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <stdexcept>
#include <string>

namespace tcu::ipc::test {

class TempSocketDir {
 public:
  TempSocketDir() {
    std::string tmpl = "/tmp/tcu-ipc-test-XXXXXX";
    std::string buf = tmpl;
    char* made = ::mkdtemp(&buf[0]);
    if (made == nullptr) {
      throw std::runtime_error("mkdtemp failed: " + std::string(std::strerror(errno)));
    }
    path_ = made;
    if (path_.rfind("/mnt/", 0) == 0) {
      throw std::runtime_error("refusing a socket directory on /mnt: bind() fails EOPNOTSUPP");
    }
    // mkdtemp already gives 0700; make the invariant explicit so the server's
    // own permission check (R-10) cannot be satisfied by luck.
    if (::chmod(path_.c_str(), 0700) != 0) {
      throw std::runtime_error("chmod 0700 failed");
    }
  }

  ~TempSocketDir() { removeTree(path_); }

  TempSocketDir(const TempSocketDir&) = delete;
  TempSocketDir& operator=(const TempSocketDir&) = delete;

  const std::string& path() const noexcept { return path_; }

  std::string socketPath(const std::string& name = "test.sock") const { return path_ + "/" + name; }

 private:
  static void removeTree(const std::string& dir) {
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) {
      return;
    }
    while (dirent* e = ::readdir(d)) {
      std::string name = e->d_name;
      if (name == "." || name == "..") {
        continue;
      }
      ::unlink((dir + "/" + name).c_str());
    }
    ::closedir(d);
    ::rmdir(dir.c_str());
  }

  std::string path_;
};

}  // namespace tcu::ipc::test
