#include "tcu/ipc/ipc_server.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "client_session.hpp"
#include "executor.hpp"
#include "log.hpp"
#include "metrics.hpp"
#include "request_router.hpp"
#include "tcu/ipc/frame_codec.hpp"
#include "unix_socket.hpp"

namespace tcu::ipc {
namespace {

using detail::ClientSession;
using detail::Executor;
using detail::LogFields;
using detail::LogLevel;
using detail::RequestRouter;
using detail::UnixSocket;

constexpr int kPollTimeoutMs = 50;
constexpr const char* kService = "ipc_server";
/// Handler threads. More than one so a slow handler cannot starve the other
/// sessions; small enough that the demo stays easy to reason about.
constexpr std::size_t kHandlerThreads = 4;

/// Extracts a flat string value for `key` from a small JSON object without a
/// JSON library (research R-01). Control payloads are produced by this same
/// library, so only the shapes of contracts/wire_protocol.md section 3 need to
/// be understood; anything else yields an empty string and the caller rejects.
std::string flatStringField(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  const std::size_t k = json.find(needle);
  if (k == std::string::npos) {
    return {};
  }
  const std::size_t colon = json.find(':', k + needle.size());
  if (colon == std::string::npos) {
    return {};
  }
  const std::size_t open = json.find('"', colon);
  if (open == std::string::npos) {
    return {};
  }
  const std::size_t close = json.find('"', open + 1);
  if (close == std::string::npos) {
    return {};
  }
  return json.substr(open + 1, close - open - 1);
}

/// Same idea for an unquoted integer, used by ipc.hello's "version".
long flatIntField(const std::string& json, const std::string& key, long fallback) {
  const std::string needle = "\"" + key + "\"";
  const std::size_t k = json.find(needle);
  if (k == std::string::npos) {
    return fallback;
  }
  const std::size_t colon = json.find(':', k + needle.size());
  if (colon == std::string::npos) {
    return fallback;
  }
  std::size_t i = colon + 1;
  while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) {
    ++i;
  }
  long value = 0;
  bool any = false;
  while (i < json.size() && json[i] >= '0' && json[i] <= '9') {
    value = value * 10 + (json[i] - '0');
    any = true;
    ++i;
  }
  return any ? value : fallback;
}

}  // namespace

struct IpcServer::Impl {
  explicit Impl(ServerOptions opts)
      : options(std::move(opts)),
        // Several worker threads, as spec section 7 specifies for the server:
        // with one, a slow handler on one session starves every other session
        // (measured by tests/fault_injection/ipc/stress_test.cpp).
        executor(options.max_sessions * 8 + 64, kHandlerThreads) {}

  ServerOptions options;
  RequestRouter router;
  detail::Metrics metrics;
  Executor executor;

  UnixSocket listener;
  bool socket_created{false};

  std::atomic<bool> running{false};
  std::atomic<bool> stopping{false};
  std::thread io_thread;
  std::once_flag stop_once;

  // Sessions live on the I/O thread; publishNotification() hands topics over
  // through this queue instead of touching them (invariant S14).
  std::mutex pending_mutex;
  std::vector<std::pair<std::string, std::string>> pending_notifications;

  std::map<std::uint64_t, std::unique_ptr<ClientSession>> sessions;
  std::uint64_t next_session_id{1};

  // Replies produced on the executor, handed back to the I/O thread.
  struct OutFrame {
    std::uint64_t session_id{0};
    std::string frame;
  };
  std::mutex out_mutex;
  std::vector<OutFrame> out_frames;

  void run();
  bool prepareSocket();
  void acceptOne();
  void serviceSession(ClientSession& session, bool& close_me);
  void handleFrame(ClientSession& session, const Frame& frame, bool& close_me);
  void dispatchHandler(std::uint64_t session_id, const RequestRouter::Handler& handler,
                       std::string method, std::uint64_t request_id, std::string payload);
  void queueOut(std::uint64_t session_id, std::string frame);
  void drainOut();
  void drainNotifications();
  void sendError(ClientSession& session, std::uint64_t request_id, const std::string& method,
                 std::string_view code, std::string_view message);
  void closeSession(std::uint64_t id, const char* reason);
  void logEvent(LogLevel level, const char* event, std::uint64_t session_id,
                const std::string& method, const std::string& reason);
};

IpcServer::IpcServer(ServerOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}

IpcServer::IpcServer(std::string socket_path) {
  ServerOptions o;
  o.socket_path = std::move(socket_path);
  impl_ = std::make_unique<Impl>(std::move(o));
}

IpcServer::~IpcServer() { stop(); }

bool IpcServer::registerHandler(std::string method, Handler handler) {
  if (impl_->running.load()) {
    return false;
  }
  return impl_->router.add(method, std::move(handler));
}

std::size_t IpcServer::sessionCount() const {
  return static_cast<std::size_t>(impl_->metrics.connections_active.load());
}

IpcMetricsSnapshot IpcServer::metrics() const { return impl_->metrics.snapshot(); }

bool IpcServer::start() {
  if (impl_->running.load()) {
    return false;
  }
  if (impl_->options.max_tx_messages == 0 || impl_->options.max_tx_bytes < kMaxFrameBytes ||
      impl_->options.max_sessions == 0) {
    impl_->logEvent(LogLevel::Error, "handshake_failed", 0, "", "invalid_options");
    return false;
  }
  if (!impl_->prepareSocket()) {
    return false;
  }

  impl_->stopping.store(false);
  impl_->executor.start();
  impl_->running.store(true);
  impl_->io_thread = std::thread([this] { impl_->run(); });

  impl_->logEvent(LogLevel::Info, "listening", 0, "", impl_->options.socket_path);
  return true;
}

void IpcServer::stop() {
  if (!impl_) {
    return;
  }
  std::call_once(impl_->stop_once, [this] {
    impl_->stopping.store(true);
    if (impl_->io_thread.joinable()) {
      impl_->io_thread.join();
    }
    impl_->executor.stop();
    impl_->sessions.clear();
    impl_->metrics.connections_active.store(0);
    impl_->metrics.tx_queue_bytes.store(0);
    impl_->listener.close();
    if (impl_->socket_created) {
      // Only a socket this server created is ever unlinked
      // (contracts/ipc_api.md, IpcServer::stop postcondition 4).
      ::unlink(impl_->options.socket_path.c_str());
      impl_->socket_created = false;
    }
    impl_->running.store(false);
    impl_->logEvent(LogLevel::Info, "stopped", 0, "", "");
  });
}

void IpcServer::publishNotification(std::string topic, std::string payload_json) {
  if (!isValidName(topic) || detail::isReservedName(topic)) {
    impl_->logEvent(LogLevel::Warn, "notification_dropped", 0, topic, "invalid_topic");
    return;
  }
  if (kFixedBodyFields + topic.size() + payload_json.size() > kMaxBodyLength) {
    impl_->metrics.queue_rejects_total.fetch_add(1);
    impl_->logEvent(LogLevel::Warn, "notification_dropped", 0, topic, "oversize_payload");
    return;
  }
  std::lock_guard<std::mutex> lock(impl_->pending_mutex);
  impl_->pending_notifications.emplace_back(std::move(topic), std::move(payload_json));
}

// --- Impl --------------------------------------------------------------------

void IpcServer::Impl::logEvent(LogLevel level, const char* event, std::uint64_t session_id,
                               const std::string& method, const std::string& reason) {
  LogFields f;
  f.level = level;
  f.service = kService;
  f.event = event;
  if (session_id != 0) {
    f.connection_id = session_id;
  }
  f.method = method;
  f.reason = reason;
  detail::emit(f);
}

bool IpcServer::Impl::prepareSocket() {
  // The runtime directory must be 0700 and owned by this user, or the UID
  // allowlist means nothing (research R-10).
  const std::size_t slash = options.socket_path.rfind('/');
  if (slash != std::string::npos && slash > 0) {
    const std::string dir = options.socket_path.substr(0, slash);
    struct stat st {};
    if (::stat(dir.c_str(), &st) == 0) {
      if (st.st_uid != ::geteuid()) {
        logEvent(LogLevel::Error, "handshake_failed", 0, "", "runtime_dir_wrong_owner");
        return false;
      }
      if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        logEvent(LogLevel::Error, "handshake_failed", 0, "", "runtime_dir_loose_permissions");
        return false;
      }
    } else if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
      logEvent(LogLevel::Error, "handshake_failed", 0, "", "runtime_dir_create_failed");
      return false;
    }
  }

  if (!listener.open()) {
    logEvent(LogLevel::Error, "handshake_failed", 0, "", "socket_failed");
    return false;
  }

  if (listener.bindPath(options.socket_path) != 0) {
    if (listener.lastErrno() != EADDRINUSE) {
      logEvent(LogLevel::Error, "handshake_failed", 0, "", std::strerror(listener.lastErrno()));
      return false;
    }
    // EADDRINUSE: probe before unlinking. Only ECONNREFUSED proves nobody is
    // listening; a successful connect means a live server owns the path and
    // this one must refuse (research R-10).
    UnixSocket probe;
    if (!probe.open()) {
      return false;
    }
    if (probe.connectPath(options.socket_path) == 0) {
      logEvent(LogLevel::Error, "handshake_failed", 0, "", "socket_path_in_use");
      return false;
    }
    if (probe.lastErrno() != ECONNREFUSED) {
      logEvent(LogLevel::Error, "handshake_failed", 0, "", "socket_path_unprobeable");
      return false;
    }
    probe.close();
    ::unlink(options.socket_path.c_str());
    if (!listener.open() || listener.bindPath(options.socket_path) != 0) {
      logEvent(LogLevel::Error, "handshake_failed", 0, "", "bind_retry_failed");
      return false;
    }
  }
  socket_created = true;

  if (!listener.setNonBlocking() || listener.listen(static_cast<int>(options.max_sessions)) != 0) {
    ::unlink(options.socket_path.c_str());
    socket_created = false;
    listener.close();
    return false;
  }
  return true;
}

void IpcServer::Impl::run() {
  while (!stopping.load()) {
    std::vector<pollfd> fds;
    std::vector<std::uint64_t> ids;
    fds.push_back(pollfd{listener.fd(), POLLIN, 0});
    ids.push_back(0);
    for (auto& entry : sessions) {
      short events = POLLIN;
      if (entry.second->wantsWrite()) {
        // POLLOUT only while there is something to write, so an idle session
        // costs one POLLIN slot (research R-02).
        events = static_cast<short>(events | POLLOUT);
      }
      fds.push_back(pollfd{entry.second->socket().fd(), events, 0});
      ids.push_back(entry.first);
    }

    // shortcut: poll() rebuilds its pollfd array every wake, which is O(n) in
    // the session count. Switch to epoll with a persistent FD registry once a
    // deployment needs more than about 100 concurrent sessions; at the 32 of
    // ServerOptions::max_sessions it costs nothing (research R-02).
    const int rc = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), kPollTimeoutMs);
    if (rc < 0 && errno != EINTR) {
      break;
    }

    if (rc > 0 && (fds[0].revents & POLLIN) != 0) {
      acceptOne();
    }

    std::vector<std::uint64_t> to_close;
    for (std::size_t i = 1; i < fds.size(); ++i) {
      const auto it = sessions.find(ids[i]);
      if (it == sessions.end()) {
        continue;
      }
      bool close_me = false;
      if ((fds[i].revents & (POLLERR | POLLNVAL)) != 0) {
        close_me = true;
      } else {
        if ((fds[i].revents & POLLOUT) != 0 && !it->second->flush()) {
          close_me = true;
        }
        if (!close_me && (fds[i].revents & POLLIN) != 0) {
          serviceSession(*it->second, close_me);
        }
        if (!close_me && (fds[i].revents & POLLHUP) != 0) {
          close_me = true;
        }
      }
      if (close_me) {
        to_close.push_back(ids[i]);
      }
    }
    for (const std::uint64_t id : to_close) {
      closeSession(id, "peer_closed");
    }

    drainOut();
    drainNotifications();

    std::size_t bytes = 0;
    for (auto& entry : sessions) {
      bytes += entry.second->txBytes();
    }
    metrics.tx_queue_bytes.store(bytes);
  }

  // Stopping: refuse further work and let stop() close the sessions.
  logEvent(LogLevel::Info, "stopping", 0, "", "");
}

void IpcServer::Impl::acceptOne() {
  UnixSocket accepted = listener.accept();
  if (!accepted.valid()) {
    return;
  }
  if (sessions.size() >= options.max_sessions) {
    logEvent(LogLevel::Warn, "rejected_peer", 0, "", "max_sessions");
    return;
  }

  detail::PeerCredentials cred{};
  if (!accepted.peerCredentials(cred)) {
    logEvent(LogLevel::Warn, "rejected_peer", 0, "", "no_peer_credentials");
    return;
  }
  // An empty allowlist means owner only, never "anyone" (invariant P3).
  bool allowed = false;
  if (options.allowed_uids.empty()) {
    allowed = cred.uid == ::geteuid();
  } else {
    for (const uid_t uid : options.allowed_uids) {
      if (uid == cred.uid) {
        allowed = true;
        break;
      }
    }
  }
  if (!allowed) {
    // The one log line that carries peer identity: the audit record B-14 asks
    // for. The session is closed before a single frame is read.
    LogFields f;
    f.level = LogLevel::Warn;
    f.service = kService;
    f.event = "rejected_peer";
    f.reason = "uid=" + std::to_string(cred.uid) + " gid=" + std::to_string(cred.gid) +
               " pid=" + std::to_string(cred.pid);
    detail::emit(f);
    return;
  }

  if (!accepted.setNonBlocking()) {
    return;
  }
  const std::uint64_t id = next_session_id++;
  auto session = std::make_unique<ClientSession>(id, std::move(accepted), options.max_tx_messages,
                                                 options.max_tx_bytes);
  session->setPeer(cred);
  sessions.emplace(id, std::move(session));
  metrics.connections_active.store(sessions.size());
  logEvent(LogLevel::Info, "accepted", id, "", "");
}

void IpcServer::Impl::serviceSession(ClientSession& session, bool& close_me) {
  bool closed_by_peer = false;
  if (!session.readSome(closed_by_peer)) {
    close_me = true;
    return;
  }
  if (session.rxOverflowed()) {
    metrics.protocol_errors_total.fetch_add(1);
    logEvent(LogLevel::Warn, "protocol_error", session.id(), "", "rx_overflow");
    close_me = true;
    return;
  }

  for (;;) {
    Frame frame;
    const DecodeResult r = session.nextFrame(frame);
    if (r == DecodeResult::Incomplete) {
      return;
    }
    if (r != DecodeResult::Ok) {
      metrics.protocol_errors_total.fetch_add(1);
      std::string_view code = detail::kCodeProtocolError;
      if (r == DecodeResult::TooLarge) {
        code = detail::kCodeFrameTooLarge;
      } else if (r == DecodeResult::UnsupportedVersion) {
        code = detail::kCodeUnsupportedVersion;
      }
      logEvent(LogLevel::Warn, "protocol_error", session.id(), "", std::string(code));
      close_me = true;
      return;
    }
    handleFrame(session, frame, close_me);
    if (close_me) {
      return;
    }
  }
}

void IpcServer::Impl::handleFrame(ClientSession& session, const Frame& frame, bool& close_me) {
  if (frame.header.type != FrameType::Request) {
    // A server receives requests only; anything else is a protocol violation.
    metrics.protocol_errors_total.fetch_add(1);
    logEvent(LogLevel::Warn, "protocol_error", session.id(), frame.name, "unexpected_frame_type");
    close_me = true;
    return;
  }

  const std::string& method = frame.name;
  logEvent(LogLevel::Debug, "request_received", session.id(), method, "");

  if (stopping.load()) {
    sendError(session, frame.header.request_id, method, detail::kCodeShuttingDown, "stopping");
    close_me = true;
    return;
  }

  if (method == detail::kMethodHello) {
    const long version = flatIntField(frame.payload, "version", -1);
    if (version != static_cast<long>(kProtocolVersion)) {
      sendError(session, frame.header.request_id, method, detail::kCodeUnsupportedVersion,
                "protocol v1 only");
      logEvent(LogLevel::Warn, "handshake_failed", session.id(), method, "version_mismatch");
      close_me = true;
      return;
    }
    session.markHelloDone(flatStringField(frame.payload, "client_id"));
    FrameHeader h;
    h.type = FrameType::Response;
    h.request_id = frame.header.request_id;
    if (!session.enqueue(encode(h, method, "{\"version\":1,\"capabilities\":[]}"))) {
      metrics.queue_rejects_total.fetch_add(1);
    }
    session.flush();
    logEvent(LogLevel::Info, "handshake_ok", session.id(), method, "");
    return;
  }

  if (!session.helloDone()) {
    // Ordering rule of contracts/wire_protocol.md section 3 (B-01, B-19).
    sendError(session, frame.header.request_id, method, detail::kCodeUnauthenticated,
              "ipc.hello required first");
    close_me = true;
    return;
  }

  if (method == detail::kMethodSubscribe || method == detail::kMethodUnsubscribe) {
    const std::string topic = flatStringField(frame.payload, "topic");
    if (topic.empty() || !isValidName(topic) || detail::isReservedName(topic)) {
      sendError(session, frame.header.request_id, method, detail::kCodeInvalidArgument, "bad topic");
      return;
    }
    const bool active = method == detail::kMethodSubscribe;
    if (active) {
      session.subscribe(topic);
    } else {
      session.unsubscribe(topic);
    }
    FrameHeader h;
    h.type = FrameType::Response;
    h.request_id = frame.header.request_id;
    const std::string payload =
        "{\"topic\":\"" + topic + "\",\"active\":" + (active ? "true" : "false") + "}";
    if (!session.enqueue(encode(h, method, payload))) {
      metrics.queue_rejects_total.fetch_add(1);
    }
    session.flush();
    logEvent(LogLevel::Info, "subscription_active", session.id(), topic, active ? "on" : "off");
    return;
  }

  if (detail::isReservedName(method)) {
    sendError(session, frame.header.request_id, method, detail::kCodeUnknownMethod,
              "unknown control method");
    return;
  }

  const RequestRouter::Handler* handler = router.find(method);
  if (handler == nullptr) {
    sendError(session, frame.header.request_id, method, detail::kCodeUnknownMethod,
              "no such method");
    return;
  }
  dispatchHandler(session.id(), *handler, method, frame.header.request_id, frame.payload);
}

void IpcServer::Impl::dispatchHandler(std::uint64_t session_id,
                                      const RequestRouter::Handler& handler, std::string method,
                                      std::uint64_t request_id, std::string payload) {
  // A copy of the handler and the payload crosses to the executor, so the
  // session is never touched off the I/O thread (invariants S14, R2).
  RequestRouter::Handler fn = handler;
  const bool posted =
      executor.post([this, session_id, fn = std::move(fn), method, request_id,
                     payload = std::move(payload)]() mutable {
        IpcResponse response;
        try {
          response = fn(std::move(payload));
        } catch (...) {
          // B-11: a throwing handler becomes INTERNAL_ERROR and the server lives.
          response.error = IpcError::RemoteError;
          response.detail = std::string(detail::kCodeInternalError);
        }

        FrameHeader h;
        h.request_id = request_id;
        if (response.error == IpcError::None) {
          h.type = FrameType::Response;
          queueOut(session_id, encode(h, method, response.payload_json));
        } else {
          h.type = FrameType::Error;
          const std::string code =
              response.detail.empty() ? std::string(detail::kCodeInternalError) : response.detail;
          queueOut(session_id,
                   encode(h, method, detail::makeErrorPayload(code, "handler rejected")));
        }
      });
  if (!posted) {
    FrameHeader h;
    h.type = FrameType::Error;
    h.request_id = request_id;
    queueOut(session_id,
             encode(h, method, detail::makeErrorPayload(detail::kCodeInternalError, "busy")));
  }
}

void IpcServer::Impl::queueOut(std::uint64_t session_id, std::string frame) {
  if (frame.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(out_mutex);
  out_frames.push_back(OutFrame{session_id, std::move(frame)});
}

void IpcServer::Impl::drainOut() {
  std::vector<OutFrame> batch;
  {
    std::lock_guard<std::mutex> lock(out_mutex);
    batch.swap(out_frames);
  }
  for (OutFrame& out : batch) {
    const auto it = sessions.find(out.session_id);
    if (it == sessions.end()) {
      continue;
    }
    if (!it->second->enqueue(out.frame)) {
      metrics.queue_rejects_total.fetch_add(1);
      if (it->second->takeFullLogLatch()) {
        logEvent(LogLevel::Warn, "tx_queue_full", out.session_id, "", "dropped");
      }
      continue;
    }
    if (!it->second->flush()) {
      closeSession(out.session_id, "write_failed");
      continue;
    }
    logEvent(LogLevel::Debug, "request_completed", out.session_id, "", "");
  }
}

void IpcServer::Impl::drainNotifications() {
  std::vector<std::pair<std::string, std::string>> batch;
  {
    std::lock_guard<std::mutex> lock(pending_mutex);
    batch.swap(pending_notifications);
  }
  for (const auto& item : batch) {
    FrameHeader h;
    h.type = FrameType::Notification;
    h.request_id = 0;
    const std::string frame = encode(h, item.first, item.second);
    if (frame.empty()) {
      continue;
    }
    std::vector<std::uint64_t> failed;
    for (auto& entry : sessions) {
      if (!entry.second->subscribed(item.first)) {
        continue;
      }
      if (!entry.second->enqueue(frame)) {
        metrics.queue_rejects_total.fetch_add(1);
        if (entry.second->takeFullLogLatch()) {
          logEvent(LogLevel::Warn, "tx_queue_full", entry.first, item.first, "notification");
        }
        continue;
      }
      if (!entry.second->flush()) {
        failed.push_back(entry.first);
        continue;
      }
      logEvent(LogLevel::Debug, "notification_sent", entry.first, item.first, "");
    }
    for (const std::uint64_t id : failed) {
      closeSession(id, "write_failed");
    }
  }
}

void IpcServer::Impl::sendError(ClientSession& session, std::uint64_t request_id,
                                const std::string& method, std::string_view code,
                                std::string_view message) {
  FrameHeader h;
  h.type = FrameType::Error;
  h.request_id = request_id;
  const std::string frame = encode(h, method, detail::makeErrorPayload(code, message));
  if (frame.empty() || !session.enqueue(frame)) {
    metrics.queue_rejects_total.fetch_add(1);
    return;
  }
  // The frame is attempted but never waited for: a closing session must not
  // block the loop on a peer that is not reading.
  session.flush();
}

void IpcServer::Impl::closeSession(std::uint64_t id, const char* reason) {
  const auto it = sessions.find(id);
  if (it == sessions.end()) {
    return;
  }
  // Dropping the session drops its subscriptions and closes its FD exactly
  // once, through UnixSocket's destructor (invariants S10, S15).
  sessions.erase(it);
  metrics.connections_active.store(sessions.size());
  logEvent(LogLevel::Info, "peer_closed", id, "", reason);
}

}  // namespace tcu::ipc
