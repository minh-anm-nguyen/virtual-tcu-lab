#pragma once

// method -> handler, plus the reserved ipc.* namespace.
//
// Lookup order is "control methods first, then handlers"
// (contracts/wire_protocol.md section 3), so an application cannot shadow the
// handshake by registering ipc.hello.

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "tcu/ipc/ipc_types.hpp"

namespace tcu::ipc::detail {

/// The transport owns every name under this prefix.
inline constexpr std::string_view kReservedPrefix = "ipc.";
inline constexpr std::string_view kMethodHello = "ipc.hello";
inline constexpr std::string_view kMethodSubscribe = "ipc.subscribe";
inline constexpr std::string_view kMethodUnsubscribe = "ipc.unsubscribe";

/// Wire error codes, contracts/wire_protocol.md section 5. Strings rather than
/// an enum because they travel on the wire and are matched by tests and logs.
inline constexpr std::string_view kCodeInvalidArgument = "INVALID_ARGUMENT";
inline constexpr std::string_view kCodeUnknownMethod = "UNKNOWN_METHOD";
inline constexpr std::string_view kCodePermissionDenied = "PERMISSION_DENIED";
inline constexpr std::string_view kCodeUnauthenticated = "UNAUTHENTICATED";
inline constexpr std::string_view kCodeUnsupportedVersion = "UNSUPPORTED_VERSION";
inline constexpr std::string_view kCodeFrameTooLarge = "FRAME_TOO_LARGE";
inline constexpr std::string_view kCodeProtocolError = "PROTOCOL_ERROR";
inline constexpr std::string_view kCodeInternalError = "INTERNAL_ERROR";
inline constexpr std::string_view kCodeShuttingDown = "SHUTTING_DOWN";

bool isReservedName(std::string_view name) noexcept;

/// Builds the payload of an Error frame: {"code":"...","message":"..."}.
/// `message` must never carry payload bytes or a credential (Principle V).
std::string makeErrorPayload(std::string_view code, std::string_view message);

class RequestRouter {
 public:
  using Handler = std::function<IpcResponse(std::string)>;

  /// False for a reserved prefix, an invalid name, a duplicate or an empty
  /// handler. Nothing is registered partially (invariant R1).
  bool add(const std::string& method, Handler handler);

  /// Null when no handler is registered for `method`.
  const Handler* find(const std::string& method) const;

  std::size_t size() const noexcept { return handlers_.size(); }

 private:
  std::unordered_map<std::string, Handler> handlers_;
};

}  // namespace tcu::ipc::detail
