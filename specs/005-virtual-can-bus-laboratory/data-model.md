# Phase 1 Data Model: Virtual CAN Bus Laboratory

**Feature**: `005-virtual-can-bus-laboratory` | **Date**: 2026-11-05

Entities, their fields, their validation rules and their state transitions. Field names are the
ones the implementation uses; the spec sections and research decisions each rule comes from are
cited inline. Wire-level byte layouts live in [contracts/frame_catalog.md](./contracts/frame_catalog.md);
method pre- and postconditions live in [contracts/can_api.md](./contracts/can_api.md).

---

## 1. Vocabulary (public enums)

Fixed by spec section 9. Adding a value is a spec change, not an implementation detail
(Constitution Principle I).

| Enum | Values | Source |
|---|---|---|
| `FrameFormat` | `Standard11`, `Extended29` | §9.1. V1 accepts only `Standard11`; `Extended29` exists so a rejection can name what it rejected (B-14). |
| `FrameKind` | `Data`, `Remote`, `Error` | §6.2. Not in the spec's §9.1 struct — see entity 2. |
| `SignalQuality` | `NeverReceived`, `Fresh`, `Stale`, `Invalid`, `LinkUnavailable` | §6.4. Computed, never stored — see entity 6 and R-13. |
| `ReadStatus` | `Received`, `Timeout`, `Interrupted`, `LinkError`, `FatalError` | §9.2. `Timeout` is normal; `Interrupted` is expected on shutdown; `LinkError` is retryable; `FatalError` needs service policy. |
| `WriteStatus` | `Sent`, `InvalidFrame`, `LinkUnavailable`, `SystemError` | §9.2. `Sent` means the local socket accepted it — never that a peer processed it (§10.2). |
| `ServiceState` | `Stopped`, `Starting`, `Running`, `Degraded`, `Stopping` | §9.4, §12.1. |
| `RejectReason` | `None`, `Flags`, `Length`, `UnknownId`, `Range`, `ReservedNonzero` | A closed set — see entity 8. |

`SteadyTime = tcu::scheduler::Clock::time_point` — one clock for the whole project (R-03).

---

## 2. `CanMessage` — one frame, as received

Public, in `tcu/can/can_types.hpp`. §9.1.

| Field | Type | Rule |
|---|---|---|
| `id` | `std::uint32_t` | Numeric identifier **with Linux flag bits already stripped** (`& CAN_SFF_MASK`). §6.2: "CAN ID is not part of the data payload... mask them before using a numeric identifier." |
| `format` | `FrameFormat` | Set from the EFF flag before masking, so a rejection can say *why*. |
| `kind` | `FrameKind` | Set from the RTR and ERR flags. Not in the spec's §9.1 struct; added because §6.2 requires rejecting EFF/RTR/ERR "rather than quietly interpreting them as data" and UT-07/B-13/B-15 test that the **validator** classifies them, which it cannot do if the flags are dropped at the socket. Recorded in [plan.md](./plan.md) Complexity Tracking. |
| `data` | `std::array<std::uint8_t, 8>` | Fixed-size, no allocation on the RX path. Bytes beyond `length` are zero. |
| `length` | `std::uint8_t` | 0..8. Payload byte count, **not** a CAN FD DLC code (§6.2). |
| `received_at` | `SteadyTime` | Stamped by the RX worker from the injected clock at the moment `read()` returned, before enqueueing. Never a sender-supplied time (§9.1). |

**Invariants**

- `length <= 8` and `data[i] == 0` for `i >= length`.
- `received_at` is non-zero for any message that left the transport.
- `id <= 0x7FF` whenever `format == Standard11`.

---

## 3. `FrameDefinition` and `SignalDefinition` — the catalog

`constexpr` tables in `tcu/can/frame_catalog.hpp`. These are the single source of truth that the
validator, the decoder, the socket filter list and the config loader all read — the same one-table
discipline project 04 used for its config (its R-04).

### 3.1 `FrameDefinition` — 7 rows, §6.1

| Field | Type | Rule |
|---|---|---|
| `can_id` | `std::uint32_t` | One of `0x100, 0x101, 0x102, 0x103, 0x110, 0x200, 0x201`. |
| `direction` | `Direction` (`ToTcu`, `FromTcu`) | `0x200` is the only `FromTcu` frame. |
| `expected_length` | `std::uint8_t` | `8` for every V1 row. FR-06 rejects any other length for a known ID. |
| `reserved_mask` | `std::array<std::uint8_t, 8>` | Which bits are reserved. Nonzero reserved bits are counted, not rejected (R-15). |

### 3.2 `SignalDefinition` — 7 rows, §6.3

| Signal name | CAN ID | Decode | Valid range | Timeout (ms) |
|---|---|---|---|---|
| `ignition` | `0x100` | byte 0, bit 0 | `{0, 1}` | 3000 |
| `speed_kmh` | `0x101` | `uint16_le(b0,b1) / 100` | `0.00 .. 250.00` | 500 |
| `battery_voltage_v` | `0x102` | `uint16_le(b0,b1) / 100` | `6.00 .. 18.00` | 3000 |
| `battery_soc_pct` | `0x102` | byte 2 | `0 .. 100` | 3000 |
| `door_state` | `0x103` | byte 0, bits 0-4 as a mask | `0 .. 31` | 3000 |
| `heartbeat` | `0x110` | byte 0 | `0 .. 255` | 3000 |
| `ecu_status` | `0x110` | byte 1 | `{0, 1}` | 3000 |

| Field | Type | Rule |
|---|---|---|
| `name` | `std::string_view` | Stable; it is the key of `readSignal()` and of the event payload. |
| `source_id` | `std::uint32_t` | Must appear in `FrameDefinition`. |
| `decode`, `byte_offset`, `bit_offset`, `bit_width` | — | How the field is extracted. Bounds-checked by `Catalog.DecodeDescriptorsStayInsideThePayload`. |
| `scale_divisor` | `double` | `U16LeScaled` only. A **divisor** (`100.0`), not a `0.01` multiplier: `raw / 100.0` is exact for every value here, so the inclusive bounds at `250.00` and `6.00` do not depend on a rounding analysis. |
| `min`, `max` | `double` | Inclusive. Checked on the **decoded physical value**, before any cache commit (§13.2 step 5). |
| `timeout` | `std::chrono::milliseconds` | `> 0`. Overridable per signal from config. |

**Notes on this table**

- `battery_voltage_v` and `battery_soc_pct` share `0x102`, and `heartbeat` and `ecu_status` share
  `0x110`. Both pairs are governed by atomic frame reject (R-14): if either field of the pair is
  out of range, neither is committed.
- `ecu_status` is **not** in the spec's section 22 sample configuration, which lists six signals.
  It is added because section 6.1 and section 6.3 both define byte 1 of `0x110` as a decoded field
  with its own valid range (`0 OK, 1 DEGRADED`), and a status that cannot be read is a status that
  cannot drive the "ECU DEGRADED" observation section 4 asks for. Recorded in
  [plan.md](./plan.md) Complexity Tracking.
- `door_state` carries five bits in one `double` because section 9.1's own refinement note accepts
  that simplification for V1 ("`double` here simplifies the introductory dashboard contract"). The
  bit layout is in the frame catalog contract.
- Ranges and timeouts are **lab example values**, not production requirements (§6.3).

---

## 4. `CanLabConfig` — the loaded configuration

Produced once at startup by `src/config_loader.cpp` from `config/can_lab.json` (R-06). Immutable
after construction; there is no hot reload in V1. Full key table, types and ranges are in
[contracts/config_schema.md](./contracts/config_schema.md).

| Field | Type | Validation |
|---|---|---|
| `schema_version` | `std::int64_t` | Must equal `1`, else `LoadError::UnsupportedVersion`. |
| `mode` | `LabMode` | Only `lab_only` parses in V1 (R-16). |
| `interface_name` | `std::string` | Non-empty, at most 15 chars (`IFNAMSIZ-1`), and must match `vcan[0-9]*` while `mode == lab_only`. §24 risk row 1. |
| `rx_queue_capacity` | `std::size_t` | `1 .. 65536`. NFR-02 forbids 0 and forbids unbounded. |
| `health_check_period` | `std::chrono::milliseconds` | `1 .. 10000`. |
| `drain_deadline` | `std::chrono::milliseconds` | `0 .. 5000`. R-19. |
| `reconnect_initial` | `std::chrono::milliseconds` | `1 .. reconnect_max`. Cross-field rule. |
| `reconnect_max` | `std::chrono::milliseconds` | `reconnect_initial .. 300000`. |
| `reconnect_jitter_pct` | `int` | `0 .. 50`. |
| `allowed_rx_ids` | `std::vector<std::uint32_t>` | 1-16 entries, each a `ToTcu` catalog row. |
| `signals` | `std::map<std::string, SignalConfig>` | Each key must name a row of `SignalDefinition`; each `can_id` must equal that row's `source_id`; each timeout `> 0`. An unknown signal name is a load error, not an ignored key. |
| `simulator` | `SimulatorConfig` | Periods and the `fixed_seed` for reproducibility. |

**Rules**

- Validation happens **before any socket or thread exists** (§13.1 step 2). An invalid config means
  `start()` returns `false` and the state stays `Stopped` (§12.1 policy).
- Errors are reported as `{LoadError, json_pointer, publicReason}` and never echo the offending
  value — the same redaction rule project 04 adopted in its R-10.

---

## 5. `BoundedFrameQueue` — the ingress queue

Private, `src/bounded_frame_queue.hpp`. R-17, the spec's section 5.1 component table, ADR-04.

| Field | Type | Rule |
|---|---|---|
| `buffer_` | `std::vector<CanMessage>` | Allocated once at construction to `capacity`; never grows. |
| `head_`, `tail_`, `size_` | `std::size_t` | Ring indices. `size_ <= capacity` always. |
| `closed_` | `bool` | Set by `close()`; wakes the consumer so it can drain and exit. |

**Transitions**

| From | Event | To | Effect |
|---|---|---|---|
| `size_ < capacity` | `push(frame)` | `size_ + 1` | FIFO order preserved; consumer notified. |
| `size_ == capacity` | `push(frame)` | unchanged | **DropNewest**: the arriving frame is discarded, `can_rx_queue_drop_total` increments, a rate-limited `WARN` is emitted. The RX worker never blocks (§13.7). |
| `size_ > 0` | `pop()` | `size_ - 1` | Returns the oldest frame. |
| `size_ == 0`, open | `pop()` | unchanged | Consumer waits on the condition variable. |
| `size_ == 0`, closed | `pop()` | unchanged | Returns empty so the consumer thread can exit. |

**Invariants** — asserted by `Metrics_InvariantsHold` and by `Queue.MultipleProducersNoRace`:

- `0 <= depth() <= capacity` (§16).
- `can_rx_enqueued_total + can_rx_queue_drop_total == can_rx_read_total` for frames that passed
  wire-level checks.

---

## 6. `SignalState` — the per-signal cache entry

Private, inside `SignalCache`. **This is the entity R-13 is about**: independent facts are stored,
and the public `SignalQuality` is derived from them on every read.

| Field | Type | Rule |
|---|---|---|
| `last_good_value` | `std::optional<double>` | Set only by an accepted, in-range decode. Never cleared by an invalid frame, a stale deadline or a link loss (§13.4, §13.6). |
| `last_good_at` | `SteadyTime` | Advances **only** in `apply()` (invariant: "`last_good_at` can only advance on valid accepted input", §17.2). A malformed frame cannot refresh a stale signal (§13.4). |
| `last_error_reason`, `last_error_at` | `std::optional<RejectReason>`, `SteadyTime` | Reason is a closed enum, never free text from input. |
| `transport_available` | `bool` | Cache-wide, mirrored from `ServiceState`; `false` in `Degraded`. |
| `unproven_since_link_loss` | `bool` | Set on every signal that holds a value when the transport goes down; cleared only by `apply()`. **Added during implementation**: without it, an outage shorter than the signal's deadline would leave the value reading `Fresh` the instant the link returned, but §13.6 says to "wait for new valid frames to restore FRESH status" and §12.2 allows `LINK_UNAVAILABLE` to `FRESH` only on "transport restored **+ valid update**". The deadline measures *age*; this is about *provenance*, so one field cannot do both jobs. Caught by `Cache.BriefOutageStillRequiresANewFrame`. |
| `generation` | `std::uint64_t` | Strictly increasing, `0` means never updated. Incremented on each accepted update (B-05). |
| `published_quality` | `SignalQuality` | The last quality an event was emitted for. Compared, not shown. |

### 6.1 Derived quality — evaluated in this order

```text
1. transport_available == false                  -> LinkUnavailable
2. !last_good_value.has_value()                  -> NeverReceived
3. unproven_since_link_loss                      -> Stale
4. now > last_good_at + timeout                  -> Stale    (strictly greater, R-12)
5. last_error && last_error_at > last_good_at    -> Invalid
6. otherwise                                     -> Fresh
```

Step 3 reports `Stale` rather than `LinkUnavailable` (the transport is up again) or `Invalid`
(nothing was malformed): the value is real but unproven, and `Stale` is the honest label for a
value a consumer should not act on.

Step 4 before step 5 is deliberate: a signal that is both stale and had a recent decode error is
reported `Stale`, because the deadline is the stronger fact for a consumer deciding whether to use
the value. Step 5 compares timestamps strictly, so an error recorded at the same instant as the
value it would discredit cannot have come from a later frame.

### 6.2 Quality transitions (spec §12.2)

Every arrow below is a **recomputation**, not a stored mutation. An event is published only when
the newly computed quality differs from `published_quality`, which is what makes B-08 ("exactly one
stale transition, no repeated spam") hold without any extra bookkeeping.

| From | Trigger | To |
|---|---|---|
| `NeverReceived` | first accepted update | `Fresh` |
| `Fresh` | accepted update | `Fresh` (value and `generation` change; a *value* event, not a *quality* event) |
| `Fresh` | health tick with `now > last_good_at + timeout` | `Stale` |
| `Stale` | accepted update | `Fresh` (one recovery event) |
| `Fresh` / `Stale` | known frame rejected for this signal | `Invalid` |
| `Invalid` | accepted update | `Fresh` |
| any | `transport_available = false` | `LinkUnavailable` |
| `LinkUnavailable` | transport restored | **not** `Fresh` — `unproven_since_link_loss` holds it at `Stale` until a new valid frame arrives, however brief the outage was (B-20) |

**Invariants** (§17.2)

- No signal is `Fresh` while `transport_available == false`.
- No signal is `Fresh` without `last_good_value.has_value()`.
- A committed update has an in-range value and `generation > 0`.

---

## 7. `SignalSample` — the public read model

Public, §9.1. Built from `SignalState` at read time; it is a value, never a handle into the cache.

| Field | Type | Rule |
|---|---|---|
| `signal_name` | `std::string` | A `SignalDefinition::name`. |
| `value` | `std::optional<double>` | **Absent** when nothing was ever accepted. Present, and equal to the last good value, in every other state including `Stale` and `Invalid`. §6.4: "`value` is optional; `last_good_value` does not imply freshness." |
| `quality` | `SignalQuality` | The derived value from 6.1. |
| `last_good_at` | `SteadyTime` | Zero when `quality == NeverReceived`. |
| `source_id` | `std::uint32_t` | The CAN ID the value came from. |
| `generation` | `std::uint64_t` | For a consumer to detect "same value, new frame". |

**The rule this entity exists to enforce** (§6.4, NFR-07): a consumer must never be handed a stale
speed as `0` or a missing ignition as `false`. `value` is absent or paired with a quality that says
how much to trust it — there is no third option.

---

## 8. `DecodeResult` — the decoder's output

Public, §9.3.

| Field | Type | Rule |
|---|---|---|
| `accepted` | `bool` | `false` means **no** `updates` are committed (R-14, atomic frame reject). |
| `reason` | `RejectReason` | A closed enum, not a string built from input. The spec's illustrative `std::string reason` is narrowed deliberately; recorded in [plan.md](./plan.md) Complexity Tracking. |
| `updates` | `std::vector<SignalSample>` | Empty when `accepted == false`. One entry per signal the frame carries — two for `0x102` and `0x110`, one otherwise. |

---

## 9. `PendingCommand` — BCM command correlation

Private, in `src/command_correlator.hpp`. §11.3, R-20.

| Field | Type | Rule |
|---|---|---|
| `sequence` | `std::uint8_t` | 0..255, wraps. Uniqueness is **not** claimed (§11.3). |
| `command` | `std::uint8_t` | `0x01` LOCK or `0x02` UNLOCK. |
| `sent_at` | `SteadyTime` | For the command timeout. |
| `state` | `Idle` \| `AwaitingAck` \| `Completed` | At most one command is `AwaitingAck` at a time. |

**Transitions**

| From | Event | To | Effect |
|---|---|---|---|
| `Idle` | `sendDemoCommand(cmd, seq)` returning `Sent` | `AwaitingAck` | `can_tx_attempt_total`, `can_tx_success_total`. |
| `Idle` | `sendDemoCommand` while service not `Running` | `Idle` | Returns `LinkUnavailable` (B-21). |
| `AwaitingAck` | `sendDemoCommand` again | `AwaitingAck` | Returns `InvalidFrame`; V1 allows one in flight (R-20). |
| `AwaitingAck` | `0x201` with matching sequence | `Completed` then `Idle` | Result byte recorded; request resolved. |
| `AwaitingAck` | `0x201` with a different sequence | `AwaitingAck` | Ignored, `can_ack_unmatched_total` increments. The pending request **stays pending** (B-23). |
| `Completed`/`Idle` | duplicate `0x201` for a closed sequence | unchanged | Ignored and counted (§15, duplicate-ACK row). |
| `AwaitingAck` | command timeout elapses | `Idle` | `can_ack_timeout_total`; the caller's request resolves as unacknowledged. |

---

## 10. `CanMetricsSnapshot`

Public, read through `CanService::metrics()`. Full definitions, meanings and invariants are in
[contracts/metrics.md](./contracts/metrics.md). Shape follows the repo convention set by
`SchedulerMetricsSnapshot` and `IpcMetricsSnapshot` (R-23): a POD of `std::uint64_t`, plus
`can_queue_depth`, `can_queue_capacity` and a decode-latency bucket array.

---

## 11. `ServiceState` — service lifecycle

Spec §12.1. Guarded by a lifecycle mutex; joins happen outside every lock a worker might need
(§17.1 rule 5).

| From | Event | To |
|---|---|---|
| `Stopped` | `start()` with valid config | `Starting` |
| `Stopped` | `start()` with invalid config | `Stopped` (returns `false`) |
| `Starting` | socket bound, workers up | `Running` |
| `Starting` | transient open/bind error | `Degraded` |
| `Running` | `receive()` returns `LinkError` | `Degraded` |
| `Degraded` | re-open + bind succeeds | `Running` |
| `Running` / `Degraded` / `Starting` | `stop()` | `Stopping` |
| `Stopping` | all threads joined | `Stopped` |
| `Running` | `start()` again | `Running` (returns `false`, B-27 — V1 rejects rather than being idempotent) |
| `Stopped` | `stop()` again | `Stopped` (no-op, no double close, B-26) |

Entering `Degraded` sets `transport_available = false` on every signal, which makes all of them
`LinkUnavailable`, and marks each one unproven so none returns to `Fresh` before a new valid frame
arrives (§13.6, B-20).

---

## 12. Entity relationships

```text
CanLabConfig --validates against--> SignalDefinition / FrameDefinition  (constexpr catalog)
     |
     +- configures -> CanService
                        +- owns -> SocketCanTransport - owns -> RX socket, TX socket, eventfd
                        +- owns -> BoundedFrameQueue  <- pushes - RX worker thread
                        |                             - pops --> decode worker thread
                        +- owns -> FrameValidator --> DemoSignalDecoder --> DecodeResult
                        +- owns -> SignalCache (map<name, SignalState>) --reads--> SignalSample
                        +- owns -> CommandCorrelator (PendingCommand)
                        +- uses -> tcu::scheduler::TimerScheduler   (health timer, FixedDelay)
                        +- uses -> tcu::scheduler::ClockAdapter     (the one clock)
                        +- uses -> tcu::logging::ILogger            (structured logs)
                        +- uses -> tcu::eventbus::EventBus          (3 appended payloads)
                        +- owns -> CanMetricsStore (atomics) --reads--> CanMetricsSnapshot
```

Ownership direction is also the destruction order of R-19: metrics and cache outlive the workers;
the eventfd outlives the RX thread.
