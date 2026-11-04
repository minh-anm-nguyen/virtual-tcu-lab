#include <gtest/gtest.h>

#include "demo.hpp"
#include "tcu/eventbus/event_bus.hpp"
#include "tcu/eventbus/metrics.hpp"

namespace {

using namespace tcu::eventbus;

// T083: runs the same scenario main.cpp runs, so the demo cannot rot silently.
// Quiet mode, because the assertions are on the metrics, not the log text.
TEST(Demo, DrainRunSatisfiesItsConservationEquation) {
  const MetricsSnapshot metrics = tcu::demo::run_demo(StopMode::Drain, false);

  EXPECT_GT(metrics.accepted_total, 0u);
  EXPECT_EQ(metrics.accepted_total, metrics.dispatched_total);
  EXPECT_EQ(metrics.discarded_on_stop_total, 0u);
  EXPECT_EQ(metrics.queue_depth, 0u);
  EXPECT_EQ(metrics.callback_failures_total, 0u);
  // The slow-handler phase must actually have exercised backpressure.
  EXPECT_GT(metrics.rejected_queue_full_total + metrics.publish_timeout_total, 0u);
}

TEST(Demo, DiscardRunSatisfiesItsConservationEquation) {
  const MetricsSnapshot metrics = tcu::demo::run_demo(StopMode::Discard, false);

  EXPECT_GT(metrics.accepted_total, 0u);
  EXPECT_EQ(metrics.accepted_total,
            metrics.dispatched_total + metrics.discarded_on_stop_total);
  EXPECT_EQ(metrics.queue_depth, 0u);
  EXPECT_EQ(metrics.callback_failures_total, 0u);
}

}  // namespace
