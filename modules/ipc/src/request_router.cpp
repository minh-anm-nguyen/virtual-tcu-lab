#include "request_router.hpp"

#include <utility>

#include "tcu/ipc/frame_codec.hpp"

namespace tcu::ipc::detail {

bool isReservedName(std::string_view name) noexcept {
  return name.size() >= kReservedPrefix.size() &&
         name.compare(0, kReservedPrefix.size(), kReservedPrefix) == 0;
}

std::string makeErrorPayload(std::string_view code, std::string_view message) {
  // Hand-written because the library has no JSON dependency (research R-01).
  // Both inputs are library-chosen literals, never peer bytes, so there is
  // nothing to escape: code comes from the closed set of
  // contracts/wire_protocol.md and message from a fixed set of reasons.
  std::string out;
  out.reserve(32 + code.size() + message.size());
  out += "{\"code\":\"";
  out.append(code);
  out += "\",\"message\":\"";
  out.append(message);
  out += "\"}";
  return out;
}

bool RequestRouter::add(const std::string& method, Handler handler) {
  if (!handler) {
    return false;
  }
  if (!isValidName(method) || isReservedName(method)) {
    return false;
  }
  // No overwrite: a duplicate is a programming error, and silently replacing a
  // handler would make the routing table depend on registration order.
  return handlers_.emplace(method, std::move(handler)).second;
}

const RequestRouter::Handler* RequestRouter::find(const std::string& method) const {
  const auto it = handlers_.find(method);
  return it == handlers_.end() ? nullptr : &it->second;
}

}  // namespace tcu::ipc::detail
