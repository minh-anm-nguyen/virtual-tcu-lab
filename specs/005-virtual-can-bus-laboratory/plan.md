# Implementation Plan: Virtual CAN Bus Laboratory

**Branch**: `005-virtual-can-bus-laboratory` | **Date**: 2026-11-05 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/005-virtual-can-bus-laboratory/spec.md`

## Summary

Build `tcu::can`, a C++17 SocketCAN library, plus four executables that make a TCU talk to simulated
ECUs over a Linux virtual CAN interface with no hardware.

The library is one RX reader owning `poll()` over the CAN socket and an `eventfd`, a bounded
`DropNewest` ring, a single decode worker, a hand-written decoder for seven synthetic frame IDs, and
a signal cache whose central idea is that **quality is computed, not stored**: `last_good_value`,
`last_good_at`, `last_error` and `transport_available` are four separate fields, and
`NeverReceived` / `Fresh` / `Stale` / `Invalid` / `LinkUnavailable` is derived from them on every
read. That one decision discharges several of the spec's hard requirements at once — exactly one
stale transition per deadline crossing, a malformed frame that cannot refresh a stale signal, a
recovered link that does not fabricate freshness, and a consumer that can never mistake a stale
speed for `0.0`.

Four processes prove it: `tcu_can_service` (the subject), `vehicle_simulator` (deterministic
periodic ECU traffic), `bcm_simulator` (synthetic lock command and ACK correlation) and
`can_control_cli` (runtime fault injection and the health/metrics surface).

**This project consumes all four earlier projects rather than re-deriving anything** — Constitution
Principle II, and in every case the earlier project already solved the harder half:

| Reused | What it gives project 05 | Research |
|---|---|---|
| 03 `tcu_scheduler` | `ClockAdapter`/`FakeClockAdapter` as the single injectable clock, and `PeriodicMode::FixedDelay` for a health timer that structurally cannot overlap itself | R-03, R-04 |
| 04 `tcu_logging` | bounded async JSONL logging with `DataClass` redaction, which turns section 16.1's "avoid logging raw payloads" warning into an enforced policy | R-05 |
| 01 `tcu_event_bus` | FR-07's publish path, via three payload alternatives **appended** to its closed variant | R-07 |
| 02 `tcu_ipc` | the control plane `can_control_cli` needs, instead of a second hand-rolled protocol | R-08 |

The one place reuse was rejected is configuration: project 04's `tcu_config` is hard-typed to its
own snapshot struct and flat key table, and `can_lab.json` has a per-signal object map it cannot
express. Project 05 loads its own file with the already-pinned nlohmann/json. That is recorded in
Complexity Tracking below, as the Constitution's Governance section requires.

**Honest boundary on this host:** the stock WSL2 kernel has no `vcan` module
(`modprobe: FATAL: Module vcan not found`), so section 18.2's integration tests cannot run here.
They skip with a printed reason rather than failing, and CI provisions `vcan` so they run for real
(R-01). Unit, stress and TSan evidence all come from `FakeCanTransport` and are available on every
host.

## Technical Context

**Language/Version**: C++17, no compiler extensions in library code (Constitution Technology
Constraints).

**Primary Dependencies**: `tcu_scheduler` (project 03), `tcu_logging` (project 04),
`tcu_event_bus` (project 01), `tcu_ipc` (project 02) — all in-repo. `nlohmann/json` v3.11.3 single
header, already pinned by SHA256 at the repo root for project 04, linked **PRIVATE** and wrapped
behind `src/json_document.hpp` so no consumer inherits it. Linux UAPI `<linux/can.h>`,
`<linux/can/raw.h>`, `<sys/eventfd.h>`, `<poll.h>`. No new third-party dependency.

**Storage**: none. Two read-only JSON inputs (`config/can_lab.json`, `config/scenarios/*.json`).
CAN frames are explicitly **not** persisted (section 13.9).

**Testing**: GoogleTest/GoogleMock via CTest. 22 unit tests (section 18.1), 8 integration scenarios
on `vcan0` (section 18.2), a stress and decoder-fuzz suite (section 18.3), and `can_bench` for
latency characterization (section 18.4). ASan+UBSan and TSan through the three existing CI jobs.

**Target Platform**: Linux with SocketCAN. `tcu_can` and its four executables sit inside the root
`CMakeLists.txt` Linux guard, next to project 02 (R-00). The public domain model includes no Linux
header, which is how NFR-10 is satisfied (R-02).

**Project Type**: single static library plus four CLI executables, in the existing monorepo layout.

**Performance Goals**: none asserted. NFR-05 and section 18.4 forbid inventing a latency target;
`can_bench` reports p50/p95/p99 with the host conditions recorded alongside.

**Constraints**: bounded RX queue with capacity > 0 and no unbounded buffering (NFR-02); RAII for
every FD, thread and subscription (NFR-04); malformed input never terminates the service (NFR-06);
stale and invalid never silently become nominal values (NFR-07); every drop and reject path has a
distinguishable counter (NFR-08); no infinite join, no callback after destruction, no close/use race
(NFR-11); `lab_only` mode refuses any interface not matching `vcan[0-9]*` (NFR-01, R-16).

**Scale/Scope**: 7 synthetic frame IDs, 7 signals, 5 quality states, 5 service states, 1 library,
4 executables, 2 owned threads in the service plus the scheduler's. 16 functional requirements,
12 non-functional, 30 BDD contracts, 19 backlog tickets (CAN-001..019).

No `NEEDS CLARIFICATION` remains: every technical unknown and all ten of the spec's section 27 open
questions are resolved in [research.md](./research.md) — see R-06 (config), R-07 (bus), R-11 (filter
mask), R-14 (open question 5), R-15 (open question 4), R-16 (lab mode), R-20 (open question 3),
R-21 (open question 8), R-01 (open question 6), R-24 (open questions 7, 9, 10).

## Constitution Check

*GATE: passed before Phase 0 research; re-checked after Phase 1 design.*

| Principle | Verdict | Evidence |
|---|---|---|
| **I. Spec Fidelity and Scope Discipline** | **Pass** | Every section 3.2 non-goal is listed as deferred in R-24 with its spec basis: no DBC, no ISO-TP/UDS/J1939/CANopen/CAN XL, no CAN FD in V1, no exactly-once delivery, no persistence, no physical-fidelity claim. Additions beyond the literal spec text are declared in Complexity Tracking rather than slipped in. |
| **II. Library-First Reuse** | **Pass** | All four earlier projects are consumed through their public CMake targets (table in Summary). No fake clock, no scheduler, no logger and no IPC protocol is re-written. The two local pieces — the `DropNewest` ring and the config loader — are justified in Complexity Tracking, and in neither case does an earlier project expose the capability publicly. |
| **III. Test-First** | **Pass by construction** | Every FR and every BDD contract has a named test below, and the task list orders each test before its implementation. Coverage is defined by the spec, not a percentage. |
| **IV. Simulation-Only** | **Pass** | `vcan` only, never a physical interface — enforced, not merely documented (R-16). Time is injectable (`ClockAdapter`), randomness is seeded (`fixed_seed`), and every fault in the section 15 matrix is injectable through `FakeCanTransport` or the IPC control plane. |
| **V. Safe Observability** | **Pass** | Raw CAN payloads are `DataClass::VehicleSensitive` and never logged above `Debug` (R-05). Config errors report a JSON pointer and a closed-template reason, never the offending value. Drop and reject logs are rate-limited. Event-publish failures are counted, never swallowed. |

### Requirement to test traceability

| FR | Requirement | Test |
|---|---|---|
| FR-01 | open the configured interface | `Integration_BindsConfiguredInterface`, `ConfigLoader_RejectsNonLabInterface` |
| FR-02 | receive classical standard frames | `Integration_CansendRoundTrip` |
| FR-03 | send validated frames | `Integration_CandumpSeesOutgoingFrame` |
| FR-04 | per-ID receive filters | `Integration_FilterExcludesUnrelatedId` |
| FR-05 | decode the whole catalog | `Decoder_GoldenVectors` (UT-01..UT-04, UT-19) |
| FR-06 | reject EFF/RTR/error/bad length | `Validator_RejectsFlags`, `Validator_RejectsLength` (UT-06..UT-08) |
| FR-07 | publish value + source ID + timestamp + quality | `EventAdapter_PublishesSignalUpdated` |
| FR-08 | detect stale signals explicitly | `Cache_StaleAtBoundary`, `Cache_StaleAfterBoundary` (UT-11, UT-12) |
| FR-09 | recover freshness on resume | `Cache_RecoversOnce` (UT-13) |
| FR-10 | bounded queue and overload policy | `Queue_DropNewestWhenFull` (UT-14), `Stress_QueueStaysBounded` |
| FR-11 | graceful start/stop including pending `poll` | `Service_RepeatedStartStop` (UT-17), `Integration_StopWakesPoll` |
| FR-12 | simulator state and reproducible seed | `Simulator_SameSeedSameSequence` (B-29) |
| FR-13 | DEGRADED state and controlled retry | `Service_EntersDegradedOnLinkError` (UT-16), `Integration_InterfaceDownUp` |
| FR-14 | BCM command/ACK sequence matching | `Correlator_IgnoresUnmatchedAck` (UT-18), `Integration_BcmLockAck` |
| FR-15 | export counters, quality and recent errors | `Metrics_InvariantsHold`, `Service_HealthJsonShape` |
| FR-16 | run with no hardware and no cloud | the whole unit and stress suite; CI debug job |

### BDD contract traceability (spec section 14)

| Contract | Test |
|---|---|
| B-01, B-27 | `Service_StartTwiceRejected` |
| B-02 (with UT-22) | `Cache_NeverReceivedHasNoValue` |
| B-03, B-04, B-05 | `Decoder_GoldenVectors`, `Cache_ZeroIsAValue`, `Cache_GenerationIncrements` |
| B-06, B-07 | `Cache_StaleAtBoundary`, `Cache_StaleAfterBoundary` |
| B-08 | `Cache_OneStaleTransitionOnly` |
| B-09 | `Cache_RecoversOnce` |
| B-10, B-11 | `Cache_InvalidDoesNotRefresh` |
| B-12 | `Service_BroadFilterCountsUnknownId` |
| B-13, B-14, B-15 | `Validator_RejectsRtr`, `Validator_RejectsExtendedNoAlias`, `Validator_RejectsErrorFrame` |
| B-16 | `Queue_DropNewestWhenFull` |
| B-17 | `Decoder_InterleavedIdsNoCrossTalk` |
| B-18 | `Cache_OnlyExpiredSignalGoesStale` |
| B-19, B-20 | `Service_EntersDegradedOnLinkError`, `Cache_LinkRestoredStaysNonFresh` |
| B-21 | `Service_SendWhenStoppedIsLinkUnavailable` |
| B-22, B-23, B-24 | `Integration_BcmLockAck`, `Correlator_IgnoresUnmatchedAck`, `Bcm_RejectsUnknownCommand` |
| B-25, B-26 | `Service_StopWakesReader`, `Service_StopIsIdempotent` |
| B-28 | `Integration_RestartIsNeverReceived` |
| B-29 | `Simulator_SameSeedSameSequence` |
| B-30 | `Stress_RandomFramesWithRestarts` |

Fault-matrix rows from section 15 that no BDD contract names are covered by
`FakeTransport_InjectsEio`, `Stress_DecoderFuzz` and `Correlator_IgnoresDuplicateAck`.

## Project Structure

### Documentation (this feature)

```text
specs/005-virtual-can-bus-laboratory/
├── plan.md              # This file
├── research.md          # Phase 0: R-00..R-25
├── data-model.md        # Phase 1: 12 entities, their rules and transitions
├── quickstart.md        # Phase 1: vcan setup and 10 validation scenarios
├── contracts/           # Phase 1
│   ├── frame_catalog.md #   the wire contract, golden vectors, filter set, rejection rules
│   ├── can_api.md       #   public API pre/postconditions and the threading table
│   ├── config_schema.md #   can_lab.json and scenario JSON keys, ranges, error reporting
│   └── metrics.md       #   counters, invariants, log contract, health surface
├── spec.md              # The design document, copied verbatim
└── tasks.md             # Phase 2 output (/speckit-tasks — not created by /speckit-plan)
```

### Source Code (repository root)

```text
modules/can/                               # NEW: the library, Linux-guarded
├── CMakeLists.txt
├── include/tcu/can/                       # no Linux header appears here (NFR-10, R-02)
│   ├── can_types.hpp                      # CanMessage, SignalSample, the five enums
│   ├── frame_catalog.hpp                  # constexpr FrameDefinition / SignalDefinition tables
│   ├── can_transport.hpp                  # ICanTransport, ReadResult
│   ├── socketcan_transport.hpp            # declaration only; UAPI stays in src/
│   ├── fake_can_transport.hpp             # PUBLIC, like project 03's FakeClockAdapter (R-22)
│   ├── frame_validator.hpp
│   ├── demo_signal_decoder.hpp
│   ├── signal_cache.hpp
│   ├── can_config.hpp                     # CanLabConfig, LoadError, LoadResult
│   ├── can_metrics.hpp                    # CanMetricsSnapshot
│   └── can_service.hpp                    # the facade
└── src/
    ├── socketcan_fd.hpp                   # RAII FD wrapper; one of two files with <linux/can.h>
    ├── socketcan_transport.cpp            # the other one
    ├── fake_can_transport.cpp
    ├── frame_validator.cpp
    ├── demo_signal_decoder.cpp
    ├── signal_cache.cpp
    ├── bounded_frame_queue.hpp            # DropNewest ring (R-17)
    ├── command_correlator.hpp/.cpp        # PendingCommand state machine
    ├── event_bus_adapter.hpp/.cpp         # publishes outside every lock
    ├── json_document.hpp/.cpp             # wraps nlohmann/json so it stays PRIVATE (R-06)
    ├── can_config_schema.hpp              # the constexpr key table
    ├── config_loader.cpp
    └── can_service.cpp

apps/tcu_can_service/main.cpp              # NEW: the subject + IPC health/metrics methods
apps/vehicle_simulator/main.cpp            # NEW: periodic TX + IPC control server (R-08)
apps/bcm_simulator/main.cpp                # NEW: 0x200 -> 0x201 + 0x103
apps/can_control_cli/main.cpp              # NEW: IPC client

config/can_lab.json                        # NEW
config/scenarios/normal_drive.json         # NEW
config/scenarios/speed_signal_loss.json    # NEW
config/scenarios/invalid_battery.json      # NEW

scripts/setup_vcan.sh                      # NEW, idempotent
scripts/teardown_vcan.sh                   # NEW, idempotent
scripts/demo_normal.sh                     # NEW
scripts/demo_signal_loss.sh                # NEW

tests/support/vcan_probe.hpp               # NEW: the R-01 skip gate
tests/unit/can/                            # NEW: UT-01..UT-22 + metrics invariants
tests/integration/can/                     # NEW: the 8 scenarios of section 18.2
tests/stress/can/                          # NEW: B-30 + decoder fuzz
benchmarks/can_bench/                      # NEW: section 18.4 characterization

modules/core/event_bus/include/tcu/eventbus/event.hpp   # MODIFIED: 3 appended payloads (R-07)
CMakeLists.txt                                          # MODIFIED: project 05 subdirectories
.github/workflows/ci.yml                                # MODIFIED: modprobe vcan step
```

**Structure Decision**: the spec's section 20 proposes a standalone-project tree
(`include/tcu/can/`, `src/`, `apps/` at the repository root). This repository is a monorepo where
projects 01-04 each live in `modules/<name>/` with `include/` and `src/` beneath, apps in
`apps/<name>/`, and tests split across `tests/{unit,integration,stress}/<name>/`. Constitution
Principle II ("each project is its own CMake target with a public header surface") and the
Technology Constraints ("each project owns its directory, its public headers, its tests and its own
CMakeLists.txt") make the monorepo layout binding, so section 20's file *names* are kept and its
*locations* are mapped onto the existing convention. Nothing in the spec depends on the paths.

## Complexity Tracking

> Deliberate deviations. The Constitution's Governance section requires each to name the simpler
> alternative and why it was rejected; an unrecorded deviation is a defect.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|---|---|---|
| Project 05 loads `can_lab.json` itself instead of using project 04's `tcu_config` (R-06) | FR-01 needs a configured interface and section 22 is a stated design contract; `signals` is a map of per-signal objects | Extending `tcu_config` means editing project 04's public `ConfigSnapshot` and its fixed 13-row flat key table — a spec change to a committed project, which Principle I says must go into that spec first. It also cannot express a per-signal object map, and it would couple a CAN config typo to the connectivity service's startup. |
| A local `BoundedFrameQueue` with `DropNewest` (R-17) | ADR-04 requires the RX reader never to block, with every drop counted | Project 01's queue blocks or rejects per publish and project 03's ready queue is an internal callback FIFO. Both are private to `src/`. Promoting either would make an implementation detail of a committed project part of its public contract, to serve a policy neither implements. |
| Three payload alternatives appended to project 01's `EventPayload` (R-07) | FR-07 names "Event subscriber integration" as its evidence, so the bus is in scope | Reusing project 01's existing `IgnitionChanged` and `VehicleSpeedUpdated` was considered first and rejected: neither carries quality or generation, so a subscriber could not distinguish a fresh 0 km/h from a stale one — exactly what NFR-07 and section 6.4 forbid. The file's own comment designates appending as the sanctioned extension, and nothing is reordered. |
| A 7th signal, `ecu_status`, beyond section 22's six | Sections 6.1 and 6.3 both define byte 1 of `0x110` as a decoded field with its own valid range (`0 OK, 1 DEGRADED`), and section 4 expects a "DEGRADED" observation | Folding it into `heartbeat` would put two unrelated quantities in one `double`, and dropping it would make an ECU degradation the spec defines unreadable through the API the spec also defines. |
| `CanMessage` carries a `FrameKind { Data, Remote, Error }` field, which the spec's section 9.1 struct does not have | Section 6.2 requires V1 to "reject frames with unsupported flags (EFF/RTR/ERR), rather than quietly interpreting them as data", and UT-07, B-13 and B-15 test that **`FrameValidator`** rejects and classifies them | Checking RTR and ERR only at the socket, as section 9.5's raw-frame snippet does, leaves the validator unable to see them — so those three tests would have nothing to assert against, and `FakeCanTransport::pushRaw` could not construct the frame the kernel refuses to carry. `FrameFormat` could not absorb them without conflating "11 or 29 bit" with "data, remote or error". |
| `DecodeResult::reason` is a `RejectReason` enum, not the spec's illustrative `std::string` | A reason must be loggable and metric-taggable | A string built from rejected input is a log-injection path, and section 16.1 explicitly warns against echoing payloads into logs (Principle V). The enum is also what lets `can_rx_invalid_total` split cleanly into three counters for NFR-08. |
| `CanService` adds `readAll()`, `metrics()` and `healthJson()` beyond section 9.4's facade | FR-15 requires counters, current quality and recent errors to be exportable, and section 16.2 specifies the JSON shape | `readSignal()` alone cannot serve FR-15 without the caller knowing every signal name, and section 16.2's surface would then have to be assembled outside the library that owns the cache mutex. |
| `--broad-filter` mode on `tcu_can_service` | B-12 requires observing `can_unknown_id_total` rise, which the socket filter otherwise prevents | Removing the filter in normal operation would violate FR-04 and section 13.3. A test-only flag keeps the production path filtered. |

## Deferred shortcuts

Each is a V1 limit with the condition that should trigger the upgrade, stated here so a later
project does not have to rediscover it. Full list and spec basis in [research.md](./research.md)
R-24.

- `shortcut: no SO_RXQ_OVFL, so can_rx_read_total cannot prove the kernel dropped nothing. Upgrade
  when a measurement needs to separate kernel loss from application loss` — open question 7.
- `shortcut: atomic frame reject discards a valid field alongside an invalid one in 0x102 and 0x110.
  Upgrade to per-signal reject when project 06's DBC makes per-signal validity independent` — R-14.
- `shortcut: nonzero reserved bytes are counted, not rejected. Upgrade when a schema version
  formalizes strict reserved-bit validation` — R-15.
- `shortcut: one BCM command in flight, so the 8-bit sequence cannot wrap ambiguously. Upgrade to a
  keyed pending map when more than one requester exists` — R-20.
- `shortcut: config is read once with no hot reload. Upgrade by routing through tcu_config once its
  snapshot type can carry a per-signal map` — R-06.
- `shortcut: a single decode worker. Upgrade only after metrics show decode is the bottleneck and
  per-signal ordering semantics are specified` — ADR-03.

## Post-implementation reconciliation

**Date**: 2026-11-05 | **Host**: Windows 11 + WSL2, kernel `6.6.87.2-microsoft-standard-WSL2`,
gcc 11.4 and clang, CMake 3.31

### Test evidence

| Suite | Result |
|---|---|
| `can-unit` (189 tests) | **all pass** |
| `can-stress` (4 tests, including 200 000 fuzz iterations) | **all pass** |
| `can-integration` (15 tests) | **all SKIPPED on this host** — see below |
| Whole repository (projects 01-05) | **576 pass, 0 fail** |
| ASan + UBSan, `can-unit` + `can-stress` (193) | **all pass, no finding** |
| ThreadSanitizer, `can-unit` + `can-stress` (193) | **all pass, no data race** |

### What did NOT run, and why

**Every `vcan0` integration test skipped.** The stock WSL2 kernel on this host ships no `vcan`
module (`modprobe: FATAL: Module vcan not found`), and creating the interface also needs
`CAP_NET_ADMIN`. The 15 integration cases in `tests/integration/can/` are written, compile, and skip
with a printed reason; `.github/workflows/ci.yml` now provisions `vcan0` in all three jobs, so they
execute there. **No claim is made that any of them has passed.** Spec section 18.2's test note
forbids exactly that.

Consequently `SocketCanTransport` — `open`, `poll`, `read`, `write`, the `CAN_RAW_FILTER` install
and the eventfd wakeup — is verified only at compile time, plus its `static_assert`s against the
real UAPI constants. Everything above the transport is verified against `FakeCanTransport`, which is
also the only way to reach the malformed-frame cases (section 18.1 note).

**The quickstart walkthrough (T112) was not performed by hand** for the same reason. The one-shot
`--health` path was exercised and produces the contracted shape: `DEGRADED`, and every signal
`LINK_UNAVAILABLE` with `"value": null`.

### Definition of Done, spec section 26

| Criterion | Status |
|---|---|
| Only fake identifiers and `vcan` by default; no physical car | **Met** — enforced rather than documented: `lab_only` is the only parseable mode and non-`vcan*` names are refused (`ConfigLoader.RejectsNonLabInterface`) |
| Config and startup validation prevent real-interface access | **Met** |
| CMake builds with a supported Ubuntu toolchain | **Met** — gcc 11.4 and clang, Ninja |
| `setup_vcan.sh` and teardown repeatable and idempotent | **Met** by construction; **not executed** here (no `vcan` module) |
| `candump` confirms simulator frames, `cansend` confirms TCU receive | **Written, not run** — `VcanRoundtrip.CansendRoundTrip`, `CandumpSeesOutgoingFrame` |
| Known inputs decode to expected values with independent golden vectors | **Met** — 17 vectors from spec section 6.2, asserted in `Decoder.GoldenVectors`; the same strings are what `cansend` injects in the integration suite |
| Signal quality explicit across all five states | **Met** |
| Only a valid frame refreshes the freshness timestamp | **Met** — `Cache.RejectedFrameCannotRescueAStaleSignal` |
| Stale and recovered emitted once each, not per timer tick | **Met** — `Cache.OneStaleTransitionOnly` holds across 40 ticks |
| Unknown, extended, RTR, error and invalid-length cases do not mutate signals | **Met** |
| RX queue within capacity; every drop counted | **Met** — asserted at 1 000-frame intervals through 50 000 frames |
| Interface failure and recovery observable, no busy loop | **Partially met** — the logic and its unit tests are in place (`Service.BackoffWakesOnStop`, `Service.EntersDegradedOnLinkError`); the real `ip link down/up` path is **unverified** on this host |
| BCM command/ACK correlation tested including mismatch and duplicate | **Met** at unit level (15 correlator cases); the on-bus path is written but unrun |
| `stop()` idempotent, wakes RX poll, no deadlock | **Met** — including 10 start/stop cycles under ASan and TSan |
| Unit tests pass; vcan integration tests pass on a supported host | **Half met** — unit yes, integration unrun here |
| ASan/UBSan findings addressed; TSan documented | **Met** — both clean |
| Benchmark conditions and results recorded without unverified claims | **Met** — `can_bench` prints host CPU, kernel, compiler, flags, queue capacity, rate, duration and peak RSS beside p50/p95/p99, and asserts nothing |
| Limitations and safety restrictions in README | **Met** — [`modules/can/README.md`](../../modules/can/README.md) |
| Demo, behaviour contracts and tickets match implemented semantics | **Met**, with the deviations below folded back into the specs |

### Design gaps the implementation found

Three things the plan had wrong, each caught by a test rather than by review, and each written back
into the specs rather than patched silently:

1. **`CanMessage` had nowhere to put RTR and ERR.** Spec section 9.1's struct has no field for
   either, yet UT-07, B-13 and B-15 require the *validator* to reject and classify them. Added
   `FrameKind`; recorded in Complexity Tracking above and in data-model.md section 2.
2. **A brief link outage left a value reading `FRESH`.** The deadline measures *age*; B-20 is about
   *provenance*. One field could not do both, so `SignalState` gained `unproven_since_link_loss`.
   Caught by `Cache.BriefOutageStillRequiresANewFrame`; data-model.md section 6 now documents the
   extra fact and the six-step precedence.
3. **The log rate limiter used `SteadyTime{}` as its "never fired" sentinel**, which a
   `FakeClockAdapter` sitting exactly at the epoch made indistinguishable from a real reading — so
   the limiter passed everything through. Caught by `Service.DropLogIsRateLimited`; replaced with an
   explicit flag.

Two smaller corrections, in tests rather than in the design: an extended frame whose low 11 bits
equal a known identifier genuinely *has* that numeric identifier, so B-14 is a test of the `format`
field and not of identifier arithmetic; and the documented pattern `vcan[0-9]*` admits a bare
`vcan`, which the first draft of `RejectsNonLabInterface` wrongly had in its reject list.

### Deliberate scope decisions

- `apply()` keeps its `bool` return from contracts/can_api.md section 4; the recovery transition is
  reported through `refresh()`, which the service calls immediately afterwards. One mechanism serves
  every transition, which is what keeps B-08 and B-09 free of extra bookkeeping.
- The degraded backoff waits on the service's own condition variable rather than the transport's
  eventfd, which research R-18 had named. The eventfd belongs to a socket that is being replaced,
  and polling a broken socket spins. `stop()` still wakes it instantly, which is the actual
  requirement; `Service.BackoffWakesOnStop` asserts that against a five-second interval.
- The eight scenarios of section 18.2 are implemented across two files rather than six, and the BCM
  side of the on-bus test is a responder built on a second `SocketCanTransport` instead of the
  `bcm_simulator` process — the correlation logic under test lives in the library, and one
  executable is easier to keep honest than a process-spawning harness.
- T082 (simulator control over IPC, as a gtest) is covered instead by `scripts/demo_signal_loss.sh`,
  which drives the real `vehicle_simulator` and `can_control_cli` processes. A gtest that forked two
  binaries would test the harness more than the behaviour.
