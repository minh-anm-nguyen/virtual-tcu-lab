# `tcu_can` — Virtual CAN Bus Laboratory (project 05)

A C++17 SocketCAN library plus four executables that let a TCU application talk
to simulated ECUs over a Linux virtual CAN interface, with no hardware.

Design and rationale: [`specs/005-virtual-can-bus-laboratory/`](../../specs/005-virtual-can-bus-laboratory/).
Run it: [`quickstart.md`](../../specs/005-virtual-can-bus-laboratory/quickstart.md).

> ## Safety boundary
>
> **Every CAN identifier and signal in this project is invented for a lab.** None
> of it is an OEM database. It must never be sent to a physical vehicle, gateway,
> production ECU or safety-critical system.
>
> This is enforced, not merely documented: the configuration loader refuses any
> interface name that does not match `vcan[0-9]*`, and `lab_only` is the only
> mode V1 will parse — there is no runtime path to `can0` at all.

## The one idea worth knowing

**Signal quality is computed, not stored.** Six independent facts are kept per
signal — last good value, when it arrived, the last decode error, whether the
transport is up, whether the value predates a link outage, and the generation —
and `NEVER_RECEIVED` / `FRESH` / `STALE` / `INVALID` / `LINK_UNAVAILABLE` is
derived from them on every read.

That one decision is what makes several hard requirements structural instead of
conventional:

- exactly one stale transition per deadline crossing, because a recomputation
  that matches the last published value reports nothing;
- a malformed frame cannot refresh a stale signal, because only `apply()` may
  move `last_good_at`;
- a recovered link does not fabricate freshness, because transport health and
  provenance are separate fields from the deadline.

The practical consequence for a caller: `SignalSample::value` is **absent** when
nothing was ever received, and otherwise is paired with a quality that says how
much to trust it. A consumer can never be handed a stale speed as `0.0` or a
missing ignition as `false`.

## Using the library

```cpp
#include "tcu/can/can_config.hpp"
#include "tcu/can/can_service.hpp"

tcu::can::CanLabConfig config;
const auto load = tcu::can::loadCanLabConfig("config/can_lab.json", config);
if (!load) {
  // load.pointer is an RFC 6901 pointer; load.publicReason comes from a closed
  // template set and never echoes a value from the file.
  return 1;
}

tcu::can::CanServiceDeps deps;
deps.logger = logger;  // optional
deps.bus = bus;        // optional
tcu::can::CanService service(std::move(deps));

if (!service.start(config)) {
  return 1;  // the configuration was rejected; nothing was created
}

if (const auto speed = service.readSignal("speed_kmh")) {
  if (speed->quality == tcu::can::SignalQuality::Fresh) {
    use(*speed->value);
  }
}

service.stop();  // idempotent; never call it from inside a callback
```

The public surface is specified in
[`contracts/can_api.md`](../../specs/005-virtual-can-bus-laboratory/contracts/can_api.md).
`FakeCanTransport` is public on purpose, like project 03's `FakeClockAdapter`:
projects 06 and 07 will need it, and it is the only way to inject the malformed
frames the kernel refuses to carry.

## Executables

| Binary | What it does |
|---|---|
| `tcu_can_service` | the subject: receives, decodes, serves health and metrics over IPC |
| `vehicle_simulator` | the synthetic ECUs: periodic frames, scenario files, runtime fault injection |
| `bcm_simulator` | the synthetic body control module: `0x200` in, `0x201` and `0x103` out |
| `can_control_cli` | the operator console: pause, resume, corrupt, read health and metrics |

`scripts/demo_normal.sh` and `scripts/demo_signal_loss.sh` drive the whole lab
end to end.

## What it reuses

Per Constitution Principle II, this project consumes the earlier ones rather
than re-deriving anything:

| Project | What it provides here |
|---|---|
| 01 `tcu_eventbus` | the publish path, via three payload alternatives **appended** to its closed variant |
| 02 `tcu_ipc` | the control plane for `can_control_cli`, instead of a second hand-rolled protocol |
| 03 `tcu_scheduler` | the single injectable clock, and `PeriodicMode::FixedDelay` for a health timer that structurally cannot overlap itself |
| 04 `tcu_logging` | bounded async JSONL logging with `DataClass` redaction, so "do not log raw payloads" is enforced rather than advised |

The one place reuse was rejected is configuration: project 04's `tcu_config` is
hard-typed to its own snapshot struct and flat key table, and `can_lab.json` has
a per-signal object map it cannot express. The reasoning is in
[`research.md`](../../specs/005-virtual-can-bus-laboratory/research.md) R-06 and
in `plan.md` Complexity Tracking.

## Threading

| Thread | Owns | Must never |
|---|---|---|
| RX reader | the sockets and the eventfd; the degrade-and-retry backoff | invoke a callback, or decode a frame |
| Decode worker | the validator and decoder | touch a descriptor |
| Scheduler worker (project 03) | — | touch a descriptor, or call `stop()` |
| Caller | `start`, `stop`, reads, `sendDemoCommand`, `metrics` | call `stop()` from inside a callback |

No callback and no event publish ever happens under the socket, queue, cache or
lifecycle mutex. Every publish is driven by a value a cache call already
returned, after its lock was released.

## Limitations

Design boundaries, not defects. Each one is a spec non-goal or a stated V2 item.

- **`vcan` is not a CAN bus.** It cannot reproduce bus arbitration timing,
  transceiver behaviour, electrical faults or authentic bus-off recovery. Those
  need HIL or real hardware.
- **A successful `send()` means the local socket accepted the frame** — never
  that a peer processed it. For the BCM demo, only a matching `0x201` proves
  delivery.
- **`can_rx_read_total` does not prove the kernel dropped nothing.**
  `SO_RXQ_OVFL` is a V2 item.
- **Some malformed frames cannot be placed on a real `vcan0` at all**, so they
  are injected through `FakeCanTransport`. An impossible on-wire test is never
  claimed as passed.
- **The decoder is hard-coded to a synthetic seven-frame catalog.** General DBC
  decoding is project 06.
- **Classical CAN only**, standard 11-bit identifiers, exactly 8 data bytes.
  CAN FD is a V2 extension.
- **A frame with any out-of-range field is rejected whole**, so a valid field can
  be discarded alongside an invalid one in `0x102` and `0x110`. Per-signal reject
  becomes the better answer once project 06 makes per-signal validity
  independent.
- **One BCM command in flight.** That is what makes the 8-bit sequence wrap
  harmless; it is not a protocol that scales to several requesters.
- **No configuration hot reload**, and no persisted signal snapshot across a
  restart: after a restart every signal is `NEVER_RECEIVED` until a new frame
  arrives.
- **Inbound frames are not authenticated.** Classical CAN offers nothing to
  authenticate with; that is a later project's problem.

## Platform

Linux only — the transport is `PF_CAN`/`SOCK_RAW`/`CAN_RAW` with `<linux/can.h>`,
plus `eventfd(2)` and `poll(2)`. No public header includes a Linux header, and a
CMake-registered test enforces that, so the domain model stays OS-neutral.

The integration suite **skips** when `vcan0` is unavailable rather than failing:
a stock WSL2 kernel ships no `vcan` module at all, and creating the interface
needs `CAP_NET_ADMIN`. CI provisions it, which is where those tests actually
run. Unit, stress, fuzz and sanitizer evidence all come from `FakeCanTransport`
and need no kernel module.
