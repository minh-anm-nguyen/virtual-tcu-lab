# Contract: `tcu_can` Public API

**Feature**: `005-virtual-can-bus-laboratory` | **Date**: 2026-11-05 | **Source**: spec sections 9, 10, 12, 13, 17

The public surface of `modules/can`. Every method below states its preconditions, its
postconditions, what it guarantees on failure, and which thread may call it. Where the spec's
section 9 snippets are illustrative, this file is the binding form — differences are marked and
recorded in [plan.md](../plan.md) Complexity Tracking.

**Platform note (R-00, R-02):** no header in this contract includes a Linux header. `<linux/can.h>`
appears only in `src/socketcan_transport.cpp` and `src/socketcan_fd.hpp`, which satisfies NFR-10.

---

## 1. Vocabulary — `tcu/can/can_types.hpp`

```cpp
namespace tcu::can {

using SteadyTime = tcu::scheduler::Clock::time_point;   // R-03: one clock, project 03's

enum class FrameFormat     { Standard11, Extended29 };
enum class SignalQuality   { NeverReceived, Fresh, Stale, Invalid, LinkUnavailable };
enum class ReadStatus      { Received, Timeout, Interrupted, LinkError, FatalError };
enum class WriteStatus     { Sent, InvalidFrame, LinkUnavailable, SystemError };
enum class ServiceState    { Stopped, Starting, Running, Degraded, Stopping };
enum class RejectReason    { None, Flags, Length, UnknownId, Range, ReservedNonzero };

struct CanMessage {
  std::uint32_t id{0};
  FrameFormat format{FrameFormat::Standard11};
  std::array<std::uint8_t, 8> data{};
  std::uint8_t length{0};
  SteadyTime received_at{};
};

struct SignalSample {
  std::string signal_name;
  std::optional<double> value;
  SignalQuality quality{SignalQuality::NeverReceived};
  SteadyTime last_good_at{};
  std::uint32_t source_id{0};
  std::uint64_t generation{0};
};

}  // namespace tcu::can
```

Adding a value to any of these enums is a spec change, not an implementation detail (Constitution
Principle I). Field-level rules are in [data-model.md](../data-model.md) sections 2 and 7.

**`SignalSample::value` is the contract that matters.** It is absent only when nothing was ever
accepted. In `Stale`, `Invalid` and `LinkUnavailable` it holds the last good value, and the caller
must read `quality` to know how much to trust it. A caller is never handed a stale speed as `0.0` or
a missing ignition as `false` (section 6.4, NFR-07).

---

## 2. `ICanTransport` — `tcu/can/can_transport.hpp`

```cpp
struct ReadResult {
  ReadStatus status{ReadStatus::Timeout};
  std::optional<CanMessage> message;
  int os_error{0};          // errno snapshot, captured immediately; 0 when none
};

class ICanTransport {
 public:
  virtual ~ICanTransport() = default;
  virtual ReadResult receive(std::chrono::milliseconds timeout) = 0;
  virtual WriteStatus send(const CanMessage& msg) = 0;
  virtual void requestStop() noexcept = 0;
};
```

### 2.1 `receive(timeout)`

**Preconditions:** called from exactly one thread for the lifetime of the object — the single
receiver rule (section 17.1 rule 1). `timeout >= 0`.

**Postconditions by status:**

| Status | `message` | Meaning | Caller's duty |
|---|---|---|---|
| `Received` | set | one complete, supported frame; `received_at` already stamped | enqueue it |
| `Timeout` | empty | nothing arrived within `timeout`; **normal** | loop |
| `Interrupted` | empty | `requestStop()` was signalled, or `EINTR` | exit the loop if stopping |
| `LinkError` | empty, `os_error` set | retryable: interface down, deleted, `ENETDOWN`/`ENODEV` | close, enter `Degraded`, back off, retry (R-18) |
| `FatalError` | empty, `os_error` set | not retryable at transport level | service policy; V1 enters `Degraded` and keeps retrying at the maximum backoff |

**Guarantees:** no exception escapes (section 9.2). A partial read is never reported as `Received`.
`os_error` is captured before any other syscall could overwrite `errno`.

### 2.2 `send(msg)`

**Preconditions:** `msg.length == 8`, `msg.format == Standard11`, `msg.id` is a `FromTcu` row of the
frame catalog. May be called from any thread; implementations serialize writes on the TX socket
(section 10.2).

**Postconditions:**

| Status | Meaning |
|---|---|
| `Sent` | the local socket accepted the full frame. **This is not proof a peer processed it** (section 10.2). For the BCM demo the only proof of delivery is a matching `0x201` (R-10). |
| `InvalidFrame` | a precondition above failed; nothing was written |
| `LinkUnavailable` | no bound socket: the service is `Stopped`, `Starting` or `Degraded` (B-21) |
| `SystemError` | `write()` failed for another reason; `os_error` recorded in metrics |

`EINTR` is retried. `EAGAIN` follows a bounded retry then returns `SystemError`. A short or partial
write is a failure, never a success.

### 2.3 `requestStop()`

`noexcept`, callable from any thread, idempotent. Signals the eventfd so a `receive()` blocked in
`poll()` returns `Interrupted` promptly (R-09). It does **not** close any FD — the receiving thread
owns that, and closing underneath it is the race section 24 lists as a crash risk.

### 2.4 `SocketCanTransport`

```cpp
class SocketCanTransport final : public ICanTransport {
 public:
  struct Options {
    std::string interface_name;             // must match vcan[0-9]* in lab_only mode
    std::vector<std::uint32_t> rx_filter;   // empty means no filter (broad-filter test mode)
    std::shared_ptr<tcu::scheduler::ClockAdapter> clock;
  };
  // Factory, not a throwing constructor (spec section 9.2).
  static std::variant<std::unique_ptr<SocketCanTransport>, OpenError> open(Options);
  ~SocketCanTransport();                    // RAII: closes RX, TX and eventfd
};
```

`open()` performs the section 10.1 sequence in order: validate the name, create the socket with
`SOCK_CLOEXEC`, resolve `if_nametoindex`, install `CAN_RAW_FILTER`, create the eventfd, bind. Any
step failing returns `OpenError` with the failing step and `errno`; no FD leaks on a partial
failure. `CAN_RAW_RECV_OWN_MSGS` is never enabled (R-10).

### 2.5 `FakeCanTransport` — `tcu/can/fake_can_transport.hpp` (public, R-22)

```cpp
class FakeCanTransport final : public ICanTransport {
 public:
  void pushFrame(CanMessage msg);                       // a well-formed frame
  void pushRaw(std::uint32_t raw_id, const std::uint8_t* data, std::uint8_t len);
                                                        // bypasses construction rules: EFF/RTR/ERR
                                                        // flags and len != 8 (UT-06..UT-08)
  void pushReadStatus(ReadStatus status, int os_error = 0);   // LinkError / FatalError injection
  std::vector<CanMessage> sent() const;                 // everything send() received
  void setSendStatus(WriteStatus status);               // force a send failure
};
```

It is public because projects 06 and 07 need it, and because the malformed frames of B-10, B-13,
B-14 and B-15 cannot be placed on a real `vcan0` at all — section 18.1's test note requires
injection instead and forbids claiming an impossible on-wire test passed.

---

## 3. Validation and decoding

`tcu/can/frame_validator.hpp` and `tcu/can/demo_signal_decoder.hpp`:

```cpp
struct ValidationResult {
  bool ok{false};
  RejectReason reason{RejectReason::None};
  bool reserved_nonzero{false};   // informational, never a rejection in V1 (R-15)
};

class FrameValidator {
 public:
  ValidationResult validate(const CanMessage& frame) const noexcept;
};

struct DecodeResult {
  bool accepted{false};
  RejectReason reason{RejectReason::None};
  std::vector<SignalSample> updates;   // empty when !accepted
};

class IDemoSignalDecoder {
 public:
  virtual ~IDemoSignalDecoder() = default;
  virtual DecodeResult decode(const CanMessage& frame) = 0;
};
```

**`validate` postconditions:** pure, `noexcept`, no allocation, no clock read. Checks in this order:
flags, known ID, length, reserved bits. The first failure sets `reason` and returns. Rules and their
tests are tabulated in [frame_catalog.md](./frame_catalog.md) section 5.

**`decode` postconditions:**

- `accepted == true` implies every entry of `updates` is in range and `updates` is non-empty.
- `accepted == false` implies `updates.empty()` — **atomic frame reject** (R-14). A `0x102` with a
  valid voltage and an invalid SOC commits neither.
- Deviation from the spec snippet: `reason` is a `RejectReason` enum, not `std::string`. A string
  built from rejected input is a log-injection path, and section 16.1 warns against echoing
  payloads. Recorded in Complexity Tracking.
- The decoder never reads a clock and never touches the cache; `updates[i].last_good_at` is copied
  from `frame.received_at` by the caller.

---

## 4. `SignalCache` — `tcu/can/signal_cache.hpp`

```cpp
struct QualityTransition {
  std::string signal_name;
  SignalQuality from;
  SignalQuality to;
  std::uint64_t generation;
};

class ISignalCache {
 public:
  virtual ~ISignalCache() = default;
  virtual bool apply(const SignalSample& update) = 0;      // true when committed
  virtual std::optional<SignalSample> get(std::string_view name) const = 0;
  virtual std::vector<SignalSample> snapshot() const = 0;
  virtual void recordError(std::string_view name, RejectReason reason, SteadyTime at) = 0;
  virtual std::vector<QualityTransition> refresh(SteadyTime now) = 0;
  virtual std::vector<QualityTransition> setTransportAvailable(bool available) = 0;
};
```

### 4.1 `apply(update)`

**Preconditions:** `update.signal_name` names a `SignalDefinition` row; `update.value` is set and
in range (the decoder already checked).

**Postconditions:** `last_good_value` and `last_good_at` are set, `generation` increments by one,
`last_error` is cleared. Returns `false` and changes nothing for an unknown signal name.

**Guarantee:** `last_good_at` advances **only** here (section 17.2). No other method may move it.

### 4.2 `recordError(name, reason, at)`

Sets `last_error = {reason, at}`. **Does not** touch `last_good_value`, `last_good_at` or
`generation` — a malformed frame cannot refresh a stale signal (section 13.4, B-10, B-11).

### 4.3 `get(name)` / `snapshot()`

Returns a value built from `SignalState` with the quality **computed** by the five-step rule in
[data-model.md](../data-model.md) section 6.1. `get` on an unknown name returns `std::nullopt` —
distinct from a known signal that was never received, which returns a sample with
`quality == NeverReceived` and no value (B-02, UT-22).

### 4.4 `refresh(now)` and `setTransportAvailable(available)`

Both recompute every signal's quality and return **only the signals whose quality differs from
`published_quality`**, updating `published_quality` as they go. A second `refresh` at a later `now`
with no new frames returns an empty vector — which is what makes B-08 ("exactly one stale
transition, no repeated spam") structural rather than a convention.

`refresh` is called by the health timer; `setTransportAvailable(false)` is called when the service
enters `Degraded`. After `setTransportAvailable(true)`, no signal becomes `Fresh` until a new valid
frame advances its `last_good_at` (B-20).

**Locking:** all six methods take the cache mutex. None of them invokes a user callback or publishes
an event while holding it — the caller publishes the returned transitions after the call returns
(section 17.1 rules 4 and 7).

---

## 5. `CanService` — `tcu/can/can_service.hpp`

```cpp
struct CanServiceDeps {
  std::shared_ptr<tcu::scheduler::ClockAdapter> clock;    // defaults to SteadyClockAdapter
  std::shared_ptr<tcu::logging::ILogger> logger;          // required
  std::shared_ptr<tcu::eventbus::EventBus> bus;           // optional; null disables publishing
  std::unique_ptr<ICanTransport> transport;               // null means "open a SocketCanTransport"
};

class CanService {
 public:
  explicit CanService(CanServiceDeps deps);
  ~CanService();                                 // calls stop()
  CanService(const CanService&) = delete;
  CanService& operator=(const CanService&) = delete;

  bool start(const CanLabConfig& config);
  void stop() noexcept;
  ServiceState state() const noexcept;

  std::optional<SignalSample> readSignal(std::string_view name) const;
  std::vector<SignalSample> readAll() const;
  WriteStatus sendDemoCommand(std::uint8_t command, std::uint8_t sequence);
  CanMetricsSnapshot metrics() const noexcept;
  std::string healthJson() const;                // the section 16.2 surface
};
```

### 5.1 `start(config)`

**Preconditions:** `state() == Stopped`.

**Postconditions:**

| Outcome | State after | Returns |
|---|---|---|
| config invalid (any rule in [config_schema.md](./config_schema.md)) | `Stopped` | `false` |
| interface name not `vcan[0-9]*` in `lab_only` mode | `Stopped` | `false` (R-16) |
| socket bound, workers started | `Running` | `true` |
| transient open/bind failure | `Degraded`, retrying | `true` |
| called while already `Running` | `Running`, unchanged | `false` (B-27 — V1 rejects, it is not idempotent) |

Config validation happens **before** any socket or thread exists (section 13.1 step 2). On success
every signal reads back `NeverReceived` with no value: startup must not report ignition OFF or
speed zero before a valid frame (section 13.1, B-02, UT-22).

Threads created: the RX reader and the decode worker. The health check is a `FixedDelay` timer on
the injected `TimerScheduler`, not a third thread (R-04).

### 5.2 `stop()`

`noexcept`, idempotent, callable from any thread **except** from inside a callback this service
invoked (section 9.4 lifecycle rule; a debug assert catches it). Performs the section 13.8 sequence:

1. `Stopping`; further `sendDemoCommand` returns `LinkUnavailable`.
2. Signal the eventfd; `poll()` wakes.
3. Join the RX worker — no further enqueues.
4. Drain the queue on the decode worker until `drain_deadline`; count leftovers in
   `can_shutdown_discarded_total` (R-19).
5. Join the decode worker; cancel the health timer; stop the scheduler with `StopMode::Drain`.
6. Release the Event Bus subscription, then the queue, then the sockets and eventfd.
7. `Stopped`.

**Guarantees:** no callback runs after the object it depends on is destroyed; no FD is closed twice
(B-26); no join is unbounded (NFR-11); the eventfd outlives the RX thread (section 17.1 rule 6).

### 5.3 `readSignal(name)` / `readAll()`

Const and thread-safe. Takes the cache mutex briefly and never blocks on IO. `readSignal` on an
unknown name returns `std::nullopt`; see 4.3 for why that differs from `NeverReceived`.

### 5.4 `sendDemoCommand(command, sequence)`

**Preconditions:** `command` is `0x01` or `0x02`.

**Postconditions:**

| Condition | Returns | Effect |
|---|---|---|
| `state() != Running` | `LinkUnavailable` | nothing sent (B-21) |
| a command is already awaiting an ACK | `InvalidFrame` | nothing sent; V1 allows one in flight (R-20) |
| `command` not in `{0x01, 0x02}` | `InvalidFrame` | nothing sent |
| otherwise | the transport's `WriteStatus` | `0x200` sent; on `Sent` the correlator moves to `AwaitingAck` |

`Sent` means the local socket accepted the frame — **not** that the BCM acted on it. The matching
`0x201` is the only application-level acknowledgment, and an ACK with a different sequence leaves
the request pending (B-23, UT-18).

### 5.5 `metrics()` / `healthJson()`

`metrics()` is `noexcept` and never blocks. `healthJson()` renders the section 16.2 shape; it is a
presentation, not a stability guarantee, and it never contains raw frame payloads.

---

## 6. Threading summary

| Thread | Owns | May touch | Must never |
|---|---|---|---|
| RX reader (service) | RX socket, eventfd, backoff state | queue `push`, metrics, `setTransportAvailable` | invoke a user callback; decode a frame |
| Decode worker (service) | validator, decoder | queue `pop`, cache `apply`/`recordError`, event publish, metrics | touch any socket FD |
| Scheduler worker (project 03) | — | cache `refresh`, event publish, metrics | touch any socket FD; call `stop()` |
| Caller thread | — | `start`, `stop`, `readSignal`, `readAll`, `sendDemoCommand`, `metrics` | call `stop()` from inside a callback |

Rule 7 of section 17.1 is absolute: **no user callback or event publish happens under the socket,
queue, cache or lifecycle mutex.** Events are published from the returned `QualityTransition`
vectors after the cache call has returned and the lock is released.
