// UT-14, UT-15, B-16.
//
// No test here uses sleep_for for ordering: a sleep either makes the test slow
// or makes it flaky, and project 01's spec forbids it outright. Ordering comes
// from the queue's own blocking semantics and from std::future.

#include "bounded_frame_queue.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <thread>
#include <vector>

namespace {

using namespace tcu::can;

CanMessage frameWithId(std::uint32_t id) {
  CanMessage msg;
  msg.id = id;
  msg.length = 8;
  return msg;
}

TEST(Queue, PreservesFifo) {
  BoundedFrameQueue queue(4);
  for (std::uint32_t id = 1; id <= 4; ++id) {
    EXPECT_TRUE(queue.push(frameWithId(id)));
  }
  for (std::uint32_t id = 1; id <= 4; ++id) {
    const auto msg = queue.pop();
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->id, id);
  }
}

// UT-14, B-16. The ARRIVING frame is the one discarded - DropNewest, not
// DropOldest. Spec section 17.4 rejects DropOldest for V1 because it "can drop
// pending state transitions/commands".
TEST(Queue, DropNewestWhenFull) {
  BoundedFrameQueue queue(2);
  EXPECT_TRUE(queue.push(frameWithId(0x100)));
  EXPECT_TRUE(queue.push(frameWithId(0x101)));

  EXPECT_FALSE(queue.push(frameWithId(0x999))) << "the third push is refused";
  EXPECT_EQ(queue.depth(), 2u) << "depth never exceeds capacity";

  EXPECT_EQ(queue.pop()->id, 0x100u);
  EXPECT_EQ(queue.pop()->id, 0x101u);
}

TEST(Queue, DepthNeverExceedsCapacityUnderRepeatedOverflow) {
  BoundedFrameQueue queue(8);
  for (int i = 0; i < 1000; ++i) {
    queue.push(frameWithId(0x101));
    EXPECT_LE(queue.depth(), queue.capacity());
  }
  EXPECT_EQ(queue.depth(), 8u);
}

TEST(Queue, PushAfterPopReusesTheSlot) {
  // A ring that leaked a slot per cycle would quietly shrink to zero capacity.
  BoundedFrameQueue queue(2);
  for (int cycle = 0; cycle < 100; ++cycle) {
    EXPECT_TRUE(queue.push(frameWithId(0x100)));
    EXPECT_TRUE(queue.push(frameWithId(0x101)));
    EXPECT_FALSE(queue.push(frameWithId(0x102)));
    EXPECT_TRUE(queue.pop().has_value());
    EXPECT_TRUE(queue.pop().has_value());
    EXPECT_EQ(queue.depth(), 0u);
  }
}

TEST(Queue, PopWakesOnClose) {
  BoundedFrameQueue queue(4);
  auto blocked = std::async(std::launch::async, [&queue] { return queue.pop(); });
  queue.close();
  EXPECT_FALSE(blocked.get().has_value()) << "a closed, empty queue returns nullopt";
}

// Closing is not discarding: spec section 13.8 step 4 drains what is queued.
TEST(Queue, CloseStillDrainsQueuedFrames) {
  BoundedFrameQueue queue(4);
  queue.push(frameWithId(0x100));
  queue.push(frameWithId(0x101));
  queue.close();

  EXPECT_EQ(queue.pop()->id, 0x100u);
  EXPECT_EQ(queue.pop()->id, 0x101u);
  EXPECT_FALSE(queue.pop().has_value());
}

TEST(Queue, PushAfterCloseIsRefused) {
  BoundedFrameQueue queue(4);
  queue.close();
  EXPECT_FALSE(queue.push(frameWithId(0x100)));
  EXPECT_EQ(queue.depth(), 0u);
}

TEST(Queue, TryPopDoesNotBlockOnEmpty) {
  // The bounded shutdown drain needs this: the worker must stop at the
  // deadline rather than wait for one more frame (research R-19).
  BoundedFrameQueue queue(4);
  EXPECT_FALSE(queue.tryPop().has_value());

  queue.push(frameWithId(0x100));
  EXPECT_EQ(queue.tryPop()->id, 0x100u);
  EXPECT_FALSE(queue.tryPop().has_value());
}

// UT-15. Several producers, one consumer: nothing lost, nothing duplicated,
// and the accounting adds up - the invariant contracts/metrics.md states as
// enqueued + dropped == read.
TEST(Queue, MultipleProducersNoRace) {
  constexpr int kProducers = 4;
  constexpr int kPerProducer = 500;
  BoundedFrameQueue queue(64);

  std::atomic<int> accepted{0};
  std::atomic<int> dropped{0};
  std::atomic<int> consumed{0};

  auto consumer = std::async(std::launch::async, [&] {
    while (queue.pop().has_value()) {
      consumed.fetch_add(1);
    }
  });

  std::vector<std::thread> producers;
  producers.reserve(kProducers);
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&] {
      for (int i = 0; i < kPerProducer; ++i) {
        if (queue.push(frameWithId(0x101))) {
          accepted.fetch_add(1);
        } else {
          dropped.fetch_add(1);
        }
      }
    });
  }
  for (auto& t : producers) {
    t.join();
  }
  queue.close();
  consumer.get();

  EXPECT_EQ(accepted.load() + dropped.load(), kProducers * kPerProducer)
      << "every push is either accepted or counted as dropped";
  EXPECT_EQ(consumed.load(), accepted.load()) << "no frame is lost or duplicated";
  EXPECT_LE(queue.depth(), queue.capacity());
}

TEST(Queue, ZeroCapacityIsCoercedToOne) {
  // The loader rejects a capacity of 0 (NFR-02), but the ring must not divide
  // by zero if one ever reaches it.
  BoundedFrameQueue queue(0);
  EXPECT_EQ(queue.capacity(), 1u);
  EXPECT_TRUE(queue.push(frameWithId(0x100)));
  EXPECT_FALSE(queue.push(frameWithId(0x101)));
}

}  // namespace
