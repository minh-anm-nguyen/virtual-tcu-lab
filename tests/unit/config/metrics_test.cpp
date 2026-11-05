// Configuration counters: contracts/metrics.md section 4, invariants M1, M2,
// M4, M5, M6.

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>

#include "support/service_fixture.hpp"

namespace tcu::config {
namespace {

using test::ServiceFixture;

// M2: the gauge and the snapshot cannot disagree.
TEST(Metrics, GenerationMatchesSnapshot) {
  ServiceFixture fx;
  EXPECT_EQ(fx.service().metrics().generation, 0u) << "nothing published yet";
  EXPECT_FALSE(fx.service().metrics().configValid);

  ASSERT_EQ(fx.service().start(), LoadError::None);
  EXPECT_EQ(fx.service().metrics().generation, fx.service().snapshot()->generation);
  EXPECT_TRUE(fx.service().metrics().configValid);

  fx.writeWith("\"period_ms\": 5000", "\"period_ms\": 2000");
  ASSERT_TRUE(fx.service().reload().changed);
  EXPECT_EQ(fx.service().metrics().generation, fx.service().snapshot()->generation);
}

// M5: after a successful start with no restart, the number of committed changes
// is exactly the generation minus the one that startup published.
TEST(Metrics, SuccessChangedEqualsGenerationMinusOne) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);

  for (int i = 0; i < 4; ++i) {
    fx.writeWith("\"period_ms\": 5000",
                 "\"period_ms\": " + std::to_string(1000 + i * 100));
    ASSERT_TRUE(fx.service().reload().changed);
  }

  const ConfigMetrics m = fx.service().metrics();
  EXPECT_EQ(m.reloadSuccessChangedTotal, m.generation - 1);
}

// M1 again, but with only failures, to pin the case where nothing succeeded.
TEST(Metrics, ReloadAccountingBalancesForFailuresOnly) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);

  fx.dir().writeRaw("{ broken");
  fx.service().reload();
  fx.service().reload();

  const ConfigMetrics m = fx.service().metrics();
  EXPECT_EQ(m.reloadAttemptsTotal, 2u);
  EXPECT_EQ(m.reloadSuccessChangedTotal, 0u);
  EXPECT_EQ(m.reloadSuccessNoopTotal, 0u);
  EXPECT_EQ(m.reloadFailuresSum(), 2u);
  EXPECT_EQ(m.failures(LoadError::ParseError), 2u);
}

// M6: every commit is eventually accounted for by a delivery or a failure.
TEST(Metrics, NotifyAccountingCoversEveryCommit) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);

  std::atomic<int> seen{0};
  fx.service().subscribe([&](ConfigPtr, ConfigPtr) { seen.fetch_add(1); });

  for (int i = 0; i < 3; ++i) {
    fx.writeWith("\"period_ms\": 5000",
                 "\"period_ms\": " + std::to_string(1000 + i * 100));
    ASSERT_TRUE(fx.service().reload().changed);
  }

  for (int i = 0; i < 100000; ++i) {
    const ConfigMetrics m = fx.service().metrics();
    if (m.notifyDeliveredTotal + m.notifyFailuresTotal >= 3) {
      break;
    }
    std::this_thread::yield();
  }

  const ConfigMetrics m = fx.service().metrics();
  EXPECT_GE(m.notifyDeliveredTotal + m.notifyFailuresTotal,
            m.reloadSuccessChangedTotal);
  EXPECT_FALSE(m.notifyDegraded) << "nothing overflowed here";
}

// M4: the failure labels are the LoadError names and nothing else, and None is
// never one of them.
TEST(Metrics, FailureReasonsAreDisjoint) {
  ServiceFixture fx;
  ASSERT_EQ(fx.service().start(), LoadError::None);

  ConfigMetrics m;
  EXPECT_EQ(m.reloadFailuresTotal.size(), kLoadErrorCount);

  fx.writeFixture("invalid_retry.json");
  fx.service().reload();
  fx.writeFixture("invalid_schema.json");
  fx.service().reload();

  m = fx.service().metrics();
  EXPECT_EQ(m.failures(LoadError::InvalidValue), 1u);
  EXPECT_EQ(m.failures(LoadError::UnsupportedVersion), 1u);
  EXPECT_EQ(m.failures(LoadError::None), 0u);
  EXPECT_EQ(m.reloadFailuresSum(), 2u) << "one reason per attempt, never two";

  // Every name is distinct, which is what keeps the label set readable.
  for (std::size_t i = 0; i < kLoadErrorCount; ++i) {
    for (std::size_t j = i + 1; j < kLoadErrorCount; ++j) {
      EXPECT_NE(loadErrorName(static_cast<LoadError>(i)),
                loadErrorName(static_cast<LoadError>(j)));
    }
  }
}

}  // namespace
}  // namespace tcu::config
