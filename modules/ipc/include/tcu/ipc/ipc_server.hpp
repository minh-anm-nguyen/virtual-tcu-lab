#pragma once

// Server side of the IPC framework: accepts sessions over an AF_UNIX pathname
// socket, authorizes the peer, routes requests to handlers off the I/O thread,
// and publishes notifications to subscribers.
//
// Contract: specs/002-ipc-service-framework/contracts/ipc_api.md
//
// Thread safety: registerHandler() before start(); publishNotification(),
// stop() and metrics() are callable from any thread. A handler runs on the
// executor, never on the I/O thread (spec section 4.1).

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "tcu/ipc/ipc_types.hpp"

namespace tcu::ipc {

class IpcServer {
 public:
  /// Receives a copy of the request payload and returns the response.
  /// Returning error != None produces an Error frame; throwing produces
  /// INTERNAL_ERROR and the server stays alive (B-11).
  using Handler = std::function<IpcResponse(std::string)>;

  explicit IpcServer(ServerOptions options);
  /// Convenience overload so spec section 6's snippet stays valid.
  explicit IpcServer(std::string socket_path);
  ~IpcServer();

  IpcServer(const IpcServer&) = delete;
  IpcServer& operator=(const IpcServer&) = delete;

  /// False for a reserved `ipc.` prefix, an invalid name, a duplicate, an
  /// empty handler, or a call after start() (invariant R1).
  bool registerHandler(std::string method, Handler handler);

  /// Creates the runtime directory 0700, binds and listens, starts the I/O and
  /// executor threads. False leaves nothing behind - see contracts/ipc_api.md
  /// for every refusal, including a live socket already on the path.
  bool start();

  /// Enqueues the notification for every session subscribed to `topic`.
  /// Best-effort and non-blocking: a session at its TX cap has it dropped and
  /// counted (spec section 8.7, research R-08).
  void publishNotification(std::string topic, std::string payload_json);

  /// Closes every session exactly once, joins both threads, and unlinks the
  /// socket only if this server created it. Idempotent and bounded.
  void stop();

  IpcMetricsSnapshot metrics() const;

  /// Live sessions. Exposed for the tests that assert isolation (B-13).
  std::size_t sessionCount() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tcu::ipc
