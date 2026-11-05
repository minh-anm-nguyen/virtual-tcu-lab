# Contract: Synthetic CAN Frame Catalog

**Feature**: `005-virtual-can-bus-laboratory` | **Date**: 2026-11-05 | **Source**: spec sections 6, 10

> **Safety boundary.** Every identifier and payload in this file is **invented for a lab**. None of
> it is an OEM DBC, and none of it may be sent to a physical vehicle, gateway, production ECU or
> safety-critical system (spec section 1, ADR-07, NFR-01). The implementation refuses any interface
> not matching `vcan[0-9]*` while `mode == lab_only`, which is the only mode V1 parses (R-16).

This is the wire contract shared by `tcu_can_service`, `vehicle_simulator`, `bcm_simulator` and
every test. It is implemented as the `constexpr` tables in `tcu/can/frame_catalog.hpp`, which the
validator, the decoder, the socket filter list and the config loader all read — so a change here is
one edit, not five.

**Scope of V1:** classical CAN, standard 11-bit identifiers only (`0x000`-`0x7FF`), exactly 8 data
bytes per frame. CAN FD is section 10.5's V2 extension and is not implemented (R-24).

---

## 1. Frame table

| CAN ID | Direction | Cycle / trigger | Bytes used | Reserved |
|---|---|---|---|---|
| `0x100` | Vehicle to TCU | on change + every 1 s | byte 0 bit 0 | byte 0 bits 1-7, bytes 1-7 |
| `0x101` | Vehicle to TCU | every 100 ms | bytes 0-1 | bytes 2-7 |
| `0x102` | Vehicle to TCU | every 1 s | bytes 0-2 | bytes 3-7 |
| `0x103` | BCM to TCU | on change + every 1 s | byte 0 bits 0-4 | byte 0 bits 5-7, bytes 1-7 |
| `0x110` | Vehicle to TCU | every 1 s | bytes 0-1 | bytes 2-7 |
| `0x200` | TCU to BCM | on demo command | bytes 0-1 | bytes 2-7 |
| `0x201` | BCM to TCU | on `0x200` | bytes 0-1 | bytes 2-7 |

`reserved_mask` is a per-byte bitmask of the reserved bits:

| CAN ID | `reserved_mask[0..7]` |
|---|---|
| `0x100` | `FE FF FF FF FF FF FF FF` |
| `0x101` | `00 00 FF FF FF FF FF FF` |
| `0x102` | `00 00 00 FF FF FF FF FF` |
| `0x103` | `E0 FF FF FF FF FF FF FF` |
| `0x110` | `00 00 FF FF FF FF FF FF` |
| `0x200` | `00 00 FF FF FF FF FF FF` |
| `0x201` | `00 00 FF FF FF FF FF FF` |

**Reserved-byte policy (R-15, section 6.1):** a V1 **sender** sets every reserved bit to zero. A V1
**receiver** ignores them, accepts the frame, and increments `can_reserved_nonzero_total` with one
rate-limited `WARN` per CAN ID per minute. Strict rejection is deferred to a future schema version.
Padding is never interpreted as a new signal.

---

## 2. Byte layouts

All multi-byte integers are **little-endian**: `value = b[0] | (b[1] << 8)`.

### `0x100` — ignition

```text
byte 0: bit 0   ignition   0 = OFF, 1 = ON
        bits 1-7          reserved, zero
bytes 1-7                 reserved, zero
```

### `0x101` — vehicle speed

```text
bytes 0-1: uint16_le  raw speed, scale 0.01 km/h
bytes 2-7:            reserved, zero
```

`speed_kmh = raw * 0.01`, valid `0.00 .. 250.00`, so raw `0 .. 25000`.

### `0x102` — battery

```text
bytes 0-1: uint16_le  raw voltage, scale 0.01 V
byte  2:   uint8      state of charge, percent
bytes 3-7:            reserved, zero
```

`battery_voltage_v = raw * 0.01`, valid `6.00 .. 18.00`, so raw `600 .. 1800`.
`battery_soc_pct = byte 2`, valid `0 .. 100`.

Both signals come from one frame, so **atomic frame reject** applies (R-14): if either is out of
range, neither is committed.

### `0x103` — doors and lock

```text
byte 0: bit 0   door front-left open
        bit 1   door front-right open
        bit 2   door rear-left open
        bit 3   door rear-right open
        bit 4   doors locked
        bits 5-7         reserved, zero
bytes 1-7                reserved, zero
```

`door_state = byte 0 & 0x1F`, valid `0 .. 31`. The five bits are carried as one numeric mask
because section 9.1's refinement note accepts that simplification for V1; a typed variant is the
production-grade shape and is left to a later project.

### `0x110` — ECU heartbeat

```text
byte 0: uint8  heartbeat counter, modulo 256
byte 1: uint8  ECU status   0 = OK, 1 = DEGRADED
bytes 2-7:     reserved, zero
```

`heartbeat = byte 0`, valid `0 .. 255` — every value is legal, and the `255` to `0` wrap is normal
behaviour, not an error (UT-19). `ecu_status = byte 1`, valid `{0, 1}`. Same frame, so atomic
reject applies here too.

### `0x200` — synthetic lock command (TCU to BCM)

```text
byte 0: uint8  command     0x01 = LOCK, 0x02 = UNLOCK
byte 1: uint8  sequence    correlation value, 0-255
bytes 2-7:     reserved, zero
```

### `0x201` — synthetic acknowledgment (BCM to TCU)

```text
byte 0: uint8  sequence    echo of the 0x200 sequence
byte 1: uint8  result      0x00 = OK, 0x01 = REJECTED, 0x02 = INVALID
bytes 2-7:     reserved, zero
```

The sequence byte is a **demo correlation mechanism only**. With 8 bits it wraps; it carries no
uniqueness guarantee and no security property (section 11.3). V1 keeps exactly one command in
flight, which removes the wrap ambiguity by construction (R-20).

---

## 3. Golden vectors

These are the independent reference values the spec's section 6.2 fixes. They are the test data for
UT-01..UT-04 and UT-19, and the same strings are what `cansend` injects in the integration tests —
so the decoder and `candump` are checked against one another rather than against themselves
(section 24, "Simulator and TCU using same code assumptions" risk row).

| `candump` form | Decoded result |
|---|---|
| `100#0100000000000000` | `ignition = 1` (ON) |
| `100#0000000000000000` | `ignition = 0` (OFF) |
| `101#7017000000000000` | raw `0x1770` = 6000, so `speed_kmh = 60.00` |
| `101#0000000000000000` | `speed_kmh = 0.00` — a **valid zero**, not a missing value (B-04) |
| `101#A861000000000000` | raw `0x61A8` = 25000, so `speed_kmh = 250.00`, the inclusive maximum |
| `101#A961000000000000` | raw `0x61A9` = 25001, so `250.01` — **rejected**, out of range (UT-09) |
| `102#B004550000000000` | voltage raw `0x04B0` = 1200, so `12.00 V`; `battery_soc_pct = 0x55` = 85 |
| `102#B004780000000000` | SOC `0x78` = 120 — **whole frame rejected**; voltage is not committed either (UT-10, R-14) |
| `103#1000000000000000` | `door_state = 0x10` = 16: all four doors closed, doors locked |
| `103#0F00000000000000` | `door_state = 0x0F` = 15: all four doors open, unlocked |
| `110#0100000000000000` | `heartbeat = 1`, `ecu_status = 0` (OK) |
| `110#FF00000000000000` | `heartbeat = 255`; a following `110#0000...` wrapping to 0 is normal (UT-19) |
| `110#0101000000000000` | `heartbeat = 1`, `ecu_status = 1` (DEGRADED) |
| `110#0102000000000000` | `ecu_status = 2` — **whole frame rejected**, out of range |
| `200#012A000000000000` | LOCK, sequence `0x2A` |
| `201#2A00000000000000` | ACK for sequence `0x2A`, result OK |
| `201#2A01000000000000` | ACK for sequence `0x2A`, result REJECTED |

---

## 4. Socket filter set

Installed as `CAN_RAW_FILTER` at open time (section 10.1 step 4, R-11). Every entry uses the mask

```text
CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_ERR_FLAG
```

with all flag bits **clear** in `can_id`, so the kernel's `(received_id & mask) == (filter_id &
mask)` test rejects an extended, remote or error frame whose low 11 bits happen to equal a known
standard ID. That is what makes B-14 fail at the kernel rather than in user space.

| Socket | Filtered IDs |
|---|---|
| `tcu_can_service` RX | `0x100`, `0x101`, `0x102`, `0x103`, `0x110`, `0x201` |
| `bcm_simulator` RX | `0x200` |
| `vehicle_simulator` | TX only; no RX filter installed |

`tcu_can_service` additionally accepts `--broad-filter`, which installs no filter. That mode exists
only so B-12 can observe `can_unknown_id_total` rising on an unexpected ID; in normal operation
unknown IDs never reach application counters (section 13.3).

Each service uses **separate RX and TX sockets** and never sets `CAN_RAW_RECV_OWN_MSGS`, so no
process decodes its own transmissions (section 10.4, R-10).

---

## 5. Rejection rules

`FrameValidator` runs before the decoder and before any cache commit. Each rule names the metric it
increments and the test that covers it.

| Rule | Rejected because | Metric reason tag | Test |
|---|---|---|---|
| `CAN_EFF_FLAG` set | V1 is standard 11-bit only | `flags` | UT-06, B-14 |
| `CAN_RTR_FLAG` set | remote frames carry no data | `flags` | UT-07, B-13 |
| `CAN_ERR_FLAG` set | an error frame is not domain data | `flags` | UT-07, B-15 |
| `length != 8` for a known ID | section 6 fixes 8 bytes for every V1 row | `length` | UT-08, B-10 |
| ID not in the frame table | unknown identifier | `unknown_id` — counts `can_unknown_id_total`, not `can_rx_invalid_total` | UT-05, B-12 |
| any decoded field outside its range | section 6.3 ranges | `range` | UT-09, UT-10, B-11 |

**What a rejection must not do** (section 13.4): it must not change any cached value, must not
advance `last_good_at`, and must not fabricate a domain command. It records `last_error` with its
reason and timestamp, which makes the signal read back as `Invalid` while its last good value stays
available.

**Where these frames come from in tests.** The kernel refuses to place a malformed classical frame
on a real `vcan0`, so the flag and length rejections above are injected through
`tcu::can::FakeCanTransport` or asserted against `FrameValidator` directly — section 18.1's test
note requires exactly this, and forbids claiming an impossible on-wire test passed (R-22).
