// BC-22 and scenario E: many producers, faster than any sink. The assertions
// are the ones a slow machine cannot explain away - every emitted line is
// well formed, the accounting balances, memory stays bounded - and the numbers
// are reported rather than gated (research R-21).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "json_lite.hpp"
#include "support/recording_sink.hpp"
#include "tcu/logging/async_logger.hpp"

namespace tcu::logging {
namespace {

using namespace std::chrono_literals;

// Default 5 seconds so CI finishes; raise it for a deliberate soak.
int stressSeconds() {
  const char* value = std::getenv("TCU_CONFIG_STRESS_SECONDS");
  if (value == nullptr) {
    return 5;
  }
  const int parsed = std::atoi(value);
  return parsed > 0 ? parsed : 5;
}

TEST(Stress, EightProducersWellFormedJsonl) {
  constexpr int kProducers = 8;
  auto sink = std::make_shared<test::RecordingSink>(SinkKind::File);

  LoggerOptions options;
  options.minLevel = Severity::Debug;
  options.queueCapacity = 4096;
  options.highPriorityWaitMs = 5ms;
  AsyncLogger logger(options, {sink});
  ASSERT_TRUE(logger.start());

  std::atomic<bool> running{true};
  std::atomic<std::uint64_t> submitted{0};
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&logger, &running, &submitted, p] {
      std::uint64_t i = 0;
      while (running.load(std::memory_order_relaxed)) {
        const Severity level = (i % 50 == 0) ? Severity::Error : Severity::Debug;
        logger.log(level, "flood", "burst", "payload with spaces and commas",
                   {f("producer", static_cast<std::int64_t>(p)),
                    f("index", static_cast<std::int64_t>(i))});
        submitted.fetch_add(1, std::memory_order_relaxed);
        ++i;
      }
    });
  }

  const auto started = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::seconds(stressSeconds()));
  running.store(false);
  for (std::thread& thread : producers) {
    thread.join();
  }
  logger.flush();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  logger.stop();

  const LoggerMetrics m = logger.metrics();
  const std::vector<std::string> lines = sink->lines();

  // Every line that was written is one complete, well formed JSON object. The
  // structural checker of project 02 is reused rather than written a third
  // time (Constitution Principle II).
  for (const std::string& line : lines) {
    ASSERT_TRUE(jsonlite::isWellFormed(line)) << line;
    ASSERT_EQ(line.find('\n'), std::string::npos) << "a record split into two lines";
  }

  // Nothing is both written and dropped, and nothing vanished.
  EXPECT_EQ(m.recordsSubmittedTotal,
            m.recordsFilteredTotal + m.recordsEnqueuedTotal + m.droppedTotal() +
                m.rejectedAfterStopTotal + m.reentrantEmergencyTotal);
  EXPECT_EQ(lines.size(), m.recordsWrittenTotal[sinkIndex(SinkKind::File)]);
  EXPECT_LE(m.queueDepth, 4096u) << "the queue stayed bounded";
  EXPECT_EQ(m.notDrainedAtStopTotal, 0u);
  EXPECT_GT(lines.size(), 0u);

  const double seconds = std::chrono::duration<double>(elapsed).count();
  std::printf("[config-logging] log throughput:  %.0f records/s over %.1f s, "
              "%llu dropped, %llu written\n",
              static_cast<double>(m.recordsSubmittedTotal) / seconds, seconds,
              static_cast<unsigned long long>(m.droppedTotal()),
              static_cast<unsigned long long>(m.writtenTotal()));
  std::printf("[config-logging] peak queue depth budget: %llu, high priority "
              "drops: %llu\n",
              static_cast<unsigned long long>(options.queueCapacity),
              static_cast<unsigned long long>(m.highPriorityDropsTotal));
}

}  // namespace
}  // namespace tcu::logging
