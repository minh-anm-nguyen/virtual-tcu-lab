// Benchmark harness. NFR-06 forbids inventing numbers: this prints what it
// measured on the machine it ran on, together with the environment, so a
// result is reproducible and never mistaken for a correctness claim.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "tcu/eventbus/event.hpp"
#include "tcu/eventbus/event_bus.hpp"
#include "tcu/eventbus/metrics.hpp"

namespace {

using namespace tcu::eventbus;
using Clock = std::chrono::steady_clock;

std::string first_line_matching(const char* path, const char* key) {
  std::ifstream file(path);
  std::string line;
  while (std::getline(file, line)) {
    if (line.find(key) != std::string::npos) {
      const std::size_t colon = line.find(':');
      if (colon != std::string::npos) {
        std::string value = line.substr(colon + 1);
        const std::size_t start = value.find_first_not_of(" \t");
        return start == std::string::npos ? std::string{} : value.substr(start);
      }
    }
  }
  return "unknown";
}

std::string read_first_line(const char* path) {
  std::ifstream file(path);
  std::string line;
  std::getline(file, line);
  return line.empty() ? "unknown" : line;
}

void print_environment(std::size_t capacity, int threads, int per_thread) {
  std::cout << "--- environment (part of the result, not decoration) ---\n";
  std::cout << "cpu_model=" << first_line_matching("/proc/cpuinfo", "model name") << '\n';
  std::cout << "hardware_concurrency=" << std::thread::hardware_concurrency() << '\n';
  std::cout << "kernel=" << read_first_line("/proc/version") << '\n';
#if defined(__clang__)
  std::cout << "compiler=clang " << __clang_major__ << '.' << __clang_minor__ << '\n';
#elif defined(__GNUC__)
  std::cout << "compiler=gcc " << __GNUC__ << '.' << __GNUC_MINOR__ << '\n';
#else
  std::cout << "compiler=unknown\n";
#endif
#ifdef NDEBUG
  std::cout << "build_type=release-like (NDEBUG defined)\n";
#else
  std::cout << "build_type=debug-like (NDEBUG not defined) - numbers are NOT representative\n";
#endif
  std::cout << "queue_capacity=" << capacity << '\n';
  std::cout << "publisher_threads=" << threads << '\n';
  std::cout << "events_per_thread=" << per_thread << '\n';
}

double percentile(std::vector<std::int64_t>& sorted_us, double fraction) {
  if (sorted_us.empty()) {
    return 0.0;
  }
  const std::size_t index =
      std::min(sorted_us.size() - 1,
               static_cast<std::size_t>(fraction * static_cast<double>(sorted_us.size())));
  return static_cast<double>(sorted_us[index]);
}

}  // namespace

int main(int argc, char** argv) {
  const int threads = argc > 1 ? std::atoi(argv[1]) : 4;
  const int per_thread = argc > 2 ? std::atoi(argv[2]) : 20000;
  const std::size_t capacity = argc > 3 ? static_cast<std::size_t>(std::atol(argv[3])) : 4096;

  print_environment(capacity, threads, per_thread);

  BusConfig config;
  config.queue_capacity = capacity;
  EventBus bus(config);

  // Enqueue-to-dispatch latency, recorded by the single dispatcher thread, so
  // the vector needs no synchronization and the measurement adds no lock.
  std::vector<std::int64_t> latencies_us;
  latencies_us.reserve(static_cast<std::size_t>(threads) * per_thread);
  auto subscription =
      bus.subscribe(event_type_of<VehicleSpeedUpdated>(), [&latencies_us](const Event& event) {
        latencies_us.push_back(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - event.created_at)
                .count());
      });

  bus.start();
  PublishOptions blocking;
  blocking.on_full = FullQueuePolicy::BlockUntil;
  blocking.timeout = std::chrono::seconds(30);

  const auto started = Clock::now();
  std::vector<std::thread> publishers;
  publishers.reserve(static_cast<std::size_t>(threads));
  for (int t = 0; t < threads; ++t) {
    publishers.emplace_back([&bus, &blocking, per_thread] {
      for (int i = 0; i < per_thread; ++i) {
        Event event{};
        event.type = event_type_of<VehicleSpeedUpdated>();
        event.created_at = Clock::now();
        event.payload = VehicleSpeedUpdated{static_cast<double>(i)};
        bus.publish(std::move(event), blocking);
      }
    });
  }
  for (std::thread& publisher : publishers) {
    publisher.join();
  }
  bus.stop(StopMode::Drain);
  const auto elapsed = Clock::now() - started;

  const MetricsSnapshot metrics = bus.metrics();
  const double seconds = std::chrono::duration<double>(elapsed).count();

  std::sort(latencies_us.begin(), latencies_us.end());
  std::cout << "--- measured ---\n";
  std::cout << "wall_clock_seconds=" << seconds << '\n';
  std::cout << "accepted=" << metrics.accepted_total << " dispatched=" << metrics.dispatched_total
            << " queue_high_watermark=" << metrics.queue_high_watermark << '\n';
  std::cout << "throughput_events_per_second="
            << (seconds > 0.0 ? static_cast<double>(metrics.dispatched_total) / seconds : 0.0)
            << '\n';
  std::cout << "latency_us_p50=" << percentile(latencies_us, 0.50) << '\n';
  std::cout << "latency_us_p95=" << percentile(latencies_us, 0.95) << '\n';
  std::cout << "latency_us_p99=" << percentile(latencies_us, 0.99) << '\n';
  std::cout << "\nNote: a good benchmark is not evidence of correctness (spec section 21).\n";
  return 0;
}
