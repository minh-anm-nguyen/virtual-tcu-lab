// can_bench - latency characterization for project 05.
//
// Spec section 18.4 is unusually explicit about what this is allowed to claim,
// and the wording is the design: "Record host CPU model, Linux kernel,
// compiler/build flags, queue capacity, frame rates, number of consumers, and
// run duration. Report p50/p95/p99 latency, max resident memory, total drops,
// and CPU usage. A 100 frames/s smoke scenario is illustrative; do not make
// universal performance claims from one laptop."
//
// So this program ASSERTS NOTHING. It prints the conditions next to the
// numbers, because a latency figure without its conditions is not a
// measurement. NFR-05 forbids inventing a target, and there is none here.
//
// It runs on FakeCanTransport, which measures the application pipeline -
// receipt to decoded - and deliberately not the kernel. Measuring the kernel
// path on vcan would characterise vcan, and spec section 15 is clear that vcan
// timing says nothing about a real bus.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include "tcu/can/can_config.hpp"
#include "tcu/can/can_metrics.hpp"
#include "tcu/can/can_service.hpp"
#include "tcu/can/fake_can_transport.hpp"
#include "tcu/can/frame_catalog.hpp"

namespace {

using namespace tcu::can;
using namespace std::chrono_literals;

/// First match of `prefix` in a /proc file, with the label stripped.
std::string procField(const std::string& path, const std::string& prefix) {
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind(prefix, 0) == 0) {
      const auto colon = line.find(':');
      if (colon == std::string::npos) {
        return line;
      }
      const auto value = line.substr(colon + 1);
      const auto first = value.find_first_not_of(" \t");
      return first == std::string::npos ? std::string{} : value.substr(first);
    }
  }
  return "unknown";
}

std::string compilerDescription() {
#if defined(__clang__)
  return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
  return std::string("gcc ") + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__);
#else
  return "unknown";
#endif
}

std::string buildDescription() {
  std::string flags;
#ifndef NDEBUG
  flags += "assertions-on ";
#else
  flags += "NDEBUG ";
#endif
#if defined(__SANITIZE_ADDRESS__)
  flags += "asan ";
#endif
#if defined(__SANITIZE_THREAD__)
  flags += "tsan ";
#endif
  return flags;
}

/// Peak resident set size as the kernel recorded it.
std::string peakResident() { return procField("/proc/self/status", "VmHWM"); }

/// /proc/version is one free-form line, not a key-value file, so splitting it
/// on a colon mangles the timestamp inside it. Read the whole line.
std::string kernelVersion() {
  std::ifstream in("/proc/version");
  std::string line;
  return std::getline(in, line) ? line : std::string("unknown");
}

/// The bucket boundaries CanMetricsStore::latencyBucket uses: bucket 0 is
/// "under a millisecond", bucket i covers [2^(i-1), 2^i) ms, and the last one
/// is open-ended.
std::string bucketLabel(std::size_t index) {
  if (index == 0) {
    return "<1ms";
  }
  if (index + 1 == kLatencyBucketCount) {
    return ">=" + std::to_string(1u << (kLatencyBucketCount - 2)) + "ms";
  }
  const auto low = index == 1 ? 1u : (1u << (index - 1));
  const auto high = 1u << index;
  return std::to_string(low) + "-" + std::to_string(high) + "ms";
}

/// A percentile read off the histogram. Reported as the bucket it falls in,
/// never as a single number pretending to more precision than buckets have.
std::string percentile(const std::array<std::uint64_t, kLatencyBucketCount>& buckets,
                       double fraction) {
  std::uint64_t total = 0;
  for (const auto count : buckets) {
    total += count;
  }
  if (total == 0) {
    return "no samples";
  }

  const auto target = static_cast<std::uint64_t>(static_cast<double>(total) * fraction);
  std::uint64_t running = 0;
  for (std::size_t i = 0; i < buckets.size(); ++i) {
    running += buckets[i];
    if (running >= target) {
      return bucketLabel(i);
    }
  }
  return bucketLabel(buckets.size() - 1);
}

std::string configText(std::size_t queue_capacity) {
  std::ostringstream out;
  out << R"json({
  "schema_version": 1,
  "mode": "lab_only",
  "can": {
    "interface": "vcan0",
    "format": "classic_standard_11bit",
    "rx_queue_capacity": )json"
      << queue_capacity << R"json(,
    "health_check_ms": 50,
    "drain_deadline_ms": 500,
    "reconnect": { "initial_ms": 250, "max_ms": 5000, "jitter_percent": 10 },
    "allowed_rx_ids": [256, 257, 258, 259, 272, 513]
  },
  "signals": {
    "ignition":          { "can_id": 256, "timeout_ms": 3000 },
    "speed_kmh":         { "can_id": 257, "timeout_ms": 500  },
    "battery_voltage_v": { "can_id": 258, "timeout_ms": 3000 },
    "battery_soc_pct":   { "can_id": 258, "timeout_ms": 3000 },
    "door_state":        { "can_id": 259, "timeout_ms": 3000 },
    "heartbeat":         { "can_id": 272, "timeout_ms": 3000 },
    "ecu_status":        { "can_id": 272, "timeout_ms": 3000 }
  },
  "simulator": { "speed_period_ms": 100, "heartbeat_period_ms": 1000, "fixed_seed": 42 }
})json";
  return out.str();
}

}  // namespace

int main(int argc, char** argv) {
  int rate_per_second = 100;  // section 18.4 calls 100 f/s "illustrative"
  int duration_seconds = 10;
  std::size_t queue_capacity = 1024;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      std::cout << "usage: can_bench [--rate N] [--duration SECONDS] [--queue N]\n\n"
                << "Characterizes the receipt-to-decoded path and prints the conditions\n"
                << "alongside the numbers. Asserts nothing: spec section 18.4 and NFR-05\n"
                << "forbid turning one host's figures into a performance claim.\n";
      return 0;
    }
    if (arg == "--rate" && i + 1 < argc) {
      rate_per_second = std::stoi(argv[++i]);
    } else if (arg == "--duration" && i + 1 < argc) {
      duration_seconds = std::stoi(argv[++i]);
    } else if (arg == "--queue" && i + 1 < argc) {
      queue_capacity = static_cast<std::size_t>(std::stoul(argv[++i]));
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return 2;
    }
  }

  if (rate_per_second <= 0 || duration_seconds <= 0 || queue_capacity == 0) {
    std::cerr << "rate, duration and queue must all be positive\n";
    return 2;
  }

  CanLabConfig config;
  const auto load = parseCanLabConfig(configText(queue_capacity), config);
  if (!load) {
    std::cerr << "internal: the benchmark's own configuration was rejected at " << load.pointer
              << ": " << load.publicReason << "\n";
    return 1;
  }

  auto fake = std::make_unique<FakeCanTransport>();
  auto* transport = fake.get();

  CanServiceDeps deps;  // real clock: this is a timing measurement
  deps.transport = std::move(fake);
  CanService service(std::move(deps));

  if (!service.start(config)) {
    std::cerr << "internal: the service refused to start\n";
    return 1;
  }

  const auto interval = std::chrono::nanoseconds(1000000000LL / rate_per_second);
  const std::int64_t planned = static_cast<std::int64_t>(rate_per_second) * duration_seconds;

  CanMessage speed;
  speed.id = 0x101;
  speed.length = 8;
  speed.data = {0x70, 0x17, 0, 0, 0, 0, 0, 0};

  const auto started = std::chrono::steady_clock::now();
  auto next = started;
  for (std::int64_t i = 0; i < planned; ++i) {
    next += interval;
    std::this_thread::sleep_until(next);
    // Stamped at the moment of "arrival", so the measured interval is exactly
    // receipt-to-decoded and nothing else.
    speed.received_at = std::chrono::steady_clock::now();
    transport->pushFrame(speed);
  }

  // Let the pipeline finish what it was handed before reading the numbers.
  const auto drain_deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < drain_deadline &&
         service.metrics().can_decoded_total + service.metrics().can_rx_queue_drop_total <
             static_cast<std::uint64_t>(planned)) {
    std::this_thread::sleep_for(10ms);
  }
  const auto elapsed = std::chrono::steady_clock::now() - started;

  const auto m = service.metrics();
  service.stop();

  std::cout << "can_bench - project 05 receipt-to-decoded characterization\n";
  std::cout << "================================================================\n\n";

  std::cout << "Conditions (spec section 18.4 requires these next to the numbers)\n";
  std::cout << "  host CPU             : " << procField("/proc/cpuinfo", "model name") << "\n";
  std::cout << "  kernel               : " << kernelVersion() << "\n";
  std::cout << "  compiler             : " << compilerDescription() << "\n";
  std::cout << "  build                : " << buildDescription() << "\n";
  std::cout << "  transport            : FakeCanTransport (application path, not the kernel)\n";
  std::cout << "  decode workers       : 1 (ADR-03)\n";
  std::cout << "  queue capacity       : " << m.can_queue_capacity << "\n";
  std::cout << "  requested rate       : " << rate_per_second << " frames/s\n";
  std::cout << "  requested duration   : " << duration_seconds << " s\n";
  std::cout << "  frames offered       : " << planned << "\n";
  std::cout << "  wall-clock elapsed   : "
            << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms\n";
  std::cout << "  peak resident (VmHWM): " << peakResident() << "\n\n";

  std::cout << "Results\n";
  std::cout << "  frames read          : " << m.can_rx_read_total << "\n";
  std::cout << "  frames decoded       : " << m.can_decoded_total << "\n";
  std::cout << "  signal updates       : " << m.signal_update_total << "\n";
  std::cout << "  queue drops          : " << m.can_rx_queue_drop_total << "\n";
  std::cout << "  rejected frames      : " << m.can_rx_invalid_total << "\n";
  std::cout << "  final queue depth    : " << m.can_queue_depth << "\n\n";

  std::cout << "Receipt-to-decoded latency (histogram buckets, not interpolated)\n";
  std::cout << "  p50                  : " << percentile(m.can_decode_latency_buckets, 0.50)
            << "\n";
  std::cout << "  p95                  : " << percentile(m.can_decode_latency_buckets, 0.95)
            << "\n";
  std::cout << "  p99                  : " << percentile(m.can_decode_latency_buckets, 0.99)
            << "\n\n";

  std::cout << "  distribution:\n";
  for (std::size_t i = 0; i < m.can_decode_latency_buckets.size(); ++i) {
    if (m.can_decode_latency_buckets[i] == 0) {
      continue;
    }
    std::cout << "    " << std::setw(10) << bucketLabel(i) << " : "
              << m.can_decode_latency_buckets[i] << "\n";
  }

  std::cout << "\nNOTE: these figures describe THIS host under THESE conditions and nothing\n"
            << "else. NFR-05 states no latency target and section 18.4 forbids universal\n"
            << "performance claims from one machine. vcan also cannot characterize a real\n"
            << "CAN bus at all (section 15), which is why the kernel path is excluded here.\n";

  // The exit code reflects whether the run was valid, never whether it was fast.
  return 0;
}
