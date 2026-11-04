#pragma once

// Client side of the IPC framework: one connection to an AF_UNIX pathname
// socket, request/response correlation with deadlines, subscriptions that
// survive a reconnect, and a bounded outgoing queue.
//
// Contract: specs/002-ipc-service-framework/contracts/ipc_api.md
//
// Thread safety: request(), subscribe(), stop(), state() and metrics() are
// callable from any thread. A response callback or a notification callback runs
// on the executor, never on the I/O thread (spec section 6).

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>

#include "tcu/ipc/ipc_types.hpp"

namespace tcu::ipc {

class IpcClient {
 public:
  explicit IpcClient(ClientOptions options);
  ~IpcClient();

  IpcClient(const IpcClient&) = delete;
  IpcClient& operator=(const IpcClient&) = delete;

  /// Starts the I/O and executor threads. `true` does NOT mean connected:
  /// Connected is reached only after the ipc.hello ACK (spec section 8.1).
  bool start();

  /// Always returns a valid future, resolved exactly once (invariant S1). The
  /// deadline starts now, before the frame is queued (invariant S2). An RPC is
  /// sent at most once per call - v1 never replays (B-20).
  std::future<IpcResponse> request(std::string method, std::string payload_json,
                                   std::chrono::milliseconds timeout);

  /// Registers interest in `topic` for the lifetime of this client and
  /// re-subscribes automatically after every reconnect (research R-07).
  /// The callback runs on the executor thread.
  void subscribe(std::string topic, std::function<void(std::string)> callback);

  /// Resolves every pending request once with Cancelled, joins both threads and
  /// closes every FD exactly once. Idempotent and bounded. Must not be called
  /// from a callback (that would self-join).
  void stop();

  ClientState state() const;
  IpcMetricsSnapshot metrics() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tcu::ipc
