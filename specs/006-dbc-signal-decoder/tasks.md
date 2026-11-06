---

description: "Task list for DBC Signal Decoder (project 06)"
---

# Tasks: DBC Signal Decoder

**Input**: Design documents from `/specs/006-dbc-signal-decoder/`

**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/, quickstart.md — all present.

**Tests**: REQUIRED, not optional. Constitution Principle III is non-negotiable: every functional
requirement FR-01..FR-18 gets a unit test naming it, every behavioural contract B-01..B-20 gets a
test, and the test is written and seen to FAIL before the implementation that satisfies it.

**Platform**: `tcu_dbc` and the `dbc-unit` suite build and run on **both** Linux/WSL and Windows.
`tcu_signal`, the integration suite and the stress suite are **Linux-only**, because they link
project 05's `tcu_can` (research R-00, R-22). Sanitizer and fuzzing runs are Linux/WSL only.

**Revision, 2026-11-06**: project 05 landed (commit `fe31dac`) after this list was first drafted.
Tasks that defined a frame-source abstraction of our own are replaced by tasks that consume
`tcu::can::ICanTransport`; DBC-013 is now **delivered** rather than deferred. See research R-22 and
R-24.

**Organization**: phases follow the ticket dependency chain of spec section 19, re-ordered so that
each layer exists before its consumer:
`DBC-001 -> DBC-002 -> DBC-003 -> DBC-004 -> DBC-009 -> DBC-005..007 -> DBC-008 -> DBC-010 -> DBC-011 -> DBC-016 -> DBC-013(seam) -> DBC-012 -> DBC-015 -> DBC-014 + DBC-022 -> DBC-017..021`.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: can run in parallel — different files, no dependency on an incomplete task
- **[Story]**: US1..US11, mapped to the backlog groups below
- Every description carries its exact file path

## Story-to-ticket mapping

| Story | Delivers | Backlog tickets (spec section 19) | Contracts / invariants covered |
|---|---|---|---|
| US1 (P1) | Strict DBC parse, validation and the immutable compiled database with version and hash | DBC-002, 003, 004, 009 | B-17, B-18, B-19; D-01..D-13, P-01, P-07..P-10 |
| US2 (P2) | Bit extraction, sign extension, physical scaling, enum labels | DBC-005, 006, 007 | B-02, B-03, B-16; P-02..P-06 |
| US3 (P3) | One-level multiplexing | DBC-008 | B-04, B-05, B-20; P-14, P-15 |
| US4 (P4) | Frame-level decode decisions and the two-axis quality cache with last-good | DBC-010 | B-06, B-07, B-08; P-11..P-13, P-16, S-01..S-09 |
| US5 (P5) | Freshness, the exact deadline and the epoch recheck | DBC-011 | B-09, B-10; S-05, S-10 |
| US6 (P6) | The `decoder.json` sidecar, invalid-raw and range policies | DBC-016 | V-14..V-17 |
| US7 (P7) | The service over project 05's transport: frame adapter, bounded queue, worker, observers, shutdown, `vcan` end to end | DBC-013 | B-01, B-13, B-14, B-15; V-01..V-08 |
| US8 (P8) | Atomic hot reload with semantic migration and rate limiting | DBC-012 | B-11, B-12; V-09..V-13 |
| US9 (P9) | `dbc_validate` and `dbc_decode` | DBC-015 | FR-17, NFR-09 |
| US10 (P10) | Event Bus integration, the demo and the ADR/README | DBC-014, DBC-022 | FR-18; quickstart section 3 steps A-I |
| US11 (P11) | Goldens and differential matrix, fuzzing, race stress, soak, benchmark, traceability | DBC-017, 018, 019, 020, 021 | NFR-01..NFR-05, M-01..M-05 |

**Deviation from the template, stated deliberately**: these eleven stories are **layers, not
independent features**, exactly as project 04's were. The template's "all stories can run in
parallel once Foundational is done" does not apply: US2 needs US1's bit plans, US4 needs US2 and
US3, US5 needs US4's cache, US7 needs US6's config, US8 needs US7's service. Each story's
checkpoint is a working increment, but the order is fixed. Parallelism exists *within* phases,
marked `[P]`.

**Metrics note**: the atomic counter struct for both libraries is declared in Foundational (T015)
and each story increments the counters it owns, because US4's `Queue_DropsNewestWhenFull` asserts
`queueDropsTotal` and cannot wait for US11. US11 completes the set and asserts M-01..M-05. This
splits the observability half of DBC-021 across the stories; intentional, not an oversight.

**Dependency direction note**: `tcu_dbc` links **nothing** — not even `tcu_logging`. A parser that
can be fuzzed and property-tested with no lifecycle at all is the whole reason the module is split
(plan.md Structure Decision). Diagnostics are *returned*, never logged, by `tcu_dbc`.

**Reuse note**: project 05 owns the transport. US7 consumes `tcu::can::ICanTransport` with
`SocketCanTransport` for `vcan0` and `FakeCanTransport` for tests, and adds exactly one adapter
function, `toFrameView(const CanMessage&, busId)`. It does **not** reuse project 05's `CanService`,
`SignalCache` or `DemoSignalDecoder` — research R-24 gives the reason and records the cost (two
caches coexist until project 07).

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: directory structure, build wiring and fixtures, so a test can be compiled before any
library code exists

- [ ] T001 Create the `modules/dbc/` tree — `include/tcu/dbc/`, `src/` — and write `modules/dbc/CMakeLists.txt` declaring static library `tcu_dbc` with `target_include_directories(tcu_dbc PUBLIC include PRIVATE src)`, `target_compile_features(... PUBLIC cxx_std_17)`, `-Wall -Wextra -Wconversion` on GCC/Clang (spec section 23 asks for `-Wconversion`), and **no `target_link_libraries` entry at all** (dependency-direction note above)
- [ ] T002 Create the `modules/signal/` tree — `include/tcu/signal/`, `src/` — and write `modules/signal/CMakeLists.txt` declaring static library `tcu_signal` with the same include/feature/warning setup, linking `PUBLIC tcu_dbc tcu_can tcu_scheduler tcu_logging` (all four appear in its public headers — `tcu_can` for `ICanTransport` and `CanMessage`, research R-22) and `PRIVATE nlohmann_json::nlohmann_json` (wrapped away by `src/json_document.hpp`, research R-16), plus `find_package(Threads REQUIRED)` linked `PUBLIC`
- [ ] T003 Extend the root `CMakeLists.txt`: add `add_subdirectory(modules/dbc)` and `add_subdirectory(apps/dbc_validate)` **outside** any platform guard, and `add_subdirectory(modules/signal)`, `add_subdirectory(apps/dbc_decode)`, `add_subdirectory(apps/virtual_dbc_demo)` **inside** the existing project 05 `if(CMAKE_SYSTEM_NAME STREQUAL "Linux")` guard or a new one next to it, because `tcu_signal` links `tcu_can` (research R-00); add `benchmarks/dbc_decoder_bench` inside the existing `TCU_BUILD_BENCHMARKS` block (Linux-guarded, as `can_bench` is), `tests/unit/dbc` unguarded and `tests/unit/signal`, `tests/integration/dbc_decoder`, `tests/stress/dbc_decoder` Linux-guarded inside the existing `TCU_BUILD_TESTS` block, and declare `option(TCU_BUILD_FUZZERS "Build libFuzzer targets (clang only)" OFF)` with `add_subdirectory(fuzz/dbc)` guarded by it and by `CMAKE_CXX_COMPILER_ID MATCHES "Clang"` (research R-21). Do not restructure the project 01-05 lines
- [ ] T004 [P] Create `tests/unit/dbc/CMakeLists.txt` building one `dbc_unit_tests` target linked against `tcu_dbc` and `gtest_main`, with `target_include_directories(... PRIVATE ${CMAKE_SOURCE_DIR}/modules/dbc/src ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tests/support)` so private headers are testable, registered with `gtest_discover_tests(... PROPERTIES LABELS "dbc-unit")`, and a `TCU_DBC_FIXTURE_DIR` compile definition pointing at `${CMAKE_SOURCE_DIR}/tests/fixtures/dbc`
- [ ] T005 [P] Create `tests/unit/signal/CMakeLists.txt` building `signal_unit_tests` the same way against `tcu_signal` plus `${CMAKE_SOURCE_DIR}/modules/signal/src`, label `signal-unit`, same fixture-dir definition
- [ ] T006 [P] Create `tests/integration/dbc_decoder/CMakeLists.txt` building `dbc_decoder_integration_tests`, linking `tcu_signal`, `tcu_dbc`, `tcu_can`, `tcu_scheduler`, `tcu_logging`, `tcu_eventbus` and `gtest_main`, label `dbc-integration`
- [ ] T007 [P] Create `tests/stress/dbc_decoder/CMakeLists.txt` building `dbc_decoder_stress_tests`, label `dbc-stress`
- [ ] T008 [P] Create `configs/virtual_tcu.dbc` as the exact lab DBC of spec section 6.1 — `VERSION "virtual-tcu-lab-v1"`, `NS_`, `BS_`, `BU_: ECU TCU`, `BO_ 256 VEHICLE_STATUS: 8 ECU` with `VehicleSpeed 0|16@1+ (0.01,0) [0|250] "km/h"`, `Ignition 16|1@1+ (1,0) [0|1] ""`, `CabinTemp 24|8@1- (1,-40) [-40|125] "degC"`; `BO_ 257 TORQUE_STATUS: 8 ECU` with `Torque 7|12@0- (0.5,0) [-1024|1023.5] "Nm"`; `BO_ 258 MUX_STATUS: 8 ECU` with `Page M 0|8@1+`, `CommonStatus 8|8@1+`, `BatteryVoltage m1 16|16@1+ (0.01,0) [0|50] "V"`, `MotorTemp m2 16|16@1- (0.1,0) [-100|100] "degC"` — plus a `VAL_ 256 Ignition 0 "Off" 1 "On" ;` line. Copy it to `tests/fixtures/dbc/virtual_tcu.dbc`. **Lab identifiers only, no OEM data**
- [ ] T009 [P] Create `configs/decoder.json` with exactly the keys and defaults of contracts/decoder_config.md section 1, and the invalid-config fixtures under `tests/fixtures/dbc/config/` — `unknown_key.json`, `out_of_range.json`, `wrong_type.json`, `fd_frame_mode.json`, `unmatched_signal_key.json` — each differing from the valid file in exactly the one way that document records. **No credential, token or key in any of them**
- [ ] T010 [P] Create the invalid DBC fixture set under `tests/fixtures/dbc/invalid/`, one file per validation class of contracts/dbc_subset.md section 4, each with a comment naming the expected code and line: `syntax_signal_before_message.dbc`, `nul_byte.dbc`, `message_id_out_of_range.dbc`, `duplicate_message_key.dbc`, `message_too_long.dbc`, `duplicate_signal_name.dbc`, `width_zero.dbc`, `width_65.dbc`, `bits_outside_payload.dbc`, `overlap_same_branch.dbc`, `overlap_unconditional.dbc`, `two_selectors.dbc`, `branch_without_selector.dbc`, `duplicate_branch_value.dbc`, `duplicate_enum_number.dbc`, `min_greater_than_max.dbc`, `factor_nan.dbc`, `sig_valtype.dbc`, `sg_mul_val.dbc`, `unknown_directive.dbc`, `val_table_reference.dbc`, and the valid-but-noisy `ignorable_statements.dbc` (comments, `BA_`, `CM_`, `SIG_GROUP_`). Also create `overlap_across_branches.dbc`, which must be **accepted** (B-19)

**Checkpoint**: `cmake -S . -B build -G Ninja && cmake --build build` configures and builds projects
01-04 plus four empty project 06 test targets, on Linux and on Windows.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: the public type vocabulary, the hash, the counters and the test seam that every story
below needs

**CRITICAL**: no story work can begin until this phase is complete

- [ ] T011 [P] Write `modules/dbc/include/tcu/dbc/dbc_types.hpp`: `enum class ByteOrder { Intel, Motorola }`, `enum class MuxRole { None, Selector, Branch }`, `enum class Severity { Warning, Error }`, `enum class DbcErrorCode` with exactly the eleven codes of contracts/dbc_subset.md section 4 plus `DBC_SOURCE`, `struct ParseDiagnostic { std::string file; uint32_t line; uint32_t column; DbcErrorCode code; Severity severity; std::string message; }`, and a `const char* toString(DbcErrorCode)` so diagnostics and metric labels use one spelling
- [ ] T012 [P] Write `modules/dbc/include/tcu/dbc/frame_view.hpp`: `enum class FrameFormat { Standard11, Extended29 }`, `enum class DecodeError` with the ten values of spec section 8, and `struct CanFrameView` exactly as spec section 8 declares it (`arbitrationId`, `format`, `data`, `size`, `isRemote`, `isError`, `rxTime` as `steady_clock::time_point`, `busId`). Add a `static constexpr uint8_t kMaxClassicalPayload = 8`
- [ ] T013 Write `tests/unit/dbc/sha256_test.cpp` with `Sha256_NistVectors` — the empty string, `"abc"`, the 56-character FIPS 180-4 vector, and one million `a` characters — then implement `modules/dbc/src/sha256.hpp` and `modules/dbc/src/sha256.cpp` (FIPS 180-4, streaming `update`/`finalHex`, research R-19). Test first; see it fail
- [ ] T014 [P] Write `modules/dbc/src/fault_hooks.hpp`: a `struct FaultHooks` with the nine default-empty `std::function<void()>` members named in spec section 15.2 (`beforeValidate`, `beforeBitRead`, `afterPinSnapshot`, `beforeCacheCommit`, `afterCacheCommit`, `beforeEventPublish`, `afterReloadParse`, `beforeReloadCommit`, `onStopRequested`) and a `FaultHooks& testHooks()` accessor. `src/`-private: no public header includes it, and there is no `#ifdef` build variant (research R-23)
- [ ] T015 [P] Write `modules/signal/include/tcu/signal/decoder_metrics.hpp`: a `struct DecoderMetrics` of `std::atomic<uint64_t>` counters for every metric of contracts/metrics.md section 1 plus the two added ones, with a `DecoderMetricsSnapshot` plain-struct `snapshot()` for assertions, and the closed label enums of section 2 as `enum class FrameResult` / `enum class LoadResultLabel`. Relaxed ordering is fine (spec section 12 rule 8); metrics are never used for synchronisation
- [ ] T016 [P] Write `modules/signal/include/tcu/signal/signal_quality.hpp`: `enum class SignalQuality` with the eight values of spec section 8, `struct SignalKey { uint32_t busId; bool isExtended; uint32_t arbitrationId; std::string signalName; }` with `operator==` and a `std::hash` specialization, `enum class QualityEventKind { SignalUpdated, SignalQualityChanged, SignalRecovered, SchemaChanged }`, and `struct QualityEvent { QualityEventKind kind; SignalKey key; SignalQuality from; SignalQuality to; }`

**Checkpoint**: the type vocabulary compiles standalone on both platforms and the SHA-256 test passes.

---

## Phase 3: User Story 1 — Strict parse, validation and the immutable database (Priority: P1) MVP

**Goal**: a DBC file becomes an immutable, versioned, hashed database with O(1) lookup, or it is
rejected whole with a diagnostic per problem.

**Independent Test**: `ctest -L dbc-unit -R "Lexer_|Parser_|Validator_|Database_"` — the valid
fixture loads and reports its stats; each of the 21 invalid fixtures is rejected with the expected
code and line; `overlap_across_branches.dbc` is accepted.

### Tests for User Story 1 (write first, see them fail)

- [ ] T017 [P] [US1] Write `tests/unit/dbc/lexer_test.cpp`: `Lexer_ReportsLineAndColumn` (a token on line 4 column 3 reports exactly that, 1-based over bytes), `Lexer_RejectsNulByte`, `Lexer_RejectsOverlongToken` (the 1 KiB token cap yields `DBC_LIMIT` naming the limit), `Lexer_HandlesCrLf`
- [ ] T018 [P] [US1] Write `tests/unit/dbc/parser_test.cpp`: `Parser_AcceptsSampleSubset` (3 messages, 8 signals, 1 enum from `virtual_tcu.dbc`), `Parser_IgnoresAllowlistedStatements` (`ignorable_statements.dbc` loads, `stats.ignoredStatements > 0`, one `Severity::Warning` per kind, no error), `Parser_RejectsSignalBeforeMessage`, `Parser_RejectsSigValtype`, `Parser_RejectsExtendedMux`, `Parser_RejectsUnknownDirective`, `Parser_RejectsValTableReference`, `Parser_ReportsAllDiagnostics` (a file with three distinct errors yields three diagnostics ordered by line then column, and `db == nullptr`)
- [ ] T019 [P] [US1] Write `tests/unit/dbc/validator_test.cpp`, one case per class of contracts/dbc_subset.md section 4 and each asserting the **code and the line**: `Validator_RejectsBitsOutsidePayload`, `Validator_RejectsOverlapInSameBranch` (B-18), `Validator_AcceptsOverlapAcrossBranches` (B-19), `Validator_RejectsOverlapWithUnconditional`, `Validator_RejectsDuplicateMessageKey`, `Validator_AcceptsStdAndEffSameNumber` (D-05, FR-07), `Validator_RejectsMessageIdOutOfRange`, `Validator_RejectsMessageTooLong`, `Validator_RejectsDuplicateSignalName`, `Validator_RejectsWidthZeroAndWidth65`, `Validator_RejectsNonFiniteFactor` (B-17), `Validator_RejectsMinGreaterThanMax`, `Validator_RejectsTwoSelectors`, `Validator_RejectsBranchWithoutSelector`, `Validator_RejectsDuplicateBranchValue`, `Validator_RejectsDuplicateEnumNumber`, `Validator_EnforcesLimits` (a generated file at `max_messages + 1`, one at `max_signals_per_message + 1`, one over `dbc_max_bytes` — each `DBC_LIMIT`, each checked **before** the allocation it bounds, D-12)
- [ ] T020 [P] [US1] Write `tests/unit/dbc/database_test.cpp`: `Database_VersionAndHashPublished` (version 1, hash equals the SHA-256 of the file bytes, byte-identical files hash alike and a one-byte change does not), `Database_IsImmutable` (a compile-time check that `CompiledDbc` exposes no non-const member function), `Database_LookupDistinguishesFormat` (B-07: standard 0x100 and extended 0x100 are two entries), `Database_SemanticHashStable` (re-loading the same file gives identical per-signal semantic hashes; changing only `factor` changes exactly that signal's hash — R-09, P-10), `Database_RejectsSymlinkAndDirectory` (`DBC_SOURCE`, R-08)

### Implementation for User Story 1

- [ ] T021 [US1] Implement `modules/dbc/src/dbc_lexer.hpp` and `.cpp`: a single-pass lexer producing `Token{Kind, text, line, column}` for identifiers, numbers (decimal and signed/floating), strings, and the punctuation of contracts/dbc_subset.md section 1, enforcing the token-length and statement-count caps as it goes, rejecting NUL bytes and invalid UTF-8, and accepting LF and CRLF
- [ ] T022 [US1] Implement `modules/dbc/src/dbc_parser.cpp` plus the public `modules/dbc/include/tcu/dbc/dbc_parser.hpp` (`struct ParseLimits` with the spec section 3.4 defaults `dbcMaxBytes = 2 * 1024 * 1024`, `maxMessages = 2000`, `maxSignalsPerMessage = 128`, `maxTotalSignals = 10000`, `maxMessageLength = 8`, `maxTokenBytes = 1024`, `maxStatements = 100000`; `struct LoadResult`; `loadDbcFile`; `loadDbcBytes`): statement-level recursive descent over the accepted grammar, the three-tier handling of section 2 and 3 (parse / warn-and-skip / reject whole file), `RawMessage`/`RawSignal` AST per data-model.md section 1.2, extended-flag resolution per D-04, and **all** diagnostics collected rather than the first. No recursion over file-controlled depth (D-13); never throws for any byte sequence
- [ ] T023 [US1] Implement `modules/dbc/src/schema_validator.hpp` and `.cpp`: the eleven check classes, with the overlap rule computing a bit set per signal and comparing only *co-active* pairs (both unconditional, same branch, or unconditional versus branch — D-09), and diagnostics carrying the offending statement's line and column. Messages quote at most 64 characters of input, never payload, and `DBC_SOURCE` never embeds a resolved absolute path (D-02)
- [ ] T024 [US1] Write `modules/dbc/include/tcu/dbc/dbc_database.hpp` (`SignalMeta`, `BitPlan`, `CompiledMessage`, `MessageKey`, `Stats`, `CompiledDbc` with `find`, `findSignal`, `stats`, `version`, `hash`, `sourcePath` — every accessor `const`) and implement `modules/dbc/src/compiled_bitplan.cpp`: AST to per-signal `BitPlan` with `lastByteTouched` precomputed, the selector/unconditional/branches split, the `(busId, format, arbitrationId)` hash map, and the R-09 semantic hash per signal over `(messageId, isExtended, busId, signalName, startBit, width, byteOrder, isSigned, factor, offset, muxRole, muxValue, unit)`
- [ ] T025 [US1] Wire `loadDbcFile` end to end: `symlink_status` and regular-file check, size check against `dbcMaxBytes` **before** the read, read, lex, parse, validate, compile, SHA-256 of the exact bytes, `version = 1`, return `shared_ptr<const CompiledDbc>` only when no `Severity::Error` diagnostic exists (D-03, P-07..P-09)

**Checkpoint**: a valid DBC loads into an immutable hashed database; every invalid fixture is
rejected with a located, classified diagnostic. US1 is independently testable and is the MVP.

---

## Phase 4: User Story 2 — Bit extraction, signedness, scaling, enums (Priority: P2)

**Goal**: raw bits become exact `uint64_t` values, correct two's-complement signed values and
finite physical values, with enum labels where the DBC gives them.

**Independent Test**: `ctest -L dbc-unit -R "BitExtractor_|SignExtend_|Scaling_"` — the six
hand-computed vectors of spec section 7.3 pass, and UBSan reports nothing.

### Tests for User Story 2 (write first, see them fail)

- [ ] T026 [P] [US2] Write `tests/unit/dbc/bit_extractor_test.cpp` with the spec section 7.3 table as the **first** cases, by hand, before any generated vector: `BitExtractor_IntelGoldens` (payload `10 27 01 00 50 00 00 00`: `VehicleSpeed` raw `0x2710` -> 100.00 km/h, `Ignition` bit 16 -> 1, `CabinTemp` raw `0x00` -> -40 degC, and an assertion that byte index 4 `0x50` is **not** read by `CabinTemp` — the off-by-one trap the spec calls out), `BitExtractor_MotorolaGoldens` (payload `AB C0 ...`: `Torque 7|12@0-` raw `0xABC`), `BitExtractor_CrossByteMotorola` (a signal spanning three bytes with the `p % 8 == 0 ? p + 15 : p - 1` step), `BitExtractor_Width1And64` (B-16: width 64 unsigned returns exactly `UINT64_MAX`), `BitExtractor_RejectsOutOfRangePosition` (returns no value rather than reading past `size`, P-02)
- [ ] T027 [P] [US2] Write `tests/unit/dbc/sign_extend_test.cpp`: `SignExtend_TwoComplementAllWidths` (for every width 1..64, the maximum positive, the minimum negative and -1 round-trip against an independently computed reference), `SignExtend_OneBitSignedIsMinusOne` (raw 1, width 1, equals -1), `SignExtend_TwelveBitAbc` (raw `0xABC`, width 12, equals -1348), `SignExtend_Int64MinNoUb` (width 64, raw `0x8000000000000000`, equals `INT64_MIN`, and UBSan is clean)
- [ ] T028 [P] [US2] Write `tests/unit/dbc/scaling_test.cpp`: `Scaling_PhysicalFromFactorOffset` (`0x2710 * 0.01 == 100.0` within 1e-9; `-1348 * 0.5 == -674.0` exactly), `Scaling_NonFinitePhysicalIsDecodeError` (a plan whose scaling overflows to infinity yields quality `DecodeError` with `NonFinitePhysical` and leaves the rest of the batch intact, P-05), `Scaling_RawU64PreservedAboveTwoPow53` (raw above 2^53 is returned exactly in `raw`, the `physical` double may round, and `raw` never passes through `double` or `int64_t`, P-06), `Scaling_SignedRawPresentOnlyWhenSigned` (P-04)

### Implementation for User Story 2

- [ ] T029 [US2] Implement `modules/dbc/include/tcu/dbc/bit_extractor.hpp` and `modules/dbc/src/bit_extractor.cpp`: `std::optional<uint64_t> extractRaw(const uint8_t* data, size_t bytes, size_t start, unsigned width, ByteOrder)` and `std::optional<int64_t> signExtend(uint64_t raw, unsigned bits)`, transcribed from spec sections 7.1 and 7.2 **verbatim** — the same loop, the same Motorola step, the same `INT64_MIN` special case, position validated against `bytes * 8` before each access, no shift of 64 or more
- [ ] T030 [US2] Write `modules/dbc/include/tcu/dbc/signal_decoder.hpp` (`SignalValue` and `DecodedBatch` exactly as spec section 8 declares them, `DecodeResult`, `struct DecodePolicy { PayloadLengthPolicy length; const InvalidRawTable* invalidRaw; const RangeEnforcement* rangeEnforce; }`, `class SignalDecoder : public ISignalDecoder`) and implement the per-signal half in `modules/dbc/src/signal_decoder.cpp`: extract, sign-extend, scale, resolve the enum label when the raw value has one, apply `invalidRaw` when the table is non-null (quality `InvalidRaw`, research R-05), apply the range only for signals listed in `rangeEnforce` (quality `OutOfRange`, **never clamped**, research R-04)
- [ ] T031 [US2] Add the `beforeBitRead` fault hook call site in `signal_decoder.cpp` (T014) and confirm with a unit test that a build with no hook installed produces identical output

**Checkpoint**: every signal in the lab DBC decodes to the documented value; US1 plus US2 is a
usable pure decoder.

---

## Phase 5: User Story 3 — One-level multiplexing (Priority: P3)

**Goal**: the active branch and only the active branch is emitted; an unknown selector never invents
a branch.

**Independent Test**: `ctest -L dbc-unit -R Mux_`.

- [ ] T032 [P] [US3] Write `tests/unit/dbc/mux_decoder_test.cpp`: `Mux_Page1EmitsBatteryOnly` (B-04: payload `01 7F D2 04 ...` yields `Page`, `CommonStatus`, `BatteryVoltage` 12.34 V, and `MotorTemp` is **absent** from the batch, not present-and-invalid), `Mux_Page2EmitsMotorTempOnly` (B-05: `02 7F 9C FF ...` yields `MotorTemp` -10 degC, `BatteryVoltage` absent), `Mux_UnknownSelectorKeepsUnconditional` (B-20: selector 7 yields `Page` and `CommonStatus`, no branch signal, `unknownMuxTotal` incremented, and the batch flagged `muxUnresolved`), `Mux_UndecodableSelectorSkipsBranches` (a selector whose bits fall outside a short payload rejects the branch half only, P-15)
- [ ] T033 [US3] Implement the message-level half of `modules/dbc/src/signal_decoder.cpp`: decode the selector first when present, select `unconditional + branches[selectorRaw]`, set `muxUnresolved` when the selector value has no branch, and never emit a plan from a non-matching branch (P-14)
- [ ] T034 [US3] Add `Decode_EnumLabelResolved` to `tests/unit/dbc/decode_frame_test.cpp`: `Ignition` raw 1 carries label `"On"`, raw 0 carries `"Off"`, and a value with no label carries no label but still carries the number (D-11)

**Checkpoint**: multiplexed messages decode correctly; `tcu_dbc` is feature-complete for v1.

---

## Phase 6: User Story 4 — Frame-level decisions and the two-axis quality cache (Priority: P4)

**Goal**: a bad frame can never produce a good value, and a good value is never erased by a bad
frame.

**Independent Test**: `ctest -L dbc-unit -R "Decode_"` plus `ctest -L signal-unit -R "Cache_|Queue_"`.

### Tests for User Story 4 (write first, see them fail)

- [ ] T035 [P] [US4] Complete `tests/unit/dbc/decode_frame_test.cpp`: `Decode_WrongLengthRejected` (B-06: a 7-byte frame for an 8-byte message returns `WrongLength` and no signal is decoded, P-12), `Decode_RejectsOversizePayload` (`size > 8` returns `WrongLength` before any lookup, R-01), `Decode_UnknownIdCounted` (FR-13: `UnknownMessage`, empty batch, no throw), `Decode_ErrorFrameRejected` and `Decode_RemoteFrameRejected` (P-11: neither reads the payload), `Decode_BatchHasOneSchemaIdentity` (NFR-06, P-13), `Decode_InvalidRawFromPolicy` (raw 65535 with the policy table present yields `InvalidRaw`; with a null table the same frame is `Valid` — FR-10), `Decode_RangeEnforcedOnlyWhenConfigured` (R-04), `Decode_NeverThrowsForAnyPayload` (a loop over crafted frames inside one `EXPECT_NO_THROW`, P-16)
- [ ] T036 [P] [US4] Write `tests/unit/signal/cache_quality_test.cpp` against spec section 9.2's matrix, row by row: `Cache_ValidUpdatesLastGood` (S-01), `Cache_InvalidKeepsLastGood` (B-08: `InvalidRaw`, `OutOfRange`, `DecodeError`, `WrongLength`, unknown id and unknown mux all leave `lastGood`, `lastGoodRxTime` and `epoch` untouched — S-04), `Cache_NotActiveDoesNotUpdate` (section 9.2 row 7), `Cache_OutOfOrderFrameIgnored` (S-02: `lastGoodRxTime` never decreases and `outOfOrderTotal` increments), `Cache_EpochIncrementsOnCommit` (S-03), `Cache_QueryReturnsBothAxes` (S-06), `Cache_RepeatedIdenticalObservationEmitsNoQualityEvent` (S-09), `Cache_NoCallbackUnderLock` (S-07: an observer that re-enters `get()` from inside the returned event dispatch must not deadlock — the test deadlocks if the rule is broken)
- [ ] T037 [P] [US4] Write `tests/unit/signal/queue_test.cpp`: `Queue_DropsNewestWhenFull` (B-13: the producer returns promptly, `queueDropsTotal` increments), `Queue_DepthNeverExceedsCapacity` (V-01), `Queue_DropCountersMonotonic` (V-03), `Queue_PreservesOrder` (V-04)

### Implementation for User Story 4

- [ ] T038 [US4] Implement `modules/signal/include/tcu/signal/signal_cache.hpp` and `modules/signal/src/signal_cache.cpp`: `SignalSnapshot` and `SignalState` per data-model.md section 3.1 (two axes, `Stale` derived, `epoch`), `get`, `commit(const DecodedBatch&)` returning `std::vector<QualityEvent>` and dispatching nothing (S-08), and `modules/signal/src/rx_queue.hpp` as a bounded MPSC mutex + condition-variable queue holding `tcu::can::CanMessage` values with drop-newest — project 05's frame already owns its payload, so nothing is copied out of a borrowed view (research R-22). Add the `beforeCacheCommit` and `afterCacheCommit` hook call sites

**Checkpoint**: the cache enforces every row of the quality matrix; a bad frame provably cannot
produce or destroy a good value.

---

## Phase 7: User Story 5 — Freshness (Priority: P5)

**Goal**: staleness is computed from the last-good monotonic timestamp, exactly at the deadline, and
a late timer cannot overwrite a fresh value.

**Independent Test**: `ctest -L signal-unit -R Freshness_` — no test sleeps.

- [ ] T039 [P] [US5] Write `tests/unit/signal/freshness_test.cpp`: `Freshness_ExactDeadlineIsStale` (B-09: with `default_timeout_ms = 500`, `now == lastGoodRxTime + 500ms` is stale and `now == +499ms` is not — S-05), `Freshness_StaleDoesNotClearLastGood` (B-09 second half), `Freshness_RecoveryAfterValid` (B-10: `SignalRecovered`, new timestamp), `Freshness_PerSignalOverrideApplies` (research R-03), `Freshness_InactiveMuxBranchUsesLastActiveInstance` (spec section 10.5: an inactive branch is not treated as packet loss), `Freshness_EpochRecheckBeatsLateTimer` (race recipe B through the `tests/support/test_gate.hpp` rendezvous: the scan samples, a valid commit lands, the scan resumes and must **not** emit stale — S-10, R-13)
- [ ] T040 [US5] Implement `SignalCache::scanFreshness(now)` in `modules/signal/src/signal_cache.cpp` as the two-step epoch-recheck of research R-13, plus `isFresh(now)` as an exact predicate, and the per-signal timeout lookup with the default fallback
- [ ] T041 [US5] Implement `modules/signal/src/freshness_monitor.hpp`: a thin adapter that registers `scanFreshness` with `tcu::scheduler::TimerScheduler::scheduleEvery(scan_period_ms, mode, cb)` and publishes the returned events **after** the cache call returns — no new thread (research R-15)

**Checkpoint**: freshness is exact under query and event-driven under the scheduler, with the
stale/valid race covered deterministically.

---

## Phase 8: User Story 6 — The `decoder.json` sidecar (Priority: P6)

**Goal**: one validated, immutable configuration struct, with unknown keys rejected and every error
naming a JSON pointer but never a value.

**Independent Test**: `ctest -L signal-unit -R Config_`.

- [ ] T042 [P] [US6] Write `tests/unit/signal/config_test.cpp`: `Config_SampleFileMatchesTable` (V-14: `configs/decoder.json` has exactly the keys of the schema table, and every value is in range), `Config_RejectsUnknownKey` (at top level and nested, V-14), `Config_RejectsOutOfRange`, `Config_RejectsWrongType`, `Config_RejectsMissingKey` (no defaults layer), `Config_RejectsNonClassicalFrameMode` (V-15, R-01), `Config_RejectsMultipleWorkers` (V-15, ADR-05), `Config_WarnsOnUnmatchedSignalKey` (V-16: a key naming no signal in the loaded DBC is a counted warning, not a silent no-op), `Config_ErrorNamesPointerNotValue` (V-17: the message contains `/rx_queue_capacity` and the kind, and does **not** contain the offending number)
- [ ] T043 [US6] Implement `modules/signal/src/json_document.hpp` and `.cpp` wrapping `nlohmann/json` (strict parse, no comments, duplicate-key rejection, depth cap 8 — the same settings project 04 chose in its research R-01) so the dependency appears in no public header of `tcu_signal`
- [ ] T044 [US6] Implement `modules/signal/include/tcu/signal/decoder_config.hpp` and `modules/signal/src/decoder_config.cpp`: one `kFieldSpecs`-style table holding every row of contracts/decoder_config.md section 1 with type, range, legal values and mandatory flag; `DecoderConfig` as an immutable struct; `loadDecoderConfig(path)` returning the struct or the full list of errors; and the `MESSAGE.Signal` / `bus<id>:MESSAGE.Signal` key parsing for the three signal-name maps

**Checkpoint**: configuration is validated before anything starts, and a typo cannot silently
disable a check.

---

## Phase 9: User Story 7 — The service (Priority: P7)

**Goal**: frames flow from a source through a bounded queue and one worker into the cache, observers
are isolated, and stop is ordered and bounded.

**Independent Test**: `ctest -L signal-unit -R "Service_|FrameAdapter_"` and
`ctest -L dbc-integration -R "Pipeline_|RaceC_|Vcan_"`.

### Tests for User Story 7 (write first, see them fail)

- [ ] T045 [P] [US7] Write `tests/unit/signal/frame_adapter_test.cpp`: `FrameAdapter_MapsCanMessage` (id, `format`, `size` from `length`, `rxTime` from `received_at` unchanged, and the view's `data` pointing at the message's own array), `FrameAdapter_MapsRemoteAndErrorKinds` (`FrameKind::Remote` -> `isRemote`, `FrameKind::Error` -> `isError`), `FrameAdapter_ExtendedFormatPreserved` (B-07 at the adapter level). Nothing here re-stamps time and nothing re-validates length — project 05 already did both
- [ ] T046 [P] [US7] Write `tests/unit/signal/service_test.cpp`: `Service_NotReadyWithoutValidDbc` (B-01 negative: cold-start failure leaves `NotReady`, every query answers `ConfigUnavailable`, no batch is published — V-05), `Service_ReadyPublishesVersionAndHash` (B-01), `Service_ObserverThrowIsolated` (B-14, NFR-08: the worker survives, `observerFailuresTotal` increments, one log line per rate-limit window), `Service_WeakObserverExpired` (a destroyed observer is skipped and unregistered, never called — FR-16), `Service_StopIsIdempotent`, `Service_StopJoinsWithinDeadline` (B-15, V-06, V-07), `Service_QueryNeverBlocksOnWorker`
- [ ] T047 [P] [US7] Write `tests/integration/dbc_decoder/pipeline_test.cpp` (`Pipeline_ReplayToCacheEndToEnd` over a scripted `FakeCanTransport`, `Pipeline_DeterministicForFixedSequence` — same file plus same fake clock yields byte-identical event and batch sequences, section 15.4 P-12) and `tests/integration/dbc_decoder/shutdown_test.cpp` (`RaceC_StopWithBlockedObserver`: race recipe C — an observer blocks on a gate, the publish queue fills, `stop(2s)` returns inside its deadline and reports `StopIncomplete` rather than blocking, and no callback runs afterwards)

### Implementation for User Story 7

- [ ] T048 [US7] Implement `modules/signal/include/tcu/signal/frame_adapter.hpp`: the single inline `toFrameView(const tcu::can::CanMessage&, uint32_t busId)` of contracts/signal_api.md section 1. **No frame-source interface, no replay source and no CAN fake is written** — `tcu::can::ICanTransport`, `SocketCanTransport` and `FakeCanTransport` are used as project 05 ships them (research R-22, Principle II)
- [ ] T049 [US7] Implement `modules/signal/include/tcu/signal/decoder_service.hpp` and `modules/signal/src/decoder_service.cpp`: `ISignalObserver` as spec section 8 declares it, `DecoderServiceDeps` (`std::unique_ptr<tcu::can::ICanTransport> transport`, scheduler, logger, injectable `now`), the lifecycle of data-model.md section 3.4, the single worker loop (pop, pin the snapshot once per frame, decode, commit, publish after unlocking), `weak_ptr` observer registry with exception isolation at the boundary, `metrics()`, and the fixed stop order of V-06 with the `onStopRequested` hook. Also `modules/signal/src/rate_limited_log.hpp`: a per-`(event, reason)` token bucket at `logging.rate_limit_per_reason_per_min` that attaches `suppressed_since_last` to the next emitted line (research R-17), with `tests/unit/signal/rate_limit_test.cpp::RateLimit_SuppressesAndReportsGap`

- [ ] T049b [US7] Write `tests/integration/dbc_decoder/vcan_test.cpp::Vcan_EndToEnd` (DBC-013): open a `SocketCanTransport` on the configured interface, `send` the three lab frames through it, and assert the cache reports the spec section 7.3 values with quality `Valid` and one schema identity. `GTEST_SKIP()` with an explicit message when the interface does not exist, so CI without `vcan` is honest rather than green by omission — and never claim an on-wire test ran when it did not (project 05's section 18.1 rule)

**Checkpoint**: the whole pipeline runs over project 05's transport — `FakeCanTransport` in tests
and `vcan0` end to end — with one owned thread, a bounded queue and isolated observers.

---

## Phase 10: User Story 8 — Atomic hot reload (Priority: P8)

**Goal**: a reload either commits atomically with a new version, or changes nothing at all.

**Independent Test**: `ctest -L signal-unit -R Reload_` and `ctest -L dbc-integration -R RaceA_`.

- [ ] T050 [P] [US8] Write `tests/unit/signal/reload_test.cpp`: `Reload_FailureKeepsActiveVersion` (B-11, NFR-07: a parse error leaves version, hash and every cache entry untouched and the service `Ready` — V-11), `Reload_CommitIncrementsVersion` (V-12), `Reload_BusyRejectsSecondRequest` (V-09: `ReloadBusy`, not queued), `Reload_RateLimited` (V-13: the seventh attempt in a minute is rejected and counted without touching the snapshot), `Reload_SymlinkRejected` (`DBC_SOURCE`, R-08), `Reload_MigrationKeepsUnchangedSignals` and `Reload_MigrationResetsChangedSignals` (R-09: unchanged semantic hash keeps last-good and freshness; a changed `factor` resets that signal to `NotReceived`; a removed signal becomes `ConfigUnavailable`), `Reload_DisabledReturnsReloadDisabled`
- [ ] T051 [P] [US8] Write `tests/integration/dbc_decoder/reload_race_test.cpp::RaceA_InFlightFramePinsOldVersion` — race recipe A through `test_gate.hpp`, 10,000 iterations: the worker pins v1 and waits, the reload commits v2, the worker resumes and its batch **must** carry `dbcVersion == 1`, and the next frame carries 2 (B-12, NFR-06, P-13)
- [ ] T052 [US8] Implement `modules/signal/src/reload_coordinator.hpp` and `.cpp`: debounce and rate limiting on `TimerScheduler`, parse/validate/compile entirely on a scheduler worker (never the decoder thread), `std::atomic_store` of the new `shared_ptr<const CompiledDbc>` as the single commit point, cache migration by semantic hash before the first frame decodes against the new snapshot, and the `afterReloadParse` / `beforeReloadCommit` / `afterPinSnapshot` hooks (V-10..V-13, R-14, R-15)
- [ ] T053 [US8] Wire `DecoderService::requestReload()` and `schema()` to the coordinator, emit `reload_requested`, `reload_committed` and `reload_rejected` with the fields of contracts/metrics.md section 3, and increment `dbcLoadAttemptsTotal` with the right label on each path

**Checkpoint**: reload is atomic under concurrent traffic, and a failed reload is a no-op.

---

## Phase 11: User Story 9 — The CLIs (Priority: P9)

**Goal**: two deterministic command-line tools with documented exit codes.

**Independent Test**: `ctest -L dbc-integration -R Cli_`.

- [ ] T054 [P] [US9] Write `tests/integration/dbc_decoder/cli_test.cpp`: `Cli_ValidateExitCodes` (0 for the valid fixture, 1 for each invalid fixture with the code and line on stdout in the contracts/dbc_subset.md section 5 format, 2 for a missing file and for bad usage), `Cli_DecodeOutputStable` (the quickstart's four `--hex` invocations produce the documented lines byte-for-byte, twice in a row, and `--json` output parses — NFR-09)
- [ ] T055 [US9] Implement `apps/dbc_validate/main.cpp` and `apps/dbc_decode/main.cpp` with their `CMakeLists.txt` files per contracts/dbc_api.md section 5: `dbc_validate` links `tcu_dbc` only and is portable; `dbc_decode` links `tcu_dbc` plus `tcu_can` for `CanMessage` and owns a small `candump_reader.hpp` for `--candump` (CLI only, not a transport — research R-22), with `Candump_ParsesLines` covering the `(1730793600.000000) vcan0 100#1027010050000000` form, an 8-hex-digit extended id, a zero-length payload and a malformed line that is skipped and counted. No wall clock, no unordered-map iteration in output, no locale-dependent formatting

**Checkpoint**: the decoder is usable from a shell with no C++ written by the user.

---

## Phase 12: User Story 10 — Event Bus, demo and documentation (Priority: P10)

**Goal**: the decoder's output reaches project 01's Event Bus, and the whole story is demonstrable
in one command.

**Independent Test**: `ctest -L dbc-integration -R EventBus_` plus a manual
`scripts/run_dbc_decoder_demo.sh`.

- [ ] T056 [P] [US10] Write `tests/integration/dbc_decoder/eventbus_test.cpp`: `EventBus_PublishesSignalsDecoded` (FR-18: an adapter observer republishes `SignalsDecoded`, `SignalQualityChanged`, `DbcReloaded` and `DbcReloadFailed`, and a subscriber receives them in order), `EventBus_PublishFailureCounted` (a full or rejecting bus increments `eventPublishFailuresTotal` and does not stall the worker), `EventBus_NoPublishUnderCacheLock` (S-07 at the integration level)
- [ ] T057 [US10] Implement `apps/virtual_dbc_demo/main.cpp` and its `CMakeLists.txt`: load `configs/decoder.json`, build the service over a `SocketCanTransport` with `--iface vcan0` or a scripted `FakeCanTransport` with `--fake`, install the Event Bus adapter observer, and run the nine-step sequence of quickstart section 3 (normal, wrong length, unknown id, unknown extended id, stale, recovery, rejected reload, committed reload, overload burst, throwing and blocking observers), printing the section 20.2 expected lines. The frames the kernel refuses on `vcan0` — bad length, RTR, ERR — are injected only in `--fake` mode, and the demo says so on screen rather than implying they went on the wire
- [ ] T058 [P] [US10] Create `tests/fixtures/dbc/capture_normal.log` in candump format for `dbc_decode --candump`, plus `tests/fixtures/dbc/invalid/overlap_reload.dbc` as the file the demo swaps in for the rejected reload
- [ ] T059 [P] [US10] Write `scripts/run_dbc_decoder_demo.sh` (configure, build, call project 05's `scripts/setup_vcan.sh` when `vcan0` is absent and the caller is root, fall back to `--fake` otherwise, run the demo, print what to look for per quickstart section 3, tear down what it created) and `modules/dbc/README.md` plus `docs/adr/006-dbc-signal-decoder.md` recording spec section 21's ADR-01..ADR-07 as accepted, each with the limitation it carries — including, in ADR-07's entry, that `vcan` is not HIL and that a successful decode is not evidence of a trustworthy sender (R-05, R-10), and an entry for R-24 recording why project 05's cache and decoder were not reused

**Checkpoint**: one command demonstrates every behaviour the spec promises, minus the `vcan`
transport.

---

## Phase 13: User Story 11 — Verification (Priority: P11)

**Goal**: the claims in the Definition of Done are backed by evidence rather than by assertion.

**Independent Test**: `ctest -L dbc-stress`, `scripts/run_dbc_fuzz.sh`, and the benchmark report.

- [ ] T060 [P] [US11] Write `scripts/generate_dbc_vectors.py`: given a `.dbc` fixture, a seed and a count, use `cantools` to emit `tests/fixtures/dbc/golden/*.json` records of `{message, signal, payload_hex, raw, signed_raw, physical, unit, dbc_hash}`, with the `cantools` version and the seed in each file's header, and a documented skip list for the intentional divergences of research R-20. Commit the generated files
- [ ] T061 [P] [US11] Write `tests/integration/dbc_decoder/golden_matrix_test.cpp`: `Golden_HandComputedVectors` (the 50+ hand-written vectors of spec section 7.3 and their siblings, as data in the test file) and `Golden_GeneratedMatrix` (every committed generated vector: `raw` compared as an **integer**, `physical` with an absolute/relative tolerance, never `==` on a double — section 15.5)
- [ ] T062 [P] [US11] Write `fuzz/dbc/fuzz_dbc_parser.cpp` (calls `loadDbcBytes` on the input, asserts only the invariant "either a database or at least one error diagnostic") and `fuzz/dbc/fuzz_frame_decoder.cpp` (derives `(busId, format, id, size, payload, rxTime)` from the input and calls `decode`, asserting no throw), with `fuzz/dbc/CMakeLists.txt` and a seed corpus copied from the valid and invalid fixtures into `tests/fixtures/dbc/corpus/`
- [ ] T063 [P] [US11] Write `scripts/run_dbc_fuzz.sh` running each target to 1,000,000 executions or a `--max-total-time` box, with `-fsanitize=fuzzer,address,undefined`, printing the build and corpus used so the NFR-05 evidence is reproducible
- [ ] T064 [P] [US11] Write `tests/stress/dbc_decoder/soak_test.cpp`: `Soak_HundredThousandFramesBounded` (100,000 frames through the service: queue depth never above capacity, drop counters monotonic, no growth in cache entry count, bounded time — NFR-02), `Soak_MetricConservation` (asserts M-01..M-05 at a quiescent point), `Soak_ReloadUnderTraffic` (alternating valid and invalid reloads while frames flow; version only ever increases, no batch mixes versions), all honouring `TCU_DBC_STRESS_SECONDS`
- [ ] T065 [P] [US11] Write `benchmarks/dbc_decoder_bench/main.cpp` and its `CMakeLists.txt`: pure-decode p99/p50/max per message kind, load time for the lab DBC and for a generated 2,000-message file, and ingress-to-publish latency, printing CPU model, compiler, build type and iteration count. Reported, never asserted (research R-07)
- [ ] T066 [US11] Run both sanitizer builds over all four labels on Linux/WSL and fix every finding: `-fsanitize=address,undefined` (GCC and clang) and `-fsanitize=thread` (clang with `TCU_SANITIZER_SETARCH=ON`), never combined. Record the exact toolchain versions used in the quickstart's sanitizer note (NFR-01)

**Checkpoint**: every NFR has evidence, and the race recipes have run 10,000 iterations each under
TSan.

---

## Phase 14: Polish & Cross-Cutting Concerns

- [ ] T067 [P] Fill in the spec section 23.1 traceability table in `specs/006-dbc-signal-decoder/` with the actual test ids, layers and evidence paths — every P0 what-if of spec section 14 mapped to an executable test (DBC-021)
- [ ] T068 [P] Re-read contracts/metrics.md section 3 against the implementation and confirm every log event exists with exactly its listed fields, no payload byte appears in any line, and no DBC-supplied name became a metric label (Principle V, section 16.1 cardinality rule)
- [ ] T069 Run `quickstart.md` end to end on a clean clone, on Linux and on Windows, and correct any command that does not work as written
- [ ] T070 Add the "Post-implementation reconciliation" section to `plan.md` in project 04's style: what the sanitizers found, where test-first held and where it did not, whether `Vcan_EndToEnd` actually ran or skipped on this machine, and the honest status of each Definition of Done item — plus any shortcut taken during implementation in `shortcut:` form, and the standing R-24 cost that two signal caches now coexist

---

## Dependencies & Execution Order

### Phase dependencies

- **Setup (Phase 1)**: no dependencies
- **Foundational (Phase 2)**: needs Setup; blocks every story
- **US1 (Phase 3)**: needs Foundational
- **US2 (Phase 4)**: needs US1's `BitPlan` and `CompiledDbc`
- **US3 (Phase 5)**: needs US2's extraction
- **US4 (Phase 6)**: needs US2 and US3 (a batch is what the cache commits)
- **US5 (Phase 7)**: needs US4's cache and epoch
- **US6 (Phase 8)**: needs Foundational only — **can be built in parallel with US1-US5** by a second developer
- **US7 (Phase 9)**: needs US4, US5, US6
- **US8 (Phase 10)**: needs US7's service
- **US9 (Phase 11)**: needs US1 and US2 — can start once Phase 4 is done
- **US10 (Phase 12)**: needs US7 and US8
- **US11 (Phase 13)**: needs everything it measures; T060-T061 can start after US3
- **Polish (Phase 14)**: last

### Within each story

- Tests are written first and must FAIL before the implementation task runs
- Types before parsers, parsers before validators, validators before compilers
- The pure library (`tcu_dbc`) before the lifecycle (`tcu_signal`)
- No story is closed while a test it introduced is skipped or disabled

### Parallel opportunities

- T004-T010 all `[P]`: separate files
- T011, T012, T014, T015, T016 all `[P]`
- Every test-writing task inside one phase is `[P]` against the others
- US6 (Phase 8) is genuinely independent of US1-US5 and is the one real two-developer split
- T060-T065 are five independent files

---

## Implementation Strategy

### MVP (US1 only)

1. Phase 1 Setup
2. Phase 2 Foundational
3. Phase 3 US1
4. **STOP and VALIDATE**: a DBC file loads into an immutable hashed database, and all 21 invalid
   fixtures are rejected with located codes. That alone is a useful tool once T055 wraps it in
   `dbc_validate`.

### Incremental delivery

1. Setup + Foundational -> the vocabulary compiles
2. US1 -> strict validation (MVP)
3. US2 + US3 -> a complete pure decoder; `dbc_decode` becomes possible
4. US4 + US5 -> trustworthy data quality
5. US6 + US7 -> a running service
6. US8 -> hot reload
7. US9 + US10 -> tools and demo
8. US11 -> evidence

### Notes

- `[P]` tasks touch different files and have no dependency on an incomplete task
- Commit after each task or logical group, with the ticket id (DBC-0xx) in the message
- Stop at any checkpoint and validate before moving on
- When a test cannot be written before the code, say so in T070 rather than quietly reordering
