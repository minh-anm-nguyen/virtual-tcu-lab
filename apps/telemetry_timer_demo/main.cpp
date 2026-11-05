// Spec section 20, steps 1-2 and 5-7: periodic telemetry sampling driven by
// tcu_scheduler and delivered over the project 01 Event Bus.
//
// Nothing here is library code. The scheduler knows nothing about events and
// the bus knows nothing about timers; the integration is one callback.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <variant>

#include "tcu/eventbus/event.hpp"
#include "tcu/eventbus/event_bus.hpp"
#include "tcu/scheduler/scheduler_metrics.hpp"
#include "tcu/scheduler/timer_scheduler.hpp"

namespace {

using namespace std::chrono_literals;

struct Options {
  int interval_ms = 1000;
  int samples = 5;
  int callback_ms = 0;
  tcu::scheduler::PeriodicMode mode =
      tcu::scheduler::PeriodicMode::FixedRateSkipMissed;
  tcu::scheduler::StopMode stop = tcu::scheduler::StopMode::Drain;
};

void usage() {
  std::fputs(
      "usage: telemetry_timer_demo [--interval-ms N] [--samples N]\n"
      "                            [--callback-ms N]\n"
      "                            [--mode fixed-rate|fixed-delay]\n"
      "                            [--stop drain|discard]\n",
      stderr);
}

bool parse(int argc, char** argv, Options* out) {
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    const bool has_value = i + 1 < argc;
    if (flag == "--interval-ms" && has_value) {
      out->interval_ms = std::atoi(argv[++i]);
    } else if (flag == "--samples" && has_value) {
      out->samples = std::atoi(argv[++i]);
    } else if (flag == "--callback-ms" && has_value) {
      out->callback_ms = std::atoi(argv[++i]);
    } else if (flag == "--mode" && has_value) {
      const std::string value = argv[++i];
      if (value == "fixed-rate") {
        out->mode = tcu::scheduler::PeriodicMode::FixedRateSkipMissed;
      } else if (value == "fixed-delay") {
        out->mode = tcu::scheduler::PeriodicMode::FixedDelay;
      } else {
        return false;
      }
    } else if (flag == "--stop" && has_value) {
      const std::string value = argv[++i];
      if (value == "drain") {
        out->stop = tcu::scheduler::StopMode::Drain;
      } else if (value == "discard") {
        out->stop = tcu::scheduler::StopMode::Discard;
      } else {
        return false;
      }
    } else {
      return false;
    }
  }
  return out->interval_ms > 0 && out->samples > 0 && out->callback_ms >= 0;
}

tcu::eventbus::Event sample(double kmh) {
  tcu::eventbus::Event e;
  e.type = tcu::eventbus::event_type_of<tcu::eventbus::VehicleSpeedUpdated>();
  e.created_at = std::chrono::steady_clock::now();
  e.payload = tcu::eventbus::VehicleSpeedUpdated{kmh};
  return e;
}

void report(const tcu::scheduler::SchedulerMetrics& m) {
  std::printf(
      "scheduler.metrics started=%llu completed=%llu failed=%llu "
      "skipped=%llu cancelled=%llu queue_depth=%llu active=%llu\n",
      static_cast<unsigned long long>(m.callbacks_started_total),
      static_cast<unsigned long long>(m.callbacks_completed_total),
      static_cast<unsigned long long>(m.callbacks_failed_total),
      static_cast<unsigned long long>(m.periods_skipped_total),
      static_cast<unsigned long long>(m.timers_cancelled_total),
      static_cast<unsigned long long>(m.ready_queue_depth),
      static_cast<unsigned long long>(m.timers_active));
  std::printf("scheduler.lateness_ms count=%llu sum=%llu\n",
              static_cast<unsigned long long>(m.start_lateness_ms.count),
              static_cast<unsigned long long>(m.start_lateness_ms.sum_ms));
}

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  if (!parse(argc, argv, &opts)) {
    usage();
    return 2;
  }

  tcu::eventbus::EventBus bus(tcu::eventbus::BusConfig{});
  bus.start();

  std::atomic<int> delivered{0};
  auto subscription = bus.subscribe(
      tcu::eventbus::event_type_of<tcu::eventbus::VehicleSpeedUpdated>(),
      [&delivered](const tcu::eventbus::Event& e) {
        const auto& payload =
            std::get<tcu::eventbus::VehicleSpeedUpdated>(e.payload);
        std::printf("telemetry.sample tick=%d vehicle_speed=%.0f\n",
                    ++delivered, payload.kmh);
        std::fflush(stdout);
      });

  tcu::scheduler::Config cfg;
  cfg.workerThreads = 2;
  tcu::scheduler::TimerScheduler scheduler(cfg);
  if (scheduler.start() != tcu::scheduler::Error::None) {
    std::fputs("scheduler.start failed\n", stderr);
    return 1;
  }
  std::printf("scheduler.started workers=%zu\n", cfg.workerThreads);

  std::atomic<int> produced{0};
  std::atomic<int> rejected{0};
  const auto result = scheduler.scheduleEvery(
      std::chrono::milliseconds(opts.interval_ms), opts.mode, [&] {
        if (opts.callback_ms > 0) {
          // A deliberately slow callback, so the two periodic modes become
          // observably different (spec section 20, step 5).
          std::this_thread::sleep_for(std::chrono::milliseconds(opts.callback_ms));
        }
        if (bus.publish(sample(0.0)) != tcu::eventbus::PublishStatus::Accepted) {
          ++rejected;  // backpressure is handled, never ignored
        }
        ++produced;
      });
  if (!result) {
    std::fputs("scheduler.schedule failed\n", stderr);
    return 1;
  }

  // Wait for the requested number of samples without polling the clock: the
  // subscriber counter is the progress signal.
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(opts.interval_ms) *
                            (opts.samples + 2) +
                        std::chrono::milliseconds(opts.callback_ms) *
                            (opts.samples + 2) +
                        2s;
  while (delivered.load() < opts.samples &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }

  scheduler.stop(opts.stop);
  bus.stop(tcu::eventbus::StopMode::Drain);
  std::printf("scheduler.stopped mode=%s\n",
              opts.stop == tcu::scheduler::StopMode::Drain ? "drain" : "discard");
  if (rejected.load() > 0) {
    std::printf("eventbus.rejected count=%d\n", rejected.load());
  }
  report(scheduler.metrics());
  return delivered.load() >= opts.samples ? 0 : 1;
}
