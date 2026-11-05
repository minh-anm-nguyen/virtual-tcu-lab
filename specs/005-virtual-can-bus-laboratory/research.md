# Phase 0 Research: Virtual CAN Bus Laboratory

**Feature**: `005-virtual-can-bus-laboratory` | **Date**: 2026-11-05

Every decision below either resolves a `NEEDS CLARIFICATION` from [plan.md](./plan.md) Technical
Context, answers one of the ten open questions in spec section 27, or fixes a point where the spec
offers a choice and the implementation needs exactly one.

Format per entry: **Decision**, **Rationale**, **Alternatives considered**.

---

## R-00: Target platform — Linux only, inside the root CMake guard

**Decision**: `tcu_can` and all four project-05 executables are added inside the existing
`if(CMAKE_SYSTEM_NAME STREQUAL "Linux")` block in the root `CMakeLists.txt`, next to project 02.

**Rationale**: the transport is `PF_CAN`/`SOCK_RAW`/`CAN_RAW` with `<linux/can.h>` and
`<linux/can/raw.h>` (spec section 3.3 and 10.1), plus `eventfd(2)` and `poll(2)`. None of these has
a Windows equivalent. Project 02 established the precedent for a Linux-only project in this
monorepo and the root `CMakeLists.txt` already carries the guard, so this costs no new CMake
machinery.

**Alternatives considered**:

- *Split into a portable `tcu_can_core` (types, validator, decoder, cache, queue) plus a Linux
  `tcu_can_socketcan`*: would let 22 of the spec's unit tests build on Windows. Rejected: two
  targets, two include trees and two test directories to buy a convenience that WSL already
  provides, and spec section 18.2 needs Linux for integration tests regardless. NFR-10 asks that
  the *domain model* not be tied to Linux, which R-02 satisfies without a second target.
- *A compile-time stub transport on Windows*: rejected — a transport that cannot transport is a
  maintenance cost with no user.

## R-01: `vcan` availability is a runtime fact, not a build fact

**Decision**: integration tests in `tests/integration/can/` **skip** (GTest `GTEST_SKIP()`) when
`vcan0` cannot be opened, and print why. They never fail for a missing interface. A helper
`tests/support/vcan_probe.hpp` performs the probe once per process: `if_nametoindex("vcan0")`
followed by a bind attempt.

**Rationale**: this answers spec open question 6 for this repository. The development host here is
Windows with WSL2, whose stock kernel (`6.6.87.2-microsoft-standard-WSL2`) ships **no `vcan`
module** — verified: `modprobe: FATAL: Module vcan not found`. Creating `vcan0` also needs
`CAP_NET_ADMIN`. A test suite that fails on every developer machine teaches people to ignore red
builds. GitHub's `ubuntu-latest` runner does load `vcan` via `sudo modprobe vcan`, so
`.github/workflows/ci.yml` gains a provisioning step and the tests run there for real.

**Alternatives considered**:

- *Fail hard when `vcan0` is absent*: rejected, see above.
- *Require every developer to run a Linux VM*: rejected as a precondition for `ctest`; it stays a
  documented option in [quickstart.md](./quickstart.md) for anyone who wants the integration layer
  locally.
- *Silent skip*: rejected — an invisible skip is indistinguishable from a pass, and spec section
  18.2's test note explicitly forbids claiming an untested path passed.

## R-02: NFR-10 is met by header hygiene, not by a second library

**Decision**: no public header under `modules/can/include/tcu/can/` includes any Linux header. The
Linux UAPI appears only in `src/socketcan_transport.cpp` and in the private
`src/socketcan_fd.hpp`. A CMake-registered grep test asserts that no `linux/` include escaped into
`include/`, and a unit test compiles every public header standalone.

**Rationale**: NFR-10 says "public domain model is not tied to Linux; `SocketCanAdapter` is
Linux-specific". That is a statement about the include graph. `CanMessage`, `SignalSample`,
`ICanTransport`, `FrameValidator`, `DemoSignalDecoder` and `SignalCache` need only `<array>`,
`<chrono>`, `<cstdint>`, `<optional>` and `<string>` — exactly as the spec's own section 9.1
listing shows.

**Alternatives considered**: a separate target (see R-00). Also rejected: `#ifdef __linux__` inside
public headers, which makes the public surface change shape per platform — the worst of both.

## R-03: One injectable clock, reused from project 03

**Decision**: the single time source is `tcu::scheduler::ClockAdapter` (project 03). `SteadyTime` in
`tcu/can/can_types.hpp` is `tcu::scheduler::Clock::time_point`. `CanService` takes a
`std::shared_ptr<tcu::scheduler::ClockAdapter>`; it defaults to `SteadyClockAdapter`. Unit tests
pass `FakeClockAdapter`.

**Rationale**: Constitution Principle II forbids re-implementing what an earlier project provides,
and project 03's `clock_adapter.hpp` says so in its own file header: *"FakeClockAdapter is
deliberately public rather than test-only: projects 10, 13 and 15 all have timeouts to test, and a
private fake would make each of them write it again."* The type also matches exactly —
`tcu::scheduler::Clock` is `std::chrono::steady_clock`, which is what spec section 9.1 names. This
is what makes UT-11, UT-12 and UT-13 (freshness boundary tests) deterministic: the RX stamp, the
cache's `last_good_at` and the freshness monitor's `now` all read one clock, so advancing it moves
all three together.

**Alternatives considered**:

- *Call `std::chrono::steady_clock::now()` directly and add a project-local `IClock`*: rejected as a
  Principle II violation and as a second fake to maintain.
- *Wall clock*: rejected by ADR-05 — NTP steps would fabricate stale transitions.

## R-04: Periodic work runs on project 03's scheduler, with the mode chosen per job

**Decision**: two jobs use `tcu::scheduler::TimerScheduler`:

| Job | Owner | Mode | Why that mode |
|---|---|---|---|
| Freshness / health check, every `health_check_ms` | `CanService` | `PeriodicMode::FixedDelay` | "next deadline = completion time + interval. Drifts, never overlaps." Spec section 25 requires that scheduling "must not overlap same signal update unexpectedly"; FixedDelay makes that structural rather than a convention. |
| Simulator periodic TX, per CAN ID | `vehicle_simulator` | `PeriodicMode::FixedRateSkipMissed` | Keeps the 100 ms / 1 s phase of section 6.1 and, per its contract, counts and skips missed slots instead of replaying them as a burst — a burst would make the TCU's queue-drop metrics meaningless. |

`CanService` therefore owns **two** threads of its own (RX reader, decode worker), not three: the
health timer is a scheduler timer.

**Rationale**: Principle II again, plus the two modes already encode decisions this project would
otherwise have to make and test. The scheduler is standard-library-only and not Linux-guarded, so
linking it adds no platform constraint.

**Alternatives considered**:

- *`sleep_until()` on a steady clock in a dedicated health thread*, which spec section 17.3 permits:
  rejected. It means a third thread, a hand-rolled overlap guard, and a second fake clock for the
  boundary tests.
- *Running the health check on the decode worker between queue pops*: rejected — a full queue would
  starve freshness checks exactly when they matter most (section 13.7).

## R-05: Structured logging reuses project 04; CAN payload bytes are vehicle-sensitive

**Decision**: `tcu_can` links `tcu_logging` and logs through `tcu::logging::ILogger` with typed
`Field`s, producing the JSONL shape of spec section 16.1. Every field carrying raw frame bytes or a
decoded physical value is tagged `DataClass::VehicleSensitive`; `signal_name`, `can_id`, `quality`,
`state` and counters are `DataClass::Public`.

**Rationale**: project 04 already provides bounded async logging, severity filtering and redaction
at ingestion *and* at formatting. Spec section 16.1 warns: "Avoid logging full raw CAN payloads
indefinitely... use access controls, retention policy, and privacy review" — project 04's
`DataClass` is exactly that mechanism, so the warning becomes an enforced policy instead of a
comment. Constitution Principle V requires it.

**Alternatives considered**: `std::cerr` (what project 03 still does in its `src/log.cpp`) —
rejected: project 04 exists now, and its plan already records that new code logs through
`tcu_logging`.

## R-06: Configuration is loaded by this project, **not** by `tcu_config`

**Decision**: `modules/can/src/config_loader.cpp` loads `config/can_lab.json` once at startup with
nlohmann/json (the pinned `nlohmann_json::nlohmann_json` INTERFACE target already created by the
root `CMakeLists.txt` for project 04) and validates it against a `constexpr` table in
`src/can_config_schema.hpp`. nlohmann/json is linked **PRIVATE** and wrapped behind
`src/json_document.hpp`, so no consumer of `tcu_can` inherits it. There is no hot reload in V1.

**Rationale**: `tcu_config`'s public `ConfigSnapshot` is hard-typed to project 04's own four
structs (`ConnectivityConfig`, `TelemetryConfig`, `LoggingConfig`, `NetworkConfig`), its
`kFieldSpecs` table is a fixed 13-row array of project 04's keys, its validator is private
(`src/validator.hpp`), and it rejects unknown keys with no defaults layer. Routing `can_lab.json`
through it would mean editing project 04's public snapshot type and its schema table — a spec change
to a committed project, which Constitution Principle I says must go into *that* spec first. It also
cannot express section 22's shape: `signals` is a map of per-signal objects, not a flat key set.
What project 05 actually needs is a one-shot typed load with range checks, which is far less than
`tcu_config` does (generations, observers, polling, restart-only diffing).

**This is recorded in [plan.md](./plan.md) Complexity Tracking** as required by the Constitution's
Governance section.

**Alternatives considered**:

- *Extend project 04's `kFieldSpecs` with ~20 flat CAN rows and put CAN keys in `tcu.json`*:
  rejected — it changes project 04's spec section 7.2, forces project 04's sample file and its
  `Schema_SampleFileMatchesTable` test to carry CAN keys, and couples a CAN config error to the
  connectivity service's startup.
- *Hard-code the configuration*: rejected — FR-01 requires a configured interface name and section
  22 is a stated design contract.
- *Reuse `apps/vehicle_service/json_lite.hpp`*: rejected — it is an app-private helper, not a
  library target, and nlohmann/json is already pinned and hashed at the root.

## R-07: Event Bus integration appends three alternatives to project 01's variant

**Decision**: `modules/core/event_bus/include/tcu/eventbus/event.hpp` gains three payload
alternatives, **appended** after `ConfigChanged`:

```cpp
struct VehicleSignalUpdated {   // FR-07
  std::string signal_name;
  double value;
  std::uint32_t source_can_id;
  std::uint64_t generation;
  std::int64_t monotonic_age_ms;
};
struct SignalQualityChanged {   // section 11.2, B-08, B-09
  std::string signal_name;
  std::uint8_t quality;         // SignalQuality, narrowed deliberately: the bus must not
                                // depend on tcu_can's headers
  std::uint64_t generation;
};
struct CanLinkStatusChanged {   // section 13.6, B-19, B-20
  bool available;
  std::uint8_t service_state;   // ServiceState, same reason
};
```

Existing alternatives are never reordered; the file's own comment states that reordering changes
every `EventType` value and is breaking.

**Rationale**: FR-07 requires publishing "decoded values with source CAN ID, timestamp and quality
status" with "Event subscriber integration" as its evidence, so the bus is in scope, not optional.
`EventPayload` is a closed `std::variant` whose comment reads *"Projects 05-30 append alternatives
here"* — appending is the sanctioned extension mechanism. The enum-as-`uint8_t` keeps the
dependency one-way: `tcu_can` links `tcu_event_bus`, never the reverse.

One event type covers stale, recovered, invalid and link-unavailable transitions because all four
are the same fact — "this signal's quality changed to X" — and B-08 requires exactly one event per
transition either way.

**Alternatives considered**:

- *Reuse project 01's existing `IgnitionChanged` and `VehicleSpeedUpdated`*: rejected. Neither
  carries quality or generation, so a subscriber cannot tell a fresh 0 km/h from a stale one —
  exactly what NFR-07 and section 6.4 forbid. They stay untouched for project 01's own demo.
- *A separate CAN-only event bus instance*: rejected — Principle II, and subscribers would have to
  know which bus to join.
- *Type-erasing the payload (`std::any` / a base class)*: rejected — a change to project 01's
  design, and its invariant T1 (type and payload cannot disagree) depends on the variant index.

## R-08: The control CLI talks to the simulator over project 02's IPC

**Decision**: `vehicle_simulator` hosts a `tcu::ipc::IpcServer` on `/tmp/tcu_can_sim.sock` with
methods `pause_id`, `resume_id`, `set_signal`, `inject_invalid` and `scenario_status`.
`can_control_cli` is a `tcu::ipc::IpcClient`. `tcu_can_service` likewise exposes `health` and
`metrics` methods, which is what spec section 16.2's JSON surface and the `metrics_cli` actor in
section 4 are served by.

**Rationale**: spec section 4 gives `can_control_cli` a "Control API" to the simulator and section
21.5 step 2 pauses speed publication *at runtime*, so launch flags alone are insufficient. Project
02 is an AF_UNIX JSON request/response plus notification framework with reconnect and timeouts
already tested — building a second control protocol would be a Principle II violation and would
need its own framing, timeout and shutdown tests.

Launch flags from section 15 (`--pause-id 0x101`) are still supported as the initial state, so the
fault matrix's own injection column stays literally true.

**Alternatives considered**:

- *Control frames on `vcan0` using a lab-only CAN ID*: rejected — it adds an ID that is not in the
  section 6.1 catalog, and the TCU would see control traffic it must then filter.
- *POSIX signals or a control file*: rejected — no response, no error reporting, and `set_signal`
  needs arguments.

## R-09: One RX reader on `poll(CAN fd, eventfd)`

**Decision**: `SocketCanTransport` owns the RX socket, a TX socket and an `eventfd`. The RX worker
is the only thread that calls `poll`/`read` on the RX socket. `requestStop()` writes to the eventfd;
`poll` wakes immediately. FDs are released by RAII in `src/socketcan_fd.hpp` after the RX thread has
joined.

**Rationale**: spec sections 5.2, 10.1 step 5/8, 17.1 rules 1 and 6, and NFR-11 all state this
directly. `eventfd` is the spec's own choice and avoids the classic
close-while-another-thread-reads race, which section 24 lists as a risk.

**Alternatives considered**: a read timeout alone (rejected — bounds shutdown latency by the
timeout, and section 13.8 wants an immediate wake); a self-pipe (equivalent, one more FD);
`pthread_cancel` (rejected — no defined C++ object lifetime).

## R-10: Separate RX and TX sockets; own-message loopback left off

**Decision**: two sockets. The RX socket installs the `CAN_RAW_FILTER` set and never enables
`CAN_RAW_RECV_OWN_MSGS`. The TX socket is used for `send()` only, with writes serialized by a mutex.

**Rationale**: section 10.4. With one socket and `RECV_OWN_MSGS` off, a service could not observe
its own command frames at all; with it on, the TCU would decode its own `0x200` as inbound traffic.
Two sockets make "did the frame reach the bus" a `candump` question and keep the application-level
`0x201` ACK the only proof of delivery, which is what section 10.2 insists on.

**Alternatives considered**: one socket with `RECV_OWN_MSGS` on plus a self-sent-frame filter —
rejected, it re-creates the aliasing problem B-14 exists to prevent.

## R-11: Receive filter mask rejects aliasing by construction

**Decision**: each `can_filter` entry is `{ .can_id = id, .can_mask = CAN_SFF_MASK | CAN_EFF_FLAG |
CAN_RTR_FLAG | CAN_ERR_FLAG }` with all flag bits clear in `can_id`. The TCU RX socket filters
`0x100, 0x101, 0x102, 0x103, 0x110, 0x201`; `bcm_simulator` filters `0x200`.

**Rationale**: section 10.1's filter detail says matching is `(received_id & mask) == (filter_id &
mask)` and warns against "a careless filter that lets different frame types alias". Including the
flag bits in the mask is what makes B-14 (an extended-ID frame whose low 11 bits equal a known
standard ID) fail at the kernel rather than in the validator. `FrameValidator` re-checks the same
flags, because a broad-filter test mode (B-12) and `FakeCanTransport` both bypass the kernel.

**Alternatives considered**: `CAN_SFF_MASK` alone (rejected — aliases EFF/RTR frames); no filter at
all with validation in user space (rejected — FR-04 requires per-ID filters, and section 13.3 wants
unknown IDs normally stopped before the application).

## R-12: Freshness boundary is strictly greater

**Decision**: a signal is stale when `now > last_good_at + timeout`. At exactly
`last_good_at + timeout` it is still `FRESH`. Comparison is on
`tcu::scheduler::Clock::time_point`, no floating point anywhere.

**Rationale**: section 13.5 and B-06/B-07 pin this semantic, and UT-11/UT-12 test both sides of the
boundary. Writing it down here is what stops an `>=` creeping in during review.

## R-13: Quality is four stored fields, not one enum

**Decision**: `SignalState` inside `SignalCache` stores `last_good_value`, `last_good_at`,
`last_error` (reason + time), `transport_available` and `generation` as separate fields. The single
`SignalQuality` of the public `SignalSample` is **computed** on read, in this precedence order:

1. `transport_available == false` → `LinkUnavailable`
2. no value ever accepted → `NeverReceived`
3. `now > last_good_at + timeout` → `Stale`
4. `last_error` is newer than `last_good_at` → `Invalid`
5. otherwise → `Fresh`

A transition event is emitted only when the computed quality differs from the last *published*
quality, which is stored per signal.

**Rationale**: section 12.2's own clarification demands exactly this ("Implementation should track
`last_good`, `last_error`, `transport_health`, and `freshness` as **separate fields** so information
is not lost"). Deriving the enum rather than storing it is what makes B-08 ("exactly one stale
transition, no repeated spam") structural: the health tick recomputes, compares to the published
value, and does nothing when they match. It also gives B-20 for free — when transport returns,
quality cannot be `Fresh` until a new frame advances `last_good_at`.

**Alternatives considered**: a stored enum mutated at each event (rejected — every path that forgets
to update it loses information, and section 13.4 requires `last_good_at` to survive an invalid
frame).

## R-14: A known frame with any invalid field is rejected whole

**Decision**: atomic frame reject. `0x102` carrying voltage 12.00 V and SOC 120 updates **neither**
battery signal; both keep their previous value and the frame increments
`can_rx_invalid_total{reason="range"}` once.

**Rationale**: this answers spec open question 5 and takes the spec's own stated V1 default
("reject entire known frame when any required field is invalid; document chosen rule"). A frame is
one sender's snapshot; accepting half of it publishes a state that the ECU never reported. UT-10
tests it.

**Alternatives considered**: per-signal reject — kept as the documented V2 option; it is the better
answer once a DBC (project 06) makes per-signal validity independent.

## R-15: Nonzero reserved bytes are counted, not rejected

**Decision**: observability only. A known frame with nonzero reserved bytes is accepted and
increments `can_reserved_nonzero_total`. One `WARN` per CAN ID per minute, rate-limited.

**Rationale**: answers open question 4 with section 6.1's stated V1 policy ("V1 receiver ignores
them but increments `reserved_nonzero_total`... unless a future schema version formalizes strict
rejection"). Rejecting would make the lab brittle against `cangen` traffic for no safety gain,
since reserved bytes carry no signal. UT-20 tests the counter.

## R-16: Strict lab mode refuses non-`vcan` interfaces

**Decision**: `mode: "lab_only"` (the default, and the only accepted value in V1) makes `start()`
return false with `publicReason="interface_not_lab"` for any interface name not matching
`vcan[0-9]*`. Overriding would require `mode: "unrestricted"`, which V1's schema rejects — so there
is no runtime path to `can0` at all.

**Rationale**: NFR-01, ADR-07 and the first row of the section 24 risk table ("only `vcan*`
interfaces accepted by default"). The spec's safety boundary in section 1 is the strongest statement
in the document; a config typo must not be the thing standing between the lab and a real bus. Making
`unrestricted` unparseable in V1 rather than merely discouraged is the cheap way to honour it.

**Alternatives considered**: a warning only (rejected — the risk table asks for refusal); a compile
flag (rejected — invisible in the config that an operator actually reads).

## R-17: The bounded queue is local to this project

**Decision**: `src/bounded_frame_queue.hpp` — a fixed-capacity ring of `CanMessage`, mutex plus
condition variable, `DropNewest` on full, `depth()` for the metric, and a `close()` that wakes the
consumer for shutdown.

**Rationale**: `DropNewest` is ADR-04 and no existing queue in this repository offers it — project
01's event queue blocks or rejects by policy per publish and project 03's ready queue is an internal
FIFO for callbacks; both are private (`src/`) and neither is a public reusable container. So this
is not re-implementing a provided capability. The ring is about 60 lines and the capacity comes from
config (NFR-02).

**Alternatives considered**: exposing project 01's queue (rejected — it would make a private
implementation detail of project 01 part of its public contract, to serve a different policy);
`std::deque` with a size check (rejected — allocates per frame on the RX path for no gain).

## R-18: Link recovery backs off on the RX thread, not on the scheduler

**Decision**: when `receive()` returns `LinkError`, the RX worker closes the socket, moves the
service to `DEGRADED`, and waits on **the eventfd alone** with a `poll` timeout equal to the current
backoff (`reconnect_initial` doubling to `reconnect_max`, ±10 % jitter), then retries open/bind.
`stop()` still wakes it instantly.

**Rationale**: section 17.1 rule 1 makes the adapter the sole owner of the socket FD and the RX
worker its only receiver. A scheduler timer firing a reopen would hand the FD to a scheduler worker
thread and create exactly the close/use race that section 24 lists as a crash risk. A bounded `poll`
timeout is not a busy loop — section 13.6's actual requirement — and needs no extra code at all,
since the `poll` call is already there.

**Alternatives considered**: a scheduler one-shot timer per retry (rejected, see above);
`sleep_for` (rejected — delays shutdown by a full backoff interval, up to 5 s).

## R-19: Shutdown drains with a deadline

**Decision**: `stop()` is `Drain` with a `drain_deadline_ms` (default 200 ms): signal the eventfd,
join the RX worker, let the decode worker finish the queued backlog until the deadline, count
leftovers in `can_shutdown_discarded_total`, join the decode worker, cancel the health timer,
`stop()` the scheduler with `StopMode::Drain`, release the Event Bus subscription, then the queue,
sockets and eventfd in that order. `stop()` is idempotent. Calling `stop()` from inside a callback
is prohibited by V1's lifecycle rule (spec section 9.4) and asserted in debug builds.

**Rationale**: section 13.8 steps 1-7 verbatim, plus its "A callback must never block `stop()`
forever" — a deadline is what makes that true without trusting callbacks. B-25, B-26 and UT-17 test
it. The ownership order matters: the eventfd must outlive the RX thread (section 17.1 rule 6).

## R-20: BCM lab policy — commands always accepted, one command in flight

**Decision**: `bcm_simulator` accepts `0x01` LOCK and `0x02` UNLOCK regardless of ignition state and
replies `0x00` OK; any other command byte gets `0x02` INVALID; a well-formed command that the lab
state machine refuses gets `0x01` REJECTED. `CanService` allows exactly **one** outstanding command;
a second `sendDemoCommand()` while one is pending returns `WriteStatus::InvalidFrame` with a reason.
An ACK whose sequence does not match the pending one is ignored and counted
(`can_ack_unmatched_total`).

**Rationale**: answers open question 3 — the spec demands this be "an explicit simulator policy, not
an assumption", and a lab with no ignition coupling is the simplest policy to state. One in-flight
command makes the 8-bit sequence wrap of section 11.3 and the section 24 risk row ("8-bit command
sequence wraps → bounded number of in-flight commands in lab") a non-issue by construction, and
B-23/B-24/UT-18 test the mismatch paths.

**Alternatives considered**: a pending-command map keyed by sequence (rejected — V1 has one
requester; a map invites the wrap ambiguity the spec warns about).

## R-21: Simulator determinism — seeded `mt19937`, real monotonic time

**Decision**: scenario values come from `std::mt19937` seeded with `simulator.fixed_seed` from the
scenario JSON. Timing uses the real `SteadyClockAdapter` in integration runs and `FakeClockAdapter`
in unit tests. B-29's assertion is on the **value sequence per CAN ID**, not on wall-clock arrival
times.

**Rationale**: answers open question 8 with the spec's own default ("real monotonic for integration,
fake clock for unit tests") and NFR-09, which scopes reproducibility to "all non-timing values".
A deterministic virtual clock shared across two processes over a real `vcan0` is not achievable
without a time broker, which is out of V1 scope.

## R-22: `FakeCanTransport` is public, like project 03's fake clock

**Decision**: `modules/can/include/tcu/can/fake_can_transport.hpp` is a **public** header: a
scripted `ICanTransport` with `pushFrame()`, `pushRaw()` (bypassing validation, for malformed
input), `pushReadStatus()` (for `LinkError`/`FatalError`/`Timeout` injection) and a recording of
everything `send()` received.

**Rationale**: project 06 (DBC decoder) replaces the decoder behind the same transport and project
07 (gateway) needs two fake buses; both would otherwise write this again, which project 03's
`clock_adapter.hpp` header comment already identifies as the Principle II failure mode. It is also
the only way to reach B-10, B-13, B-14, B-15 and UT-06..UT-08: section 18.1's test note says the
kernel refuses to carry those malformed frames, so they cannot be produced on a real `vcan0`.

## R-23: Metrics are a plain snapshot struct

**Decision**: `CanMetricsSnapshot` is a POD of `std::uint64_t` counters plus `can_queue_depth`, read
through `CanService::metrics()`. Counters live as `std::atomic<std::uint64_t>` with
`memory_order_relaxed`. `can_decode_latency_ms` is **not** a counter: it is an
`std::array<std::uint64_t, N>` bucket histogram, reported as p50/p95/p99 by
`benchmarks/can_bench`.

**Rationale**: matches `tcu::scheduler::SchedulerMetricsSnapshot`, project 04's `ConfigMetrics` and
`tcu::ipc::IpcMetricsSnapshot` — one convention across the repo. NFR-08 requires every drop and
reject path to have a distinguishable counter, and spec section 16's invariant column
(`enqueued <= read`, `0 <= depth <= capacity`) becomes an assertion in `Metrics_InvariantsHold`. A
histogram, not a mean, because NFR-05 asks for p50/p95/p99 and forbids inventing a latency target.

## R-24: Explicitly out of V1

Listed so review does not mistake absence for oversight. Each is a spec non-goal (section 3.2) or a
stated V2 item:

| Deferred | Spec basis |
|---|---|
| CAN FD (`CAN_RAW_FD_FRAMES`, `canfd_frame`) | Section 10.5 — "not required for V1" |
| `SO_RXQ_OVFL` kernel-overflow notification | Open question 7 — "in V2" |
| Generic DBC parsing | Section 3.2 non-goal; ADR-02; project 06 |
| ISO-TP, UDS, J1939, CANopen, CAN XL | Section 3.2 non-goal |
| `DropOldest`, blocking producer, per-ID coalescing, priority queue | Section 17.4 — V1 chooses `DropNewest` |
| IPC export of decoded signals to another process | Section 2 — "optional later extension" |
| Multiple interfaces (`vcan0` + `vcan1`) | Section 25 — project 07 |
| Config hot reload | Not required by any FR; see R-06 |
| Persisted signal snapshot across restart | Section 13.9 — "unless a later project implements" |
| Authentication of inbound frames | NFR-12, ADR-07 — later projects |

## R-25: Test scope, sanitizers and benchmark conditions

**Decision**:

- `tests/unit/can/` — all 22 UT ids from section 18.1, all on `FakeCanTransport` and
  `FakeClockAdapter`; no `sleep_for` anywhere (reuse `tests/support/test_gate.hpp` for worker
  ordering).
- `tests/integration/can/` — the 8 scenarios of section 18.2, each gated by the R-01 probe.
- `tests/stress/can/` — B-30: a bounded run of randomized frames through the fake transport with
  repeated `start()`/`stop()`, plus a decoder fuzz loop over random IDs, flags, lengths and
  payloads.
- `benchmarks/can_bench/` — section 18.4: records host CPU, kernel, compiler flags, queue capacity,
  frame rate and duration, then prints p50/p95/p99 decode latency, peak RSS and total drops. No
  target is asserted.
- The three existing CI jobs (debug, asan+ubsan, tsan) pick all of this up automatically; the
  `vcan` provisioning step from R-01 is added to each.

**Rationale**: Constitution Principle III scopes coverage by the spec, not a percentage, and the
spec enumerates its own tests. NFR-03 asks for TSAN on the fake transport, which works without
`vcan` — so the concurrency evidence does not depend on kernel modules. NFR-05 and section 18.4
forbid an invented latency target, hence a report rather than an assertion.
