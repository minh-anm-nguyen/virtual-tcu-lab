// IT-04 (B-19) and flow 12.13: a scheduler timer guards an IPC request, and
// exactly one side completes it.
//
// Linux only: it drives a real project 02 AF_UNIX server (research R-00 of
// specs/002-ipc-service-framework). The scheduler itself is portable.
//
// The point of the test is the rule spec sections 12.13 and 16 state and the
// library deliberately does not implement: cancel() alone cannot guarantee a
// single completion, because RunningCancelRequested means the timeout callback
// is already executing. The caller owns an atomic request state.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <string>
#include <thread>

#include "support/temp_socket_dir.hpp"
#include "tcu/ipc/ipc_client.hpp"
#include "tcu/ipc/ipc_server.hpp"
#include "tcu/ipc/ipc_types.hpp"
#include "tcu/scheduler/timer_scheduler.hpp"

using namespace std::chrono_literals;
using tcu::ipc::ClientOptions;
using tcu::ipc::ClientState;
using tcu::ipc::IpcClient;
using tcu::ipc::IpcError;
using tcu::ipc::IpcResponse;
using tcu::ipc::IpcServer;
using tcu::ipc::ServerOptions;
using tcu::scheduler::CancelResult;
using tcu::scheduler::Config;
using tcu::scheduler::Error;
using tcu::scheduler::StopMode;
using tcu::scheduler::TimerId;
using tcu::scheduler::TimerScheduler;

namespace {

/// The single-completion state of one request. It lives in the caller, not in
/// the scheduler: the library has no request concept (Principle I).
enum class RequestState : int { Pending, Replied, TimedOut };

bool claim(std::atomic<RequestState>& state, RequestState outcome) {
  RequestState expected = RequestState::Pending;
  return state.compare_exchange_strong(expected, outcome);
}

struct Fixture {
  tcu::ipc::test::TempSocketDir dir;
  IpcServer server;
  IpcClient client;

  explicit Fixture(std::chrono::milliseconds handler_delay)
      : server(makeServerOptions()), client(makeClientOptions()) {
    server.registerHandler("slow", [handler_delay](std::string) {
      std::this_thread::sleep_for(handler_delay);  // a deliberately slow peer
      IpcResponse r;
      r.payload_json = "{}";
      return r;
    });
  }

  ServerOptions makeServerOptions() {
    ServerOptions o;
    o.socket_path = dir.socketPath("scheduler.sock");
    return o;
  }

  /// IpcClient::start() connects asynchronously, so the first request can
  /// otherwise race the handshake and come back NotConnected.
  bool waitForConnected() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      if (client.state() == tcu::ipc::ClientState::Connected) return true;
      std::this_thread::yield();
    }
    return client.state() == tcu::ipc::ClientState::Connected;
  }

  ClientOptions makeClientOptions() {
    ClientOptions o;
    o.socket_path = dir.socketPath("scheduler.sock");
    // Long on purpose: the scheduler owns the deadline in this test, so the
    // IPC layer must never be the one that times out.
    o.request_timeout = 30s;
    o.reconnect_enabled = false;
    return o;
  }
};

}  // namespace

TEST(IpcDeadline, ReplyBeatsTimeout) {
  Fixture f(20ms);
  ASSERT_TRUE(f.server.start());
  ASSERT_TRUE(f.client.start());
  ASSERT_TRUE(f.waitForConnected());

  TimerScheduler s(Config{2, 64, 16});
  ASSERT_EQ(s.start(), Error::None);

  std::atomic<RequestState> state{RequestState::Pending};
  const TimerId deadline =
      s.scheduleAfter(2s, [&state] { claim(state, RequestState::TimedOut); }).id;
  ASSERT_NE(deadline, 0u);

  auto future = f.client.request("slow", "{}", 30s);
  const IpcResponse response = future.get();
  ASSERT_EQ(response.error, IpcError::None) << response.detail;
  EXPECT_TRUE(claim(state, RequestState::Replied));
  EXPECT_EQ(s.cancel(deadline), CancelResult::Cancelled);

  EXPECT_EQ(state.load(), RequestState::Replied);
  s.stop(StopMode::Discard);
  f.client.stop();
  f.server.stop();
}

TEST(IpcDeadline, TimeoutBeatsLateReply) {
  Fixture f(400ms);
  ASSERT_TRUE(f.server.start());
  ASSERT_TRUE(f.client.start());
  ASSERT_TRUE(f.waitForConnected());

  TimerScheduler s(Config{2, 64, 16});
  ASSERT_EQ(s.start(), Error::None);

  std::atomic<RequestState> state{RequestState::Pending};
  std::atomic<bool> timed_out{false};
  ASSERT_TRUE(s.scheduleAfter(50ms, [&] {
    if (claim(state, RequestState::TimedOut)) timed_out = true;
  }));

  auto future = f.client.request("slow", "{}", 30s);
  const IpcResponse response = future.get();  // the reply arrives, but late
  ASSERT_EQ(response.error, IpcError::None) << response.detail;

  EXPECT_TRUE(timed_out.load()) << "the timer won";
  EXPECT_FALSE(claim(state, RequestState::Replied))
      << "a late reply must be ignored, not applied";
  EXPECT_EQ(state.load(), RequestState::TimedOut);

  s.stop(StopMode::Drain);
  f.client.stop();
  f.server.stop();
}

TEST(IpcDeadline, SingleCompletionUnderARealRace) {  // IT-04, B-19
  // The deadline equals the handler delay, so which side wins is genuinely
  // undecided. The assertion is not who wins: it is that the totals add up.
  constexpr int kRounds = 60;
  Fixture f(15ms);
  ASSERT_TRUE(f.server.start());
  ASSERT_TRUE(f.client.start());
  ASSERT_TRUE(f.waitForConnected());

  TimerScheduler s(Config{4, 256, 64});
  ASSERT_EQ(s.start(), Error::None);

  std::atomic<int> replied{0};
  std::atomic<int> timed_out{0};
  std::atomic<int> double_completions{0};

  for (int round = 0; round < kRounds; ++round) {
    auto state = std::make_shared<std::atomic<RequestState>>(RequestState::Pending);
    const TimerId deadline = s.scheduleAfter(15ms, [state, &timed_out] {
                                  if (claim(*state, RequestState::TimedOut)) ++timed_out;
                                }).id;
    ASSERT_NE(deadline, 0u);

    auto future = f.client.request("slow", "{}", 30s);
    const IpcResponse response = future.get();
    ASSERT_EQ(response.error, IpcError::None) << response.detail;
    if (claim(*state, RequestState::Replied)) {
      ++replied;
      s.cancel(deadline);
    }
    // Whichever side lost must not be able to claim the request afterwards.
    if (claim(*state, RequestState::Replied)) ++double_completions;
  }

  // Let any still-pending timeout callback land before counting.
  s.stop(StopMode::Drain);

  EXPECT_EQ(double_completions.load(), 0) << "a request completed twice";
  EXPECT_EQ(replied.load() + timed_out.load(), kRounds)
      << "every request must reach exactly one outcome";

  f.client.stop();
  f.server.stop();
}

TEST(IpcDeadline, CancelAloneIsNotSufficient) {
  // Flow 12.13 spelled out: the timeout callback is already running, so
  // cancel() returns RunningCancelRequested and cannot un-fire it. Only the
  // atomic state keeps the outcome single.
  Fixture f(5ms);
  ASSERT_TRUE(f.server.start());
  ASSERT_TRUE(f.client.start());
  ASSERT_TRUE(f.waitForConnected());

  TimerScheduler s(Config{1, 64, 16});
  ASSERT_EQ(s.start(), Error::None);

  std::atomic<RequestState> state{RequestState::Pending};
  TimerId id = 0;
  std::atomic<bool> inside{false};
  std::atomic<bool> release{false};

  id = s.scheduleAfter(0ms, [&] {
         inside = true;
         while (!release.load()) {
           std::this_thread::yield();
         }
         claim(state, RequestState::TimedOut);
       }).id;
  ASSERT_NE(id, 0u);
  while (!inside.load()) {
    std::this_thread::yield();
  }

  EXPECT_EQ(s.cancel(id), CancelResult::RunningCancelRequested)
      << "cancel cannot stop a callback that has started";
  release = true;

  s.stop(StopMode::Drain);
  EXPECT_EQ(state.load(), RequestState::TimedOut)
      << "the cancelled-too-late timer still completed the request";
  f.client.stop();
  f.server.stop();
}
