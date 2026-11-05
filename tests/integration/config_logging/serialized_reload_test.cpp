// BC-09 and invariant S3: concurrent reloads are serialized. Generations come
// out gap-free and strictly increasing, and no snapshot ever mixes fields from
// two different files.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "support/stack.hpp"

namespace tcu::itest {
namespace {

TEST(Reload, ConcurrentCallsAreSerialized) {
  Stack stack;
  ASSERT_EQ(stack.service->start(), config::LoadError::None);

  // Two shapes that differ in two fields at once. A torn snapshot would show
  // the period of one and the retry base of the other.
  const auto writeA = [&stack] {
    std::string text = config::test::TempConfigDir::readFixture("tcu.json");
    const std::string p = "\"period_ms\": 5000";
    text.replace(text.find(p), p.size(), "\"period_ms\": 2000");
    const std::string r = "\"retry_base_ms\": 1000";
    text.replace(text.find(r), r.size(), "\"retry_base_ms\": 2000");
    stack.dir.writeAtomic(text);
  };
  const auto writeB = [&stack] {
    std::string text = config::test::TempConfigDir::readFixture("tcu.json");
    const std::string p = "\"period_ms\": 5000";
    text.replace(text.find(p), p.size(), "\"period_ms\": 3000");
    const std::string r = "\"retry_base_ms\": 1000";
    text.replace(text.find(r), r.size(), "\"retry_base_ms\": 3000");
    stack.dir.writeAtomic(text);
  };

  std::mutex mutex;
  std::vector<std::uint64_t> committed;
  std::atomic<bool> torn{false};
  std::atomic<bool> go{false};

  const auto worker = [&](int index) {
    while (!go.load()) {
      std::this_thread::yield();
    }
    for (int i = 0; i < 10; ++i) {
      if (index % 2 == 0) {
        writeA();
      } else {
        writeB();
      }
      const config::ReloadResult result = stack.service->reload();
      if (result.changed) {
        std::lock_guard<std::mutex> lock(mutex);
        committed.push_back(result.generation);
      }
      const config::ConfigPtr snapshot = stack.service->snapshot();
      // Both candidate files set the two fields to the same number, so any
      // snapshot where they differ was assembled from two candidates. The
      // check starts at generation 2: generation 1 is the untouched baseline,
      // whose two fields legitimately differ.
      if (snapshot != nullptr && snapshot->generation >= 2 &&
          snapshot->telemetry.periodMs != snapshot->connectivity.retryBaseMs) {
        torn.store(true);
      }
    }
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back(worker, i);
  }
  go.store(true);
  for (std::thread& thread : threads) {
    thread.join();
  }

  EXPECT_FALSE(torn.load()) << "a snapshot mixed two candidates";

  std::lock_guard<std::mutex> lock(mutex);
  ASSERT_FALSE(committed.empty());
  const std::set<std::uint64_t> unique(committed.begin(), committed.end());
  EXPECT_EQ(unique.size(), committed.size()) << "a generation was handed out twice";

  std::uint64_t expected = 2;  // startup took generation 1
  std::vector<std::uint64_t> sorted(committed.begin(), committed.end());
  std::sort(sorted.begin(), sorted.end());
  for (const std::uint64_t generation : sorted) {
    EXPECT_EQ(generation, expected) << "generations must be gap-free";
    ++expected;
  }
  EXPECT_EQ(stack.service->snapshot()->generation, sorted.back());
}

}  // namespace
}  // namespace tcu::itest
