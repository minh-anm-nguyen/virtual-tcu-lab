// The soak: the log flood, the reload storm and real rotation at the same
// time, for TCU_CONFIG_STRESS_SECONDS (default 5). Section 17 asks for a 1 to
// 24 hour run; that is this same test with the variable raised, which the
// quickstart records. The assertions are bounded memory and a bounded file
// count; everything else is printed (research R-21).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "support/service_fixture.hpp"
#include "tcu/logging/async_logger.hpp"
#include "tcu/logging/file_sink.hpp"

namespace tcu {
namespace {

using namespace std::chrono_literals;

int stressSeconds() {
  const char* value = std::getenv("TCU_CONFIG_STRESS_SECONDS");
  if (value == nullptr) {
    return 5;
  }
  const int parsed = std::atoi(value);
  return parsed > 0 ? parsed : 5;
}

std::size_t countLogFiles(const std::filesystem::path& dir) {
  std::size_t n = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().filename().string().rfind("soak.log", 0) == 0) {
      ++n;
    }
  }
  return n;
}

TEST(Stress, SoakKeepsMemoryAndFilesBounded) {
  const int seconds = stressSeconds();
  config::test::ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), config::LoadError::None);

  // A real rotating file sink with a small maximum, so rotation happens
  // hundreds of times rather than never.
  logging::FileSinkOptions file_options;
  file_options.path = fx.dir().dir() / "soak.log";
  file_options.fileMaxBytes = 64 * 1024;
  file_options.fileCount = 3;
  auto file_sink = std::make_shared<logging::FileSink>(file_options);
  ASSERT_TRUE(file_sink->open());

  logging::LoggerOptions logger_options;
  logger_options.minLevel = logging::Severity::Debug;
  logger_options.queueCapacity = 2048;
  logging::AsyncLogger logger(logger_options, {file_sink});
  ASSERT_TRUE(logger.start());

  std::atomic<bool> running{true};
  std::atomic<std::uint64_t> reloads{0};
  std::atomic<std::uint64_t> max_files{0};

  std::vector<std::thread> workers;
  for (int p = 0; p < 4; ++p) {
    workers.emplace_back([&logger, &running, p] {
      std::uint64_t i = 0;
      while (running.load(std::memory_order_relaxed)) {
        logger.log(logging::Severity::Debug, "soak", "burst",
                   "a line long enough to rotate the file reasonably often",
                   {logging::f("producer", static_cast<std::int64_t>(p)),
                    logging::f("index", static_cast<std::int64_t>(i++))});
      }
    });
  }

  // The reload storm, plus a file-count check on the way past.
  workers.emplace_back([&] {
    std::uint64_t i = 0;
    while (running.load(std::memory_order_relaxed)) {
      fx.writeWith("\"period_ms\": 5000",
                   "\"period_ms\": " + std::to_string(2000 + (i % 7) * 100));
      if (fx.service().reload().changed) {
        reloads.fetch_add(1);
      }
      const std::uint64_t files = countLogFiles(fx.dir().dir());
      if (files > max_files.load()) {
        max_files.store(files);
      }
      ++i;
    }
  });

  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  running.store(false);
  for (std::thread& thread : workers) {
    thread.join();
  }
  logger.flush();
  logger.stop();

  const logging::LoggerMetrics m = logger.metrics();
  const config::ConfigMetrics cm = fx.service().metrics();

  // F3: the file budget held for the whole run, not just at the end.
  EXPECT_LE(max_files.load(), file_options.fileCount + 1)
      << "the rotation budget was exceeded at some point during the run";
  EXPECT_LE(m.queueDepth, logger_options.queueCapacity);
  EXPECT_EQ(m.recordsSubmittedTotal,
            m.recordsFilteredTotal + m.recordsEnqueuedTotal + m.droppedTotal() +
                m.rejectedAfterStopTotal + m.reentrantEmergencyTotal);
  EXPECT_EQ(cm.reloadAttemptsTotal, cm.reloadSuccessChangedTotal +
                                        cm.reloadSuccessNoopTotal +
                                        cm.reloadFailuresSum());
  EXPECT_EQ(cm.generation, fx.service().snapshot()->generation);

  std::printf("[config-logging] soak %d s: %llu records submitted, %llu written, "
              "%llu dropped, %llu rotations, %llu reloads, peak files %llu\n",
              seconds,
              static_cast<unsigned long long>(m.recordsSubmittedTotal),
              static_cast<unsigned long long>(m.writtenTotal()),
              static_cast<unsigned long long>(m.droppedTotal()),
              static_cast<unsigned long long>(file_sink->rotations()),
              static_cast<unsigned long long>(reloads.load()),
              static_cast<unsigned long long>(max_files.load()));
  std::printf("[config-logging] sink errors: %llu, format failures: %llu, "
              "not drained at stop: %llu\n",
              static_cast<unsigned long long>(
                  m.sinkErrorsTotal[logging::sinkIndex(logging::SinkKind::File)]),
              static_cast<unsigned long long>(m.formatFailuresTotal),
              static_cast<unsigned long long>(m.notDrainedAtStopTotal));
  std::printf("[config-logging] NOTE these are measurements of this host, not "
              "guarantees (spec section 18)\n");
}

}  // namespace
}  // namespace tcu
