#pragma once

// Public value types of the IPC framework.
// Contract: specs/002-ipc-service-framework/contracts/ipc_api.md
// Entities: specs/002-ipc-service-framework/data-model.md section 3

#include <sys/types.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tcu::ipc {

/// Outcome of one request. Exactly the ten values of spec section 6; a remote
/// error code travels in IpcResponse::detail, not as a new enumerator (R-11).
enum class IpcError {
  None,
  NotConnected,
  Timeout,
  Cancelled,
  QueueFull,
  PermissionDenied,
  ProtocolError,
  PeerClosed,
  RemoteError,
  ShuttingDown
};

/// Client connection state, spec section 7.1.
enum class ClientState { Stopped, Connecting, Connected, Backoff, Stopping };

inline const char* toString(IpcError error) noexcept {
  switch (error) {
    case IpcError::None: return "None";
    case IpcError::NotConnected: return "NotConnected";
    case IpcError::Timeout: return "Timeout";
    case IpcError::Cancelled: return "Cancelled";
    case IpcError::QueueFull: return "QueueFull";
    case IpcError::PermissionDenied: return "PermissionDenied";
    case IpcError::ProtocolError: return "ProtocolError";
    case IpcError::PeerClosed: return "PeerClosed";
    case IpcError::RemoteError: return "RemoteError";
    case IpcError::ShuttingDown: return "ShuttingDown";
  }
  return "Unknown";
}

inline const char* toString(ClientState state) noexcept {
  switch (state) {
    case ClientState::Stopped: return "Stopped";
    case ClientState::Connecting: return "Connecting";
    case ClientState::Connected: return "Connected";
    case ClientState::Backoff: return "Backoff";
    case ClientState::Stopping: return "Stopping";
  }
  return "Unknown";
}

struct IpcResponse {
  IpcError error{IpcError::None};
  /// Opaque application bytes. The library never parses them (research R-01).
  std::string payload_json;
  /// A wire error code or a short local reason. Never a payload excerpt
  /// (invariant P2, Principle V).
  std::string detail;
};

struct ClientOptions {
  std::string socket_path{"/tmp/virtual-tcu/vehicle.sock"};
  std::chrono::milliseconds request_timeout{2000};
  std::size_t max_pending_requests{128};
  std::size_t max_tx_messages{256};
  std::size_t max_tx_bytes{1024 * 1024};
  bool reconnect_enabled{true};
  /// Sent in ipc.hello. Empty means "use the pid".
  std::string client_id{};
};

struct ServerOptions {
  std::string socket_path{"/tmp/virtual-tcu/vehicle.sock"};
  /// Empty means owner only, i.e. {geteuid()} - never "anyone" (invariant P3).
  std::vector<uid_t> allowed_uids{};
  std::size_t max_tx_messages{256};
  std::size_t max_tx_bytes{1024 * 1024};
  std::size_t max_sessions{32};
};

/// Fixed buckets in milliseconds; the last one is the overflow bucket.
/// contracts/metrics.md, "Histogram".
struct LatencyHistogram {
  static constexpr std::size_t kBucketCount = 12;
  static constexpr std::uint32_t kUpperBoundsMs[kBucketCount - 1] = {1,   2,   5,   10,  25,  50,
                                                                     100, 250, 500, 1000, 2000};
  std::uint64_t buckets[kBucketCount]{};
  std::uint64_t count{0};
  std::uint64_t sum_ms{0};
};

/// Spec section 11, plus the two terms its own conservation sentence needs
/// (contracts/metrics.md).
struct IpcMetricsSnapshot {
  std::uint64_t connections_active{0};
  std::uint64_t connect_attempts_total{0};
  std::uint64_t reconnect_total{0};
  std::uint64_t requests_created_total{0};
  std::uint64_t requests_sent_total{0};
  std::uint64_t requests_completed_total{0};
  std::uint64_t requests_failed_total{0};
  std::uint64_t request_timeouts_total{0};
  std::uint64_t queue_rejects_total{0};
  std::uint64_t protocol_errors_total{0};
  std::uint64_t late_responses_total{0};
  std::uint64_t tx_queue_bytes{0};
  LatencyHistogram request_latency_ms{};
};

}  // namespace tcu::ipc
