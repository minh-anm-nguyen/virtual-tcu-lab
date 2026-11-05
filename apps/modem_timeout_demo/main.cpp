// Spec section 20, steps 3-4: an AT command timeout racing a simulated reply.
//
// The point of the demo is spec section 12.13: cancelling the timer is NOT
// enough to guarantee a single completion, because the timeout callback may
// already be running. The request state is an atomic, and whichever side loses
// the compare-exchange reports itself as ignored.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "tcu/scheduler/scheduler_metrics.hpp"
#include "tcu/scheduler/timer_scheduler.hpp"

namespace {

using namespace std::chrono_literals;

struct Options {
  int timeout_ms = 3000;
  int reply_ms = 2000;
  int repeat = 1;
};

void usage() {
  std::fputs(
      "usage: modem_timeout_demo [--timeout-ms N] [--reply-ms N] [--repeat N]\n",
      stderr);
}

bool parse(int argc, char** argv, Options* out) {
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    const bool has_value = i + 1 < argc;
    if (flag == "--timeout-ms" && has_value) {
      out->timeout_ms = std::atoi(argv[++i]);
    } else if (flag == "--reply-ms" && has_value) {
      out->reply_ms = std::atoi(argv[++i]);
    } else if (flag == "--repeat" && has_value) {
      out->repeat = std::atoi(argv[++i]);
    } else {
      return false;
    }
  }
  return out->timeout_ms > 0 && out->reply_ms >= 0 && out->repeat > 0;
}

enum class RequestState : int { Pending, Replied, TimedOut };

bool claim(std::atomic<RequestState>& state, RequestState outcome) {
  RequestState expected = RequestState::Pending;
  return state.compare_exchange_strong(expected, outcome);
}

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  if (!parse(argc, argv, &opts)) {
    usage();
    return 2;
  }

  tcu::scheduler::Config cfg;
  cfg.workerThreads = 2;
  cfg.maxTimers = 4096;
  tcu::scheduler::TimerScheduler scheduler(cfg);
  if (scheduler.start() != tcu::scheduler::Error::None) {
    std::fputs("scheduler.start failed\n", stderr);
    return 1;
  }
  std::printf("scheduler.started workers=%zu\n", cfg.workerThreads);

  std::atomic<int> replied{0};
  std::atomic<int> timed_out{0};
  std::atomic<int> double_completions{0};
  const bool verbose = opts.repeat <= 10;

  for (int round = 0; round < opts.repeat; ++round) {
    const int request_id = 17 + round;
    auto state = std::make_shared<std::atomic<RequestState>>(RequestState::Pending);

    if (verbose) {
      std::printf("modem.request id=%d timeout_ms=%d\n", request_id, opts.timeout_ms);
    }
    const auto timer = scheduler.scheduleAfter(
        std::chrono::milliseconds(opts.timeout_ms), [&, state, request_id] {
          if (claim(*state, RequestState::TimedOut)) {
            ++timed_out;
            if (verbose) std::printf("modem.timeout id=%d\n", request_id);
          }
        });
    if (!timer) {
      std::fputs("scheduler.schedule failed\n", stderr);
      return 1;
    }

    // The simulated modem: a reply after reply_ms, on its own thread.
    std::thread modem([&, state, request_id] {
      std::this_thread::sleep_for(std::chrono::milliseconds(opts.reply_ms));
      if (claim(*state, RequestState::Replied)) {
        ++replied;
        if (verbose) {
          std::printf("modem.reply id=%d latency_ms=%d\n", request_id, opts.reply_ms);
        }
        const auto outcome = scheduler.cancel(timer.id);
        if (verbose) {
          std::printf("scheduler.cancel id=%llu result=%s\n",
                      static_cast<unsigned long long>(timer.id),
                      outcome == tcu::scheduler::CancelResult::Cancelled
                          ? "cancelled"
                          : "running_cancel_requested");
        }
      } else if (verbose) {
        std::printf("modem.late_reply id=%d ignored=true\n", request_id);
      }
    });
    modem.join();

    // Whoever lost must not be able to complete the request afterwards.
    if (claim(*state, RequestState::Replied)) ++double_completions;
    std::fflush(stdout);
  }

  // Drain, so a timeout callback still in flight lands before the totals.
  scheduler.stop(tcu::scheduler::StopMode::Drain);
  std::printf("scheduler.stopped mode=drain\n");

  const int total = replied.load() + timed_out.load();
  std::printf("modem.summary rounds=%d replied=%d timed_out=%d completed=%d\n",
              opts.repeat, replied.load(), timed_out.load(), total);

  const auto m = scheduler.metrics();
  std::printf("scheduler.metrics started=%llu completed=%llu cancelled=%llu\n",
              static_cast<unsigned long long>(m.callbacks_started_total),
              static_cast<unsigned long long>(m.callbacks_completed_total),
              static_cast<unsigned long long>(m.timers_cancelled_total));

  if (double_completions.load() != 0) {
    std::fprintf(stderr, "FAIL: %d requests completed twice\n",
                 double_completions.load());
    return 1;
  }
  if (total != opts.repeat) {
    std::fprintf(stderr, "FAIL: %d rounds but %d completions\n", opts.repeat, total);
    return 1;
  }
  return 0;
}
