# Contract: `tcu_can` Metrics and Health Surface

**Feature**: `005-virtual-can-bus-laboratory` | **Date**: 2026-11-05 | **Source**: spec sections 16, 17.2

NFR-08 is the rule this file exists to satisfy: **any drop or reject path must increment a
distinguishable metric.** No path may fail silently, and no counter may be shared between two
causes that an operator would need to tell apart.

Shape follows the repo convention (R-23) set by `tcu::scheduler::SchedulerMetricsSnapshot` and
`tcu::ipc::IpcMetricsSnapshot`: a POD of `std::uint64_t` read through one `noexcept` accessor.
Counters are `std::atomic<std::uint64_t>` with `memory_order_relaxed` — they are observability, not
synchronization.

---

## 1. `CanMetricsSnapshot`

```cpp
struct CanMetricsSnapshot {
  // Receive path
  std::uint64_t can_rx_read_total{0};
  std::uint64_t can_rx_enqueued_total{0};
  std::uint64_t can_rx_queue_drop_total{0};
  std::uint64_t can_rx_invalid_total{0};
  std::uint64_t can_unknown_id_total{0};
  std::uint64_t can_reserved_nonzero_total{0};
  std::uint64_t can_decoded_total{0};

  // Reject reasons, split so one cause is never hidden behind another (NFR-08)
  std::uint64_t can_reject_flags_total{0};
  std::uint64_t can_reject_length_total{0};
  std::uint64_t can_reject_range_total{0};

  // Signals
  std::uint64_t signal_update_total{0};
  std::uint64_t signal_stale_transitions_total{0};
  std::uint64_t signal_recovered_total{0};
  std::uint64_t signal_invalid_transitions_total{0};

  // Transmit and command correlation
  std::uint64_t can_tx_attempt_total{0};
  std::uint64_t can_tx_success_total{0};
  std::uint64_t can_ack_matched_total{0};
  std::uint64_t can_ack_unmatched_total{0};
  std::uint64_t can_ack_timeout_total{0};

  // Link lifecycle
  std::uint64_t can_link_error_total{0};
  std::uint64_t can_link_reconnect_total{0};
  std::uint64_t can_shutdown_discarded_total{0};

  // Event Bus adapter
  std::uint64_t event_publish_total{0};
  std::uint64_t event_publish_failed_total{0};

  // Gauges and distribution
  std::size_t can_queue_depth{0};
  std::size_t can_queue_capacity{0};
  std::array<std::uint64_t, 16> can_decode_latency_buckets{};
};
```

---

## 2. Meaning and caveat per metric

The caveat column is as important as the meaning: section 16's own table repeatedly warns against
reading a guarantee into a counter that does not provide one.

| Metric | Increments when | Caveat |
|---|---|---|
| `can_rx_read_total` | a complete frame was read from the RAW socket | **Does not prove no frames were lost in the kernel.** Kernel-level overflow needs `SO_RXQ_OVFL`, deferred to V2 (R-24) |
| `can_rx_enqueued_total` | a frame was admitted to the bounded queue | always `<= can_rx_read_total` |
| `can_rx_queue_drop_total` | `DropNewest` discarded an arriving frame | every intentional application-level drop, no exceptions (ADR-04) |
| `can_rx_invalid_total` | the validator or decoder rejected a **known** ID | sum of the three `can_reject_*_total` counters |
| `can_unknown_id_total` | an ID absent from the catalog reached the application | normally **zero**, because the socket filter stops them. Non-zero only in `--broad-filter` mode (B-12) |
| `can_reserved_nonzero_total` | a known frame carried nonzero reserved bits | the frame was still **accepted** (R-15) |
| `can_decoded_total` | a frame decoded successfully | **does not equal** `signal_update_total`: `0x102` and `0x110` each yield two signals |
| `can_reject_flags_total` | EFF, RTR or ERR flag set | UT-06, UT-07 |
| `can_reject_length_total` | `length != 8` on a known ID | UT-08 |
| `can_reject_range_total` | a decoded value fell outside its range | counts **once per frame**, not once per field, because reject is atomic (R-14) |
| `signal_update_total` | an accepted value was committed to the cache | may exceed `can_decoded_total` |
| `signal_stale_transitions_total` | a signal went `Fresh` to `Stale` | **not** once per health tick — once per transition (B-08) |
| `signal_recovered_total` | a signal returned to `Fresh` from `Stale`, `Invalid` or `LinkUnavailable` | only after a valid frame, never on transport recovery alone (B-20) |
| `signal_invalid_transitions_total` | a signal's computed quality became `Invalid` | repeated invalid frames give one transition plus a growing `can_reject_*` count (section 13.4) |
| `can_tx_attempt_total` | `sendDemoCommand` reached the transport | split from success so a failing link is visible |
| `can_tx_success_total` | `write()` accepted the full frame | **does not assert an application-level ACK** (section 10.2) |
| `can_ack_matched_total` | a `0x201` matched the pending sequence | |
| `can_ack_unmatched_total` | a `0x201` arrived for a sequence that is not pending | covers both the mismatch of B-23 and the duplicate ACK of the section 15 fault matrix |
| `can_ack_timeout_total` | a pending command expired without an ACK | |
| `can_link_error_total` | `receive()` returned `LinkError` or `FatalError` | |
| `can_link_reconnect_total` | a re-open **and** bind succeeded after a failure | no false successes during retries — it increments after bind, not after the attempt |
| `can_shutdown_discarded_total` | frames still queued when the drain deadline expired | R-19; zero in a clean idle shutdown |
| `event_publish_total` | an event was accepted by the bus | |
| `event_publish_failed_total` | the bus returned anything but `Accepted` | a publish failure is counted, never swallowed (Constitution Principle V) |
| `can_queue_depth` | gauge | current occupancy |
| `can_queue_capacity` | gauge | the configured bound, so a reader can compute utilisation |
| `can_decode_latency_buckets` | a frame finished decoding | receipt-to-decode duration. A **histogram**, not a mean: NFR-05 asks for p50/p95/p99 and forbids inventing a target |

---

## 3. Invariants

Each is asserted by `Metrics_InvariantsHold` in `tests/unit/can/`, and the stress test re-checks
them after its run.

| Invariant | Why |
|---|---|
| `can_rx_enqueued_total + can_rx_queue_drop_total == can_rx_read_total` | every frame read is either admitted or counted as dropped; none vanishes (NFR-08) |
| `can_rx_enqueued_total <= can_rx_read_total` | section 16's stated invariant |
| `0 <= can_queue_depth <= can_queue_capacity` | section 16's stated invariant; NFR-02 |
| `can_rx_invalid_total == can_reject_flags_total + can_reject_length_total + can_reject_range_total` | the split counters account for the total exactly |
| `can_tx_success_total <= can_tx_attempt_total` | |
| `can_ack_matched_total <= can_tx_success_total` | an ACK cannot precede the command it answers |
| `signal_recovered_total <= signal_stale_transitions_total + signal_invalid_transitions_total + can_link_error_total` | a recovery needs a preceding non-fresh state |
| `event_publish_total + event_publish_failed_total` equals the number of publish attempts | no publish is unaccounted for |
| every counter is monotonically non-decreasing while the service lives | counters are never reset by `stop()`; a fresh `CanService` starts at zero |

---

## 4. Structured log contract

Emitted through `tcu::logging::ILogger` as newline-delimited JSON (project 04's format), matching
the shape of spec section 16.1. The `DataClass` column is the redaction policy of R-05 and
Constitution Principle V. The record timestamp is added by `tcu_logging`; project 05 contributes no
date field of its own.

| Event | Severity | Fields | `DataClass` |
|---|---|---|---|
| `can_service.started` | `Info` | `interface`, `state`, `rx_queue_capacity` | Public |
| `can_service.stopped` | `Info` | `state`, `discarded_frames` | Public |
| `can_service.link_unavailable` | `Warn` | `interface`, `state`, `os_error` | Public |
| `can_service.link_restored` | `Info` | `interface`, `state`, `backoff_ms` | Public |
| `signal.updated` | `Info` | `name`, `can_id`, `quality`, `generation` | Public; `value` is **VehicleSensitive** |
| `signal.stale` | `Warn` | `name`, `age_ms`, `timeout_ms` | Public |
| `signal.recovered` | `Info` | `name`, `generation` | Public; `value` **VehicleSensitive** |
| `frame.rejected` | `Warn` | `can_id`, `reason` | Public; raw bytes **VehicleSensitive** and logged at `Debug` only |
| `queue.dropped` | `Warn` | `can_id`, `depth`, `capacity` | Public — rate-limited to one per second (section 13.7) |
| `command.sent` / `command.acked` | `Info` | `command`, `sequence`, `result` | Public |

Two rules apply to every row:

- **Raw CAN payload bytes are never logged above `Debug`**, and they are tagged
  `VehicleSensitive` so project 04's formatter redacts them when the configured level or sink says
  so. Section 16.1 asks for exactly this.
- **Rate limiting is mandatory on the drop and reject paths.** An overload condition must not turn
  the logger into the bottleneck; section 13.7 and the rate-limited-logging note in section 10.3
  both require it.

---

## 5. Health surface

`CanService::healthJson()` renders the section 16.2 example shape:

```json
{
  "service_state": "RUNNING",
  "interface": "vcan0",
  "rx_queue_depth": 0,
  "rx_queue_capacity": 1024,
  "signals": {
    "speed_kmh": { "value": 60.0, "quality": "FRESH", "source_can_id": "0x101", "age_ms": 72 },
    "ignition":  { "value": null, "quality": "NEVER_RECEIVED" }
  }
}
```

`value` is `null` exactly when `quality` is `NEVER_RECEIVED`. In `STALE` and `INVALID` the last good
value is present and the quality is what tells the reader not to trust it — the same contract as
`SignalSample` (section 6.4). `age_ms` is a monotonic age in milliseconds, not a date.

This JSON is **an example presentation, not a guaranteed schema** (section 16.2's own wording). It
is served over project 02's IPC as the `health` method (R-08) and is never given raw frame bytes.
