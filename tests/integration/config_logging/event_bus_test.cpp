// R-20 and spec section 22: the configuration library links no bus. The demo
// installs an observer that publishes ConfigChanged{generation} on project 01,
// and the event carries the generation and nothing else - a consumer re-reads
// the snapshot rather than trusting a copy that can go stale.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "support/stack.hpp"
#include "tcu/eventbus/event_bus.hpp"

namespace tcu::itest {
namespace {

using namespace std::chrono_literals;

TEST(EventBus, ConfigChangedCarriesOnlyTheGeneration) {
  // The bus is declared BEFORE the stack so it outlives the configuration
  // service whose observer publishes to it: a notification still in flight
  // during teardown would otherwise reach a destroyed bus. ThreadSanitizer
  // caught the other order.
  eventbus::BusConfig bus_config;
  bus_config.queue_capacity = 64;
  eventbus::EventBus bus(bus_config);
  bus.start();

  Stack stack;
  ASSERT_EQ(stack.service->start(), config::LoadError::None);

  std::atomic<std::uint64_t> received{0};
  std::atomic<int> count{0};
  eventbus::Subscription subscription = bus.subscribe(
      eventbus::event_type_of<eventbus::ConfigChanged>(),
      [&](const eventbus::Event& event) {
        const auto& payload = std::get<eventbus::ConfigChanged>(event.payload);
        received.store(payload.generation);
        count.fetch_add(1);
      });

  // The observer the demo installs: publish the generation, nothing else.
  stack.service->subscribe([&bus](config::ConfigPtr current, config::ConfigPtr) {
    eventbus::Event event;
    event.type = eventbus::event_type_of<eventbus::ConfigChanged>();
    event.created_at = std::chrono::steady_clock::now();
    event.payload = eventbus::ConfigChanged{current->generation};
    bus.publish(std::move(event));
  });

  stack.writeWith("\"period_ms\": 5000", "\"period_ms\": 2000");
  const config::ReloadResult result = stack.service->reload();
  ASSERT_TRUE(result.changed);

  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline && count.load() == 0) {
    std::this_thread::yield();
  }

  EXPECT_EQ(count.load(), 1);
  EXPECT_EQ(received.load(), 2u);

  // The payload type has exactly one field, so there is no room on the bus for
  // a configuration value even by accident.
  static_assert(sizeof(eventbus::ConfigChanged) == sizeof(std::uint64_t),
                "ConfigChanged must carry only the generation");

  subscription.unsubscribe();
  bus.stop(eventbus::StopMode::Drain);
}

}  // namespace
}  // namespace tcu::itest
