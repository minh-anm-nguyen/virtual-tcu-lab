// The reload pipeline: BC-03, BC-04, BC-06, BC-07, BC-08 and invariants C11,
// C12, S8, S9.

#include <gtest/gtest.h>

#include <string>

#include "support/service_fixture.hpp"

namespace tcu::config {
namespace {

using test::ServiceFixture;

// BC-03: malformed JSON cannot disturb what is running.
TEST(Reload, InvalidJsonKeepsSnapshot) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);
  const ConfigPtr before = fx.service().snapshot();

  fx.dir().writeRaw(R"({"schema_version": 1, "telemetry": {)");
  const ReloadResult result = fx.service().reload();

  EXPECT_EQ(result.error, LoadError::ParseError);
  EXPECT_FALSE(result.changed);
  EXPECT_EQ(result.generation, 1u);
  EXPECT_EQ(fx.service().snapshot(), before) << "the very same snapshot object";
}

// BC-04.
TEST(Reload, OutOfRangeKeepsSnapshot) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);

  fx.writeWith("\"period_ms\": 5000", "\"period_ms\": 1");
  const ReloadResult result = fx.service().reload();

  EXPECT_EQ(result.error, LoadError::InvalidValue);
  EXPECT_EQ(fx.service().snapshot()->telemetry.periodMs, 5000);
  EXPECT_EQ(fx.service().snapshot()->generation, 1u);
}

// C11, S8: one case per failure kind, each asserting that nothing moved.
TEST(Reload, InvalidKeepsGeneration) {
  struct Case {
    const char* name;
    LoadError expected;
  };

  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);

  // ParseError
  fx.dir().writeRaw("{ not json");
  EXPECT_EQ(fx.service().reload().error, LoadError::ParseError);

  // SchemaError
  fx.writeWith("\"file_count\": 5", "\"file_countz\": 5");
  EXPECT_EQ(fx.service().reload().error, LoadError::SchemaError);

  // InvalidValue
  fx.writeWith("\"file_count\": 5", "\"file_count\": 99");
  EXPECT_EQ(fx.service().reload().error, LoadError::InvalidValue);

  // UnsupportedVersion
  fx.writeFixture("invalid_schema.json");
  EXPECT_EQ(fx.service().reload().error, LoadError::UnsupportedVersion);

  // TooLarge is a service option rather than a file property, so it is covered
  // in loader_test; FileNotFound:
  std::filesystem::remove(fx.dir().path());
  EXPECT_EQ(fx.service().reload().error, LoadError::FileNotFound);

  // Through all of it, generation 1 is still what is running.
  ASSERT_NE(fx.service().snapshot(), nullptr);
  EXPECT_EQ(fx.service().snapshot()->generation, 1u);
  EXPECT_EQ(fx.service().snapshot()->telemetry.periodMs, 5000);
  EXPECT_EQ(fx.service().metrics().generation, 1u);
}

// BC-06, S9: an identical file is a success that changes nothing. The
// generation is a count of effective changes, not of reload attempts.
TEST(Reload, NoOpDoesNotBumpGenerationOrNotify) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);

  std::atomic<int> notifications{0};
  fx.service().subscribe([&](ConfigPtr, ConfigPtr) { notifications.fetch_add(1); });

  fx.writeFixture("tcu.json");  // byte-identical content
  const ReloadResult result = fx.service().reload();

  EXPECT_EQ(result.error, LoadError::None);
  EXPECT_FALSE(result.changed);
  EXPECT_EQ(result.generation, 1u);
  EXPECT_EQ(fx.service().metrics().reloadSuccessNoopTotal, 1u);
  EXPECT_EQ(fx.service().metrics().reloadSuccessChangedTotal, 0u);

  fx.service().stop();
  EXPECT_EQ(notifications.load(), 0) << "a no-op must not wake observers";
}

// BC-07, C12: a hot change commits, exactly one generation at a time.
TEST(Reload, HotChangeCommitsAndNotifies) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);

  fx.writeWith("\"period_ms\": 5000", "\"period_ms\": 2000");
  const ReloadResult result = fx.service().reload();

  EXPECT_EQ(result.error, LoadError::None);
  EXPECT_TRUE(result.changed);
  EXPECT_EQ(result.generation, 2u);
  EXPECT_EQ(fx.service().snapshot()->telemetry.periodMs, 2000);
  EXPECT_EQ(fx.service().metrics().reloadSuccessChangedTotal, 1u);
  EXPECT_EQ(fx.service().metrics().generation, 2u);
}

// BC-08 and research R-09: the whole candidate is rejected, and the hot key
// that moved in the same file is NOT applied. That is the difference between a
// transactional snapshot and a pile of settings.
TEST(Reload, RestartOnlyKeyRejectsWholeCandidate) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);

  fx.writeFixture("restart_only.json");  // queue_capacity AND level both moved
  const ReloadResult result = fx.service().reload();

  EXPECT_EQ(result.error, LoadError::RestartRequired);
  EXPECT_FALSE(result.changed);
  EXPECT_EQ(result.generation, 1u);
  EXPECT_NE(result.publicReason.find("/logging/queue_capacity"), std::string::npos)
      << result.publicReason;

  const ConfigPtr s = fx.service().snapshot();
  EXPECT_EQ(s->logging.queueCapacity, 8192);
  EXPECT_EQ(s->logging.level, tcu::logging::Severity::Info)
      << "the co-changed hot key must not be applied either";
}

TEST(Reload, RestartOnlyReasonNamesARestartPointer) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);

  fx.writeWith("\"file_count\": 5", "\"file_count\": 6");
  const ReloadResult result = fx.service().reload();

  EXPECT_EQ(result.error, LoadError::RestartRequired);
  EXPECT_NE(result.publicReason.find("/logging/file_count"), std::string::npos);
}

// Several hot keys in one file commit together, as one generation.
TEST(Reload, SeveralHotKeysCommitAsOneGeneration) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);

  std::string text = test::TempConfigDir::readFixture("tcu.json");
  const std::string from_period = "\"period_ms\": 5000";
  text.replace(text.find(from_period), from_period.size(), "\"period_ms\": 2000");
  const std::string from_level = "\"level\": \"INFO\"";
  text.replace(text.find(from_level), from_level.size(), "\"level\": \"DEBUG\"");
  fx.dir().writeAtomic(text);

  const ReloadResult result = fx.service().reload();
  EXPECT_EQ(result.error, LoadError::None) << result.publicReason;
  EXPECT_EQ(result.generation, 2u);
  EXPECT_EQ(fx.service().snapshot()->telemetry.periodMs, 2000);
  EXPECT_EQ(fx.service().snapshot()->logging.level, tcu::logging::Severity::Debug);
}

// M1: the attempt accounting balances after a mixed run.
TEST(Metrics, ReloadAccountingBalances) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);

  fx.writeWith("\"period_ms\": 5000", "\"period_ms\": 2000");
  fx.service().reload();                       // changed
  fx.service().reload();                       // no-op
  fx.dir().writeRaw("{ broken");
  fx.service().reload();                       // ParseError
  fx.writeFixture("invalid_retry.json");
  fx.service().reload();                       // InvalidValue

  const ConfigMetrics m = fx.service().metrics();
  EXPECT_EQ(m.reloadAttemptsTotal,
            m.reloadSuccessChangedTotal + m.reloadSuccessNoopTotal +
                m.reloadFailuresSum());
  EXPECT_EQ(m.failures(LoadError::None), 0u) << "None is never a failure label";
  EXPECT_EQ(m.generation, fx.service().snapshot()->generation);
}

}  // namespace
}  // namespace tcu::config
