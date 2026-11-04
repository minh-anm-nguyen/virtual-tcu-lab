#include <iostream>

#include "demo.hpp"
#include "tcu/eventbus/event_bus.hpp"

int main() {
  std::cout << "=== Virtual TCU Lab / project 01 - Thread-Safe Event Bus demo ===\n";

  std::cout << "\n--- run 1: stop(Drain) ---\n";
  const tcu::eventbus::MetricsSnapshot drained =
      tcu::demo::run_demo(tcu::eventbus::StopMode::Drain, true);

  std::cout << "\n--- run 2: stop(Discard) ---\n";
  const tcu::eventbus::MetricsSnapshot discarded =
      tcu::demo::run_demo(tcu::eventbus::StopMode::Discard, true);

  std::cout << "\n--- conservation check (contracts/metrics.md) ---\n";
  std::cout << "Drain:   accepted=" << drained.accepted_total
            << " dispatched=" << drained.dispatched_total
            << "  equal=" << (drained.accepted_total == drained.dispatched_total) << '\n';
  std::cout << "Discard: accepted=" << discarded.accepted_total
            << " dispatched=" << discarded.dispatched_total
            << " discarded=" << discarded.discarded_on_stop_total << "  equal="
            << (discarded.accepted_total ==
                discarded.dispatched_total + discarded.discarded_on_stop_total)
            << '\n';
  return 0;
}
