#include "tcu/can/socketcan_transport.hpp"

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <utility>

#include "can_flags.hpp"
#include "socketcan_fd.hpp"
#include "tcu/can/frame_catalog.hpp"

namespace tcu::can {
namespace {

// can_flags.hpp declares these so fake_can_transport.cpp can build malformed
// frames without including the UAPI. If the two ever disagreed, every flag
// decision in the project would be wrong at once - so they are checked here,
// in the one translation unit that sees both.
static_assert(flags::kEffFlag == CAN_EFF_FLAG, "EFF flag mismatch");
static_assert(flags::kRtrFlag == CAN_RTR_FLAG, "RTR flag mismatch");
static_assert(flags::kErrFlag == CAN_ERR_FLAG, "ERR flag mismatch");
static_assert(flags::kSffMask == CAN_SFF_MASK, "SFF mask mismatch");
static_assert(flags::kEffMask == CAN_EFF_MASK, "EFF mask mismatch");
static_assert(kStandardIdMask == CAN_SFF_MASK, "frame_catalog.hpp SFF mask mismatch");

/// Spec section 10.1 filter detail: matching is
/// (received_id & mask) == (filter_id & mask), and the warning is against "a
/// careless filter that lets different frame types alias". Putting the three
/// flag bits in the mask, with all of them clear in can_id, is what makes B-14
/// fail at the kernel rather than in user space (research R-11).
constexpr std::uint32_t kFilterMask = CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_ERR_FLAG;

OpenError errorAt(OpenError::Step step) { return OpenError{step, errno}; }

}  // namespace

const char* openErrorStepName(OpenError::Step step) noexcept {
  switch (step) {
    case OpenError::Step::Validate: return "validate";
    case OpenError::Step::Socket:   return "socket";
    case OpenError::Step::IfIndex:  return "if_nametoindex";
    case OpenError::Step::Filter:   return "setsockopt_filter";
    case OpenError::Step::EventFd:  return "eventfd";
    case OpenError::Step::Bind:     return "bind";
  }
  return "validate";
}

struct SocketCanTransport::Impl {
  std::string interface_name;
  std::shared_ptr<tcu::scheduler::ClockAdapter> clock;

  // Separate receive and transmit sockets, and CAN_RAW_RECV_OWN_MSGS left off:
  // with one socket the service would either never see its own command frames
  // or would decode them as inbound traffic (spec section 10.4, research R-10).
  SocketFd rx;
  SocketFd tx;
  SocketFd wakeup;

  // Spec section 10.2: writes that share a socket are serialized.
  std::mutex tx_mutex;
};

SocketCanTransport::SocketCanTransport(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

SocketCanTransport::~SocketCanTransport() = default;

const std::string& SocketCanTransport::interfaceName() const noexcept {
  return impl_->interface_name;
}

std::variant<std::unique_ptr<SocketCanTransport>, OpenError> SocketCanTransport::open(
    Options options) {
  // Step 1: validate. IFNAMSIZ includes the terminator, so the longest usable
  // name is one shorter.
  if (options.interface_name.empty() || options.interface_name.size() >= IFNAMSIZ) {
    errno = ENAMETOOLONG;
    return errorAt(OpenError::Step::Validate);
  }
  if (!options.clock) {
    errno = EINVAL;
    return errorAt(OpenError::Step::Validate);
  }

  auto impl = std::make_unique<Impl>();
  impl->interface_name = options.interface_name;
  impl->clock = std::move(options.clock);

  // Step 2: sockets. SOCK_CLOEXEC in the socket() call rather than a later
  // fcntl, so there is no window in which a fork would inherit the descriptor.
  impl->rx = SocketFd(::socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC, CAN_RAW));
  if (!impl->rx.valid()) {
    return errorAt(OpenError::Step::Socket);
  }
  impl->tx = SocketFd(::socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC, CAN_RAW));
  if (!impl->tx.valid()) {
    return errorAt(OpenError::Step::Socket);
  }

  // Step 3: interface index. A missing interface fails here, explicitly, which
  // is what lets the service enter DEGRADED and retry instead of guessing.
  const unsigned int ifindex = ::if_nametoindex(impl->interface_name.c_str());
  if (ifindex == 0) {
    return errorAt(OpenError::Step::IfIndex);
  }

  // Step 4: receive filter. An empty list means no filter at all - the
  // --broad-filter mode B-12 needs - and is NOT the same as a filter that
  // matches nothing.
  if (!options.rx_filter.empty()) {
    std::vector<can_filter> filters;
    filters.reserve(options.rx_filter.size());
    for (const auto id : options.rx_filter) {
      can_filter filter{};
      filter.can_id = id & CAN_SFF_MASK;  // every flag bit clear
      filter.can_mask = kFilterMask;
      filters.push_back(filter);
    }
    if (::setsockopt(impl->rx.get(), SOL_CAN_RAW, CAN_RAW_FILTER, filters.data(),
                     static_cast<socklen_t>(filters.size() * sizeof(can_filter))) < 0) {
      return errorAt(OpenError::Step::Filter);
    }
  }

  // Step 5: the shutdown wakeup. Created before bind so that a transport which
  // reaches the bound state always has one: the RX thread must never be
  // unwakeable (spec section 17.1 rule 6, NFR-11).
  impl->wakeup = SocketFd(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
  if (!impl->wakeup.valid()) {
    return errorAt(OpenError::Step::EventFd);
  }

  // Step 6: bind both sockets to the interface.
  sockaddr_can addr{};
  addr.can_family = AF_CAN;
  addr.can_ifindex = static_cast<int>(ifindex);
  if (::bind(impl->rx.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    return errorAt(OpenError::Step::Bind);
  }
  if (::bind(impl->tx.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    return errorAt(OpenError::Step::Bind);
  }

  // Every failure above returned before this point, and SocketFd closed each
  // descriptor as `impl` unwound - so a partial failure leaks nothing.
  return std::unique_ptr<SocketCanTransport>(new SocketCanTransport(std::move(impl)));
}

ReadResult SocketCanTransport::receive(std::chrono::milliseconds timeout) {
  // Index 0 is the wakeup, so a stop request is noticed in the same poll() as
  // incoming traffic rather than after it.
  pollfd fds[2]{};
  fds[0].fd = impl_->wakeup.get();
  fds[0].events = POLLIN;
  fds[1].fd = impl_->rx.get();
  fds[1].events = POLLIN;

  const int ready = ::poll(fds, 2, static_cast<int>(timeout.count()));
  if (ready < 0) {
    const int err = errno;
    if (err == EINTR) {
      // Expected on shutdown and on any signal; not a link problem.
      return ReadResult{ReadStatus::Interrupted, std::nullopt, err};
    }
    return ReadResult{ReadStatus::FatalError, std::nullopt, err};
  }
  if (ready == 0) {
    return ReadResult{ReadStatus::Timeout, std::nullopt, 0};
  }

  if ((fds[0].revents & POLLIN) != 0) {
    // Drained but not acted on beyond returning: requestStop() is the only
    // writer, and the service decides what Interrupted means.
    std::uint64_t ignored = 0;
    (void)::read(impl_->wakeup.get(), &ignored, sizeof(ignored));
    return ReadResult{ReadStatus::Interrupted, std::nullopt, 0};
  }

  // An error on the socket itself: the interface went down or away.
  if ((fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
    return ReadResult{ReadStatus::LinkError, std::nullopt, ENETDOWN};
  }
  if ((fds[1].revents & POLLIN) == 0) {
    return ReadResult{ReadStatus::Timeout, std::nullopt, 0};
  }

  can_frame raw{};
  const ssize_t n = ::read(impl_->rx.get(), &raw, sizeof(raw));
  if (n < 0) {
    const int err = errno;
    if (err == EINTR || err == EAGAIN || err == EWOULDBLOCK) {
      return ReadResult{ReadStatus::Interrupted, std::nullopt, err};
    }
    // ENETDOWN, ENODEV and ENXIO are the interface disappearing: retryable.
    // Anything else is not something a retry is known to fix.
    if (err == ENETDOWN || err == ENODEV || err == ENXIO) {
      return ReadResult{ReadStatus::LinkError, std::nullopt, err};
    }
    return ReadResult{ReadStatus::FatalError, std::nullopt, err};
  }
  // A partial read is never reported as Received: half a frame is not a frame
  // (contracts/can_api.md section 2.1).
  if (n != static_cast<ssize_t>(CAN_MTU)) {
    return ReadResult{ReadStatus::LinkError, std::nullopt, EIO};
  }

  const auto decoded = flags::decodeRawId(raw.can_id);

  CanMessage msg;
  msg.id = decoded.id;
  msg.format = decoded.format;
  msg.kind = decoded.kind;
  msg.length = std::min<std::uint8_t>(raw.len, static_cast<std::uint8_t>(msg.data.size()));
  std::memcpy(msg.data.data(), raw.data, msg.length);
  // Stamped here, as close to the read as possible, and from the injected clock
  // so a fake clock moves it together with the cache and the freshness monitor
  // (research R-03).
  msg.received_at = impl_->clock->now();

  return ReadResult{ReadStatus::Received, msg, 0};
}

WriteStatus SocketCanTransport::send(const CanMessage& msg) {
  // V1 sends only standard, non-remote, non-error frames of exactly 8 bytes,
  // and only identifiers the catalog marks FromTcu.
  if (msg.format != FrameFormat::Standard11 || msg.kind != FrameKind::Data) {
    return WriteStatus::InvalidFrame;
  }
  const FrameDefinition* definition = findFrame(msg.id);
  if (definition == nullptr || definition->direction != Direction::FromTcu ||
      msg.length != definition->expected_length) {
    return WriteStatus::InvalidFrame;
  }

  can_frame raw{};
  raw.can_id = msg.id & CAN_SFF_MASK;
  raw.len = msg.length;
  std::memcpy(raw.data, msg.data.data(), msg.length);

  std::lock_guard<std::mutex> lock(impl_->tx_mutex);
  for (;;) {
    const ssize_t n = ::write(impl_->tx.get(), &raw, sizeof(raw));
    if (n == static_cast<ssize_t>(CAN_MTU)) {
      // Spec section 10.2: this means the local socket accepted the frame. It
      // is NOT evidence that any peer processed it.
      return WriteStatus::Sent;
    }
    if (n < 0) {
      const int err = errno;
      if (err == EINTR) {
        continue;  // retryable without bound: no data was written
      }
      if (err == ENETDOWN || err == ENODEV || err == ENXIO) {
        return WriteStatus::LinkUnavailable;
      }
      return WriteStatus::SystemError;
    }
    // A short write on a datagram socket should not happen; treating it as
    // success would report a truncated frame as sent.
    return WriteStatus::SystemError;
  }
}

void SocketCanTransport::requestStop() noexcept {
  // Closes nothing: the receiving thread owns the descriptors, and closing
  // underneath it is the race spec section 24 lists as a crash risk. Writing 1
  // to a counting eventfd is enough for our purpose - any nonzero value makes
  // poll() report it readable.
  const std::uint64_t one = 1;
  (void)::write(impl_->wakeup.get(), &one, sizeof(one));
}

}  // namespace tcu::can
