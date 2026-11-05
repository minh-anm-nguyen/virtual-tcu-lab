#include "tcu/logging/file_sink.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <system_error>

namespace tcu::logging {
namespace {

// <filesystem> rather than open(2): it covers permissions, rename and remove on
// both platforms, which is what keeps this library POSIX-free and therefore
// buildable on Windows (research R-00).
class DefaultFileOps final : public FileOps {
 public:
  bool open(const std::filesystem::path& path,
            std::uint64_t& size_out,
            int& error_out) override {
    error_out = 0;
    std::error_code ec;

    // A symlink at the active path is refused rather than followed. That is the
    // one piece of symlink hardening section 10.4 scopes into v1; the rest is
    // deferred to a production-hardening phase.
    if (std::filesystem::is_symlink(path, ec)) {
      error_out = EPERM;
      return false;
    }

    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent, ec)) {
      std::filesystem::create_directories(parent, ec);
      if (ec) {
        error_out = EACCES;
        return false;
      }
      // Owner-only on the directory too: a log directory anyone can list leaks
      // event names and timing even when the files themselves are protected.
      std::error_code ignored;
      std::filesystem::permissions(parent, std::filesystem::perms::owner_all,
                                   std::filesystem::perm_options::replace,
                                   ignored);
    }

    stream_.open(path, std::ios::binary | std::ios::app);
    if (!stream_.is_open()) {
      error_out = EACCES;
      return false;
    }

    std::error_code size_ec;
    const auto size = std::filesystem::file_size(path, size_ec);
    size_out = size_ec ? 0 : static_cast<std::uint64_t>(size);

    applyOwnerOnly(path);
    return true;
  }

  bool write(std::string_view bytes, int& error_out) override {
    error_out = 0;
    if (!stream_.is_open()) {
      error_out = EBADF;
      return false;
    }
    stream_.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!stream_.good()) {
      error_out = EIO;
      stream_.clear();
      return false;
    }
    return true;
  }

  void close() override {
    if (stream_.is_open()) {
      stream_.flush();
      stream_.close();
    }
    stream_.clear();
  }

  bool rename(const std::filesystem::path& from,
              const std::filesystem::path& to,
              int& error_out) override {
    error_out = 0;
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (ec) {
      error_out = EACCES;
      return false;
    }
    applyOwnerOnly(to);
    return true;
  }

  void flushStream() {
    if (stream_.is_open()) {
      stream_.flush();
    }
  }

 private:
  // Invariant F4. perm_options::replace, so an inherited umask cannot leave the
  // file group- or world-readable.
  static void applyOwnerOnly(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);
  }

  std::ofstream stream_;
};

}  // namespace

std::unique_ptr<FileOps> makeDefaultFileOps() {
  return std::make_unique<DefaultFileOps>();
}

struct FileSink::Impl {
  FileSinkOptions options;
  std::unique_ptr<FileOps> ops;

  mutable std::mutex mutex;
  bool is_open{false};
  std::uint64_t size{0};
  std::uint64_t rotations{0};
  int last_error{0};
  std::atomic<bool> healthy{true};
  std::chrono::steady_clock::time_point next_probe{};

  Impl(FileSinkOptions opts, std::unique_ptr<FileOps> injected)
      : options(std::move(opts)),
        ops(injected ? std::move(injected) : makeDefaultFileOps()) {}

  void markUnhealthy(int error) {
    last_error = error;
    healthy.store(false, std::memory_order_relaxed);
    next_probe = std::chrono::steady_clock::now() + options.recoveryProbeInterval;
    if (is_open) {
      ops->close();
      is_open = false;
    }
  }

  bool openLocked() {
    std::uint64_t size_out = 0;
    int error = 0;
    if (!ops->open(options.path, size_out, error)) {
      markUnhealthy(error);
      return false;
    }
    is_open = true;
    size = size_out;
    last_error = 0;
    healthy.store(true, std::memory_order_relaxed);
    return true;
  }

  // Rename chain, oldest first, so no name is ever overwritten while it still
  // holds the newest copy: tcu.log.N is dropped, .N-1 becomes .N, and the
  // active file becomes .1 (invariant F3).
  void rotateLocked() {
    ops->close();
    is_open = false;

    const std::filesystem::path& base = options.path;
    std::error_code ec;

    const std::filesystem::path oldest =
        base.string() + "." + std::to_string(options.fileCount);
    std::filesystem::remove(oldest, ec);

    for (std::size_t i = options.fileCount; i > 1; --i) {
      const std::filesystem::path from =
          base.string() + "." + std::to_string(i - 1);
      const std::filesystem::path to = base.string() + "." + std::to_string(i);
      if (std::filesystem::exists(from, ec)) {
        int error = 0;
        ops->rename(from, to, error);
      }
    }

    if (std::filesystem::exists(base, ec)) {
      int error = 0;
      const std::filesystem::path to = base.string() + ".1";
      if (!ops->rename(base, to, error)) {
        markUnhealthy(error);
        return;
      }
    }

    ++rotations;
    size = 0;
    openLocked();
  }

  // EINTR is a signal, not a disk problem, so it is retried a bounded number of
  // times. Bounded, because an unbounded retry on a genuinely broken fd is the
  // spin the failure matrix forbids.
  bool writeWithRetry(std::string_view bytes, int& error) {
    for (int attempt = 0; attempt < 4; ++attempt) {
      if (ops->write(bytes, error)) {
        return true;
      }
      if (error != EINTR) {
        return false;
      }
    }
    return false;
  }

  void emergency(std::string_view line) {
    if (!options.fallbackToStderr) {
      return;
    }
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fputc('\n', stderr);
  }
};

FileSink::FileSink(FileSinkOptions options, std::unique_ptr<FileOps> ops)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(ops))) {}

FileSink::~FileSink() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->is_open) {
    impl_->ops->close();
    impl_->is_open = false;
  }
}

bool FileSink::open() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->openLocked();
}

bool FileSink::write(std::string_view line) {
  std::lock_guard<std::mutex> lock(impl_->mutex);

  if (!impl_->healthy.load(std::memory_order_relaxed) || !impl_->is_open) {
    // An unhealthy sink does not retry on every record - that is the spin
    // section 12 scenario F forbids. The record takes the fallback and the
    // failure is reported to the writer, which counts it.
    impl_->emergency(line);
    return false;
  }

  // Rotation is decided BEFORE the write, so the active file never exceeds the
  // configured maximum rather than approximately not exceeding it (F2).
  const std::uint64_t needed = static_cast<std::uint64_t>(line.size()) + 1;
  if (impl_->size + needed > impl_->options.fileMaxBytes) {
    impl_->rotateLocked();
    if (!impl_->healthy.load(std::memory_order_relaxed)) {
      impl_->emergency(line);
      return false;
    }
  }

  int error = 0;
  if (!impl_->writeWithRetry(line, error) ||
      !impl_->writeWithRetry(std::string_view("\n", 1), error)) {
    impl_->markUnhealthy(error);
    impl_->emergency(line);
    return false;
  }
  impl_->size += needed;
  return true;
}

void FileSink::flush() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (auto* ops = dynamic_cast<DefaultFileOps*>(impl_->ops.get())) {
    ops->flushStream();
  }
}

SinkHealth FileSink::health() const noexcept {
  return impl_->healthy.load(std::memory_order_relaxed) ? SinkHealth::Healthy
                                                        : SinkHealth::Unhealthy;
}

void FileSink::periodic() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->healthy.load(std::memory_order_relaxed)) {
    return;
  }
  if (std::chrono::steady_clock::now() < impl_->next_probe) {
    return;  // at most one probe per interval, and no spin in between (F6)
  }
  // openLocked pushes next_probe forward on failure, so a failing probe costs
  // one attempt per interval and nothing more.
  impl_->openLocked();
}

std::chrono::steady_clock::duration FileSink::nextPeriodicIn() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->healthy.load(std::memory_order_relaxed)) {
    return std::chrono::steady_clock::duration::zero();
  }
  const auto now = std::chrono::steady_clock::now();
  if (impl_->next_probe <= now) {
    return std::chrono::steady_clock::duration::zero();
  }
  return impl_->next_probe - now;
}

std::uint64_t FileSink::rotations() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->rotations;
}

std::uint64_t FileSink::currentSize() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->size;
}

int FileSink::lastError() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->last_error;
}

}  // namespace tcu::logging
