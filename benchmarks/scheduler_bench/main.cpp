// PT-01 measurement for spec section 19.4.
//
// This reports; it never gates. Spec section 6 calls the p99 target a benchmark
// and section 19.4 forbids reading a workstation run as a real-time guarantee
// (research R-18), so there is no threshold and no non-zero exit on latency.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "tcu/scheduler/scheduler_metrics.hpp"
#include "tcu/scheduler/timer_scheduler.hpp"

namespace {

using namespace std::chrono_literals;

struct Options {
  int timers = 10000;
  int duration_s = 30;
  int workers = 2;
  int max_ready = 1024;
  int interval_ms = 1000;
};

void usage() {
  std::fputs(
      "usage: scheduler_bench [--timers N] [--duration-s N] [--workers N]\n"
      "                       [--max-ready N] [--interval-ms N]\n",
      stderr);
}

bool parse(int argc, char** argv, Options* out) {
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    const bool has_value = i + 1 < argc;
    if (flag == "--timers" && has_value) {
      out->timers = std::atoi(argv[++i]);
    } else if (flag == "--duration-s" && has_value) {
      out->duration_s = std::atoi(argv[++i]);
    } else if (flag == "--workers" && has_value) {
      out->workers = std::atoi(argv[++i]);
    } else if (flag == "--max-ready" && has_value) {
      out->max_ready = std::atoi(argv[++i]);
    } else if (flag == "--interval-ms" && has_value) {
      out->interval_ms = std::atoi(argv[++i]);
    } else {
      return false;
    }
  }
  return out->timers > 0 && out->duration_s > 0 && out->workers > 0 &&
         out->max_ready > 0 && out->interval_ms > 0;
}

/// Bucket boundaries only bound a percentile; they do not pinpoint it. The
/// report says "at most N ms", which is the honest reading of a histogram.
std::string percentile(const tcu::scheduler::Histogram& h, double fraction) {
  if (h.count == 0) return "n/a";
  const std::uint64_t target =
      static_cast<std::uint64_t>(static_cast<double>(h.count) * fraction);
  std::uint64_t cumulative = 0;
  for (std::size_t i = 0; i < tcu::scheduler::Histogram::kBucketCount; ++i) {
    cumulative += h.buckets[i];
    if (cumulative >= target) {
      if (i + 1 == tcu::scheduler::Histogram::kBucketCount) {
        return std::string("over ") +
               std::to_string(tcu::scheduler::Histogram::kUpperBoundsMs.back());
      }
      return std::string("<=") +
             std::to_string(tcu::scheduler::Histogram::kUpperBoundsMs[i]);
    }
  }
  return "n/a";
}

const char* compilerId() {
#if defined(__clang__)
  return "clang " __clang_version__;
#elif defined(__GNUC__)
  return "gcc " __VERSION__;
#elif defined(_MSC_VER)
  return "msvc";
#else
  return "unknown";
#endif
}

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  if (!parse(argc, argv, &opts)) {
    usage();
    return 2;
  }

  tcu::scheduler::Config cfg;
  cfg.workerThreads = static_cast<std::size_t>(opts.workers);
  cfg.maxTimers = static_cast<std::size_t>(opts.timers) + 16;
  cfg.maxReadyTasks = static_cast<std::size_t>(opts.max_ready);

  tcu::scheduler::TimerScheduler scheduler(cfg);
  if (scheduler.start() != tcu::scheduler::Error::None) {
    std::fputs("scheduler.start failed\n", stderr);
    return 1;
  }

  const auto began = std::chrono::steady_clock::now();
  int accepted = 0;
  for (int i = 0; i < opts.timers; ++i) {
    // Spread the phases so the load is a stream, not one burst.
    const auto offset = std::chrono::milliseconds(
        (static_cast<long long>(i) * opts.interval_ms) / opts.timers);
    if (scheduler.scheduleEvery(std::chrono::milliseconds(opts.interval_ms) + offset,
                                tcu::scheduler::PeriodicMode::FixedRateSkipMissed,
                                [] {})) {
      ++accepted;
    }
  }
  const auto armed = std::chrono::steady_clock::now();

  const auto until = armed + std::chrono::seconds(opts.duration_s);
  while (std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(100ms);
  }

  const auto m = scheduler.metrics();
  scheduler.stop(tcu::scheduler::StopMode::Discard);

  const double elapsed_s =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - armed).count();
  const double arm_ms =
      std::chrono::duration<double, std::milli>(armed - began).count();

  std::printf("# scheduler_bench PT-01 (spec section 19.4)\n");
  std::printf("host.hw_threads      %u\n", std::thread::hardware_concurrency());
  std::printf("host.compiler        %s\n", compilerId());
  std::printf("host.build_type      %s\n",
#if defined(NDEBUG)
              "release-ish (NDEBUG)"
#else
              "debug (assertions on)"
#endif
  );
  std::printf("config.timers        %d requested, %d accepted\n", opts.timers, accepted);
  std::printf("config.interval_ms   %d\n", opts.interval_ms);
  std::printf("config.workers       %zu\n", cfg.workerThreads);
  std::printf("config.max_timers    %zu\n", cfg.maxTimers);
  std::printf("config.max_ready     %zu\n", cfg.maxReadyTasks);
  std::printf("arm.duration_ms      %.1f\n", arm_ms);
  std::printf("run.duration_s       %.1f\n", elapsed_s);
  std::printf("callbacks.started    %llu\n",
              static_cast<unsigned long long>(m.callbacks_started_total));
  std::printf("callbacks.rate_per_s %.0f\n",
              elapsed_s > 0.0
                  ? static_cast<double>(m.callbacks_started_total) / elapsed_s
                  : 0.0);
  std::printf("callbacks.failed     %llu\n",
              static_cast<unsigned long long>(m.callbacks_failed_total));
  std::printf("periods.skipped      %llu\n",
              static_cast<unsigned long long>(m.periods_skipped_total));
  std::printf("queue.depth_at_probe %llu of %zu\n",
              static_cast<unsigned long long>(m.ready_queue_depth),
              cfg.maxReadyTasks);
  std::printf("lateness.mean_ms     %.3f\n",
              m.start_lateness_ms.count > 0
                  ? static_cast<double>(m.start_lateness_ms.sum_ms) /
                        static_cast<double>(m.start_lateness_ms.count)
                  : 0.0);
  std::printf("lateness.p50_ms      %s\n", percentile(m.start_lateness_ms, 0.50).c_str());
  std::printf("lateness.p95_ms      %s\n", percentile(m.start_lateness_ms, 0.95).c_str());
  std::printf("lateness.p99_ms      %s\n", percentile(m.start_lateness_ms, 0.99).c_str());
  std::printf("duration.p99_ms      %s\n", percentile(m.callback_duration_ms, 0.99).c_str());
  std::printf("# peak RSS is reported by the runner, e.g. /usr/bin/time -v\n");
  std::printf("# measurement only: no latency threshold is asserted anywhere\n");
  return 0;
}
