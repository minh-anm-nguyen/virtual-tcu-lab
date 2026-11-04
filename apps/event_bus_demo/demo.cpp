#include "demo.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "tcu/eventbus/event.hpp"

namespace tcu::demo {
namespace {

using namespace tcu::eventbus;

// Only the keys approved in contracts/metrics.md. Payload values are part of
// this demo's own vocabulary, never secrets (constitution Principle V).
void emit(bool verbose, const std::string& fields) {
  if (verbose) {
    std::cout << "INFO  " << fields << std::endl;
  }
}

const char* status_name(PublishStatus status) {
  switch (status) {
    case PublishStatus::Accepted:
      return "Accepted";
    case PublishStatus::QueueFull:
      return "QueueFull";
    case PublishStatus::TimedOut:
      return "TimedOut";
    case PublishStatus::Stopped:
      return "Stopped";
    case PublishStatus::InvalidArgument:
      return "InvalidArgument";
  }
  return "Unknown";
}

Event ignition(bool on) {
  Event event{};
  event.type = event_type_of<IgnitionChanged>();
  event.created_at = std::chrono::steady_clock::now();
  event.payload = IgnitionChanged{on};
  return event;
}

// Holds the dispatcher so the queue can be filled on purpose, without sleeps.
class Gate {
 public:
  void park() {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    entered_cv_.notify_all();
    open_cv_.wait(lock, [this] { return open_; });
  }
  void wait_until_parked() {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_cv_.wait(lock, [this] { return entered_; });
  }
  void release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      open_ = true;
    }
    open_cv_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable entered_cv_;
  std::condition_variable open_cv_;
  bool entered_ = false;
  bool open_ = false;
};

}  // namespace

MetricsSnapshot run_demo(StopMode mode, bool verbose) {
  // Step 1: capacity 3, PowerManager then Telemetry, in that order.
  BusConfig config;
  config.queue_capacity = 3;
  EventBus bus(config);

  Gate gate;
  std::atomic<int> power_calls{0};

  auto power_manager = bus.subscribe(event_type_of<IgnitionChanged>(), [&](const Event& event) {
    emit(verbose, "event_id=" + std::to_string(event.id) +
                      " event_type=IgnitionChanged subscriber=PowerManager callback_status=ok");
    // The slow-handler phase of step 5: park on the third event onwards.
    if (power_calls.fetch_add(1) == 2) {
      gate.park();
    }
  });
  auto telemetry = bus.subscribe(event_type_of<IgnitionChanged>(), [&](const Event& event) {
    emit(verbose, "event_id=" + std::to_string(event.id) +
                      " event_type=IgnitionChanged subscriber=Telemetry callback_status=ok");
  });

  bus.start();
  emit(verbose, "bus_state=Running capacity=3");

  // Step 2: the vehicle simulator sends OFF -> ON -> OFF.
  for (const bool on : {false, true, false}) {
    const PublishStatus status = bus.publish(ignition(on));
    emit(verbose, std::string("event_type=IgnitionChanged publisher=VehicleSimulator") +
                      " publish_status=" + status_name(status) +
                      " queue_depth=" + std::to_string(bus.metrics().queue_depth));
  }

  // Step 5: with the dispatcher parked, fill the queue and show both policies.
  gate.wait_until_parked();
  // Fill the queue by publishing with Reject until it refuses. The dispatcher
  // is parked, so no slot can free up and the loop always terminates.
  while (bus.publish(ignition(true)) == PublishStatus::Accepted) {
  }

  const PublishStatus rejected = bus.publish(ignition(true));
  emit(verbose, std::string("publisher=VehicleSimulator policy=Reject publish_status=") +
                    status_name(rejected));

  PublishOptions blocking;
  blocking.on_full = FullQueuePolicy::BlockUntil;
  blocking.timeout = std::chrono::milliseconds(50);
  const PublishStatus timed_out = bus.publish(ignition(true), blocking);
  emit(verbose, std::string("publisher=VehicleSimulator policy=BlockUntil publish_status=") +
                    status_name(timed_out));

  // Steps 6 and 7: stop in the requested mode and print the summary.
  std::thread stopper([&] { bus.stop(mode); });
  gate.release();
  stopper.join();

  const MetricsSnapshot metrics = bus.metrics();
  emit(verbose, std::string("bus_state=Stopped mode=") +
                    (mode == StopMode::Discard ? "Discard" : "Drain") +
                    " accepted=" + std::to_string(metrics.accepted_total) +
                    " dispatched=" + std::to_string(metrics.dispatched_total) +
                    " discarded=" + std::to_string(metrics.discarded_on_stop_total) +
                    " callback_failures=" + std::to_string(metrics.callback_failures_total));
  return metrics;
}

}  // namespace tcu::demo
