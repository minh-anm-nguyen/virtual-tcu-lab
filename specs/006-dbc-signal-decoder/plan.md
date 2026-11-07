# Implementation Plan: DBC Signal Decoder

**Branch**: `master` (feature directory `006-dbc-signal-decoder`) | **Date**: 2026-11-05 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/006-dbc-signal-decoder/spec.md`

## Summary

Build two C++17 libraries, two CLIs and one demo.

`tcu_dbc` is pure and stateless: a lexer and a recursive-descent parser over the documented DBC
subset, a strict validator that rejects the whole file on any error with file/line/column
diagnostics, a compiler that turns the validated model into immutable per-signal bit plans and an
O(1) `(bus, format, id)` lookup table, a SHA-256 of the file bytes as the schema hash, and a decoder
that extracts raw bits (Intel LSB-first, Motorola sawtooth MSB-first), sign-extends widths 1..64
without undefined behaviour, scales to physical values, resolves enum labels and attaches a quality
to every signal. It has no thread, no clock and no file watcher, and it throws for no frame content.

`tcu_signal` owns the lifecycle: project 05's `ICanTransport` as its frame source, a bounded RX
queue, one decoder
worker, a quality cache that keeps the last observation and the last-good value on separate axes
with freshness derived from the last-good monotonic timestamp, a freshness scan, and a reload
coordinator that parses and validates off the hot path and publishes a new immutable snapshot with a
single atomic store. It reuses project 03's `TimerScheduler` for everything periodic and project
04's `tcu_logging` for diagnostics, so it adds exactly one thread to the process.

`apps/dbc_validate` prints diagnostics for a DBC file. `apps/dbc_decode` decodes a hex frame or a
`candump` file. `apps/virtual_dbc_demo` replays a frame file through the service and republishes
batches and quality changes onto project 01's Event Bus.

The spec already fixes the design: scope (section 3), requirements (section 4), architecture
(section 5), the DBC subset and the eleven validation classes (section 6), the reference bit
algorithm and six hand-computed goldens (section 7), the API (section 8), the quality matrix
(section 9), operational behaviour (section 10), three sequences (section 11), ten concurrency rules
and twelve property invariants (section 12), twenty behavioural contracts (section 13), the what-if
matrix (section 14), the test pyramid and three race recipes (section 15), metrics and log lines
(section 16), the config sidecar (section 17), the layout (section 18), a 22-ticket backlog
(section 19), the demo (section 20), seven ADRs (section 21) and the Definition of Done
(section 23). This plan redesigns none of it. It closes the ten open questions of section 24 plus
the mechanisms the spec states as a goal without naming an implementation; those closures are
[research.md](./research.md) R-00..R-23.

**Revision note, 2026-11-06**: the first draft of this plan was written while project 05 was
specified but unimplemented, and deferred ticket DBC-013 on that basis. Project 05 has since landed
(commit `fe31dac`, CAN-001..019). Project 06 now **reuses project 05's transport** —
`tcu::can::ICanTransport`, `CanMessage`, `SocketCanTransport`, `FakeCanTransport` — instead of
defining a frame-source abstraction of its own, so DBC-013 and the `vcan0` demo are **delivered**,
not deferred. It deliberately does **not** reuse project 05's `CanService`, `SignalCache` or
`DemoSignalDecoder`, because their `SignalSample`/`DecodeResult` cannot carry this project's raw
`uint64_t`, signed raw, unit, enum label, eight-value quality or per-batch DBC identity. Both
decisions, and the two-caches cost they imply, are in [research.md](./research.md) R-22 and R-24.

## Technical Context

**Language/Version**: C++17, no compiler extensions in library code.

**Primary Dependencies**: `tcu_dbc` — standard library only (`<string>`, `<vector>`,
`<unordered_map>`, `<optional>`, `<chrono>`, `<cstdint>`, `<fstream>`, `<filesystem>`, `<cmath>`).
`tcu_signal` — `tcu_dbc`, plus `tcu_can` (project 05) for `ICanTransport`, `CanMessage`,
`SocketCanTransport` and `FakeCanTransport` (R-22), plus `tcu_scheduler` (project 03) for the
freshness scan, the reload debounce and the reload job, plus `tcu_logging` (project 04) for
diagnostics, plus `nlohmann/json` v3.11.3 (already pinned at the repository root by project 04)
wrapped away in `src/` for `decoder.json`. `tcu_eventbus` (project 01) is linked by the demo and
one integration test only, never by either library (R-18).

**Storage**: the DBC file and `decoder.json` are read, never written. Nothing in this project writes
a file except through the logger, which project 04 owns.

**Testing**: GoogleTest + CTest, four labels — `dbc-unit`, `signal-unit`, `dbc-integration`,
`dbc-stress`. Fixtures in `tests/fixtures/dbc/` (valid and invalid `.dbc` files, hand-computed
goldens, and the generated 10,000-vector matrix, all committed — R-20). The existing
`tests/support/test_gate.hpp` rendezvous is reused for the three race recipes of spec section 15.3;
no test sleeps. The clock is injected everywhere freshness is involved (R-06). Faults arrive through
the nine-point `FaultHooks` seam in a `src/`-private header, never through a public API and never
behind a preprocessor build variant (R-23). Stress duration is `TCU_DBC_STRESS_SECONDS`, default 5.
Sanitizers: address plus undefined, and a separate thread-sanitizer build (clang with
`TCU_SANITIZER_SETARCH=ON`), never combined. Fuzzing is `TCU_BUILD_FUZZERS=ON`, clang only (R-21).

**Target Platform**: `tcu_dbc` builds and tests on Linux/Ubuntu 22.04+ x86-64 (GCC 11+ or Clang
14+) **and** Windows/MSVC 19.3x, outside the root `CMakeLists.txt` Linux guard — it makes no POSIX
call and carries the whole golden matrix, every validation class and both fuzz targets.
`tcu_signal` sits **inside** that Linux guard, next to projects 02 and 05, because it links
`tcu_can` and that target compiles `socketcan_transport.cpp` (R-00, R-22). Sanitizer and fuzzing
jobs are Linux-only anyway, a toolchain limit, so the spec section 23 criteria for them are
evaluated on Linux/WSL and the quickstart says so.

**Project Type**: two static libraries + three executables + four test suites + one benchmark,
inside the Virtual TCU Lab monorepo.

**Performance Goals**: measured, never asserted (R-07). Spec section 4.2 calls its numbers "goals
for the lab". `benchmarks/dbc_decoder_bench` reports decode p99 against the 200 us NFR-03 target and
load time against the 500 ms NFR-04 target, printing CPU, compiler and build type. The stress suite
asserts the *shape* — bounded queue depth, monotonic drops, no RSS trend, and the five conservation
equations of [contracts/metrics.md](./contracts/metrics.md) section 4.

**Constraints**: bounded memory and bounded time everywhere. DBC file capped at `max_dbc_bytes`
before the read; messages, signals per message, total signals, token length and statement count each
capped during the parse, before the allocation they bound (D-12). RX queue capped at
`rx_queue_capacity` with drop-newest and a monotonic counter. No lock is held across a decode, and
no observer callback runs under the cache mutex or the reload mutex (S-07, section 12 rule 5). The
decoder pins the DBC snapshot exactly once per frame, so a batch cannot mix schema versions (P-13).
No shift by 64 or more, no read outside `frame.size`, no `double` round-trip for a raw value
(P-02, P-06). No exception escapes `decode()`. The one thread this project owns is joined, never
detached.

**Scale/Scope**: the lab DBC has 3 messages and 8 signals; the limits allow 2,000 messages and
10,000 signals, and a limit test builds a file at each bound. 100,000 frames in the soak, 10,000
iterations per race recipe, 50+ hand-computed goldens and 10,000 generated vectors. Roughly 2,600
lines of library code plus about 3,200 lines of tests.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Gate | Status |
|---|---|---|
| I. Spec Fidelity and Scope Discipline | Deliver section 3.1 exactly; nothing from section 3.2 (V2) or 3.3 (out of scope) | **PASS with one deferral.** R-01..R-10 close all ten open questions of section 24 with an explicit v1 boundary, and every contract file ends with a "Not in v1" list. CAN FD, float signals, extended multiplexing, E2E profiles, encoding and J1939 are rejected at load with `UNSUPPORTED_FEATURE` rather than half-supported (R-01, R-02, ADR-07). Six types and one error code are added beyond section 8; all are recorded in Complexity Tracking. Nothing is deferred: DBC-013 and the `vcan0` demo are delivered on project 05's transport (R-22). |
| II. Library-First Reuse | One CMake target per library with a public header surface; consume earlier projects rather than copy them | **PASS with two justified exceptions.** Project 05's `ICanTransport`, `CanMessage`, `SocketCanTransport` and `FakeCanTransport` are reused as-is, so this project writes no transport, no frame-source interface and no CAN fake (R-22). `tcu_scheduler` is genuinely reused for the freshness scan, the reload debounce and the reload job, so this project owns one thread instead of three (R-15). `tcu_logging` is reused for every diagnostic, `nlohmann/json` at the root pin, `tests/support/test_gate.hpp` as-is, and `scripts/setup_vcan.sh` for the `vcan` test and demo. Exception 1: `decoder.json` is **not** loaded through `tcu_config`, because `tcu_config` is the TCU application's configuration, not a generic schema engine (R-16). Exception 2: project 05's `CanService`, `SignalCache` and `DemoSignalDecoder` are **not** reused, because their `SignalSample`/`DecodeResult` cannot express this project's required per-signal raw/unit/label or per-batch schema identity — full argument and the two-caches cost in R-24. Both are in Complexity Tracking. |
| III. Test-First | Every FR has a unit test naming it; every Given/When/Then has a test; every DoD item is observable | **PASS** — the two traceability tables below map FR-01..FR-19 and B-01..B-20 onto named tests, and each task writes its test first. The property invariants of section 15.4 become the assertions listed in [data-model.md](./data-model.md); the conservation equations of [contracts/metrics.md](./contracts/metrics.md) section 4 turn "no frame went unaccounted for" into arithmetic a test can check. |
| IV. Simulation-Only | No real hardware; time and faults injectable; deterministic | **PASS** — nothing but files, threads and memory. Frames come from project 05's `FakeCanTransport` in every unit and integration test; the clock is a `std::function` everywhere freshness is computed; the nine fault points of section 15.2 are empty `std::function`s in a release build; the three race recipes of section 15.3 use test gates, not sleeps. Only `Vcan_EndToEnd` touches `vcan0`, and it skips with a message rather than failing or pretending when the interface is absent — project 05's rule that an impossible on-wire test must never be claimed as passed. |
| V. Safe Observability | Structured logs and metrics where the spec asks; no secrets; failures isolated and counted | **PASS** — log events and fields are enumerated in [contracts/metrics.md](./contracts/metrics.md) section 3, with per-reason rate limiting so a poisoned bus cannot fill a disk, and a `suppressed_since_last` count so suppression is visible rather than silent (R-17). No diagnostic contains a payload byte, a file's contents or a resolved absolute path (D-02). Metric labels are closed sets; no DBC-supplied name becomes a label, because a DBC is untrusted input with up to 10,000 signals (section 16.1). Every observer exception is caught at the publish boundary, counted in `decoder_observer_failures_total` and logged with the subscription id (V-08). |

**Post-Phase 1 re-check**: still PASS. Phase 1 added no third-party dependency (`nlohmann/json` was
already pinned), one in-tree SHA-256 (R-19, justified below), and no abstraction with a single
implementation at all — the transport seam is project 05's, with two implementations it already
ships.

### Requirement to test traceability

| FR (spec 4.1) | Covered by |
|---|---|
| FR-01 load immutable database with version/hash | `Database_VersionAndHashPublished`, `Database_IsImmutable`, `Sha256_NistVectors` |
| FR-02 parse and validate the subset | `Parser_AcceptsSampleSubset`, `Parser_IgnoresAllowlistedStatements`, the eleven `Validator_*` cases |
| FR-03 fail closed on unsupported syntax | `Parser_RejectsSigValtype`, `Parser_RejectsExtendedMux`, `Parser_RejectsUnknownDirective` |
| FR-04 Intel LSB and Motorola MSB start bits | `BitExtractor_IntelGoldens`, `BitExtractor_MotorolaGoldens`, `BitExtractor_CrossByteMotorola`, `Golden_GeneratedMatrix` |
| FR-05 signed extension 1..64 | `SignExtend_TwoComplementAllWidths`, `SignExtend_OneBitSignedIsMinusOne`, `SignExtend_Int64MinNoUb` |
| FR-06 physical scaling, reject non-finite | `Scaling_PhysicalFromFactorOffset`, `Scaling_NonFinitePhysicalIsDecodeError`, `Scaling_RawU64PreservedAboveTwoPow53` |
| FR-07 never conflate STD and EFF | `Validator_AcceptsStdAndEffSameNumber`, `Database_LookupDistinguishesFormat`, `Decode_UnknownIdCounted` |
| FR-08 validate length, flags, metadata first | `Decode_WrongLengthRejected`, `Decode_ErrorFrameRejected`, `Decode_RemoteFrameRejected` |
| FR-09 decode only the active mux branch | `Mux_Page1EmitsBatteryOnly`, `Mux_Page2EmitsMotorTempOnly`, `Mux_UndecodableSelectorSkipsBranches` |
| FR-10 invalid raw values from config only | `Decode_InvalidRawFromPolicy`, `Config_WarnsOnUnmatchedSignalKey` |
| FR-11 last-good and quality reported separately | `Cache_QueryReturnsBothAxes`, `Cache_InvalidKeepsLastGood` |
| FR-12 monotonic freshness; invalid never refreshes | `Freshness_ExactDeadlineIsStale`, `Cache_OutOfOrderFrameIgnored`, `Freshness_StaleDoesNotClearLastGood` |
| FR-13 unknown ids counted, never fatal | `Decode_UnknownIdCounted`, `RateLimit_SuppressesAndReportsGap` |
| FR-14 atomic reload, old config kept on failure | `Reload_FailureKeepsActiveVersion`, `Reload_CommitIncrementsVersion`, `RaceA_InFlightFramePinsOldVersion` |
| FR-15 stats and structured diagnostics, rate limited | `Parser_ReportsAllDiagnostics`, `Lexer_ReportsLineAndColumn`, `RateLimit_SuppressesAndReportsGap`, `Soak_MetricConservation` |
| FR-16 graceful stop, bounded queues, no late callback | `Service_StopIsIdempotent`, `Service_StopJoinsWithinDeadline`, `Service_WeakObserverExpired`, `Queue_DepthNeverExceedsCapacity`, `RaceC_StopWithBlockedObserver` |
| FR-17 CLI replay, deterministic tests with fake clock | `Cli_ValidateExitCodes`, `Cli_DecodeOutputStable`, `Candump_ParsesLines`, `Pipeline_DeterministicForFixedSequence` |
| FR-18 optional Event Bus publish (SHOULD) | `EventBus_PublishesSignalsDecoded`, `EventBus_PublishFailureCounted` |
| DBC-013 project 05 transport integration | `FrameAdapter_MapsCanMessage`, `Pipeline_ReplayToCacheEndToEnd` (over `FakeCanTransport`), `Vcan_EndToEnd` (over `vcan0`, skipped when the interface is absent) |
| FR-19 CAN FD behind a flag (OPTIONAL, V2) | **not implemented.** `Config_RejectsNonClassicalFrameMode` asserts the v1 boundary instead (R-01) |

| NFR (spec 4.2) | Evidence |
|---|---|
| NFR-01 zero sanitizer findings | ASan/UBSan and TSan runs over all four labels (Linux/WSL) |
| NFR-02 no unbounded growth over 100,000 frames | `Soak_HundredThousandFramesBounded`, `Soak_MetricConservation` |
| NFR-03 decode p99 under 200 us | `benchmarks/dbc_decoder_bench`, reported not asserted (R-07) |
| NFR-04 load under 500 ms | the same benchmark, load phase |
| NFR-05 parser fuzz, 1M inputs, no finding | `scripts/run_dbc_fuzz.sh` with the committed seed corpus (R-21) |
| NFR-06 no mixed schema in one decode | `Decode_BatchHasOneSchemaIdentity`, `RaceA_InFlightFramePinsOldVersion` |
| NFR-07 failed reload keeps the running version | `Reload_FailureKeepsActiveVersion`, `Reload_SymlinkRejected` |
| NFR-08 observer exception does not kill the worker | `Service_ObserverThrowIsolated` |
| NFR-09 deterministic for a known frame order | `Pipeline_DeterministicForFixedSequence`, `Cli_DecodeOutputStable` |
| NFR-10 every invalid input returns a classifiable code | `Parser_ReportsAllDiagnostics`, `Config_RejectsUnknownKey`, the `Decode_*` rejection cases |

### BDD contract traceability (spec section 13)

| ID | Covered by | Kind |
|---|---|---|
| B-01 | `Service_ReadyPublishesVersionAndHash`, `Service_NotReadyWithoutValidDbc` | unit |
| B-02 | `BitExtractor_IntelGoldens` (speed 100, ignition 1, cabin -40) | unit |
| B-03 | `BitExtractor_MotorolaGoldens` (raw 0xABC, signed -1348, physical -674) | unit |
| B-04 | `Mux_Page1EmitsBatteryOnly` | unit |
| B-05 | `Mux_Page2EmitsMotorTempOnly` | unit |
| B-06 | `Decode_WrongLengthRejected`, `Cache_InvalidKeepsLastGood` | unit |
| B-07 | `Database_LookupDistinguishesFormat`, `Decode_UnknownIdCounted` | unit |
| B-08 | `Cache_InvalidKeepsLastGood` | unit |
| B-09 | `Freshness_ExactDeadlineIsStale` | unit |
| B-10 | `Freshness_RecoveryAfterValid` | unit |
| B-11 | `Reload_FailureKeepsActiveVersion` | unit |
| B-12 | `RaceA_InFlightFramePinsOldVersion` | integration + TSan |
| B-13 | `Queue_DropsNewestWhenFull`, `Queue_DropCountersMonotonic` | unit |
| B-14 | `Service_ObserverThrowIsolated` | unit |
| B-15 | `Service_StopJoinsWithinDeadline`, `RaceC_StopWithBlockedObserver` | integration |
| B-16 | `BitExtractor_Width1And64`, `Scaling_RawU64PreservedAboveTwoPow53` | unit |
| B-17 | `Validator_RejectsNonFiniteFactor` | unit |
| B-18 | `Validator_RejectsOverlapInSameBranch` (asserts the reported line number) | unit |
| B-19 | `Validator_AcceptsOverlapAcrossBranches` | unit |
| B-20 | `Mux_UnknownSelectorKeepsUnconditional` | unit |

B-12 is a sanitizer target as well: part of what it asserts is the *absence* of a race, which only
TSan observes. Four cases exist beyond the spec's twenty contracts, each because a Phase 1 invariant
would otherwise be unobservable: `Cache_NoCallbackUnderLock` (S-07 — a re-entrant observer deadlocks
the test if the rule is broken), `Freshness_EpochRecheckBeatsLateTimer` (R-13, race recipe B),
`Config_SampleFileMatchesTable` (V-14 — one table is only a single source of truth if the shipped
sample cannot drift from it) and `Soak_MetricConservation` (M-01..M-05).

## Project Structure

### Documentation (this feature)

```text
specs/006-dbc-signal-decoder/
  spec.md                      # The design document, copied verbatim
  plan.md                      # This file
  research.md                  # Phase 0: closes the spec's open decisions (R-00..R-23)
  data-model.md                # Phase 1: entities, states, invariants D*/P*/S*/V*/M*
  quickstart.md                # Phase 1: build, test, demo sequence, acceptance checklist
  contracts/
    dbc_subset.md              # Accepted grammar, ignore list, the eleven validation classes
    dbc_api.md                 # tcu_dbc public API: pre/postconditions, thread safety, not-in-v1
    signal_api.md              # tcu_signal: source seam, cache, service, observer, reload
    decoder_config.md          # decoder.json field table, errors, relation to the DBC file
    metrics.md                 # Metric definitions, label sets, log vocabulary, conservation
  tasks.md                     # Phase 2 output (/speckit-tasks - NOT created here)
```

### Source Code (repository root)

Spec section 18's layout, re-rooted into the monorepo conventions (tests live under `tests/`, not
inside the module, as projects 01-04 do):

```text
CMakeLists.txt                                   # add the project 06 block, no platform guard
modules/dbc/
  CMakeLists.txt                                 # tcu_dbc: standard library only
  include/tcu/dbc/dbc_types.hpp                  # ByteOrder, MuxRole, DbcErrorCode, ParseDiagnostic
  include/tcu/dbc/frame_view.hpp                 # FrameFormat, CanFrameView (spec section 8)
  include/tcu/dbc/dbc_database.hpp               # SignalMeta, BitPlan, CompiledMessage, CompiledDbc
  include/tcu/dbc/dbc_parser.hpp                 # ParseLimits, LoadResult, loadDbcFile/loadDbcBytes
  include/tcu/dbc/bit_extractor.hpp              # extractRaw, signExtend
  include/tcu/dbc/signal_decoder.hpp             # SignalValue, DecodedBatch, DecodePolicy, SignalDecoder
  src/dbc_lexer.hpp, src/dbc_lexer.cpp           # tokens with line/column, limits enforced
  src/dbc_parser.cpp                             # statement-level recursive descent (R-11)
  src/schema_validator.hpp, .cpp                 # the eleven classes of contracts/dbc_subset.md
  src/compiled_bitplan.cpp                       # AST to BitPlan, lookup table, semantic hashes
  src/bit_extractor.cpp                          # spec sections 7.1 and 7.2, verbatim
  src/signal_decoder.cpp                         # frame to DecodedBatch
  src/sha256.hpp, src/sha256.cpp                 # FIPS 180-4, about 110 lines (R-19)
  src/fault_hooks.hpp                            # private test seam (R-23)
modules/signal/
  CMakeLists.txt                                 # tcu_signal: tcu_dbc + tcu_can + tcu_scheduler + tcu_logging
  include/tcu/signal/frame_adapter.hpp           # tcu::can::CanMessage -> tcu::dbc::CanFrameView (R-22)
  include/tcu/signal/signal_quality.hpp          # SignalQuality, QualityEvent, SignalKey
  include/tcu/signal/signal_cache.hpp            # SignalSnapshot, SignalCache
  include/tcu/signal/decoder_config.hpp          # DecoderConfig, loadDecoderConfig
  include/tcu/signal/decoder_service.hpp         # DecoderService, ISignalObserver, deps
  src/decoder_config.cpp                         # decoder.json schema table + validation (R-16)
  src/json_document.hpp, .cpp                    # nlohmann wrapper, kept out of public headers
  src/rx_queue.hpp                               # bounded MPSC of CanMessage, drop-newest
  src/signal_cache.cpp                           # two-axis commit, scan, migrate
  src/reload_coordinator.hpp, .cpp               # debounce, rate limit, validate off-path, swap
  src/decoder_service.cpp                        # worker loop, observers, lifecycle
  src/rate_limited_log.hpp                       # per-reason token bucket (R-17)
apps/dbc_validate/      CMakeLists.txt main.cpp  # links tcu_dbc only; portable
apps/dbc_decode/        CMakeLists.txt main.cpp  # + candump_reader.hpp, CLI-only (R-22)
apps/virtual_dbc_demo/  CMakeLists.txt main.cpp  # vcan0 or fake transport + Event Bus adapter (R-18)
configs/decoder.json                             # the sample sidecar, asserted against the table
configs/virtual_tcu.dbc                          # the lab DBC of spec section 6.1
benchmarks/dbc_decoder_bench/  CMakeLists.txt main.cpp
fuzz/dbc/  CMakeLists.txt fuzz_dbc_parser.cpp fuzz_frame_decoder.cpp   # TCU_BUILD_FUZZERS=ON
scripts/generate_dbc_vectors.py                  # cantools oracle, run by hand (R-20)
scripts/run_dbc_fuzz.sh                          # the NFR-05 budget
scripts/run_dbc_decoder_demo.sh                  # the section 20 sequence, replay instead of vcan
tests/unit/dbc/        lexer_test.cpp parser_test.cpp validator_test.cpp bit_extractor_test.cpp
                       sign_extend_test.cpp scaling_test.cpp mux_decoder_test.cpp database_test.cpp
                       sha256_test.cpp decode_frame_test.cpp
tests/unit/signal/     cache_quality_test.cpp freshness_test.cpp config_test.cpp queue_test.cpp
                       service_test.cpp reload_test.cpp rate_limit_test.cpp frame_adapter_test.cpp
tests/integration/dbc_decoder/
                       pipeline_test.cpp reload_race_test.cpp stale_race_test.cpp shutdown_test.cpp
                       eventbus_test.cpp golden_matrix_test.cpp cli_test.cpp vcan_test.cpp
tests/stress/dbc_decoder/
                       soak_test.cpp
tests/fixtures/dbc/    virtual_tcu.dbc, invalid/, golden/, corpus/
```

**Structure Decision**: two modules, because the spec names two libraries with genuinely different
properties — `tcu_dbc` is pure and has no dependency, `tcu_signal` has a thread and three. Keeping
them apart is what lets the parser and the bit decoder be fuzzed and property-tested with no
lifecycle at all, and lets `dbc_validate` link only what it needs. Tests live under `tests/`,
following projects 01-04 rather than spec section 18's in-module `tests/` directory.

## Complexity Tracking

> Deviations that need justification.

| Deviation | Why needed | Simpler alternative rejected because |
|---|---|---|
| Six types beyond spec section 8 (`ByteOrder`, `MuxRole`, `DbcErrorCode`, `ParseDiagnostic`, `LoadResult`, `SignalMeta`) and one error code (`DBC_SOURCE`) | Section 6.2 defines the error codes and section 6.3 defines the diagnostic shape; `bool` cannot carry them, and NFR-10 requires every failure to be classifiable | Returning `bool` plus a logged string: the CLI exit codes of FR-17 and the `reload_rejected` metric would have nothing to switch on, and B-18 asserts a reported line number |
| In-tree SHA-256, about 110 lines | Section 6.3 names SHA-256 for `dbc_hash` | `std::hash` is not a digest and is not stable across runs; OpenSSL adds a linked system dependency to a library that otherwise has none, for one call. Not a security boundary (R-08), and verified against NIST vectors |
| `decoder.json` not loaded through `tcu_config` (Principle II exception) | `tcu_config`'s schema table and snapshot struct *are* the TCU application's configuration by design (its own R-04); section 17 defines this sidecar as separate | Adding a `/dbc/...` section to project 04's snapshot couples two unrelated files and edits a finished, tested schema; extracting a generic schema engine is a refactor of a finished project nobody has asked for. The validation *pattern* is reused instead (R-16) |
| Per-reason rate limiting written here, not in `tcu_logging` | Sections 10.3 and 16.1 require sampled logs for unknown ids and repeated parse errors | Putting it in project 04 changes a finished library for one consumer, and the policy is decoder-specific (R-17) |
| Two signal caches now exist: `tcu::can::SignalCache` (project 05) and `tcu::signal::SignalCache` (this project) | Project 06's spec section 8 and 9 require an eight-value quality, the observation/last-good split, the epoch recheck and semantic-hash migration; project 05's `SignalSample` carries one `double` and a five-value quality | Extending project 05's cache would widen a finished project's public types and change its tested quality semantics for one consumer; plugging into `CanService` would discard the raw value, unit, enum label and DBC identity this project exists to produce (R-24). Retiring 05's cache belongs to project 07, which is the first consumer that needs one cache for two buses |
| Two added metrics (`decoder_observer_failures_total`, `decoder_out_of_order_total`) | V-08 and S-02 are otherwise unobservable, and Principle V requires isolated failures to be counted | Logging only: a test cannot assert a log line's absence as cheaply as a counter's value |

## Deferred shortcuts

Recorded here so they are visible rather than discovered later, in the repository's `shortcut:`
form — the limit, and when to upgrade.

1. **Project 05's `DemoSignalDecoder` is superseded but not retired.**
   `shortcut: two decoders and two signal caches coexist - project 05 keeps its seven hard-coded
   identifiers for its own demo and tests, project 06 owns the DBC path; upgrade in project 07,
   which is the first consumer needing one cache across two buses, by migrating project 05's tests
   onto tcu::signal::SignalCache.` (R-24)
2. **`decoder.json` is not hot-reloadable.**
   `shortcut: only the DBC file reloads; a limit or timeout change needs a restart; upgrade when a
   consumer needs runtime limits.` Section 17 does not ask for sidecar reload, and section 10.4 is
   about the DBC.
3. **Hot-path strings are not interned.**
   `shortcut: messageName and unit are copied per batch; upgrade after the benchmark shows the
   allocation matters.` Spec section 8 explicitly leaves interning until after measurement (R-07).
4. **The differential matrix is generated by hand, not in CI.**
   `shortcut: scripts/generate_dbc_vectors.py needs Python and cantools and is run by a developer;
   the generated vectors are committed; upgrade when CI gets a pinned Python environment.` (R-20)
5. **Fuzzing is not part of ctest.**
   `shortcut: TCU_BUILD_FUZZERS is OFF by default and needs clang; the NFR-05 budget runs from a
   script; upgrade when CI has a clang job with a time box.` (R-21)
6. **One mux level only.** Not a shortcut but a spec boundary (section 3.1): nested multiplexing is
   rejected with `UNSUPPORTED_FEATURE` and is V2 work.

---

## Post-implementation reconciliation

Written after the code existed, in project 04's style: what the plan got wrong,
what the sanitizers found, where test-first held, and the honest status of every
Definition of Done item. Dated 2026-11-06.

### What this plan got wrong

Three entries were written before the facts and had to be corrected. All three
corrections sit in [research.md](./research.md) next to the original reasoning
rather than silently replacing it.

1. **R-22 assumed project 05 did not exist.** The first draft deferred ticket
   DBC-013 and designed an `ICanSource` abstraction of our own. Project 05
   landed mid-session (`fe31dac`), so project 06 now reuses
   `tcu::can::ICanTransport`, `CanMessage`, `SocketCanTransport` and
   `FakeCanTransport`. That *deleted* work: no frame-source interface, no replay
   transport, no CAN fake - roughly 250 lines plus their tests. The lesson is
   the obvious one: a plan that assumes the state of the repository should check
   it.
2. **R-15 claimed one owned thread.** Not implementable against the spec it
   cited. `receive()` blocks and must be called from one thread, so a single
   receive-then-decode thread leaves the bounded queue nothing to absorb and
   B-13 could never arise from real traffic. Corrected to an RX reader plus one
   decoder worker, which is what spec section 10.2 describes.
3. **R-00 claimed both libraries were portable.** True of `tcu_dbc`, false of
   `tcu_signal` once it links `tcu_can`. `tcu_signal` is Linux-only and sits in
   the root Linux guard; `tcu_dbc` and its 91 unit tests still run on Windows.

### What the sanitizers found

**Nothing.** All 172 project 06 tests pass under `-fsanitize=address,undefined`
with leak detection enabled: 91 `dbc-unit`, 66 `signal-unit`, 12
`dbc-integration`, 3 `dbc-stress`. UBSan is the interesting half, because P-02
(no shift of 64 or more, no read outside `frame.size`) is exactly the kind of
claim a reviewer cannot check by eye.

**ThreadSanitizer found five things, and all five were in my own test code.**
That is worth stating plainly, because the first reading of the report was
wrong: the stack for the lock-order inversion runs through project 01's
`EventBus`, and I initially recorded it as a pre-existing defect there. It is
not. The second mutex in the cycle is at a stack address - the test's own.

- **Four data races on the `FaultHooks` global.** It held `std::function`
  members, which worker threads read on every frame while the test thread
  assigned and cleared them. A data race on the function object's internals,
  however carefully the test is written. Fixed at the root rather than per call
  site: each slot is now `std::atomic<std::function<void()>*>`, which is
  race-free by construction and cheaper on the hot path than `std::function`'s
  own bool conversion. Two test harnesses also cleared the hooks *before*
  stopping the threads that read them; that order is reversed, and hook objects
  are now declared before the harness so they outlive it.
- **One lock-order inversion, created by my Event Bus test.** It held its own
  mutex across `subscription.unsubscribe()`, while the dispatcher holds the
  subscription's `in_flight` mutex and then takes the test's mutex inside the
  handler - closing the cycle. Project 01 documents that `unsubscribe()` waits
  out an in-flight invocation, which is exactly why a caller must not hold a
  mutex the handler also takes. Fixed by copying under the lock and releasing
  before unsubscribing.

After those fixes: **172 tests, 0 failures, 0 ThreadSanitizer warnings** under
`-fsanitize=thread` with clang 14 and `TCU_SANITIZER_SETARCH=ON`.

The production code came out of this with no change at all, which is the result
worth having - but only because TSan was run. None of the five would have been
caught by reading, and the two test-harness ordering bugs would have surfaced
later as intermittent CI failures that looked like flakiness.

### Where test-first held, and where it did not

It held everywhere except one file. `src/sha256.cpp` was written before
`sha256_test.cpp`; every other implementation task followed its tests. Recorded
here rather than left to be discovered.

Test-first earned its keep once, concretely: `Lexer_ReportsLineAndColumn` was
written first, failed with column 10 instead of 1, and the reason was a real
parser bug - the `SG_` receiver list was unbounded and swallowed the following
line's statement, so an unknown directive would have passed silently. That
defeats the whole fail-closed design of ADR-01, and reading the code had not
caught it.

Three further bugs were found, all in my own tests rather than in the code, and
all of the same family - asserting on the wrong thing:

- `Config_SampleFileMatchesTable` loaded `configs/decoder.json` by a relative
  path. It passed by hand and failed under `ctest`, which is precisely why a
  test must not depend on the working directory.
- `EventBus_PublishFailureCounted` waited on `framesReceived`, which the reader
  thread increments *before* enqueuing, so it read the cache before anything had
  been decoded.
- `FreshnessTest.EpochRecheckUnderRealThreads` asserted `events.empty()` after
  spawning a writer thread. With a real thread the commit may land either side
  of phase 2, so both outcomes are correct; the test was asserting a scheduling
  accident. It now asserts what holds under every interleaving - the valid value
  wins the final state and no update is lost.

### Deviations from the planned structure

Recorded in full in [tasks.md](./tasks.md); the three that change the layout:

- No separate `freshness_monitor` or `reload_coordinator` translation unit. Both
  live in `src/decoder_service.cpp`, the single owner of the snapshot pointer.
- No third `json_document` wrapper. Projects 04 and 05 each have one to keep
  nlohmann out of a public header; here the dependency is already confined to
  one `.cpp` and appears in no header.
- `SignalQuality` lives in `tcu_dbc`, not `tcu_signal`, because the decoder
  attaches a quality to every value it produces and `tcu_dbc` may not depend on
  `tcu_signal`.

### Definition of Done, item by item

| Spec section 23 item | Status |
|---|---|
| Builds with CMake, all tests pass, warnings enabled | **met.** 759 tests across projects 01-06 on Linux, 392 on Windows, zero failures |
| Strict subset documented; decode-affecting directives rejected | **met.** [contracts/dbc_subset.md](./contracts/dbc_subset.md) plus the `Parser_Rejects*` cases |
| STD/EFF mapping and DLC rules validated | **met.** `Validator_AcceptsStdAndEffSameNumber`, `Database_LookupDistinguishesFormat`, `Decode_WrongLengthRejected` |
| Intel and Motorola verified against hand vectors **and an independent oracle** | **met, and the strongest result here.** The six hand-computed vectors of section 7.3 pass as written, and 10,000 generated frames / 23,222 signals agree exactly with `cantools` 39.4.4 |
| Signed 1..64, scaling, NaN/Inf, overflow without UB | **met.** `SignExtend_*`, `Scaling_*`, UBSan clean |
| One-level mux and `VAL_`; nested rejected | **met.** `Mux_*`, `Decode_EnumLabelResolved`, `Parser_RejectsExtendedMux` |
| No invalid/unknown/wrong-DLC frame refreshes last-good | **met.** `cache_quality_test.cpp` in full, plus the service-level `WrongLengthKeepsLastGood` |
| Monotonic freshness; reload cannot silently reinterpret | **met.** `Freshness_*` and `Reload_MigrationResetsChangedSignals` |
| Atomic reload tested against decode, timer and shutdown | **met.** Race A at 10,000 iterations, race B deterministically and under threads, race C with a blocked observer |
| Bounded queue and load-shedding metrics; no growth in soak | **met.** 100,000 frames, depth never above 64, cache stays at one entry per signal |
| Every P0 what-if mapped to a test with CI evidence | **mostly met.** [traceability.md](./traceability.md) maps all 116 P0 cases: 91 to a named test, 11 to a mechanism, 7 to a recorded design argument, and **7 to nothing** — the 7 are named there with what each would take |
| Zero sanitizer findings (NFR-01) | **met.** ASan + UBSan + leak detection: 172 tests, no findings. TSan: 172 tests, no findings, after fixing the five issues it found in the test code |
| Parser and frame fuzz clean for the agreed budget | **not met.** Both targets written, clang 14 available, budget not run - NFR-05 has no evidence |
| Standard demo on `vcan0` documented and repeatable | **partly met.** Written and wired; **unrun**, because the WSL2 kernel has no `vcan` module. `Vcan_EndToEnd` skips with that message and the demo was exercised with `--fake` |
| Limitations explicit | **met.** [docs/adr/006-dbc-signal-decoder.md](../../docs/adr/006-dbc-signal-decoder.md) states the limitation each ADR carries, including that a successful decode is not evidence of a genuine sender |
| ADR, docs, test report, benchmark retained | **met.** ADRs, `modules/dbc/README.md`, the measured table in [quickstart.md](./quickstart.md), and benchmark output carrying CPU, compiler and build type |

### Shortcuts taken during implementation

1. `shortcut: invalid_raw_codes and range_enforcement are looked up by building a
   "MESSAGE.Signal" string per signal when either table is non-empty; upgrade to
   indices resolved at load time if the benchmark ever shows it matters.` It does
   not today - decode p99 is 2.4-7.3 us against a 200 us target.
2. `shortcut: the invalid-DBC cases are inline strings in validator_test.cpp
   rather than 21 fixture files; upgrade when the fuzz corpus needs them as
   files, which scripts/run_dbc_fuzz.sh already generates on the fly.`
3. `shortcut: two signal caches coexist - project 05's and this one; upgrade in
   project 07, the first consumer needing one cache across two buses.` Full
   argument in R-24 and ADR-08.

### Measured numbers

i5-10300H, WSL2 Ubuntu 22.04, gcc 11.4, **Debug** builds - so these are
pessimistic, and they are reported rather than asserted (research R-07):

| Measurement | Result | Spec target |
|---|---|---|
| Decode p99, three message shapes | 2.4 - 7.3 us | NFR-03: 200 us |
| `dbc_load`, 3 messages | 0.31 ms | NFR-04: 500 ms |
| `dbc_load`, 2,000 messages (the configured limit) | 233 ms | NFR-04: 500 ms |
| 100,000-frame soak | queue depth never above 64, counters monotonic | NFR-02 |
