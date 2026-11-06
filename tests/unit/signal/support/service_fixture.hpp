#pragma once

// Shared scaffolding for the service tests: a temporary DBC file, project 05's
// FakeCanTransport (not one of ours - research R-22), a real TimerScheduler and
// an injectable clock.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "tcu/can/fake_can_transport.hpp"
#include "tcu/signal/decoder_service.hpp"

namespace tcu::signal::test {

inline const char* labDbc() {
  return "VERSION \"lab\"\n"
         "BO_ 256 VEHICLE_STATUS: 8 ECU\n"
         " SG_ VehicleSpeed : 0|16@1+ (0.01,0) [0|250] \"km/h\" TCU\n"
         " SG_ Ignition : 16|1@1+ (1,0) [0|1] \"\" TCU\n";
}

/// A unique directory under the system temp path, removed on destruction.
class TempDir {
 public:
  TempDir() {
    static std::atomic<unsigned> counter{0};
    path_ = std::filesystem::temp_directory_path() /
            ("tcu_dbc_test_" + std::to_string(counter.fetch_add(1)) + "_" +
             std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  std::string write(const std::string& name, const std::string& contents) const {
    const auto file = path_ / name;
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << contents;
    out.close();
    return file.string();
  }

  /// Replaces a file the way a real tool does - write a temporary, then rename
  /// over the target - so a reload never sees a half-written file.
  void replace(const std::string& name, const std::string& contents) const {
    const auto target = path_ / name;
    const auto staging = path_ / (name + ".tmp");
    std::ofstream out(staging, std::ios::binary | std::ios::trunc);
    out << contents;
    out.close();
    std::filesystem::rename(staging, target);
  }

 private:
  std::filesystem::path path_;
};

inline DecoderConfig configFor(const std::string& dbcPath) {
  DecoderConfig config;
  config.dbcPath = dbcPath;
  config.rxQueueCapacity = 64;
  config.freshness.defaultTimeout = std::chrono::milliseconds{500};
  config.scanPeriod = std::chrono::milliseconds{10};
  config.reload.debounce = std::chrono::milliseconds{0};
  config.reload.maxAttemptsPerMinute = 6;
  config.logRateLimitPerReasonPerMinute = 60;
  return config;
}

inline tcu::can::CanMessage frameOf(std::uint32_t id, std::vector<std::uint8_t> bytes,
                                    long long rxMs) {
  tcu::can::CanMessage message;
  message.id = id;
  message.length = static_cast<std::uint8_t>(bytes.size());
  for (std::size_t i = 0; i < bytes.size() && i < 8; ++i) message.data[i] = bytes[i];
  message.received_at = tcu::can::SteadyTime{std::chrono::milliseconds{rxMs}};
  return message;
}

/// Records everything the service published, so a test can assert on order and
/// on isolation without racing the worker.
class RecordingObserver : public ISignalObserver {
 public:
  void onBatch(const dbc::DecodedBatch& batch) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      batches.push_back(batch);
    }
    if (throwOnBatch) throw std::runtime_error("observer failure, on purpose");
  }
  void onQualityChanged(const SignalKey& key, dbc::SignalQuality quality) override {
    std::lock_guard<std::mutex> lock(mutex_);
    qualityChanges.emplace_back(key, quality);
  }
  void onSchemaChanged(std::uint64_t version, const std::string& hash, bool committed) override {
    (void)hash;
    std::lock_guard<std::mutex> lock(mutex_);
    schemaChanges.emplace_back(version, committed);
  }

  std::size_t batchCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return batches.size();
  }
  std::size_t schemaChangeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return schemaChanges.size();
  }

  mutable std::mutex mutex_;
  std::vector<dbc::DecodedBatch> batches;
  std::vector<std::pair<SignalKey, dbc::SignalQuality>> qualityChanges;
  std::vector<std::pair<std::uint64_t, bool>> schemaChanges;
  bool throwOnBatch{false};
};

/// Waits on a condition rather than on a guessed duration: the worker is a real
/// thread, and a fixed sleep is the flakiness the spec forbids.
template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds budget = std::chrono::milliseconds{5000}) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::yield();
  }
  return predicate();
}

}  // namespace tcu::signal::test
