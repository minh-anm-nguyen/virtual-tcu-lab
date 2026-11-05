---

description: "Task list for project 05: Virtual CAN Bus Laboratory"
---

# Tasks: Virtual CAN Bus Laboratory

**Input**: Design documents from `/specs/005-virtual-can-bus-laboratory/`

**Prerequisites**: [plan.md](./plan.md), [spec.md](./spec.md), [research.md](./research.md),
[data-model.md](./data-model.md), [contracts/](./contracts/)

**Tests**: **Included and mandatory.** Constitution Principle III is non-negotiable, and spec
section 18 enumerates its own tests (UT-01..UT-22, eight `vcan0` integration scenarios, stress and
fuzz). Every test task below is written *before* the implementation it covers and must fail first.

**Organization**: grouped by user story, derived from spec section 4.1's six core demo stories.
Each story is independently testable; the MVP story is marked.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: can run in parallel (different files, no dependency on an incomplete task)
- **[Story]**: US1..US6, mapping to the demo stories of spec section 4.1
- Exact file paths are given in every task

## Path Conventions

Monorepo layout, per [plan.md](./plan.md) Structure Decision — **not** the standalone tree of spec
section 20:

- Library: `modules/can/include/tcu/can/`, `modules/can/src/`
- Apps: `apps/<name>/main.cpp`
- Tests: `tests/unit/can/`, `tests/integration/can/`, `tests/stress/can/`, `tests/support/`
- Benchmarks: `benchmarks/can_bench/`
- Data: `config/`, `scripts/`

## Story map

| Story | Spec section 4.1 demo story | Priority | Backlog tickets |
|---|---|---|---|
| US1 | 1 + 2: vehicle startup, speed changes — receive, validate, decode, cache with explicit quality | P1 (MVP) | CAN-006, CAN-007, CAN-009, CAN-010 |
| US2 | 3: signal loss — stale detection and recovery | P1 | CAN-011 |
| US3 | 4: CAN interface issue — degraded and controlled recovery | P2 | CAN-012 |
| US4 | 5: command/ack demo — synthetic BCM lock | P2 | CAN-005, CAN-014 |
| US5 | deterministic ECU simulator and runtime fault control | P3 | CAN-004, CAN-016 |
| US6 | 6: noisy traffic — bounded overload and full observability | P3 | CAN-008, CAN-013, CAN-015 |

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: directories, build wiring and data files, so every later task has somewhere to land.

- [X] T001 Create the library directory skeleton `modules/can/include/tcu/can/` and `modules/can/src/`, plus an empty `modules/can/CMakeLists.txt`
- [X] T002 [P] Fill `modules/can/CMakeLists.txt`: `add_library(tcu_can STATIC ...)`, `target_include_directories(tcu_can PUBLIC include)` and `PRIVATE src`, `target_compile_features(tcu_can PUBLIC cxx_std_17)`, `-Wall -Wextra` under `GNU|Clang`, and `target_link_libraries(tcu_can PUBLIC tcu_scheduler tcu_logging tcu_event_bus Threads::Threads PRIVATE nlohmann_json::nlohmann_json)` — nlohmann stays PRIVATE per research R-06, mirroring `modules/config/CMakeLists.txt`
- [X] T003 **Done in two steps.** `add_subdirectory(modules/can)` is wired inside the Linux guard; the four `apps/` subdirectories are added as each app lands in its own phase, because CMake fails on an `add_subdirectory` whose directory has no `CMakeLists.txt`. Add project 05 to the root `CMakeLists.txt` **inside** the existing `if(CMAKE_SYSTEM_NAME STREQUAL "Linux")` block next to project 02, with a comment citing research R-00: `add_subdirectory(modules/can)` plus the four app subdirectories
- [X] T004 **Done in two steps.** `tests/unit/can` is wired and green; `tests/integration/can`, `tests/stress/can` and `benchmarks/can_bench` are added with their phases, for the same CMake reason as T003. Add the project 05 test and benchmark subdirectories to the root `CMakeLists.txt`: `tests/unit/can`, `tests/integration/can`, `tests/stress/can` under `TCU_BUILD_TESTS` and inside the Linux guard, and `benchmarks/can_bench` under `TCU_BUILD_BENCHMARKS`
- [X] T005 [P] Create `config/can_lab.json` exactly as the sample in [contracts/config_schema.md](./contracts/config_schema.md) section 2 — `schema_version: 1`, `mode: "lab_only"`, `interface: "vcan0"`, `rx_queue_capacity: 1024`, `health_check_ms: 50`, `drain_deadline_ms: 200`, reconnect `250`/`5000`/`10`, `allowed_rx_ids: [256,257,258,259,272,513]`, all seven signals, simulator `100`/`1000`/`42`
- [X] T006 [P] Create the three scenario files `config/scenarios/normal_drive.json`, `config/scenarios/speed_signal_loss.json` and `config/scenarios/invalid_battery.json` per [contracts/config_schema.md](./contracts/config_schema.md) section 3, each with `schema_version: 1`, a `name`, `fixed_seed: 42` and a `steps` array whose `at_ms` is monotonically non-decreasing
- [X] T007 [P] Create `scripts/setup_vcan.sh` and `scripts/teardown_vcan.sh` — both idempotent (check before `ip link add`/`del`), both refusing to touch an interface not matching `vcan[0-9]*`, per spec section 21.2 and the Definition of Done
- [X] T008 Add a `sudo modprobe vcan` plus `ip link add dev vcan0 type vcan` and `ip link set dev vcan0 up` provisioning step to all three jobs in `.github/workflows/ci.yml`, so the integration tests run for real on `ubuntu-latest` (research R-01)

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: the vocabulary, the catalog, the transports, the queue and the config loader. Every
user story needs all of it.

**CRITICAL**: no user story work can begin until this phase is complete.

### Vocabulary and catalog

- [X] T009 [P] Create `modules/can/include/tcu/can/can_types.hpp` with `SteadyTime = tcu::scheduler::Clock::time_point` (research R-03) and the six enums from [contracts/can_api.md](./contracts/can_api.md) section 1: `FrameFormat`, `SignalQuality`, `ReadStatus`, `WriteStatus`, `ServiceState`, `RejectReason`. Include only `<array>`, `<chrono>`, `<cstdint>`, `<optional>`, `<string>` and the scheduler header — **no Linux header** (NFR-10, research R-02)
- [X] T010 [P] Add `CanMessage` and `SignalSample` to `modules/can/include/tcu/can/can_types.hpp` per [data-model.md](./data-model.md) sections 2 and 7. Constraints to encode: `data` is `std::array<std::uint8_t, 8>`; `length` is `0..8` and is "payload byte length (not the wire-level DLC encoding for CAN FD)"; `id` is "stripped of Linux flag bits"; `received_at` is "Set on receive, not sender wall-clock"; `SignalSample::value` is `std::optional<double>` because "`value` is optional; `last_good_value` does not imply freshness"
- [X] T011 [P] Create `modules/can/include/tcu/can/frame_catalog.hpp` with the `constexpr` `FrameDefinition` array of 7 rows and `SignalDefinition` array of 7 rows from [data-model.md](./data-model.md) section 3. Encode every range verbatim: `ignition` `{0,1}`; `speed_kmh` `0.00 .. 250.00`; `battery_voltage_v` `6.00 .. 18.00`; `battery_soc_pct` `0 .. 100`; `door_state` `0 .. 31`; `heartbeat` `0 .. 255`; `ecu_status` `{0,1}`. Default timeouts are 3000 ms for all except `speed_kmh` at 500 ms. `expected_length` is `8` for every row. `reserved_mask` values are the table in [contracts/frame_catalog.md](./contracts/frame_catalog.md) section 1
- [X] T012 [P] Write `tests/unit/can/frame_catalog_test.cpp` before T011 lands: `Catalog_EverySignalIdIsAFrameRow`, `Catalog_EveryFrameExpectsEightBytes`, `Catalog_RangesAreNonEmptyAndOrdered`, `Catalog_ReservedMaskMatchesContract`

### Metrics

- [X] T013 [P] Create `modules/can/include/tcu/can/can_metrics.hpp` with `CanMetricsSnapshot` exactly as listed in [contracts/metrics.md](./contracts/metrics.md) section 1 — 24 `std::uint64_t` counters, `can_queue_depth` and `can_queue_capacity` as `std::size_t`, and `can_decode_latency_buckets` as `std::array<std::uint64_t, 16>`
- [X] T014 [P] Create `modules/can/src/can_metrics_store.hpp` holding the same fields as `std::atomic<std::uint64_t>` with `memory_order_relaxed` access and a `snapshot()` method, following the shape of `modules/scheduler/include/tcu/scheduler/scheduler_metrics.hpp` (research R-23)

### Configuration

- [X] T015 [P] Create `modules/can/src/json_document.hpp` and `.cpp` wrapping nlohmann/json behind an opaque handle, so nlohmann never appears in a public header — the technique `modules/config/src/json_document.hpp` uses (research R-06)
- [X] T016 [P] Create `modules/can/src/can_config_schema.hpp` with the `constexpr` key table of all 16 rows in [contracts/config_schema.md](./contracts/config_schema.md) section 1, each row carrying its JSON pointer, type and inclusive bounds: `rx_queue_capacity` `1..65536`, `health_check_ms` `1..10000`, `drain_deadline_ms` `0..5000`, `reconnect/initial_ms` and `max_ms` `1..300000`, `jitter_percent` `0..50`, `signals/<name>/timeout_ms` `1..600000`, `simulator/speed_period_ms` `10..10000`, `heartbeat_period_ms` `100..60000`, `fixed_seed` `0..2147483647`
- [X] T017 [P] Create `modules/can/include/tcu/can/can_config.hpp` with `CanLabConfig`, `LabMode`, `LoadError` (10 values) and `LoadResult` per [data-model.md](./data-model.md) section 4 and [contracts/config_schema.md](./contracts/config_schema.md) section 4
- [X] T018 Write `tests/unit/can/config_loader_test.cpp` before the loader exists: `ConfigLoader_SampleFileMatchesTable`, `ConfigLoader_RejectsUnknownKey`, `ConfigLoader_RejectsMissingKey`, `ConfigLoader_RejectsOutOfRange`, `ConfigLoader_RejectsReconnectInitialAboveMax`, `ConfigLoader_RejectsNonLabInterface`, `ConfigLoader_RejectsUnsupportedVersion`, `ConfigLoader_RejectsUnknownSignalName`, `ConfigLoader_RejectsSignalIdMismatchingCatalog`, `ConfigLoader_PublicReasonNeverEchoesValue`, `ConfigLoader_RejectsFileOver256KiB`
- [X] T019 Implement `modules/can/src/config_loader.cpp`: load, parse, validate against T016's table, apply the cross-field rule `initial_ms <= max_ms`, enforce the `vcan[0-9]*` pattern while `mode == lab_only` (research R-16), reject an unknown signal name and any `can_id` that disagrees with the catalog, cap the file at 256 KiB, and build `publicReason` only from the closed template set `{out_of_range, wrong_type, unknown_key, missing_key, cross_field, not_in_catalog, interface_not_lab, unsupported_version}` — never the offending value

### Transports

- [X] T020 [P] Create `modules/can/include/tcu/can/can_transport.hpp` with `ReadResult` and the `ICanTransport` interface per [contracts/can_api.md](./contracts/can_api.md) section 2
- [X] T021 [P] Create `modules/can/include/tcu/can/fake_can_transport.hpp` — **public**, per research R-22 — declaring `pushFrame`, `pushRaw`, `pushReadStatus`, `sent()` and `setSendStatus()` as in [contracts/can_api.md](./contracts/can_api.md) section 2.5
- [X] T022 Implement `modules/can/src/fake_can_transport.cpp`. `pushRaw` must be able to produce frames the kernel would refuse: EFF, RTR and ERR flags set, and `length != 8` — that is the only way UT-06..UT-08 and B-10/B-13/B-14/B-15 are reachable (spec section 18.1 test note)
- [X] T023 [P] Write `tests/unit/can/fake_can_transport_test.cpp`: `FakeTransport_DeliversPushedFrames`, `FakeTransport_InjectsEio` (`LinkError` with a chosen `os_error`), `FakeTransport_RecordsSentFrames`, `FakeTransport_TimeoutWhenEmpty`
- [X] T024 [P] Create `modules/can/src/socketcan_fd.hpp` — a move-only RAII wrapper closing the descriptor once, never twice (NFR-04, NFR-11). One of exactly two files permitted to include `<linux/can.h>`
- [X] T025 [P] Create `modules/can/include/tcu/can/socketcan_transport.hpp` declaring `SocketCanTransport`, its `Options` and the `open()` factory returning `std::variant<std::unique_ptr<SocketCanTransport>, OpenError>` — a factory, not a throwing constructor (spec section 9.2)
- [X] T026 Implement `modules/can/src/socketcan_transport.cpp` following spec section 10.1 step by step: validate the name length against `IFNAMSIZ-1`, `socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC, CAN_RAW)`, `if_nametoindex()`, install `CAN_RAW_FILTER` with mask `CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_ERR_FLAG` and all flag bits clear (research R-11), create the `eventfd`, `bind()`. Use **separate RX and TX sockets** and never set `CAN_RAW_RECV_OWN_MSGS` (research R-10). Leak no FD on a partial failure
- [X] T027 Implement `SocketCanTransport::receive` and `send` in the same file: `poll()` over the CAN fd and the eventfd; read exactly `CAN_MTU` into `struct can_frame`; map `errno` to `LinkError` versus `FatalError`; capture `os_error` immediately; serialize TX writes under a mutex; retry `EINTR`; treat a short write as failure. No exception escapes
- [X] T028 [P] Write `tests/support/vcan_probe.hpp` — a once-per-process probe (`if_nametoindex("vcan0")` then a bind attempt) returning a reason string, so every integration test can `GTEST_SKIP()` with a printed explanation rather than fail on a kernel without `vcan` (research R-01)

### Validation and the queue

- [X] T029 [P] Write `tests/unit/can/frame_validator_test.cpp` before the validator: `Validator_AcceptsGoldenVectors`, `Validator_RejectsFlags` (UT-06), `Validator_RejectsRtr` and `Validator_RejectsErrorFrame` (UT-07), `Validator_RejectsLength` with length 7 (UT-08), `Validator_RejectsExtendedNoAlias` — an EFF frame whose low 11 bits equal `0x101` must be rejected, not aliased (B-14) — `Validator_UnknownIdIsNotInvalid` (UT-05: it counts `can_unknown_id_total`, not `can_rx_invalid_total`), `Validator_FlagsReservedNonzeroWithoutRejecting` (UT-20, research R-15)
- [X] T030 [P] Create `modules/can/include/tcu/can/frame_validator.hpp` and implement `modules/can/src/frame_validator.cpp`: `ValidationResult validate(const CanMessage&) const noexcept`, checking in order flags, known ID, length, reserved bits, returning on the first failure. Pure, `noexcept`, no allocation, no clock read
- [X] T031 [P] Write `tests/unit/can/bounded_frame_queue_test.cpp`: `Queue_PreservesFifo`, `Queue_DropNewestWhenFull` (UT-14, B-16 — asserting `depth() <= capacity` and that the **arriving** frame is the one discarded), `Queue_PopWakesOnClose`, `Queue_MultipleProducersNoRace` (UT-15, using `tests/support/test_gate.hpp` for ordering, never `sleep_for`)
- [X] T032 Implement `modules/can/src/bounded_frame_queue.hpp`: a fixed-capacity ring of `CanMessage` allocated once at `start()` and never grown, mutex plus condition variable, `DropNewest` on full, `depth()`, and a `close()` that wakes the consumer (research R-17, ADR-04, NFR-02)
- [X] T033 Create `tests/unit/can/CMakeLists.txt` registering the unit test target with `gtest_discover_tests`, linking `tcu_can`, `GTest::gtest_main` and `GTest::gmock`, and adding `tests/support` to the include path — follow the shape of `tests/unit/scheduler/CMakeLists.txt`
- [X] T034 [P] Add a CMake-registered hygiene test asserting that no file under `modules/can/include/` contains a `linux/` include, and that every public header compiles standalone (NFR-10, research R-02)

**Checkpoint**: the library builds, the config loads, both transports work, frames are validated and
queued. User story work can begin.

---

## Phase 3: User Story 1 — Receive, decode and expose signals with explicit quality (Priority: P1) — MVP

**Goal**: a frame on `vcan0` becomes a typed physical value with an honest quality, and a signal that
was never received is distinguishable from one that is genuinely zero.

**Independent Test**: start the service, read every signal and see `NeverReceived` with no value;
`cansend vcan0 101#7017000000000000`; read `speed_kmh` and see `60.00` with `FRESH` and
`source_can_id 0x101`. Then send `101#0000000000000000` and see a valid `0.00`, not a missing value.

### Tests for User Story 1

> Write these first and confirm they fail.

- [X] T035 [P] [US1] Write `tests/unit/can/demo_signal_decoder_test.cpp` with `Decoder_GoldenVectors` covering all 17 rows of [contracts/frame_catalog.md](./contracts/frame_catalog.md) section 3: `101#7017...` is `60.00` (UT-01), `102#B00455...` is `12.00 V` and `85 %` (UT-02), ignition 0 and 1 (UT-03), `103#10...` is `door_state 16` and `103#0F...` is `15` (UT-04), `110#FF...` then `110#00...` is a normal wrap (UT-19)
- [X] T036 [P] [US1] Add `Decoder_RejectsOutOfRange` (UT-09: `101#A961...` is `250.01`, rejected; `101#A861...` is `250.00`, accepted at the inclusive bound), `Decoder_AtomicRejectOnMultiSignalFrame` (UT-10: `102#B00478...` has SOC 120, so **neither** battery signal is committed — research R-14), `Decoder_RejectsEcuStatusTwo`, and `Decoder_InterleavedIdsNoCrossTalk` (B-17) to the same file
- [X] T037 [P] [US1] Write `tests/unit/can/signal_cache_test.cpp` with `Cache_NeverReceivedHasNoValue` (B-02, UT-22: `value` absent, quality `NeverReceived`, and specifically not `0.0` or `false`), `Cache_ZeroIsAValue` (B-04), `Cache_GenerationIncrements` (B-05), `Cache_UnknownNameReturnsNullopt` (distinct from `NeverReceived`)
- [X] T038 [P] [US1] Add `Cache_InvalidDoesNotRefresh` to the same file (B-10, B-11, spec section 13.4): after `recordError`, `last_good_value` and `last_good_at` are unchanged, `generation` is unchanged, and the computed quality is `Invalid`
- [X] T039 [P] [US1] Write `tests/unit/can/can_service_test.cpp` with `Service_StartRejectsInvalidConfig` (state stays `Stopped`, returns `false`, no thread created), `Service_StartTwiceRejected` (B-01, B-27), `Service_StartLeavesSignalsNeverReceived` (spec section 13.1: "Startup must not report ignition OFF or speed zero before receiving a valid frame")
- [X] T040 [P] [US1] Write `tests/unit/can/event_adapter_test.cpp` with `EventAdapter_PublishesSignalUpdated` (FR-07: payload carries `signal_name`, `value`, `source_can_id`, `generation`, `monotonic_age_ms`) and `EventAdapter_PublishFailureIsCountedNotThrown` (Principle V)
- [X] T041 [P] [US1] Write `tests/integration/can/decode_roundtrip_test.cpp`, gated by `tests/support/vcan_probe.hpp`: `Integration_BindsConfiguredInterface` (FR-01), `Integration_CansendRoundTrip` (FR-02, B-03), `Integration_FilterExcludesUnrelatedId` (FR-04), `Integration_CandumpSeesOutgoingFrame` (FR-03)

### Implementation for User Story 1

- [X] T042 [P] [US1] Create `modules/can/include/tcu/can/demo_signal_decoder.hpp` with `DecodeResult` and `IDemoSignalDecoder` per [contracts/can_api.md](./contracts/can_api.md) section 3 — `reason` is a `RejectReason` enum, not the spec snippet's `std::string` (declared in plan.md Complexity Tracking)
- [X] T043 [US1] Implement `modules/can/src/demo_signal_decoder.cpp` from the catalog table: little-endian `uint16` extraction, the `0.01` scale factors, the `0x1F` door mask, range checks on the **decoded physical value** before any commit, and atomic whole-frame reject when any field of `0x102` or `0x110` is out of range
- [X] T044 [P] [US1] Create `modules/can/include/tcu/can/signal_cache.hpp` with `QualityTransition` and `ISignalCache` (six methods) per [contracts/can_api.md](./contracts/can_api.md) section 4
- [X] T045 [US1] Implement `modules/can/src/signal_cache.cpp` with `SignalState` storing `last_good_value`, `last_good_at`, `last_error`, `transport_available`, `generation` and `published_quality` as **separate fields**, and quality **computed** on read in the five-step precedence order of [data-model.md](./data-model.md) section 6.1 (research R-13). `apply()` is the only method that may advance `last_good_at`
- [X] T046 [US1] Implement `modules/can/src/event_bus_adapter.hpp` and `.cpp`, publishing from a `QualityTransition` vector **after** the cache lock is released (spec section 17.1 rules 4 and 7), counting `event_publish_total` and `event_publish_failed_total`
- [X] T047 [US1] Append the three payload alternatives `VehicleSignalUpdated`, `SignalQualityChanged` and `CanLinkStatusChanged` to `EventPayload` in `modules/core/event_bus/include/tcu/eventbus/event.hpp`, **after** `ConfigChanged` and without reordering anything — the file's own comment states that reordering changes every `EventType` value and is breaking (research R-07). Add a comment citing project 05
- [X] T048 [US1] Create `modules/can/include/tcu/can/can_service.hpp` with `CanServiceDeps` and the `CanService` facade per [contracts/can_api.md](./contracts/can_api.md) section 5, non-copyable, with `~CanService()` calling `stop()`
- [X] T049 [US1] Implement the `start()` path in `modules/can/src/can_service.cpp`: validate config before creating anything, open the transport, allocate the queue, start the RX reader and the decode worker, and transition `Stopped` to `Starting` to `Running`. `start()` while `Running` returns `false` (B-27)
- [X] T050 [US1] Implement the RX reader loop in the same file: `receive()`, stamp `received_at` from the injected clock, count `can_rx_read_total`, push to the queue, count `can_rx_enqueued_total` or `can_rx_queue_drop_total`. It must never invoke a user callback and never decode (plan.md threading table)
- [X] T051 [US1] Implement the decode worker loop: pop FIFO, `validate`, `decode`, `apply` or `recordError`, publish the returned transitions after the lock is released, and record the receipt-to-decode duration into `can_decode_latency_buckets`
- [X] T052 [US1] Implement `readSignal()`, `readAll()` and `healthJson()` rendering the shape of [contracts/metrics.md](./contracts/metrics.md) section 5 — `value` is `null` exactly when quality is `NEVER_RECEIVED`, and no raw frame bytes ever appear
- [X] T053 [US1] Add the structured log calls for `can_service.started`, `can_service.stopped`, `signal.updated` and `frame.rejected` per [contracts/metrics.md](./contracts/metrics.md) section 4, tagging `value` and raw bytes `DataClass::VehicleSensitive` and keeping raw bytes at `Debug` only (research R-05)
- [X] T054 [US1] Create `apps/tcu_can_service/main.cpp` and its `CMakeLists.txt`: parse `--config`, `--interface` and `--broad-filter`, build the logger, scheduler, bus and service, install a `SIGTERM`/`SIGINT` handler that calls `stop()` from the main thread (never from a callback), and print the health JSON on request
- [X] T055 [US1] Create `tests/integration/can/CMakeLists.txt` registering the integration target with the `vcan_probe` include path

**Checkpoint**: US1 is complete. A `cansend` frame becomes a decoded, fresh, quality-tagged signal,
and an unreceived signal is honestly unknown.

---

## Phase 4: User Story 2 — Signal loss is detected and recovery happens once (Priority: P1)

**Goal**: when one ECU stops transmitting, only its signal goes stale, exactly one transition is
reported, and a resumed frame restores freshness exactly once.

**Independent Test**: with speed fresh, stop sending `0x101` while `0x110` continues. After 500 ms
`speed_kmh` is `STALE` with its last good value still present and `heartbeat` is still `FRESH`.
Resume `0x101`: one recovery event, `generation` higher.

### Tests for User Story 2

- [X] T056 [P] [US2] Add `Cache_StaleAtBoundary` to `tests/unit/can/signal_cache_test.cpp` — at exactly `last_good_at + timeout` the signal is **still** `FRESH` (B-06, UT-11, research R-12: the comparison is strictly greater)
- [X] T057 [P] [US2] Add `Cache_StaleAfterBoundary` — at `last_good_at + timeout + 1ms` the signal is `STALE` and `last_good_value` is unchanged (B-07, UT-12), driving time with `tcu::scheduler::FakeClockAdapter`
- [X] T058 [P] [US2] Add `Cache_OneStaleTransitionOnly` — repeated `refresh()` calls after the deadline return a transition the first time and an **empty** vector every time after (B-08, UT-12)
- [X] T059 [P] [US2] Add `Cache_RecoversOnce` — a valid update after staleness yields exactly one `Stale` to `Fresh` transition and a higher `generation` (B-09, UT-13)
- [X] T060 [P] [US2] Add `Cache_OnlyExpiredSignalGoesStale` — with `speed_kmh` at 500 ms and `heartbeat` at 3000 ms, advancing 600 ms expires only speed (B-18)
- [X] T061 [P] [US2] Write `tests/integration/can/signal_loss_test.cpp`, probe-gated: `Integration_PausedIdGoesStaleAlone` and `Integration_ResumedIdRecoversOnce` (spec section 18.2 scenarios 4 and 5)

### Implementation for User Story 2

- [X] T062 [US2] Implement `refresh(now)` in `modules/can/src/signal_cache.cpp`: recompute every signal's quality and return only those whose computed quality differs from `published_quality`, updating `published_quality` as it goes
- [X] T063 [US2] Schedule the health check in `modules/can/src/can_service.cpp` as a `tcu::scheduler::TimerScheduler` periodic timer with `PeriodicMode::FixedDelay` and period `health_check_ms` — FixedDelay because its contract is "next deadline = completion time + interval. Drifts, never overlaps", which is what spec section 25 requires of this exact job (research R-04). **Do not create a third thread**
- [X] T064 [US2] Publish the transitions returned by the health tick and count `signal_stale_transitions_total`, `signal_recovered_total` and `signal_invalid_transitions_total`, then emit the `signal.stale` and `signal.recovered` logs with `age_ms` and `timeout_ms`

**Checkpoint**: US1 and US2 both work independently.

---

## Phase 5: User Story 3 — Interface failure degrades and recovers under control (Priority: P2)

**Goal**: a missing, downed or deleted `vcan0` moves the service to `DEGRADED` with bounded retry and
no busy loop, and recovery never fabricates freshness.

**Independent Test**: `ip link set vcan0 down`, observe `DEGRADED`, all signals `LINK_UNAVAILABLE`
and near-zero CPU. `ip link set vcan0 up`, observe `RUNNING`, `can_link_reconnect_total` up by one,
and signals still non-fresh until a new frame arrives.

### Tests for User Story 3

- [X] T065 [P] [US3] Add `Service_EntersDegradedOnLinkError` to `tests/unit/can/can_service_test.cpp` using `FakeCanTransport::pushReadStatus(ReadStatus::LinkError, EIO)` (UT-16, B-19)
- [X] T066 [P] [US3] Add `Cache_LinkRestoredStaysNonFresh` to `tests/unit/can/signal_cache_test.cpp` — after `setTransportAvailable(false)` then `(true)`, no signal is `Fresh` until a new valid frame advances its `last_good_at` (B-20)
- [X] T067 [P] [US3] Add `Service_BackoffIsBoundedAndWakesOnStop` — successive `LinkError`s produce intervals that double from `initial_ms` and cap at `max_ms`, and a `stop()` during a backoff wait returns promptly rather than after the full interval
- [X] T068 [P] [US3] Add `Service_StartOnMissingInterfaceEntersDegraded` — `start()` returns `true` and the state is `Degraded`, not `Stopped`, when the interface does not exist yet (spec section 12.1)
- [X] T069 [P] [US3] Write `tests/integration/can/link_recovery_test.cpp`, probe-gated: `Integration_InterfaceDownUp`, asserting the **final observable state** rather than a specific errno sequence, because "Actual error order and recovery mechanism can vary across Linux versions" (spec sections 21.6 and 18.2 scenario 7)

### Implementation for User Story 3

- [X] T070 [US3] Implement the degrade-and-retry path in the RX reader: on `LinkError` or `FatalError`, close the socket, set `ServiceState::Degraded`, call `setTransportAvailable(false)`, count `can_link_error_total`, then wait on **the eventfd alone** with a `poll` timeout equal to the current backoff, and retry open and bind. Backoff doubles from `reconnect_initial` to `reconnect_max` with the configured jitter (research R-18). The reopen happens on the RX thread, never on a scheduler worker, because spec section 17.1 rule 1 makes the adapter the sole FD owner
- [X] T071 [US3] On a successful reopen and bind, count `can_link_reconnect_total`, set `ServiceState::Running`, call `setTransportAvailable(true)` and publish the resulting transitions plus a `CanLinkStatusChanged` event. Increment only **after** bind succeeds, so a retry attempt is never counted as a success
- [X] T072 [US3] Emit `can_service.link_unavailable` and `can_service.link_restored` structured logs with `interface`, `state`, `os_error` and `backoff_ms`

**Checkpoint**: US1, US2 and US3 all work independently.

---

## Phase 6: User Story 4 — Synthetic BCM command and acknowledgment (Priority: P2)

**Goal**: the TCU sends `0x200`, the BCM replies `0x201` with a matching sequence, the request
completes, and an unmatched or duplicate ACK changes nothing.

**Independent Test**: `can_control_cli lock --sequence 0x2A`; `candump` shows `200#012A...` then
`201#2A00...`; the service logs `command.acked` with `result=OK` and `0x103` follows with the lock
bit set. An ACK for a sequence nobody awaits only increments `can_ack_unmatched_total`.

### Tests for User Story 4

- [X] T073 [P] [US4] Write `tests/unit/can/command_correlator_test.cpp`: `Correlator_CompletesOnMatchingAck`, `Correlator_IgnoresUnmatchedAck` (UT-18, B-23 — the pending request **stays pending**), `Correlator_IgnoresDuplicateAck` (spec section 15 duplicate-ACK row), `Correlator_RejectsSecondInFlightCommand` (research R-20), `Correlator_TimesOutPendingCommand`
- [X] T074 [P] [US4] Add `Service_SendWhenStoppedIsLinkUnavailable` to `tests/unit/can/can_service_test.cpp` (B-21) and `Service_SendRejectsUnknownCommandByte`
- [X] T075 [P] [US4] Write `tests/unit/can/bcm_policy_test.cpp`: `Bcm_AcksLockAndUnlock` with result `0x00`, `Bcm_RejectsUnknownCommand` with result `0x02` INVALID and no state change (B-24), `Bcm_AcceptsCommandRegardlessOfIgnition` — the explicit lab policy of research R-20, answering spec open question 3
- [X] T076 [P] [US4] Write `tests/integration/can/bcm_command_test.cpp`, probe-gated: `Integration_BcmLockAck` (B-22, FR-14, spec section 18.2 scenario 6)

### Implementation for User Story 4

- [X] T077 [P] [US4] Implement `modules/can/src/command_correlator.hpp` and `.cpp` with the `PendingCommand` state machine of [data-model.md](./data-model.md) section 9 — `Idle`, `AwaitingAck`, `Completed`, at most one `AwaitingAck` at a time
- [X] T078 [US4] Implement `CanService::sendDemoCommand(command, sequence)` per [contracts/can_api.md](./contracts/can_api.md) section 5.4: `LinkUnavailable` when not `Running`, `InvalidFrame` when a command is already pending or the command byte is not `0x01`/`0x02`, otherwise build the `0x200` frame and send it, counting `can_tx_attempt_total` and `can_tx_success_total`
- [X] T079 [US4] Route inbound `0x201` from the decode worker to the correlator, counting `can_ack_matched_total`, `can_ack_unmatched_total` and `can_ack_timeout_total`, and emit the `command.sent` and `command.acked` logs
- [X] T080 [US4] Create `apps/bcm_simulator/main.cpp` and its `CMakeLists.txt`: filter `0x200` only, validate the command byte, apply the lab lock state, reply `0x201` with the echoed sequence and a result byte, and publish `0x103` with the new door and lock bits on change and every 1 s

**Checkpoint**: four stories independently functional.

---

## Phase 7: User Story 5 — Deterministic ECU simulator and runtime fault control (Priority: P3)

**Goal**: reproducible periodic vehicle traffic, and a CLI that can pause, resume, set and corrupt
signals at runtime.

**Independent Test**: run `vehicle_simulator` twice with `fixed_seed: 42` and compare the value
sequence per CAN ID — identical. Then `can_control_cli pause-id 0x101` and watch the frames stop on
`candump` without restarting anything.

### Tests for User Story 5

- [X] T081 [P] [US5] Write `tests/unit/can/simulator_scenario_test.cpp`: `Simulator_SameSeedSameSequence` (B-29, FR-12 — the assertion is on the **value sequence per CAN ID**, not on wall-clock arrival times, per research R-21), `Simulator_RejectsNonMonotonicScenarioSteps`, `Simulator_PauseIdStopsOnlyThatFrame`
- [~] T082 [P] [US5] Write `tests/integration/can/simulator_control_test.cpp`, probe-gated: `Integration_PauseResumeOverIpc` and `Integration_InjectInvalidProducesRejection`

### Implementation for User Story 5

- [X] T083 [P] [US5] Implement scenario file loading in `apps/vehicle_simulator/` reusing `modules/can/src/json_document.hpp`, validating that `at_ms` is monotonically non-decreasing across `steps` and that `action` is one of `set_signal`, `pause_id`, `resume_id`, `inject_invalid`. `value` is deliberately **not** range-checked, so a scenario can emit an out-of-range value for B-11
- [X] T084 [US5] Implement the frame encoders in `apps/vehicle_simulator/` for `0x100`, `0x101`, `0x102` and `0x110`, setting every reserved bit to zero as the V1 sender rule requires ([contracts/frame_catalog.md](./contracts/frame_catalog.md) section 1)
- [X] T085 [US5] Schedule periodic TX with `tcu::scheduler::TimerScheduler` using `PeriodicMode::FixedRateSkipMissed` — it "stays aligned to the original phase; missed slots are counted and skipped, never replayed as a burst", and a burst would make the TCU's queue-drop metrics meaningless (research R-04)
- [X] T086 [US5] Seed `std::mt19937` from `fixed_seed` and use it for every non-timing value (NFR-09)
- [X] T087 [US5] Host a `tcu::ipc::IpcServer` in `apps/vehicle_simulator/main.cpp` on `/tmp/tcu_can_sim.sock` with the methods `pause_id`, `resume_id`, `set_signal`, `inject_invalid` and `scenario_status` — reusing project 02 rather than inventing a control protocol (research R-08)
- [X] T088 [US5] Support the launch flags `--pause-id`, `--scenario`, `--config` and `--interface` as the simulator's **initial** state, so the section 15 fault-matrix injection column stays literally true, and route them through the same handlers the IPC methods call
- [X] T089 [US5] Create `apps/can_control_cli/main.cpp` and its `CMakeLists.txt` as a `tcu::ipc::IpcClient` exposing `pause-id`, `resume-id`, `set-signal`, `inject-invalid`, `lock`, `unlock`, `health` and `metrics`
- [X] T090 [US5] Add the `health` and `metrics` IPC methods to `apps/tcu_can_service/main.cpp`, serving `CanService::healthJson()` and the metrics snapshot, which is what the `metrics_cli` actor of spec section 4 needs
- [X] T091 [P] [US5] Create `scripts/demo_normal.sh` and `scripts/demo_signal_loss.sh` driving the section 21.5 sequence end to end

**Checkpoint**: the full lab runs with runtime fault injection.

---

## Phase 8: User Story 6 — Noisy traffic stays bounded and fully observable (Priority: P3)

**Goal**: unrelated IDs, malformed frames and heavy load leave the service bounded, alive and honest
about what it dropped.

**Independent Test**: `cangen vcan0 -I 101 -L 8 -g 0` for a few seconds, then read metrics:
`can_queue_depth <= can_queue_capacity` throughout, `can_rx_queue_drop_total` rising,
`can_rx_enqueued_total + can_rx_queue_drop_total == can_rx_read_total`, resident memory flat, and
drop warnings rate-limited rather than one line per frame.

### Tests for User Story 6

- [X] T092 [P] [US6] Write `tests/unit/can/metrics_test.cpp` asserting all nine invariants of [contracts/metrics.md](./contracts/metrics.md) section 3, named `Metrics_InvariantsHold`
- [X] T093 [P] [US6] Add `Service_BroadFilterCountsUnknownId` to `tests/unit/can/can_service_test.cpp` (B-12: with no filter installed, an unknown ID increments `can_unknown_id_total` and changes no signal)
- [X] T094 [P] [US6] Add `Service_HealthJsonShape` — `value` is `null` exactly when quality is `NEVER_RECEIVED`, a `STALE` signal still carries its last good value, and no raw frame bytes appear anywhere in the output
- [X] T095 [P] [US6] Add `Service_DropLogIsRateLimited` — a sustained full queue produces at most one `queue.dropped` warning per second while the counter rises per frame (spec section 13.7)
- [X] T096 [P] [US6] Add `Service_CallbackThrowDoesNotKillService` (UT-21) and `Service_ReservedNonzeroIsCountedNotRejected` (UT-20)
- [X] T097 [P] [US6] Write `tests/integration/can/overload_test.cpp`, probe-gated: `Integration_BurstTrafficStaysBounded` using `cangen`

### Implementation for User Story 6

- [X] T098 [US6] Wire every remaining counter from [contracts/metrics.md](./contracts/metrics.md) section 1 into its increment site, checking against the invariant list that no path is unaccounted for (NFR-08)
- [X] T099 [US6] Implement `--broad-filter` on `apps/tcu_can_service/main.cpp`, installing no `CAN_RAW_FILTER` so unknown IDs reach the application for B-12. Keep the filtered path the default (FR-04, spec section 13.3)
- [X] T100 [US6] Add rate limiting to the drop and reject log paths — one line per second per cause, with the counter still incrementing per event, so an overload cannot make the logger the bottleneck
- [X] T101 [US6] Isolate every user callback and event publish behind a `try`/`catch` that counts the failure and keeps the service alive (NFR-06, UT-21, Principle V)
- [X] T102 [US6] Implement `CanMetricsSnapshot CanService::metrics() const noexcept` — never blocking, never allocating

**Checkpoint**: all six stories independently functional.

---

## Phase 9: Polish and Cross-Cutting Concerns

- [X] T103 Implement the `stop()` sequence in `modules/can/src/can_service.cpp` exactly as [contracts/can_api.md](./contracts/can_api.md) section 5.2 lists it: `Stopping`, signal the eventfd, join the RX worker, drain until `drain_deadline_ms` counting `can_shutdown_discarded_total`, join the decode worker, cancel the health timer, stop the scheduler with `StopMode::Drain`, release the bus subscription, then queue, sockets and eventfd in that order. Idempotent, with a debug assert against being called from inside a callback
- [X] T104 [P] Add `Service_StopWakesReader` (B-25), `Service_StopIsIdempotent` (B-26, no double close) and `Service_RepeatedStartStop` (UT-17, no orphan thread or leaked FD across many cycles) to `tests/unit/can/can_service_test.cpp`
- [X] T105 [P] Write `tests/stress/can/can_stress_test.cpp` with `Stress_RandomFramesWithRestarts` (B-30: randomized frames through `FakeCanTransport` with repeated `start()`/`stop()`, asserting the metric invariants still hold afterwards) and `Stress_QueueStaysBounded`
- [X] T106 [P] Write `tests/stress/can/decoder_fuzz_test.cpp` — random IDs, flags, lengths and payloads through `FakeCanTransport::pushRaw`, asserting no crash, no unchecked allocation, and that no signal is ever committed from a rejected frame (spec section 18.3)
- [X] T107 [P] Create `tests/stress/can/CMakeLists.txt` following `tests/stress/scheduler/CMakeLists.txt`
- [X] T108 [P] Write `tests/integration/can/lifecycle_test.cpp`, probe-gated: `Integration_StopWakesPoll` (FR-11) and `Integration_RestartIsNeverReceived` (B-28: after a restart every signal is `NeverReceived`, because frames are not persisted and not replayed — spec section 13.9)
- [X] T109 [P] Create `benchmarks/can_bench/` with its `CMakeLists.txt` and `main.cpp`: record host CPU model, kernel release, compiler and flags, queue capacity, frame rate and run duration, then report p50/p95/p99 decode latency, peak RSS and total drops. **Assert no target** — NFR-05 and spec section 18.4 forbid turning one host's numbers into a performance claim
- [X] T110 Run the ASan+UBSan and TSan configurations from [quickstart.md](./quickstart.md) section 4 and fix every finding. TSan is the important one here and needs no `vcan`, since the concurrency evidence comes from `FakeCanTransport` (NFR-03)
- [X] T111 [P] Write `modules/can/README.md` covering the public API, the `lab_only` safety boundary, and every limitation in [quickstart.md](./quickstart.md) section 8 — a Definition-of-Done item from spec section 26
- [~] T112 **Not done on this host** - the WSL2 kernel has no vcan module (plan.md reconciliation). Walk all ten scenarios in [quickstart.md](./quickstart.md) section 6 by hand on a host that has `vcan`, and record which ones ran versus which skipped
- [X] T113 Fill the **Post-implementation reconciliation** section of [plan.md](./plan.md): each of the 19 Definition-of-Done items from spec section 26 marked met or unmet with a reason, what the sanitizers found, and an explicit statement of which integration tests ran on `vcan0` versus skipped for a missing kernel module. Spec section 18.2's test note forbids claiming an untested path passed
- [X] T114 Verify the Constitution gate before committing: clean build, all tests passing or honestly skipped, every FR and BDD contract in the plan.md traceability tables covered by a named test, and every deviation present in Complexity Tracking

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: no dependencies
- **Foundational (Phase 2)**: needs Setup — **blocks every user story**
- **US1 (Phase 3)**: needs Foundational. The MVP
- **US2 (Phase 4)**: needs Foundational and `SignalCache` from US1 (T045)
- **US3 (Phase 5)**: needs Foundational and the RX loop from US1 (T050)
- **US4 (Phase 6)**: needs Foundational and the decode worker from US1 (T051)
- **US5 (Phase 7)**: needs Foundational; `apps/vehicle_simulator` needs only the catalog and the transport, so it can start in parallel with US1
- **US6 (Phase 8)**: needs Foundational and the counters wired through US1..US4
- **Polish (Phase 9)**: needs every story that is being shipped

### Within each story

- Every test task precedes the implementation it covers and must fail first (Principle III)
- Types before the catalog, catalog before the validator, validator before the decoder
- Cache before the health timer that calls it
- `stop()` (T103) is deliberately in Polish: it is simpler to write once every thread and
  subscription it must tear down exists, and B-25/B-26/UT-17 are its tests

### Parallel Opportunities

- Phase 1: T002, T005, T006 and T007 in parallel
- Phase 2: the three clusters — vocabulary and catalog (T009-T012), metrics (T013-T014), config
  (T015-T019) — are independent; transports (T020-T028) depend only on T009-T010
- Every `[P]` test task inside a story can be written concurrently
- US5's simulator work can run alongside US1 once Phase 2 is done
- The four executables (T054, T080, T089 and the simulator) touch different files throughout

---

## Parallel Example: User Story 1

```bash
# Write all US1 tests first, in parallel:
Task: "Decoder golden vectors in tests/unit/can/demo_signal_decoder_test.cpp"             # T035
Task: "Decoder range and atomic-reject cases in the same file"                            # T036
Task: "Cache never-received and zero-is-a-value in tests/unit/can/signal_cache_test.cpp"  # T037
Task: "Cache invalid-does-not-refresh in the same file"                                   # T038
Task: "Service start and restart cases in tests/unit/can/can_service_test.cpp"            # T039
Task: "Event adapter publish cases in tests/unit/can/event_adapter_test.cpp"              # T040
Task: "vcan round-trip cases in tests/integration/can/decode_roundtrip_test.cpp"          # T041

# Then the headers, in parallel:
Task: "demo_signal_decoder.hpp"                                                           # T042
Task: "signal_cache.hpp"                                                                  # T044
```

---

## Implementation Strategy

### MVP first (US1 only)

1. Phase 1 Setup
2. Phase 2 Foundational — blocks everything
3. Phase 3 US1
4. **Stop and validate**: run quickstart scenarios 6.1, 6.2 and 6.3. A frame decodes to the right
   physical value, and an unreceived signal is honestly unknown
5. That alone discharges FR-01..FR-07 and the spec's first two demo stories

### Incremental delivery

1. Setup + Foundational → the library builds and configures
2. + US1 → decode and quality (**MVP**; quickstart 6.1-6.3)
3. + US2 → stale and recovery (quickstart 6.4, 6.5, 6.6)
4. + US3 → link failure and recovery (quickstart 6.8)
5. + US4 → command and ACK (quickstart 6.7)
6. + US5 → deterministic simulator and the control CLI
7. + US6 → bounded overload and full observability (quickstart 6.9)
8. + Polish → shutdown, stress, fuzz, sanitizers, benchmark, docs (quickstart 6.10 and section 7)

### Parallel team strategy

1. Everyone on Setup + Foundational
2. Then: developer A on US1 then US2 (the cache is one file, so they serialize); developer B on US5's
   simulator and CLI from the start; developer C on US3 then US4 once the RX loop and decode worker
   from US1 exist

---

## Notes

- `[P]` means a different file with no dependency on an incomplete task
- The quoted ranges, masks and golden vectors in these tasks come from
  [contracts/frame_catalog.md](./contracts/frame_catalog.md),
  [contracts/config_schema.md](./contracts/config_schema.md) and
  [data-model.md](./data-model.md) — do not re-derive them
- No test may use `std::this_thread::sleep_for` for ordering; use `tests/support/test_gate.hpp` and
  `tcu::scheduler::FakeClockAdapter`
- Integration tests skip, never fail, when `vcan0` is unavailable (research R-01). The dev host here
  is WSL2, whose kernel has no `vcan` module
- Commit per task or per logical group, and stop at any checkpoint to validate a story on its own
