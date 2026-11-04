#pragma once

// One accepted connection on the server side.
//
// The session owns its FD, its RX buffer and its bounded TX queue, and is
// touched only by the I/O thread (invariant S14). Handlers get a copy of the
// payload and return a value, so no session field is ever shared with the
// executor.
//
// data-model.md section 6.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <set>
#include <string>
#include <utility>

#include "tcu/ipc/frame_codec.hpp"
#include "unix_socket.hpp"

namespace tcu::ipc::detail {

class ClientSession {
 public:
  ClientSession(std::uint64_t id, UnixSocket socket, std::size_t max_tx_messages,
                std::size_t max_tx_bytes)
      : id_(id),
        socket_(std::move(socket)),
        max_tx_messages_(max_tx_messages),
        max_tx_bytes_(max_tx_bytes) {}

  ClientSession(const ClientSession&) = delete;
  ClientSession& operator=(const ClientSession&) = delete;

  std::uint64_t id() const noexcept { return id_; }
  UnixSocket& socket() noexcept { return socket_; }

  bool helloDone() const noexcept { return hello_done_; }
  void markHelloDone(std::string client_id) {
    hello_done_ = true;
    client_id_ = std::move(client_id);
  }
  const std::string& clientId() const noexcept { return client_id_; }

  void setPeer(const PeerCredentials& cred) noexcept { peer_ = cred; }
  const PeerCredentials& peer() const noexcept { return peer_; }

  // --- RX -------------------------------------------------------------------

  /// Reads whatever is ready. Returns false when the peer closed or the socket
  /// failed; `closed_by_peer` distinguishes an orderly shutdown from an error.
  bool readSome(bool& closed_by_peer);

  /// Decodes the next buffered frame, consuming it on Ok. Call until it returns
  /// Incomplete so several frames in one read are all handled (B-05).
  DecodeResult nextFrame(Frame& out);

  /// True once the RX buffer holds more than two maximum frames, which only a
  /// peer ignoring kMaxBodyLength can cause (invariant S12).
  bool rxOverflowed() const noexcept { return rx_.size() > 2 * kMaxFrameBytes; }

  // --- TX -------------------------------------------------------------------

  /// Appends an encoded frame. Returns false when either cap would be
  /// breached; the frame is then dropped, and the caller counts it
  /// (invariant S11, research R-08).
  bool enqueue(const std::string& frame);

  /// Writes as much of the queue as the socket accepts. Returns false when the
  /// socket failed, so the caller closes the session.
  bool flush();

  bool wantsWrite() const noexcept { return !tx_.empty(); }
  std::size_t txBytes() const noexcept { return tx_bytes_; }
  std::size_t txMessages() const noexcept { return tx_.size(); }

  /// Latches so one log line is written per transition into the full state,
  /// not one per dropped frame.
  bool takeFullLogLatch();
  void clearFullLogLatch() noexcept { tx_full_logged_ = false; }

  // --- subscriptions --------------------------------------------------------

  void subscribe(const std::string& topic) { topics_.insert(topic); }
  void unsubscribe(const std::string& topic) { topics_.erase(topic); }
  bool subscribed(const std::string& topic) const { return topics_.count(topic) != 0; }

 private:
  std::uint64_t id_{0};
  UnixSocket socket_;
  std::size_t max_tx_messages_{0};
  std::size_t max_tx_bytes_{0};

  PeerCredentials peer_{};
  bool hello_done_{false};
  std::string client_id_;

  std::string rx_;
  std::deque<std::string> tx_;
  std::size_t tx_offset_{0};
  std::size_t tx_bytes_{0};
  bool tx_full_logged_{false};

  std::set<std::string> topics_;
};

}  // namespace tcu::ipc::detail
