// T037: the RX queue is bounded, drops the newest frame when full, counts every
// drop monotonically, and preserves order (invariants V-01..V-04, B-13).

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

#include "rx_queue.hpp"

namespace {

using tcu::can::CanMessage;
using tcu::signal::detail::RxQueue;

CanMessage frameOf(std::uint32_t id) {
  CanMessage message;
  message.id = id;
  message.length = 8;
  message.data = {static_cast<std::uint8_t>(id & 0xFF), 0, 0, 0, 0, 0, 0, 0};
  return message;
}

TEST(Queue_DropsNewestWhenFull, ProducerReturnsImmediately) {
  RxQueue queue(4);
  for (std::uint32_t i = 0; i < 4; ++i) {
    EXPECT_TRUE(queue.push(frameOf(i))) << i;
  }
  // Full: the next push is refused rather than blocking the RX thread.
  const auto before = std::chrono::steady_clock::now();
  EXPECT_FALSE(queue.push(frameOf(99)));
  EXPECT_LT(std::chrono::steady_clock::now() - before, std::chrono::milliseconds{100});
  EXPECT_EQ(queue.dropped(), 1u);
  EXPECT_EQ(queue.depth(), 4u);
}

TEST(Queue_DepthNeverExceedsCapacity, UnderABurst) {
  RxQueue queue(16);
  for (std::uint32_t i = 0; i < 1000; ++i) {
    queue.push(frameOf(i));
    ASSERT_LE(queue.depth(), queue.capacity()) << i;
  }
  EXPECT_EQ(queue.depth(), 16u);
  EXPECT_EQ(queue.dropped(), 1000u - 16u);
  EXPECT_LE(queue.highWaterMark(), queue.capacity());
}

TEST(Queue_DropCountersMonotonic, NeverDecreases) {
  RxQueue queue(2);
  std::uint64_t previous = 0;
  for (std::uint32_t i = 0; i < 50; ++i) {
    queue.push(frameOf(i));
    const std::uint64_t now = queue.dropped();
    ASSERT_GE(now, previous) << i;
    previous = now;
  }
  EXPECT_GT(previous, 0u);
}

TEST(Queue_PreservesOrder, FifoForOneBus) {
  RxQueue queue(8);
  for (std::uint32_t i = 0; i < 8; ++i) queue.push(frameOf(i));
  for (std::uint32_t i = 0; i < 8; ++i) {
    const auto message = queue.pop();
    ASSERT_TRUE(message.has_value()) << i;
    EXPECT_EQ(message->id, i);
  }
}

TEST(Queue_PreservesOrder, DroppingTheNewestKeepsTheOldest) {
  // Drop-newest means the queue holds the OLDEST frames, which is what makes
  // the surviving sequence contiguous rather than full of holes.
  RxQueue queue(3);
  for (std::uint32_t i = 0; i < 10; ++i) queue.push(frameOf(i));
  for (std::uint32_t i = 0; i < 3; ++i) {
    const auto message = queue.pop();
    ASSERT_TRUE(message.has_value());
    EXPECT_EQ(message->id, i);
  }
}

TEST(Queue_CloseWakesAWaitingWorker, NoHangOnShutdown) {
  RxQueue queue(4);
  std::optional<CanMessage> popped;
  std::thread worker([&] { popped = queue.pop(); });
  queue.close();
  worker.join();
  EXPECT_FALSE(popped.has_value()) << "a closed, empty queue returns nothing rather than blocking";
}

TEST(Queue_StopAcceptingRefusesNewFramesButKeepsTheBacklog, DrainOrder) {
  RxQueue queue(4);
  queue.push(frameOf(1));
  queue.push(frameOf(2));
  queue.stopAccepting();
  EXPECT_FALSE(queue.push(frameOf(3)));
  EXPECT_EQ(queue.depth(), 2u) << "the backlog survives so it can be drained";

  queue.close();
  EXPECT_TRUE(queue.pop().has_value());
  EXPECT_TRUE(queue.pop().has_value());
  EXPECT_FALSE(queue.pop().has_value());
}

TEST(Queue_DiscardPendingIsCounted, ForTheStopPolicy) {
  RxQueue queue(8);
  for (std::uint32_t i = 0; i < 5; ++i) queue.push(frameOf(i));
  queue.discardPending();
  EXPECT_EQ(queue.depth(), 0u);
  EXPECT_EQ(queue.discarded(), 5u);
}

TEST(Queue_ConcurrentProducersStayBounded, FourThreads) {
  RxQueue queue(64);
  std::vector<std::thread> producers;
  for (int t = 0; t < 4; ++t) {
    producers.emplace_back([&] {
      for (std::uint32_t i = 0; i < 2000; ++i) queue.push(frameOf(i));
    });
  }
  std::thread consumer([&] {
    for (int i = 0; i < 1000; ++i) {
      if (!queue.pop().has_value()) break;
    }
  });
  for (auto& producer : producers) producer.join();
  queue.close();
  consumer.join();

  EXPECT_LE(queue.depth(), queue.capacity());
  EXPECT_LE(queue.highWaterMark(), queue.capacity());
}

}  // namespace
