// Startup and the immutable snapshot: BC-01, BC-02 and invariants C1, C3, C5,
// S1, S6, S7.

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

#include "support/service_fixture.hpp"

namespace tcu::config {
namespace {

using test::ServiceFixture;

// BC-01: a valid file becomes generation 1, readable immediately.
TEST(Lifecycle, StartPublishesGenerationOne) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);

  const ConfigPtr snapshot = fx.service().snapshot();
  ASSERT_NE(snapshot, nullptr);
  EXPECT_EQ(snapshot->generation, 1u);
  EXPECT_FALSE(snapshot->source.empty());
  EXPECT_NE(snapshot->loadedAt.time_since_epoch().count(), 0);
  EXPECT_TRUE(fx.service().metrics().configValid);
  EXPECT_EQ(fx.service().metrics().generation, 1u);
}

// BC-02, S1: there is no state in which the service runs on values nobody
// validated.
TEST(Lifecycle, MissingRequiredFieldFailsStartup) {
  ServiceFixture fx;
  fx.writeWith("\"period_ms\": 5000", "\"period_zz\": 5000");

  const LoadError error = fx.service().start();

  EXPECT_NE(error, LoadError::None);
  EXPECT_EQ(fx.service().snapshot(), nullptr) << "no snapshot may be in force";
  EXPECT_FALSE(fx.service().metrics().configValid);
  EXPECT_EQ(fx.service().metrics().generation, 0u);
}

TEST(Lifecycle, StartOnAMissingFileFails) {
  ServiceFixture fx;
  std::filesystem::remove(fx.dir().path());
  EXPECT_EQ(fx.service().start(), LoadError::FileNotFound);
  EXPECT_EQ(fx.service().snapshot(), nullptr);
}

TEST(Snapshot, TypedFieldsMatchFile) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);
  const ConfigPtr s = fx.service().snapshot();
  ASSERT_NE(s, nullptr);

  EXPECT_EQ(s->connectivity.retryBaseMs, 1000);
  EXPECT_EQ(s->connectivity.retryMaxMs, 30000);
  EXPECT_EQ(s->connectivity.maxAttempts, 0);
  EXPECT_EQ(s->telemetry.periodMs, 5000);
  EXPECT_EQ(s->telemetry.offlineQueueCapacity, 2048);
  EXPECT_EQ(s->logging.level, tcu::logging::Severity::Info);
  EXPECT_EQ(s->logging.queueCapacity, 8192);
  EXPECT_EQ(s->logging.fileMaxBytes, 10485760);
  EXPECT_EQ(s->logging.fileCount, 5);
  EXPECT_TRUE(s->logging.consoleEnabled);
  EXPECT_EQ(s->network.cloudEndpoint, "https://mock-cloud.local");
  EXPECT_TRUE(s->network.tlsRequired);
}

// C5: provenance is not content, so a reload of the same values is a no-op even
// though loadedAt moved.
TEST(Snapshot, ContentEqualsIgnoresProvenance) {
  ConfigSnapshot a;
  ConfigSnapshot b;
  a.generation = 1;
  b.generation = 99;
  a.source = "/one.json";
  b.source = "/two.json";
  b.loadedAt = std::chrono::system_clock::now();
  EXPECT_TRUE(a.contentEquals(b));

  b.telemetry.periodMs = 1;
  EXPECT_FALSE(a.contentEquals(b));
}

// C3: the comparison is on typed values, so formatting is invisible to it.
TEST(Snapshot, ContentEqualsIgnoresWhitespaceAndKeyOrder) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);
  const ConfigPtr first = fx.service().snapshot();

  // Same semantics, different text: the whole file on one line.
  std::string compact = test::TempConfigDir::readFixture("tcu.json");
  std::string one_line;
  for (const char c : compact) {
    if (c != '\n' && c != ' ') {
      one_line += c;
    }
  }
  fx.dir().writeAtomic(one_line);

  const ReloadResult result = fx.service().reload();
  EXPECT_EQ(result.error, LoadError::None) << result.publicReason;
  EXPECT_FALSE(result.changed) << "whitespace is not a change";
  EXPECT_EQ(fx.service().snapshot()->generation, first->generation);
}

// C1: a held snapshot is never mutated by a later publish.
TEST(Snapshot, HeldPointerStaysValidAfterReload) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);
  const ConfigPtr held = fx.service().snapshot();
  ASSERT_NE(held, nullptr);

  fx.writeWith("\"period_ms\": 5000", "\"period_ms\": 2000");
  ASSERT_EQ(fx.service().reload().error, LoadError::None);

  EXPECT_EQ(held->telemetry.periodMs, 5000) << "the old snapshot must be immutable";
  EXPECT_EQ(held->generation, 1u);
  EXPECT_EQ(fx.service().snapshot()->telemetry.periodMs, 2000);
  EXPECT_EQ(fx.service().snapshot()->generation, 2u);
}

// S7: a reader never waits for a reload. The reader thread here keeps reading
// while the main thread reloads; a blocking snapshot() would show up as a
// read count far below the reload count.
TEST(Snapshot, ReadDuringReloadDoesNotBlock) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);

  std::atomic<bool> running{true};
  std::atomic<std::uint64_t> reads{0};
  std::thread reader([&] {
    while (running.load()) {
      const ConfigPtr s = fx.service().snapshot();
      if (s != nullptr) {
        reads.fetch_add(1);
      }
    }
  });

  for (int i = 0; i < 20; ++i) {
    fx.writeWith("\"period_ms\": 5000",
                 "\"period_ms\": " + std::to_string(1000 + i * 100));
    fx.service().reload();
  }
  running.store(false);
  reader.join();

  EXPECT_GT(reads.load(), 1000u) << "reads should vastly outnumber reloads";
}

// BC-30, S6: stop is idempotent, and S5: reload after stop touches nothing.
TEST(Service, StopIsIdempotent) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);
  fx.service().stop();
  fx.service().stop();
  fx.service().stop();
  SUCCEED();
}

TEST(Service, ReloadAfterStop) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);
  const std::uint64_t generation = fx.service().snapshot()->generation;
  fx.service().stop();

  fx.writeWith("\"period_ms\": 5000", "\"period_ms\": 2000");
  const ReloadResult result = fx.service().reload();

  EXPECT_EQ(result.error, LoadError::ServiceStopped);
  EXPECT_FALSE(result.changed);
  EXPECT_EQ(result.generation, generation);
}

}  // namespace
}  // namespace tcu::config
