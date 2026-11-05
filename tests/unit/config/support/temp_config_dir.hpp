#pragma once

// A unique directory under the system temp path, plus the temp-file + rename
// write the spec recommends (section 25.4). Every configuration test writes
// through writeAtomic(), so no test ever observes a half-written file by
// accident - the one test that wants a truncated file writes it on purpose.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace tcu::config::test {

class TempConfigDir {
 public:
  TempConfigDir() {
    static std::atomic<unsigned> counter{0};
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    dir_ = std::filesystem::temp_directory_path() /
           ("tcu_cfg_" + std::to_string(stamp) + "_" +
            std::to_string(counter.fetch_add(1)));
    std::filesystem::create_directories(dir_);
  }

  ~TempConfigDir() {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);  // best effort; a test must not fail on cleanup
  }

  TempConfigDir(const TempConfigDir&) = delete;
  TempConfigDir& operator=(const TempConfigDir&) = delete;

  const std::filesystem::path& dir() const { return dir_; }
  std::filesystem::path path(const std::string& name = "tcu.json") const {
    return dir_ / name;
  }

  // Temp file + rename, the atomic publish an operator is told to use.
  //
  // The temp name carries its own sequence number, because several threads may
  // publish at once (BC-09 exercises exactly that) and a shared temp path would
  // make them race on one another intermediate file. The rename uses the
  // error_code overload: losing that race is an expected outcome here, not an
  // exception that should take the process down.
  void writeAtomic(const std::string& contents,
                   const std::string& name = "tcu.json") const {
    const std::filesystem::path target = path(name);
    const int sequence = ++bumps_;
    const std::filesystem::path tmp =
        target.string() + "." + std::to_string(sequence) + ".tmp";
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      out << contents;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
      std::filesystem::remove(tmp, ec);
      return;
    }
    stampMtime(target, sequence);
  }

  // A deliberately non-atomic, deliberately broken write, for BC-14.
  void writeRaw(const std::string& contents,
                const std::string& name = "tcu.json") const {
    const int sequence = ++bumps_;
    {
      std::ofstream out(path(name), std::ios::binary | std::ios::trunc);
      out << contents;
    }
    stampMtime(path(name), sequence);
  }

  // The poller compares mtime and size (research R-03), and on this platform
  // the filesystem timestamp has ONE SECOND of resolution: two writes inside
  // the same second carry the same mtime, and a replacement value of the same
  // length leaves the size unchanged, so a fast test would see no change at
  // all. Every write therefore gets its own stamp, one second apart, counted
  // from a base captured when the fixture was built.
  //
  // This is a test determinism device, not a workaround: the production limit
  // is recorded as a shortcut comment in modules/config/src/file_poller.cpp.
  void stampMtime(const std::filesystem::path& p, int sequence) const {
    std::error_code ec;
    std::filesystem::last_write_time(p, base_ + std::chrono::seconds(sequence), ec);
  }

  static std::string readFixture(const std::string& name) {
    std::ifstream in(std::filesystem::path(TCU_CONFIGS_DIR) / name,
                     std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
  }

 private:
  std::filesystem::path dir_;
  std::filesystem::file_time_type base_{
      std::filesystem::file_time_type::clock::now()};
  mutable std::atomic<int> bumps_{0};
};

}  // namespace tcu::config::test
