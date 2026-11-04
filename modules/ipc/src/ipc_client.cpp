#include "tcu/ipc/ipc_client.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "executor.hpp"
#include "log.hpp"
#include "metrics.hpp"
#include "request_router.hpp"
#include "tcu/ipc/frame_codec.hpp"
#include "unix_socket.hpp"

namespace tcu::ipc {
namespace {

using clock_type = std::chrono::steady_clock;
using detail::Executor;
using detail::LogFields;
using detail::LogLevel;
using detail::UnixSocket;

constexpr int kMaxPollTimeoutMs = 1000;
constexpr const char* kService = "ipc_client";

/// Backoff schedule of spec section 3.2: 100 ms doubling to a 5 s cap. No
/// jitter in v1 - the spec marks it "trong ban nang cao".
constexpr std::chrono::milliseconds kBackoffFirst{100};
constexpr std::chrono::milliseconds kBackoffCap{5000};

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

}  // namespace

struct IpcClient::Impl {
  explicit Impl(ClientOptions opts) : options(std::move(opts)), executor(256) {}

  ClientOptions options;
  detail::Metrics metrics;
  Executor executor;

  std::atomic<ClientState> state{ClientState::Stopped};
  std::atomic<bool> stopping{false};
  std::atomic<std::uint64_t> generation{0};
  std::thread io_thread;
  std::once_flag stop_once;

  // --- pending requests, guarded by one mutex (research R-03) ---------------
  struct Pending {
    std::uint64_t request_id{0};
    std::uint64_t generation{0};
    std::string method;
    clock_type::time_point created{};
    clock_type::time_point deadline{};
    std::promise<IpcResponse> promise;
    bool sent{false};
  };
  /// A queued frame. `is_request` separates user requests from the control
  /// frames (ipc.hello, ipc.subscribe), because requests_sent_total counts
  /// request frames only (contracts/metrics.md).
  struct TxFrame {
    std::string bytes;
    bool is_request{false};
  };

  mutable std::mutex mutex;
  std::unordered_map<std::uint64_t, Pending> pending;
  std::uint64_t next_request_id{1};
  std::deque<TxFrame> tx;
  std::size_t tx_bytes{0};

  // --- subscriptions --------------------------------------------------------
  struct SubscriptionRecord {
    std::function<void(std::string)> callback;
    bool active{false};
  };
  std::map<std::string, SubscriptionRecord> subscriptions;
  /// request_id -> topic, for the subscribe ACKs still in flight.
  std::unordered_map<std::uint64_t, std::string> pending_subscribes;

  // --- owned by the I/O thread ---------------------------------------------
  UnixSocket socket;
  std::string rx;
  std::size_t tx_offset{0};
  std::uint64_t hello_request_id{0};
  std::chrono::milliseconds backoff{kBackoffFirst};
  clock_type::time_point retry_at{};
  bool had_one_connection{false};

  // Self-pipe so request()/subscribe()/stop() can wake a sleeping poll()
  // immediately (research R-04).
  //
  // The read end is touched only by the I/O thread, so it can be a UnixSocket.
  // The write end is written by any thread, so it is a bare atomic descriptor
  // instead: routing it through UnixSocket raced on that object's last_errno_
  // field, which ThreadSanitizer caught between subscribe() on a user thread
  // and sendSubscribe() on the I/O thread. The wake byte's errno is of no
  // interest - a full pipe means the loop is already about to wake.
  UnixSocket wake_read;
  std::atomic<int> wake_fd{-1};

  void run();
  void wake();
  bool openWakePipe();

  bool tryConnect();
  void onConnected();
  void onDisconnected(const char* reason);
  void serviceSocket(bool& disconnected);
  void handleFrame(const Frame& frame);
  void sweepDeadlines();
  int pollTimeoutMs();
  bool flushTx();
  void sendHello();
  void sendSubscribe(const std::string& topic);

  /// Resolves exactly one pending request: erase under the lock, move the
  /// promise out, then set_value() with the lock released (invariants S1, S6).
  void resolve(std::uint64_t request_id, IpcResponse response);
  void failGeneration(std::uint64_t gen, IpcError error, const char* detail_text);
  std::future<IpcResponse> resolvedNow(IpcError error, const char* detail_text);
  void logEvent(LogLevel level, const char* event, const std::string& method,
                const std::string& reason);
};

IpcClient::IpcClient(ClientOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}

IpcClient::~IpcClient() { stop(); }

ClientState IpcClient::state() const { return impl_->state.load(); }

IpcMetricsSnapshot IpcClient::metrics() const {
  IpcMetricsSnapshot s = impl_->metrics.snapshot();
  s.connections_active = impl_->state.load() == ClientState::Connected ? 1 : 0;
  return s;
}

bool IpcClient::start() {
  if (impl_->state.load() != ClientState::Stopped) {
    return false;
  }
  // Invariant P1: an unusable option set fails start(), never UB.
  if (impl_->options.request_timeout.count() <= 0 || impl_->options.max_pending_requests == 0 ||
      impl_->options.max_tx_messages == 0 || impl_->options.max_tx_bytes < kMaxFrameBytes) {
    impl_->logEvent(LogLevel::Error, "handshake_failed", "", "invalid_options");
    return false;
  }
  if (!impl_->openWakePipe()) {
    impl_->logEvent(LogLevel::Error, "handshake_failed", "", "wake_pipe_failed");
    return false;
  }
  if (impl_->options.client_id.empty()) {
    impl_->options.client_id = std::to_string(::getpid());
  }

  impl_->stopping.store(false);
  impl_->executor.start();
  impl_->state.store(ClientState::Connecting);
  impl_->io_thread = std::thread([this] { impl_->run(); });
  return true;
}

void IpcClient::stop() {
  if (!impl_) {
    return;
  }
  std::call_once(impl_->stop_once, [this] {
    impl_->stopping.store(true);
    impl_->state.store(ClientState::Stopping);
    impl_->wake();
    if (impl_->io_thread.joinable()) {
      impl_->io_thread.join();
    }
    // Every pending request is resolved exactly once with Cancelled
    // (contracts/ipc_api.md, stop postcondition 2).
    impl_->failGeneration(0, IpcError::Cancelled, "cancelled by stop");
    impl_->executor.stop();
    impl_->socket.close();
    impl_->wake_read.close();
    // Clear the descriptor before closing it, so a wake() racing this call
    // sees -1 rather than a closed descriptor.
    const int wake_fd = impl_->wake_fd.exchange(-1, std::memory_order_acq_rel);
    if (wake_fd >= 0) {
      ::close(wake_fd);
    }
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      impl_->tx.clear();
      impl_->tx_bytes = 0;
    }
    impl_->metrics.tx_queue_bytes.store(0);
    impl_->state.store(ClientState::Stopped);
    impl_->logEvent(LogLevel::Info, "stopped", "", "");
  });
}

std::future<IpcResponse> IpcClient::request(std::string method, std::string payload_json,
                                            std::chrono::milliseconds timeout) {
  impl_->metrics.requests_created_total.fetch_add(1);

  if (!isValidName(method) || detail::isReservedName(method) ||
      kFixedBodyFields + method.size() + payload_json.size() > kMaxBodyLength) {
    return impl_->resolvedNow(IpcError::ProtocolError, "invalid request");
  }
  if (timeout.count() <= 0) {
    timeout = impl_->options.request_timeout;
  }

  const ClientState now_state = impl_->state.load();
  if (now_state == ClientState::Stopping || now_state == ClientState::Stopped) {
    return impl_->resolvedNow(IpcError::ShuttingDown, "client stopped");
  }
  if (now_state != ClientState::Connected) {
    // v1 does not queue across a disconnect (contracts/ipc_api.md).
    return impl_->resolvedNow(IpcError::NotConnected, "not connected");
  }

  std::future<IpcResponse> future;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->pending.size() >= impl_->options.max_pending_requests) {
      return impl_->resolvedNow(IpcError::QueueFull, "tx queue full");
    }
    if (impl_->tx.size() >= impl_->options.max_tx_messages) {
      return impl_->resolvedNow(IpcError::QueueFull, "tx queue full");
    }

    const std::uint64_t id = impl_->next_request_id++;
    FrameHeader h;
    h.type = FrameType::Request;
    h.request_id = id;
    const std::string frame = encode(h, method, payload_json);
    if (frame.empty()) {
      return impl_->resolvedNow(IpcError::ProtocolError, "invalid request");
    }
    if (impl_->tx_bytes + frame.size() > impl_->options.max_tx_bytes) {
      return impl_->resolvedNow(IpcError::QueueFull, "tx queue full");
    }

    Impl::Pending p;
    p.request_id = id;
    p.generation = impl_->generation.load();
    p.method = method;
    p.created = clock_type::now();
    // The deadline runs from creation, queue time included (invariant S2).
    p.deadline = p.created + timeout;
    future = p.promise.get_future();
    impl_->pending.emplace(id, std::move(p));

    impl_->tx_bytes += frame.size();
    impl_->tx.push_back(Impl::TxFrame{frame, true});
    impl_->metrics.tx_queue_bytes.store(impl_->tx_bytes);
  }
  impl_->wake();
  return future;
}

void IpcClient::subscribe(std::string topic, std::function<void(std::string)> callback) {
  if (!callback || !isValidName(topic) || detail::isReservedName(topic)) {
    impl_->logEvent(LogLevel::Warn, "subscription_active", topic, "invalid_topic");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Impl::SubscriptionRecord record;
    record.callback = std::move(callback);
    record.active = false;
    impl_->subscriptions[topic] = std::move(record);
  }
  if (impl_->state.load() == ClientState::Connected) {
    impl_->sendSubscribe(topic);
  }
  impl_->wake();
}

// --- Impl --------------------------------------------------------------------

void IpcClient::Impl::logEvent(LogLevel level, const char* event, const std::string& method,
                               const std::string& reason) {
  LogFields f;
  f.level = level;
  f.service = kService;
  f.event = event;
  f.method = method;
  f.reason = reason;
  detail::emit(f);
}

bool IpcClient::Impl::openWakePipe() {
  int fds[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    return false;
  }
  wake_read = UnixSocket(fds[0]);
  const int flags = ::fcntl(fds[1], F_GETFL, 0);
  if (flags < 0 || ::fcntl(fds[1], F_SETFL, flags | O_NONBLOCK) != 0) {
    ::close(fds[1]);
    return false;
  }
  wake_fd.store(fds[1]);
  return wake_read.setNonBlocking();
}

void IpcClient::Impl::wake() {
  // Callable from any thread, so it touches no shared mutable state beyond one
  // atomic load. The result is deliberately ignored: a full pipe already means
  // a wake is pending, and after stop() the descriptor is gone.
  const int fd = wake_fd.load(std::memory_order_acquire);
  if (fd < 0) {
    return;
  }
  const char byte = 1;
  const ssize_t ignored = ::send(fd, &byte, 1, MSG_NOSIGNAL);
  (void)ignored;
}

std::future<IpcResponse> IpcClient::Impl::resolvedNow(IpcError error, const char* detail_text) {
  std::promise<IpcResponse> promise;
  auto future = promise.get_future();
  IpcResponse response;
  response.error = error;
  response.detail = detail_text;
  metrics.recordTerminal(error, 0, false);
  promise.set_value(std::move(response));
  return future;
}

void IpcClient::Impl::resolve(std::uint64_t request_id, IpcResponse response) {
  Pending taken;
  bool found = false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = pending.find(request_id);
    if (it != pending.end()) {
      taken = std::move(it->second);
      pending.erase(it);
      found = true;
    }
  }
  if (!found) {
    return;
  }
  const auto latency =
      std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - taken.created)
          .count();
  const IpcError error = response.error;
  const bool observe = error == IpcError::None || error == IpcError::RemoteError;
  // The counter moves before the promise, so a caller that already holds the
  // value can trust the conservation equations (contracts/metrics.md E1).
  // Both happen outside the lock: a promise is never resolved while a mutex is
  // held (invariant S6).
  metrics.recordTerminal(error, static_cast<std::uint64_t>(latency), observe);
  taken.promise.set_value(std::move(response));
}

void IpcClient::Impl::failGeneration(std::uint64_t gen, IpcError error, const char* detail_text) {
  std::vector<Pending> taken;
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto it = pending.begin(); it != pending.end();) {
      // gen == 0 means "every generation", used by stop().
      if (gen == 0 || it->second.generation == gen) {
        taken.push_back(std::move(it->second));
        it = pending.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (Pending& p : taken) {
    IpcResponse response;
    response.error = error;
    response.detail = detail_text;
    metrics.recordTerminal(error, 0, false);
    p.promise.set_value(std::move(response));
  }
}

int IpcClient::Impl::pollTimeoutMs() {
  auto next = clock_type::now() + std::chrono::milliseconds(kMaxPollTimeoutMs);
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& entry : pending) {
      next = std::min(next, entry.second.deadline);
    }
  }
  if (state.load() == ClientState::Backoff) {
    next = std::min(next, retry_at);
  }
  const auto delta =
      std::chrono::duration_cast<std::chrono::milliseconds>(next - clock_type::now()).count();
  if (delta <= 0) {
    return 0;
  }
  return static_cast<int>(std::min<long long>(delta, kMaxPollTimeoutMs));
}

bool IpcClient::Impl::tryConnect() {
  metrics.connect_attempts_total.fetch_add(1);
  if (!socket.open()) {
    return false;
  }
  if (socket.connectPath(options.socket_path) != 0) {
    socket.close();
    return false;
  }
  if (!socket.setNonBlocking()) {
    socket.close();
    return false;
  }
  rx.clear();
  tx_offset = 0;
  sendHello();
  return true;
}

void IpcClient::Impl::sendHello() {
  FrameHeader h;
  h.type = FrameType::Request;
  const std::string payload = "{\"version\":1,\"client_id\":\"" + options.client_id + "\"}";
  std::lock_guard<std::mutex> lock(mutex);
  const std::uint64_t id = next_request_id++;
  hello_request_id = id;
  h.request_id = id;
  // The handshake frame jumps the queue: nothing else may be sent before it
  // (contracts/wire_protocol.md section 3).
  std::string frame = encode(h, std::string(detail::kMethodHello), payload);
  tx_bytes += frame.size();
  tx.push_front(TxFrame{std::move(frame), false});
}

void IpcClient::Impl::sendSubscribe(const std::string& topic) {
  {
    std::lock_guard<std::mutex> lock(mutex);
    const std::uint64_t id = next_request_id++;
    FrameHeader h;
    h.type = FrameType::Request;
    h.request_id = id;
    const std::string frame =
        encode(h, std::string(detail::kMethodSubscribe), "{\"topic\":\"" + topic + "\"}");
    if (frame.empty() || tx.size() >= options.max_tx_messages ||
        tx_bytes + frame.size() > options.max_tx_bytes) {
      metrics.queue_rejects_total.fetch_add(1);
      return;
    }
    pending_subscribes.emplace(id, topic);
    tx_bytes += frame.size();
    tx.push_back(TxFrame{frame, false});
  }
  wake();
}

bool IpcClient::Impl::flushTx() {
  for (;;) {
    TxFrame front;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (tx.empty()) {
        metrics.tx_queue_bytes.store(tx_bytes);
        return true;
      }
      front = tx.front();
    }
    const std::size_t remaining = front.bytes.size() - tx_offset;
    const ssize_t n = socket.sendSome(front.bytes.data() + tx_offset, remaining);
    if (n < 0) {
      const int err = socket.lastErrno();
      if (err == EAGAIN || err == EWOULDBLOCK) {
        return true;
      }
      return false;
    }
    tx_offset += static_cast<std::size_t>(n);
    {
      std::lock_guard<std::mutex> lock(mutex);
      tx_bytes -= static_cast<std::size_t>(n);
      if (tx_offset == front.bytes.size()) {
        tx.pop_front();
        tx_offset = 0;
        // Control frames are not requests (contracts/metrics.md).
        if (front.is_request) {
          metrics.requests_sent_total.fetch_add(1);
        }
      }
      metrics.tx_queue_bytes.store(tx_bytes);
    }
  }
}

void IpcClient::Impl::serviceSocket(bool& disconnected) {
  char buf[8192];
  const ssize_t n = socket.recvSome(buf, sizeof(buf));
  if (n == 0) {
    disconnected = true;
    return;
  }
  if (n < 0) {
    const int err = socket.lastErrno();
    if (err != EAGAIN && err != EWOULDBLOCK) {
      disconnected = true;
    }
    return;
  }
  rx.append(buf, static_cast<std::size_t>(n));
  if (rx.size() > 2 * kMaxFrameBytes) {
    metrics.protocol_errors_total.fetch_add(1);
    disconnected = true;
    return;
  }

  for (;;) {
    Frame frame;
    std::size_t consumed = 0;
    const DecodeResult r = tryDecode(rx, frame, consumed);
    if (r == DecodeResult::Incomplete) {
      return;
    }
    if (r != DecodeResult::Ok) {
      metrics.protocol_errors_total.fetch_add(1);
      logEvent(LogLevel::Warn, "protocol_error", "", "decode_failed");
      disconnected = true;
      return;
    }
    rx.erase(0, consumed);
    handleFrame(frame);
    if (state.load() == ClientState::Stopping) {
      return;
    }
  }
}

void IpcClient::Impl::handleFrame(const Frame& frame) {
  if (frame.header.type == FrameType::Notification) {
    std::function<void(std::string)> callback;
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto it = subscriptions.find(frame.name);
      if (it != subscriptions.end() && it->second.active) {
        callback = it->second.callback;
      }
    }
    if (callback) {
      // Callbacks run on the executor, never on the I/O thread (invariant B3).
      std::string payload = frame.payload;
      executor.post(
          [callback, payload = std::move(payload)]() mutable { callback(std::move(payload)); });
    }
    return;
  }

  const std::uint64_t id = frame.header.request_id;

  // The handshake answer is tracked outside the pending map so it never shows
  // up in the user-visible request counters.
  if (hello_request_id != 0 && id == hello_request_id) {
    hello_request_id = 0;
    if (frame.header.type == FrameType::Error) {
      const std::string code = flatStringField(frame.payload, "code");
      logEvent(LogLevel::Error, "handshake_failed", std::string(detail::kMethodHello), code);
      if (code == std::string(detail::kCodeUnsupportedVersion)) {
        // A fixed configuration error must not spin (B-19, research R-06):
        // terminal, not Backoff.
        stopping.store(true);
        state.store(ClientState::Stopping);
      } else {
        onDisconnected("handshake_failed");
      }
      return;
    }
    onConnected();
    return;
  }

  // A subscribe ACK activates its record (research R-07).
  {
    std::string topic;
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto it = pending_subscribes.find(id);
      if (it != pending_subscribes.end()) {
        topic = it->second;
        pending_subscribes.erase(it);
      }
    }
    if (!topic.empty()) {
      if (frame.header.type == FrameType::Response) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = subscriptions.find(topic);
        if (it != subscriptions.end()) {
          it->second.active = true;
        }
      }
      logEvent(LogLevel::Info,
               metrics.reconnect_total.load() > 0 ? "subscription_restored" : "subscription_active",
               topic, "");
      return;
    }
  }

  IpcResponse response;
  if (frame.header.type == FrameType::Error) {
    response.error = IpcError::RemoteError;
    response.detail = flatStringField(frame.payload, "code");
  } else {
    response.error = IpcError::None;
    response.payload_json = frame.payload;
  }

  bool known = false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    known = pending.count(id) != 0;
  }
  if (!known) {
    // A response whose request already timed out: dropped and counted, never
    // matched to another entry (invariant S4, B-12).
    metrics.late_responses_total.fetch_add(1);
    logEvent(LogLevel::Warn, "late_response", frame.name, "dropped");
    return;
  }
  resolve(id, std::move(response));
}

void IpcClient::Impl::onConnected() {
  // A new generation, so a frame from the old socket can be discarded
  // (research R-12, invariant C4).
  generation.fetch_add(1);
  backoff = kBackoffFirst;
  state.store(ClientState::Connected);
  if (had_one_connection) {
    metrics.reconnect_total.fetch_add(1);
  }
  had_one_connection = true;
  logEvent(LogLevel::Info, "handshake_ok", "", "protocol=v1");

  // Re-subscribe every record and clear active until each ACK arrives.
  std::vector<std::string> topics;
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& entry : subscriptions) {
      entry.second.active = false;
      topics.push_back(entry.first);
    }
    pending_subscribes.clear();
  }
  for (const std::string& topic : topics) {
    sendSubscribe(topic);
  }
}

void IpcClient::Impl::onDisconnected(const char* reason) {
  const std::uint64_t gen = generation.load();
  socket.close();
  rx.clear();
  {
    std::lock_guard<std::mutex> lock(mutex);
    tx.clear();
    tx_bytes = 0;
    tx_offset = 0;
    pending_subscribes.clear();
    for (auto& entry : subscriptions) {
      entry.second.active = false;
    }
  }
  metrics.tx_queue_bytes.store(0);
  // Every request of the dead generation fails exactly once (invariant C3).
  // Nothing is replayed - spec section 8.4 and B-20 forbid it.
  failGeneration(gen, IpcError::PeerClosed, "peer closed");

  if (stopping.load() || !options.reconnect_enabled) {
    state.store(ClientState::Stopping);
    stopping.store(true);
    return;
  }
  state.store(ClientState::Backoff);
  retry_at = clock_type::now() + backoff;
  logEvent(LogLevel::Info, "reconnecting", "", reason);
  backoff = std::min(kBackoffCap, backoff * 2);
}

void IpcClient::Impl::sweepDeadlines() {
  std::vector<Pending> expired;
  const auto now = clock_type::now();
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto it = pending.begin(); it != pending.end();) {
      if (it->second.deadline <= now) {
        expired.push_back(std::move(it->second));
        it = pending.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (Pending& p : expired) {
    IpcResponse response;
    response.error = IpcError::Timeout;
    response.detail = "deadline exceeded";
    metrics.recordTerminal(IpcError::Timeout, 0, false);
    p.promise.set_value(std::move(response));
    logEvent(LogLevel::Warn, "request_timeout", p.method, "deadline");
  }
}

void IpcClient::Impl::run() {
  retry_at = clock_type::now();

  while (!stopping.load()) {
    if (state.load() == ClientState::Backoff && clock_type::now() >= retry_at) {
      state.store(ClientState::Connecting);
    }
    if (state.load() == ClientState::Connecting && !socket.valid()) {
      if (!tryConnect()) {
        onDisconnected("connect_failed");
        continue;
      }
    }

    pollfd fds[2];
    int nfds = 0;
    fds[nfds++] = pollfd{wake_read.fd(), POLLIN, 0};
    const bool have_socket = socket.valid();
    if (have_socket) {
      short events = POLLIN;
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (!tx.empty()) {
          events = static_cast<short>(events | POLLOUT);
        }
      }
      fds[nfds++] = pollfd{socket.fd(), events, 0};
    }

    const int rc = ::poll(fds, static_cast<nfds_t>(nfds), pollTimeoutMs());
    if (rc < 0 && errno != EINTR) {
      break;
    }

    if ((fds[0].revents & POLLIN) != 0) {
      char drain[64];
      while (wake_read.recvSome(drain, sizeof(drain)) > 0) {
      }
    }

    if (have_socket && nfds == 2) {
      bool disconnected = false;
      if ((fds[1].revents & (POLLERR | POLLNVAL)) != 0) {
        disconnected = true;
      } else {
        if ((fds[1].revents & POLLOUT) != 0 && !flushTx()) {
          disconnected = true;
        }
        if (!disconnected && (fds[1].revents & POLLIN) != 0) {
          serviceSocket(disconnected);
        }
        if (!disconnected && (fds[1].revents & POLLHUP) != 0) {
          disconnected = true;
        }
        if (!disconnected && !stopping.load() && !flushTx()) {
          disconnected = true;
        }
      }
      if (disconnected) {
        onDisconnected("peer_closed");
        continue;
      }
    }

    sweepDeadlines();
  }
}

}  // namespace tcu::ipc
