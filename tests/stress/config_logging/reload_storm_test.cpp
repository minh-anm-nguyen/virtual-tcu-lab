// BC-10 and invariant S2: readers never see a torn snapshot. 100 reloads
// alternate two candidates while eight readers spin on snapshot(); every
// observed snapshot must be internally consistent with one of the two files.
//
// Part of what this asserts is the absence of a race, which only
// ThreadSanitizer can observe - so this case is also a TSan target.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "support/service_fixture.hpp"

namespace tcu::config {
namespace {

using test::ServiceFixture;
using test::TempConfigDir;

// Both files set period_ms and retry_base_ms to the same number, so a snapshot
// where the two differ was assembled from two candidates.
std::string candidate(std::int64_t value) {
  std::string text = TempConfigDir::readFixture("tcu.json");
  const std::string period = "\"period_ms\": 5000";
  text.replace(text.find(period), period.size(),
               "\"period_ms\": " + std::to_string(value));
  const std::string retry = "\"retry_base_ms\": 1000";
  text.replace(text.find(retry), retry.size(),
               "\"retry_base_ms\": " + std::to_string(value));
  return text;
}

TEST(Stress, SnapshotReaderSeesOldOrNewNeverTorn) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);

  std::atomic<bool> running{true};
  std::atomic<std::uint64_t> reads{0};
  std::atomic<bool> torn{false};

  std::vector<std::thread> readers;
  for (int i = 0; i < 8; ++i) {
    readers.emplace_back([&] {
      while (running.load(std::memory_order_relaxed)) {
        const ConfigPtr snapshot = fx.service().snapshot();
        if (snapshot == nullptr) {
          continue;
        }
        reads.fetch_add(1, std::memory_order_relaxed);
        if (snapshot->generation >= 2 &&
            snapshot->telemetry.periodMs != snapshot->connectivity.retryBaseMs) {
          torn.store(true);
        }
        // Holding the pointer across more reads is the other half of the
        // contract: an old snapshot must stay readable and unchanged.
        const std::int64_t period = snapshot->telemetry.periodMs;
        if (snapshot->telemetry.periodMs != period) {
          torn.store(true);
        }
      }
    });
  }

  const auto started = std::chrono::steady_clock::now();
  std::vector<double> latencies_ms;
  latencies_ms.reserve(100);
  for (int i = 0; i < 100; ++i) {
    fx.dir().writeAtomic(candidate(2000 + (i % 2) * 1000));
    const auto before = std::chrono::steady_clock::now();
    const ReloadResult result = fx.service().reload();
    const auto after = std::chrono::steady_clock::now();
    latencies_ms.push_back(std::chrono::duration<double, std::milli>(after - before).count());
    ASSERT_EQ(result.error, LoadError::None) << result.publicReason;
  }
  running.store(false);
  for (std::thread& thread : readers) {
    thread.join();
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

  EXPECT_FALSE(torn.load()) << "a reader saw a snapshot assembled from two files";
  EXPECT_GT(reads.load(), 1000u) << "readers were starved, so this proved little";
  EXPECT_EQ(fx.service().metrics().generation, fx.service().snapshot()->generation);

  std::sort(latencies_ms.begin(), latencies_ms.end());
  const auto at = [&latencies_ms](double q) {
    const std::size_t index =
        static_cast<std::size_t>(q * (latencies_ms.size() - 1));
    return latencies_ms[index];
  };
  std::printf("[config-logging] reload latency ms:  p50 %.2f  p95 %.2f  p99 %.2f  (n=%zu)\n",
              at(0.50), at(0.95), at(0.99), latencies_ms.size());
  std::printf("[config-logging] snapshot reads: %llu in %.1f s across 8 readers\n",
              static_cast<unsigned long long>(reads.load()), seconds);
}

}  // namespace
}  // namespace tcu::config
