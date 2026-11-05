---

description: "Task list for Configuration & Logging Service (project 04)"
---

# Tasks: Configuration & Logging Service

**Input**: Design documents from `/specs/004-configuration-logging-service/`

**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/, quickstart.md — all present.

**Tests**: REQUIRED, not optional. Constitution Principle III is non-negotiable: every spec goal gets
a unit test naming it, every behavioural contract BC-01..BC-30 gets a test, and the test is written
and seen to FAIL before the implementation that satisfies it.

**Platform**: every command here runs on **both** Linux/WSL and Windows (research R-00) — both
libraries use the standard library plus one header-only JSON dependency, and no POSIX call. Sanitizer
runs are Linux/WSL only.

**Organization**: phases follow the ticket dependency chain of spec section 20, re-ordered so the
logger exists before the configuration service that logs through it:
`LOG-001 -> LOG-002 -> LOG-005 -> LOG-003 -> LOG-004 -> CFG-001..004 -> CFG-005 -> CFG-006 -> CFG-007 -> CFG-008 -> CFG-010 -> CFG-009`.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: can run in parallel — different files, no dependency on an incomplete task
- **[Story]**: US1..US9, mapped to the backlog groups below
- Every description carries its exact file path

## Story-to-ticket mapping

| Story | Delivers | Backlog tickets (spec section 20) | Contracts covered |
|---|---|---|---|
| US1 (P1) | A bounded async logger: filter, redact, enqueue, format, console sink, lifecycle | TCU-LOG-001, 002, 004, 005 | BC-16..19, BC-23, BC-26, BC-29, BC-30; L1-L9, Q1-Q5 |
| US2 (P2) | Rotating file sink with health, fallback and bounded recovery | TCU-LOG-003 | BC-20, BC-21, BC-27; F1-F6 |
| US3 (P3) | Load, validate and publish generation 1 from one JSON file | TCU-CFG-001..004 | BC-01, BC-02, BC-14, BC-15, BC-28; C1-C13, S1, S2, S7 |
| US4 (P4) | The reload pipeline: hot commit, no-op, restart-only rejection | TCU-CFG-005 | BC-03..BC-09; S3, S8-S10 |
| US5 (P5) | Subscriber registry, ordered async notification, overflow resync | TCU-CFG-006 | BC-11, BC-12, BC-25; N1-N6, S4, S6 |
| US6 (P6) | Change detection by bounded poll, with debounce for free | TCU-CFG-007 | BC-13 |
| US7 (P7) | The full metric set, the closed log vocabulary and the health line | TCU-CFG-008 (observability half) | M1-M9 |
| US8 (P8) | The demo application, the Event Bus integration and the demo script | TCU-CFG-008 (demo half), TCU-CFG-010 | spec scenarios A-F |
| US9 (P9) | Stress, fault injection, sanitizers, soak and the performance report | TCU-CFG-009 | BC-10, BC-22, BC-24 |

**Deviation from the template, stated deliberately**: these nine stories are **layers, not
independent features**. The template's "all stories can run in parallel once Foundational is done"
does not apply: US3 cannot log without US1, US4's diff needs US3's snapshot, US5's notifier needs
US4's commit point, US6 triggers US4, and US7's conservation equations need counters US1-US6 own.
Each story's checkpoint is a working increment, but the order is fixed. Parallelism exists *within*
phases, marked `[P]`.

**Metrics note**: the atomic counters for both libraries are declared in Foundational (T019) and each
story increments the ones it owns, because US1's `Queue_DropsInfoWhenFull` asserts
`recordsDroppedTotal` and cannot wait for US7. US7 completes the set, adds the structured log lines
and asserts M1-M9. This splits ticket TCU-CFG-008 across the stories; intentional, not an oversight.

**Dependency direction note**: `tcu_logging` links **nothing** — not even project 03, whose
`Histogram` it deliberately re-declares rather than include (contracts/metrics.md section 2). Any
project from 01 to 30 must be able to link the logger without inheriting a timer library.

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: directory structure and build wiring, so a test can be compiled before any library code
exists

- [X] T001 Create the `modules/logging/` tree — `include/tcu/logging/`, `src/` — and write `modules/logging/CMakeLists.txt` declaring a static library target `tcu_logging` with `target_include_directories(tcu_logging PUBLIC include PRIVATE src)`, `target_compile_features(... PUBLIC cxx_std_17)`, `-Wall -Wextra` on GCC/Clang, `find_package(Threads REQUIRED)` linked `PUBLIC`, and **no other `target_link_libraries` entry** (dependency-direction note above)
- [X] T002 Create the `modules/config/` tree — `include/tcu/config/`, `src/` — and write `modules/config/CMakeLists.txt` declaring static library `tcu_config` with the same include/feature/warning setup, linking `PUBLIC tcu_logging tcu_scheduler` (both appear in its public headers) and `PRIVATE nlohmann_json::nlohmann_json` (wrapped away by `src/json_document.hpp`, research R-01)
- [X] T003 Extend the root `CMakeLists.txt`: declare the `nlohmann_json` `FetchContent` pin at exact tag `v3.11.3` with `JSON_BuildTests OFF` **unconditionally and before** the `TCU_BUILD_TESTS` block (the library needs it even with tests off), then add the project 04 block **outside** the existing `if(CMAKE_SYSTEM_NAME STREQUAL "Linux")` guard — `add_subdirectory(modules/logging)`, `add_subdirectory(modules/config)`, `add_subdirectory(apps/config_logging_demo)` — and inside the existing `TCU_BUILD_TESTS` block the four test directories. Do not restructure the project 01, 02 or 03 lines
- [X] T004 [P] Create `tests/unit/logging/CMakeLists.txt` building one `logging_unit_tests` target linked against `tcu_logging` and `gtest_main`, with `target_include_directories(... PRIVATE ${CMAKE_SOURCE_DIR}/modules/logging/src ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tests/support)` so private headers are testable, registered with `gtest_discover_tests(... PROPERTIES LABELS "logging-unit")`
- [X] T005 [P] Create `tests/unit/config/CMakeLists.txt` building `config_unit_tests` the same way against `tcu_config` plus `${CMAKE_SOURCE_DIR}/modules/config/src`, label `config-unit`
- [X] T006 [P] Create `tests/integration/config_logging/CMakeLists.txt` building `config_logging_integration_tests`, linking `tcu_config`, `tcu_logging`, `tcu_scheduler`, `tcu_eventbus` and `gtest_main`, label `config-logging-integration`
- [X] T007 [P] Create `tests/stress/config_logging/CMakeLists.txt` building `config_logging_stress_tests`, label `config-logging-stress`
- [X] T008 [P] Create the four fixture files of contracts/config_schema.md section 2 — `configs/tcu.json` (the vetted baseline: `schema_version` 1, `retry_base_ms` 1000, `retry_max_ms` 30000, `max_attempts` 0, `period_ms` 5000, `offline_queue_capacity` 2048, `level` "INFO", `queue_capacity` 8192, `file_max_bytes` 10485760, `file_count` 5, `console_enabled` true, `cloud_endpoint` "https://mock-cloud.local", `tls_required` true), `configs/invalid_retry.json`, `configs/invalid_schema.json`, `configs/restart_only.json` — each differing from the baseline in exactly the one way that document records. **No credential, token or key in any of them**
- [X] T009 [P] Create `tests/unit/logging/support/fake_clock.hpp` (a callable returning a settable `system_clock::time_point`, so formatter output is byte-exact — research R-17) and `tests/unit/logging/support/recording_sink.hpp` (an `ILogSink` that appends every line to a vector, reports a configurable `SinkHealth`, and can be told to fail the Nth write)
- [X] T010 [P] Create `tests/unit/logging/support/faulty_file_ops.hpp` (a `FileOps` implementation that injects `ENOSPC`, `EACCES`, a short write or `EINTR` on the Nth call — research R-14) and `tests/unit/config/support/temp_config_dir.hpp` (a fixture creating a unique directory under `std::filesystem::temp_directory_path()`, a `writeAtomic(json)` helper that writes a temp file and renames it, and teardown that removes the directory). Reuse `tests/support/test_gate.hpp` as-is for every rendezvous — check its interface before writing anything new (Principle II)

**Checkpoint**: `cmake -S . -B build -G Ninja && cmake --build build` configures and builds projects
01-03 plus four empty project 04 test targets, on Linux and on Windows.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: the public vocabulary of both libraries and the counter storage every story needs

- [X] T011 Create `modules/logging/include/tcu/logging/log_types.hpp` with exactly the declarations of contracts/logging_api.md section 1: `enum class Severity { Trace, Debug, Info, Warn, Error, Fatal }`, `enum class DataClass { Public, VehicleSensitive, Secret }`, `struct Field { std::string key; std::variant<std::int64_t, double, bool, std::string> value; DataClass cls{DataClass::Public}; }`, and `struct LogRecord` with the eight fields of data-model.md section 4.1 in that order. Add `constexpr std::size_t kSeverityCount = 6` and a `severityName(Severity)` returning the upper-case name
- [X] T012 [P] Create `modules/logging/include/tcu/logging/logger.hpp` declaring `class ILogger` with the two `log(...) noexcept` overloads, `bool enabled(Severity) const noexcept`, `void flush()` and `void stop()`, each carrying the one-line contract from contracts/logging_api.md section 2 as a comment
- [X] T013 [P] Create `modules/logging/include/tcu/logging/log_sink.hpp` declaring `enum class SinkHealth { Healthy, Unhealthy }`, `constexpr std::size_t kSinkCount = 2`, and `class ILogSink` with `name()` (**closed set: "console", "file"**), `bool write(std::string_view line)` (**one complete JSONL line, no trailing newline; returns false instead of throwing**), `flush()` and `health()`
- [X] T014 [P] Create `modules/logging/include/tcu/logging/logger_metrics.hpp` declaring its **own** `struct Histogram` (nine buckets with upper bounds `{1, 5, 10, 25, 50, 100, 250, 1000, +inf}` ms, `count`, `sum_ms`) and `struct LoggerMetrics` holding the plain-value fields of contracts/metrics.md section 2 in that order. Do not include any `tcu/scheduler/` header here
- [X] T015 Create `modules/config/include/tcu/config/config_types.hpp` with `enum class LoadError` holding exactly the ten values of contracts/config_api.md section 1 (`None, FileNotFound, ReadError, TooLarge, ParseError, SchemaError, InvalidValue, RestartRequired, UnsupportedVersion, ServiceStopped`) plus `constexpr std::size_t kLoadErrorCount`, `loadErrorName(LoadError)`, `struct ReloadResult`, the four sub-config structs and `struct ConfigSnapshot` of data-model.md section 1.1 (**integer fields are `std::int64_t`**, so ranges are checked before any narrowing), `bool contentEquals(const ConfigSnapshot&) const` (**compares every field except `generation`, `source` and `loadedAt`** — C5), `using ConfigPtr`, `using SubscriptionId` (**0 is never issued**) and `using ConfigObserver`
- [X] T016 Create `modules/config/include/tcu/config/config_schema.hpp` with `enum class FieldType { Int, Bool, String, Enum }`, `enum class ReloadPolicy { Hot, Restart }`, `struct FieldSpec { std::string_view pointer; FieldType type; std::int64_t min; std::int64_t max; ReloadPolicy policy; }` and `inline constexpr std::array<FieldSpec, 13> kFieldSpecs` holding the 13 rows of contracts/config_schema.md section 1 verbatim, in file order, with the exact bounds: `/schema_version` 1..1 Restart; `/connectivity/retry_base_ms` 100..60000 Hot; `/connectivity/retry_max_ms` 100..300000 Hot; `/connectivity/max_attempts` 0..100 Hot; `/telemetry/period_ms` 200..3600000 Hot; `/telemetry/offline_queue_capacity` 16..100000 Restart; `/logging/level` Enum Hot; `/logging/queue_capacity` 128..65536 Restart; `/logging/file_max_bytes` 1048576..104857600 Restart; `/logging/file_count` 1..20 Restart; `/logging/console_enabled` Bool Hot; `/network/cloud_endpoint` String Restart; `/network/tls_required` Bool Restart
- [X] T017 [P] Write `tests/unit/config/schema_test.cpp` and see it FAIL: `Schema_EveryFieldHasReloadPolicy` (C7), `Schema_PointersAreUnique` (C8), `Schema_SampleFileMatchesTable` (reads `configs/tcu.json` and asserts the pointer sets are equal in both directions — C9), `Schema_IntRowsHaveMinLessThanMax`
- [X] T018 [P] Create `modules/config/include/tcu/config/config_metrics.hpp` declaring `struct ConfigMetrics` with the gauges and counters of contracts/metrics.md section 1, the failure counters as `std::array<std::uint64_t, kLoadErrorCount>`
- [X] T019 Create `modules/logging/src/metrics.hpp` and `modules/config/src/metrics.hpp`: internal structs of `std::atomic<std::uint64_t>` matching every field of their public snapshot struct, plus `recordWriteLatency(duration)` on the logging one (clamping at 0) and a `snapshot()` returning the public struct by value. Failure reasons are an array indexed by enum, never a map (M4)
- [X] T020 Create `modules/config/include/tcu/config/configuration_service.hpp` declaring `struct ServiceOptions` (`path`, `maxFileBytes{1 MiB}`, `pollInterval{500ms}` — **0 disables polling**), `class IConfigurationService` with the four pure virtuals of contracts/config_api.md section 1, and `class ConfigurationService final : public IConfigurationService` with the `(ServiceOptions, TimerScheduler&, ILogger&)` constructor, `start()`, `stop()`, `metrics()`, deleted copy, `struct Impl; std::unique_ptr<Impl> impl_;`, and a comment stating the **`workerThreads == 1` requirement** on the scheduler it is handed (R-02)

**Checkpoint**: both libraries compile as empty shells; `schema_test.cpp` fails for the right reason
(no implementation yet), not a compile error.

---

## Phase 3: User Story 1 — Bounded async logger (Priority: P1) 🎯 MVP

**Goal**: a process can log structured records at a severity, non-public fields never reach a sink,
the queue is bounded with a severity-aware overload policy, and shutdown drains or reports.

**Independent Test**: `ctest -L logging-unit` passes with a `RecordingSink` only — no file, no
config, no scheduler. Spec goals 6, 7 and 8 are satisfiable on the console path alone.

### Tests for User Story 1 (write first, see them FAIL)

- [X] T021 [P] [US1] Write `tests/unit/logging/formatter_test.cpp`: `Formatter_EmitsWellFormedJsonl` (golden line with `fake_clock` fixed to `2026-10-30T01:00:00.000Z`), `Formatter_KeyOrderIsFixed` (`timestamp_utc, severity, component, event, correlation_id, message, fields`), `Formatter_EscapesControlCharacters` (quote, backslash, `\n`, `\t`, 0x01 — **no raw newline may appear, so one record is always one line**, L3), `Formatter_FieldTypesRoundTrip` (int unquoted, double unquoted, bool bare, string quoted — L5), `Formatter_UnicodeIsPreserved`
- [X] T022 [P] [US1] Write `tests/unit/logging/redaction_test.cpp`: `Redaction_SecretAbsentFromEverySink` (BC-19 — assert the secret literal appears in no recorded line **and** in no metric or error string), `Redaction_AppliedAtIngestionAndFormatting` (assert both stages independently: a record built by hand with a non-public plaintext field is still redacted by the formatter), `Redaction_VehicleSensitiveIsRedactedToo` (R-11), `Redaction_PublicFieldsPassThrough`
- [X] T023 [P] [US1] Write `tests/unit/logging/queue_test.cpp`: `Queue_DepthNeverExceedsCapacity` (Q1), `Queue_DropsInfoWhenFull` (BC-17 — non-blocking, counter rises, **the incoming record is dropped, not the oldest queued one**), `Queue_BoundedWaitForError` (BC-18 — returns within `highPriorityWaitMs` plus slack, `highPriorityDropsTotal` rises, **never an indefinite stall**), `Record_OversizeTruncatedAndCounted` (BC-29 — line `<= maxRecordBytes`, still valid JSON, `"truncated":true`, message truncated before fields — R-12), `Queue_RecordOwnsItsStrings` (L1 — pass a `string_view` into a buffer that is then overwritten before the writer runs)
- [X] T024 [P] [US1] Write `tests/unit/logging/filter_test.cpp`: `Logger_FilterBelowLevelNeverFormats` (BC-16 — a sink that fails the test if called, plus `recordsFilteredTotal`), `Logger_EnabledMatchesLevel`, `Logger_SetLevelAppliesImmediately` (R-15, hot), `Logger_FatalAlwaysPasses`
- [X] T025 [P] [US1] Write `tests/unit/logging/lifecycle_test.cpp`: `Logger_StopIsIdempotent` (BC-30, L6 — no double join), `Logger_RejectsAfterStop` (Q5 — counted no-op, no throw), `Logger_ReentrantLogFromSinkDoesNotDeadlock` (BC-26, Q4 — a sink that logs; assert `reentrantEmergencyTotal` rises and the test terminates), `Logger_FlushIsNoOpFromWriterThread` (no self-deadlock), `Logger_FormatFailureIsCountedNotThrown` (L9), `Logger_StopFromSinkCallbackDoesNotSelfJoin`

### Implementation for User Story 1

- [X] T026 [US1] Create `modules/logging/src/redaction.hpp`: one function replacing a non-`Public` field's value with the literal `[REDACTED]`, applied to a whole `std::vector<Field>` in place; used at ingestion and again by the formatter (R-11)
- [X] T027 [US1] Create `modules/logging/src/json_formatter.hpp` and `src/json_formatter.cpp`: `LogRecord -> std::string`, hand-written escaping (no JSON dependency in `tcu_logging`), ISO-8601 UTC with milliseconds, the fixed key order of T021, the truncation order of R-12 with `"truncated":true` appended at top level, and `formatFailuresTotal` on any internal failure. Make T021 and T022 pass
- [X] T028 [US1] Create `modules/logging/src/log_queue.hpp` and `src/log_queue.cpp`: a `std::deque<LogRecord>` behind one mutex with capacity `queueCapacity`, one condition variable for the writer and one for waiting producers, `bool admit(LogRecord&&, Severity)` implementing the severity policy of contracts/logging_api.md section 2, `std::deque<LogRecord> drainAll()` that **swaps the whole deque under the lock and formats outside it** (research R-08), and a `shortcut:` comment recording that this is a mutex queue, upgradeable to lock-free only after a profile. Make T023 pass
- [X] T029 [P] [US1] Create `modules/logging/include/tcu/logging/console_sink.hpp` and `src/console_sink.cpp`: writes the same JSONL to stdout (R-18), honours an atomic `enabled` flag, always reports `Healthy`
- [X] T030 [US1] Create `modules/logging/include/tcu/logging/async_logger.hpp`: `struct LoggerOptions` with the eight fields and defaults of contracts/logging_api.md section 1, and `class AsyncLogger final : public ILogger` with `(LoggerOptions, std::vector<std::shared_ptr<ILogSink>>)`, `start()`, `setLevel(Severity)`, `setConsoleEnabled(bool)`, `metrics()`, deleted copy, pimpl
- [X] T031 [US1] Create `modules/logging/src/async_logger.cpp`: the five-step `log()` order of contracts/logging_api.md section 2 (level check first, then the `thread_local` reentrancy guard, then owning copies, then redaction, then admission), the writer thread loop with a **timed** wait, the pre-opened stderr emergency path for FATAL and for reentrant calls, `flush()` with a generation counter so it returns only when records queued at call time have been handed to the sinks, and `stop()` draining within `shutdownDeadline`, flushing, closing and joining — idempotent, never self-joining. Make T024 and T025 pass
- [X] T032 [US1] Run `ctest --test-dir build -L logging-unit --output-on-failure` and confirm every T021-T025 case passes; then run the ASan build of the same label

**Checkpoint**: `tcu_logging` is usable by any project with a console sink. BC-16..19, BC-23 (partly),
BC-26, BC-29, BC-30 are green.

---

## Phase 4: User Story 2 — Rotating file sink with health and recovery (Priority: P2)

**Goal**: records reach a size-rotated file with owner-only permissions, and a disk or permission
failure degrades the sink instead of the application.

**Independent Test**: `ctest -L logging-unit -R FileSink` passes with `FaultyFileOps`; no real disk is
filled and no real permission is changed.

### Tests for User Story 2 (write first, see them FAIL)

- [X] T033 [P] [US2] Write `tests/unit/logging/file_sink_test.cpp`: `FileSink_WritesAndRotates` (BC-20, F2 — **rotation happens before the write that would exceed `fileMaxBytes`**, so the active file never exceeds it), `FileSink_RetainsAtMostFileCount` (F3 — `tcu.log` plus `.1`..`.N`, oldest removed), `FileSink_PermissionsAreOwnerOnly` (F4 — active and rotated files; parent directory created when missing), `FileSink_RefusesSymlinkPath` (R-13), `FileSink_DiskFullMarksUnhealthyAppSurvives` (BC-21, F1 — injected `ENOSPC`; `sinkErrorsTotal` rises, `log()` still `noexcept`, the producer is unaffected), `FileSink_UnwritablePathFallsBackToStderr` (BC-27, F5 — injected `EACCES` at open; the logger still starts), `FileSink_ShortWriteIsAFailureNotAPartialLine`, `FileSink_EintrIsRetriedThenFails`, `FileSink_RecoveryProbeIsRateLimited` (F6 — at most one probe per `recoveryProbeInterval`, asserted by counting `FileOps::open` calls, **no spin**)
- [X] T034 [P] [US2] Write `tests/unit/logging/console_sink_test.cpp`: `ConsoleSink_DisabledEmitsNothing`, `ConsoleSink_HotToggleTakesEffectWithoutRestart` (R-15)

### Implementation for User Story 2

- [X] T035 [US2] Create `modules/logging/include/tcu/logging/file_sink.hpp`: `struct FileSinkOptions` (`path`, `fileMaxBytes`, `fileCount`, `fallbackToStderr{true}`), the three-function `FileOps` seam (`open`, `write`, `rename`) with a default implementation using `std::ofstream` and `std::filesystem`, and `class FileSink final : public ILogSink`
- [X] T036 [US2] Create `modules/logging/src/file_sink.cpp`: open with the symlink refusal and `std::filesystem::permissions(..., owner_read | owner_write, perm_options::replace)`, size-tracked writes, the rename chain from oldest to newest with `rotationsTotal`, the `Healthy`/`Unhealthy` transition with `nextProbeAt`, and the stderr fallback. Add a `shortcut:` comment for size-only rotation (no time retention, no compression) and for the rename running on the writer thread. Make T033 pass
- [X] T037 [US2] Wire recovery into `async_logger.cpp`: the writer's timed wait becomes `min(idle wait, time until the earliest sink's next probe)`, so probing costs no thread (R-14); assert with `FileSink_RecoveryProbeIsRateLimited`

**Checkpoint**: the logger is feature-complete per spec section 10. Spec goals 6, 7 and 8 are met;
tickets TCU-LOG-001..005 closed.

---

## Phase 5: User Story 3 — Load, validate, publish generation 1 (Priority: P3)

**Goal**: a valid file becomes one immutable typed snapshot at generation 1; an invalid one stops
startup with a redacted reason and no implicit defaults.

**Independent Test**: `ctest -L config-unit -R 'Loader_|Validator_|Snapshot_|Lifecycle_'` passes using
`temp_config_dir` fixtures and a `RecordingSink` logger; no reload, no observer, no poller.

### Tests for User Story 3 (write first, see them FAIL)

- [X] T038 [P] [US3] Write `tests/unit/config/loader_test.cpp`: `Loader_ParsesSampleConfig`, `Loader_RejectsOversizeBeforeParse` (BC-15 — **the size is probed before any read**, so assert the parser was never entered), `Loader_RejectsDuplicateKey` (R-01), `Loader_RejectsDepthOverEight`, `Loader_RejectsTruncatedFile` (BC-14), `Loader_RejectsInvalidUtf8`, `Loader_RejectsTrailingDataAndComments`, `Loader_RejectsUnsupportedSchemaVersion` (BC-28), `Loader_MissingFileIsFileNotFound`
- [X] T039 [P] [US3] Write `tests/unit/config/validator_test.cpp`: `Validator_RejectsWrongType` (including `5.0` where an int is required), `Validator_RejectsOutOfRange` (BC-04, one case per numeric row, both bounds), `Validator_RejectsCrossFieldRetryOrder` (BC-05 — `retry_max_ms < retry_base_ms`), `Validator_RejectsTlsFalseForNonlocalEndpoint`, `Validator_RejectsBadEndpointScheme`, `Validator_RejectsUnknownKey` (R-05), `Validator_RejectsMissingRequiredKey` (BC-02 — **every one of the 13 keys, each omitted in turn; none may be silently defaulted**), `Validator_AcceptsLevelEnumNamesOnly`, `Redaction_ReasonStringsNeverQuoteInput` (R-10 — put a recognisable literal in every field of a bad file and assert it appears in no `publicReason`)
- [X] T040 [P] [US3] Write `tests/unit/config/snapshot_test.cpp`: `Lifecycle_StartPublishesGenerationOne` (BC-01), `Lifecycle_MissingRequiredFieldFailsStartup` (BC-02, S1 — state `Failed`, `snapshot() == nullptr`), `Snapshot_TypedFieldsMatchFile`, `Snapshot_ContentEqualsIgnoresProvenance` (C5), `Snapshot_ContentEqualsIgnoresWhitespaceAndKeyOrder` (C3 — two files, same semantics, different formatting), `Snapshot_HeldPointerStaysValidAfterReload` (C1), `Snapshot_ReadDuringReloadDoesNotBlock` (S7), `Service_StopIsIdempotent` (BC-30, S6), `Service_ReloadAfterStop` (S5)

### Implementation for User Story 3

- [X] T041 [US3] Create `modules/config/src/json_document.hpp` and `src/json_document.cpp`: the nlohmann wrapper that keeps `nlohmann::json` out of every public header, a `parse(std::string_view)` returning a result type, using `json::parse` with a `parser_callback_t` that rejects a duplicate key inside one object (one key set per open object) and nesting deeper than 8, plus typed pointer accessors `getInt/getBool/getString(pointer)` and `collectPointers()` for the unknown-key check. **Never forward the library's exception text** (R-10); report a byte offset instead. Add the `shortcut:` comment for the per-object key set
- [X] T042 [US3] Create `modules/config/src/loader.hpp` and `src/loader.cpp`: stages 1-4 of data-model.md section 2.2 — `probeSize` (`> maxFileBytes` -> `TooLarge`), `readAll` into a string sized by the probe, `parse`, `checkVersion` (`schema_version != 1` -> `UnsupportedVersion`). Make T038 pass
- [X] T043 [US3] Create `modules/config/src/validator.hpp` and `src/validator.cpp`: the `kFieldSpecs` walk (presence, type, range), the unknown-key check against `collectPointers()`, the three cross-field rules of data-model.md section 2.3, and the closed reason-template set of contracts/config_schema.md section 4 as the **only** way a `publicReason` is built. Make T039 pass
- [X] T044 [US3] Create `modules/config/src/configuration_service.cpp` part 1: `Impl` with the state enum of data-model.md section 2.1, `start()` running stages 1-5 then publishing with a single `std::atomic_store(&current_, candidate)` (S2, AS-03), `snapshot()` as one `std::atomic_load` (`noexcept`, non-blocking), `stop()` idempotent, `metrics()`, and the structured startup log lines `startup_loaded` / `startup_failed` through the injected `ILogger`
- [X] T045 [US3] Run `ctest -L config-unit --output-on-failure`; confirm T017 and T038-T040 all pass

**Checkpoint**: a process can start on a validated config and read it from any thread. Tickets
TCU-CFG-001..004 closed.

---

## Phase 6: User Story 4 — Reload pipeline (Priority: P4)

**Goal**: a reload commits a hot change atomically, reports a no-op without touching the generation,
and rejects the whole candidate when any restart-only key moved.

**Independent Test**: `ctest -L config-unit -R Reload_` passes with explicit `reload()` calls only —
no poller, no observers.

### Tests for User Story 4 (write first, see them FAIL)

- [X] T046 [P] [US4] Write `tests/unit/config/reload_test.cpp`: `Reload_InvalidJsonKeepsSnapshot` (BC-03), `Reload_OutOfRangeKeepsSnapshot` (BC-04), `Reload_InvalidKeepsGeneration` (C11, S8 — **one case per `LoadError` value**, each asserting the generation and every field are unchanged), `Reload_NoOpDoesNotBumpGenerationOrNotify` (BC-06, S9 — `changed == false`, `error == None`), `Reload_HotChangeCommitsAndNotifies` (BC-07, C12 — generation +1 exactly), `Reload_RestartOnlyKeyRejectsWholeCandidate` (BC-08 — a file changing `queue_capacity` **and** `level`; assert `RestartRequired` **and** that the level did not move), `Reload_ReasonNamesTheFirstRestartOnlyPointer`
- [X] T047 [P] [US4] Write `tests/integration/config_logging/serialized_reload_test.cpp`: `Reload_ConcurrentCallsAreSerialized` (BC-09, S3 — 8 threads calling `reload()` against alternating valid files; assert generations are gap-free, strictly increasing, and no snapshot mixes fields from two files)

### Implementation for User Story 4

- [X] T048 [US4] Create `modules/config/src/snapshot_diff.hpp`: given two snapshots, return the set of changed `kFieldSpecs` rows and whether any has `ReloadPolicy::Restart`. Comparison is on the typed struct, never the JSON text (C3)
- [X] T049 [US4] Extend `configuration_service.cpp` with `reload()`: the reload mutex, stages 1-7 in order with the diff before the publish, `ReloadResult` filled per the table in contracts/config_api.md section 3, the `reload_committed` / `reload_noop` / `reload_rejected` log lines with `fields.reason` and `fields.field`, and the attempt/success/failure counters. Make T046 and T047 pass

**Checkpoint**: last-known-good is guaranteed on every failure path. Ticket TCU-CFG-005 closed.

---

## Phase 7: User Story 5 — Subscriber registry and ordered notification (Priority: P5)

**Goal**: observers learn about a committed generation asynchronously, in order, with their failures
isolated, and an overflow degrades to a resync instead of losing the fact of the change.

**Independent Test**: `ctest -L config-unit -R 'Notify_|Lock_'` passes with a real `TimerScheduler`
configured `workerThreads = 1` and a tiny `maxReadyTasks` to force overflow.

### Tests for User Story 5 (write first, see them FAIL)

- [X] T050 [P] [US5] Write `tests/unit/config/subscriber_test.cpp`: `Notify_RunsAfterCommit` (S10 — the observer reads `snapshot()` and sees at least its own generation), `Notify_OrderedAcrossGenerations` (N1), `Notify_ObserverThrowIsIsolatedAndCounted` (BC-11, N2 — three observers, the middle one throws; the third still runs, `notifyFailuresTotal` rises, an `observer_failed` line carries `fields.subscription_id`), `Notify_UnsubscribeStopsFutureCallbacks` (BC-12, N3), `Notify_UnsubscribeFromInsideCallbackIsAllowed`, `Notify_PreviousIsNullOnFirstAndResync` (N5), `Notify_OverflowMarksDegradedAndResyncs` (BC-25, N4 — a blocked observer plus a full ready queue; assert `notifyDegraded` latches, `resyncNotificationsTotal` rises, the commit is **not** rolled back and `reload()` still reported success), `Notify_NewSubscriberIsNotCalledForTheCurrentGeneration`
- [X] T051 [P] [US5] Write `tests/unit/config/lock_test.cpp`: `Lock_ObserverCanReadSnapshotAndLog` (S4, section 14 — an observer that calls `snapshot()`, `subscribe()`, `unsubscribe()` and `logger.log()`; a nested lock would deadlock this test), `Lock_ReloadDuringNotificationDoesNotDeadlock`

### Implementation for User Story 5

- [X] T052 [US5] Extend `configuration_service.cpp` with the registry: `std::vector<Entry>` of `{id, observer, alive}` under its own mutex, `subscribe()` issuing a strictly increasing non-zero id, `unsubscribe()` with the "no new callback starts" guarantee, and a notification task that **copies the live entries under the lock and calls them with it released** (S4)
- [X] T053 [US5] Implement delivery through the injected scheduler: one `scheduleAfter(0ms, ...)` per published generation, and on `Error::CapacityExceeded` the degrade-count-resync path of R-06 with a `notify_overflow` log line and `previous == nullptr` on the next accepted notification
- [X] T054 [US5] Complete `stop()`: cancel the notifier work it owns, wait for an in-flight reload, clear the registry so no captured observer outlives the service, and stay idempotent and callable from inside a callback (S6, BC-30). Make T050 and T051 pass

**Checkpoint**: the full section 9 pipeline works end to end on explicit reloads. Ticket TCU-CFG-006
closed.

---

## Phase 8: User Story 6 — Change detection (Priority: P6)

**Goal**: editing the file with temp-write plus rename causes a validated reload within one poll
interval, and a burst of writes causes at most one.

**Independent Test**: `ctest -L config-unit -R Poller_` passes using `temp_config_dir::writeAtomic`
and a short poll interval.

### Tests for User Story 6 (write first, see them FAIL)

- [X] T055 [P] [US6] Write `tests/unit/config/poller_test.cpp`: `Poller_DetectsAtomicRename` (the section 25.4 write pattern), `Poller_DetectsSizeChangeAndMtimeChange` (one case each), `Poller_DebouncesRepeatedEvents` (BC-13 — five rapid writes, assert at most one effective change notification and one generation bump), `Poller_DisabledWhenIntervalIsZero`, `Poller_RejectedReloadStillUpdatesTheRecordedStat` (so a bad file is not re-read every interval), `Poller_MissingFileDoesNotSpin`

### Implementation for User Story 6

- [X] T056 [US6] Create `modules/config/src/file_poller.hpp` and `src/file_poller.cpp`: a fixed-delay periodic timer on the injected scheduler that reads `last_write_time` and `file_size` and calls `reload()` when either differs from the recorded pair, updating the pair on **both** success and rejection. Add the `shortcut:` comment recording the two accepted limits — a write preserving both mtime and size is missed, and latency is up to one interval (R-03)
- [X] T057 [US6] Arm the poller at the end of `start()` when `pollInterval > 0` and cancel it first in `stop()` (spec section 11 shutdown order). Make T055 pass

**Checkpoint**: scenario B works without an operator command. Ticket TCU-CFG-007 closed.

---

## Phase 9: User Story 7 — Metrics, log vocabulary and health (Priority: P7)

**Goal**: both libraries expose the section 16 metric set with bounded labels, and the counters add
up.

**Independent Test**: `ctest -L logging-unit -R Metrics_` and `ctest -L config-unit -R Metrics_` both
pass; every conservation equation of contracts/metrics.md section 4 is asserted, not inspected.

### Tests for User Story 7 (write first, see them FAIL)

- [X] T058 [P] [US7] Write `tests/unit/logging/metrics_test.cpp`: `Metrics_ConservationAfterFlood` (M3), `Metrics_EnqueuedAccountingBalances` (M7), `Metrics_FailedWriteDoesNotCountAsWritten` (M8), `Metrics_QueueDepthZeroAfterFlushAndStop` (M9), `Metrics_LabelsAreClosedSets` (M4 — each array length equals its enum size), `Metrics_WriteLatencyHistogramSumsMatchCount`
- [X] T059 [P] [US7] Write `tests/unit/config/metrics_test.cpp`: `Metrics_ReloadAccountingBalances` (M1), `Metrics_GenerationMatchesSnapshot` (M2), `Metrics_SuccessChangedEqualsGenerationMinusOne` (M5), `Metrics_NotifyAccountingCoversEveryCommit` (M6), `Metrics_FailureReasonsAreDisjoint` (one counter per `LoadError`, `None` never used as a failure label)

### Implementation for User Story 7

- [X] T060 [US7] Complete every counter increment in both libraries so T058 and T059 pass, and fix the structured log lines to the closed event vocabulary and field set of contracts/metrics.md section 6 — `startup_loaded`, `reload_committed`, `reload_rejected`, `reload_noop`, `resync`, `observer_failed`, `notify_overflow`, `queue_drops_detected`, `sink_error`, `sink_recovered`, `rotated`, `shutdown_drained`, `health`, with **no `payload`, `value` or `config` field anywhere**
- [X] T061 [US7] Add the `queue_drops_detected` aggregation: the writer emits one WARN per drain in which drops occurred, carrying `fields.count`, rather than one line per dropped record — otherwise the drop report is itself the flood

**Checkpoint**: observability is complete and self-consistent. Ticket TCU-CFG-008's observability half
closed.

---

## Phase 10: User Story 8 — Demo and Event Bus integration (Priority: P8)

**Goal**: the spec section 21 sequence runs on a developer machine and shows all six scenarios.

**Independent Test**: `scripts/run_config_logging_demo.sh` completes and prints the expected line for
each of scenarios A-F; `ctest -L config-logging-integration` passes.

### Tests for User Story 8 (write first, see them FAIL)

- [X] T062 [P] [US8] Write `tests/integration/config_logging/startup_test.cpp` (scenario A — generation 1, `config_valid=true`, `logger_healthy=true`) and `tests/integration/config_logging/rejection_test.cpp` (scenarios C and D — generation unchanged, reason and field correct, the co-changed hot key not applied)
- [X] T063 [P] [US8] Write `tests/integration/config_logging/hot_reload_test.cpp`: scenario B end to end — `writeAtomic` a new `telemetry.period_ms`, wait on a `TestGate` opened by the observer (**no sleep**), assert the new generation, that the telemetry timer was cancelled and rescheduled exactly once, and that `config_generation_applied` moved
- [X] T064 [P] [US8] Write `tests/integration/config_logging/shutdown_test.cpp`: `Logger_StopDrainsWithinDeadline` (BC-23, L7), `Logger_RejectsAfterStop`, `Service_ShutdownOrderIsPollerThenReloadThenRegistry` (section 11), `Logger_QueueLossOnCrashIsDocumented` (BC-24 — assert the documented claim: records queued at a hard stop are reported as not drained, and no API promises durability)
- [X] T065 [P] [US8] Write `tests/integration/config_logging/event_bus_test.cpp`: the demo's observer publishes `ConfigChanged{generation}` on project 01's bus; assert a subscriber receives it and that **the event carries the generation and no configuration value** (R-20, section 22)

### Implementation for User Story 8

- [X] T066 [US8] Create `apps/config_logging_demo/main.cpp` and `apps/config_logging_demo/CMakeLists.txt`: parse `--config <path>` (and nothing else — R-16), build the scheduler with `workerThreads = 1`, the logger with console plus file sinks from the generation-1 snapshot, the config service, a telemetry worker (periodic sample on the scheduler) and a connectivity worker (retry backoff), then run until SIGINT with the shutdown order of spec section 11
- [X] T067 [US8] Implement the demo's config observer: apply `logging.level` and `logging.console_enabled` through `setLevel`/`setConsoleEnabled` (R-15), reschedule the telemetry timer on a `telemetry.period_ms` change, publish `ConfigChanged{generation}` on the Event Bus, and update `config_generation_applied{component}` for `telemetry` and `connectivity`
- [X] T068 [US8] Add the demo's `--flood <producers>x<records>` mode for scenario E and a `health` status line built per contracts/metrics.md section 5 (**no configuration value in it**)
- [X] T069 [US8] Create `scripts/run_config_logging_demo.sh`: the six-step section 21 sequence with `jq`-based temp-write-plus-rename edits, printing the expected output before each step and the actual after

**Checkpoint**: the project is demonstrable. Tickets TCU-CFG-008 and TCU-CFG-010 closed.

---

## Phase 11: User Story 9 — Stress, faults, sanitizers (Priority: P9)

**Goal**: the concurrency claims hold under load and under sanitizers, and the performance numbers
are reported rather than asserted.

**Independent Test**: `ctest -L config-logging-stress` passes in about 5 s by default and at any
`TCU_CONFIG_STRESS_SECONDS`; the ASan and TSan builds pass the whole suite on Linux.

- [X] T070 [P] [US9] Write `tests/stress/config_logging/log_flood_test.cpp`: `Stress_EightProducersWellFormedJsonl` (BC-22 — 8 threads, duration from `TCU_CONFIG_STRESS_SECONDS` default 5; assert every recorded line parses as JSON, no record is both written and dropped, memory stays bounded, and the conservation equations hold afterwards)
- [X] T071 [P] [US9] Write `tests/stress/config_logging/reload_storm_test.cpp`: `Snapshot_ReaderSeesOldOrNewNeverTorn` (BC-10 — 100 reloads alternating two valid files while 8 reader threads spin on `snapshot()`; assert every observed snapshot is internally consistent with one of the two files, never a mix)
- [X] T072 [P] [US9] Write `tests/stress/config_logging/soak_test.cpp`: the combined loop (log flood plus reload storm plus rotation) asserting bounded memory growth and that the file count never exceeds `fileCount + 1`; document in a comment that section 17's 1-24 h soak is this test with `TCU_CONFIG_STRESS_SECONDS` raised
- [X] T073 [US9] Add the performance report of quickstart.md section 5 to the stress suite's teardown in `tests/stress/config_logging/soak_test.cpp` — host profile, throughput, reload and snapshot-read percentiles, rotations, sink errors, peak queue depth — **printed, never asserted** (R-21)
- [X] T074 [US9] Run the ASan/UBSan build over all four labels on Linux/WSL and fix what it finds
- [X] T075 [US9] Run the TSan build (`clang++`, `TCU_SANITIZER_SETARCH=ON`) over all four labels and fix what it finds; record in `README.md` that sanitizer coverage is Linux-only
- [X] T076 [US9] Walk spec section 15's failure matrix row by row and confirm each row has a test or a documented manual procedure; add the missing ones to `tests/unit/logging/file_sink_test.cpp` or `tests/unit/config/reload_test.cpp`

**Checkpoint**: ticket TCU-CFG-009 closed; spec section 24's sanitizer and stress criteria are
evidenced.

---

## Phase 12: Polish & Cross-Cutting Concerns

- [X] T077 [P] Extend `README.md` with the configuration and logging sections: architecture summary, build and test commands, the metric list, and the seven known limitations of quickstart.md section 4 — stated, not hidden
- [X] T078 [P] Add the four project 04 CTest labels to the CI workflow under `.github/workflows/`, with the sanitizer jobs Linux-only, matching how projects 01-03 are wired there
- [X] T079 Audit every `shortcut:` comment in `modules/config/` and `modules/logging/` against plan.md's Deferred shortcuts table: each of the five project 04 rows must exist in the code, in the stated form, and nothing undocumented may carry the marker
- [X] T080 Run quickstart.md sections 1-3 verbatim on a clean clone and correct any command that does not work as written
- [X] T081 Re-check the Constitution gate in `plan.md`: confirm the Complexity Tracking entries are still exactly three (nlohmann, `TooLarge`, `UnsupportedVersion`), that no public API grew beyond `contracts/`, and that nothing from the section 3 non-goals was built
- [X] T082 Walk spec section 24's Definition of Done checklist against the command in quickstart.md section 4 and tick each line; **report any unmet criterion explicitly rather than closing the project** (Constitution, Development Workflow, step 3)

---

## Dependencies & Execution Order

### Phase dependencies

- **Setup (Phase 1)**: no dependencies
- **Foundational (Phase 2)**: needs Setup; blocks every story
- **US1 (Phase 3)**: needs Foundational — the MVP
- **US2 (Phase 4)**: needs US1 (the writer thread it extends, the formatter it writes)
- **US3 (Phase 5)**: needs US1 (it logs through `ILogger`) — **not** US2
- **US4 (Phase 6)**: needs US3 (the snapshot it diffs against)
- **US5 (Phase 7)**: needs US4 (the commit point it notifies after)
- **US6 (Phase 8)**: needs US4 (the `reload()` it triggers)
- **US7 (Phase 9)**: needs US1-US6 (the counters it balances)
- **US8 (Phase 10)**: needs US1-US7
- **US9 (Phase 11)**: needs US8 (the demo's wiring is what the soak exercises)
- **Polish (Phase 12)**: needs everything intended to ship

### Within each story

- Tests are written and seen to FAIL before implementation (Principle III)
- Types before logic, logic before wiring, wiring before demo
- A story is not done until its checkpoint command passes

### Parallel opportunities

- Setup: T004-T010 all `[P]`
- Foundational: T012, T013, T014, T017, T018 all `[P]`; T011 before T012-T014, T015/T016 before T017/T018
- US1: the five test files T021-T025 are `[P]` with each other; T029 is `[P]` with T027/T028
- US2: T033 and T034 `[P]`
- US3-US6: each story's test files are `[P]` with each other
- US8: T062-T065 all `[P]`
- US9: T070-T072 `[P]`
- Polish: T077 and T078 `[P]`
- **Across stories**: US2 and US3 can run in parallel by two developers once US1 is green — they touch disjoint files (`file_sink.*` versus `modules/config/**`). Nothing else parallelises across stories.

---

## Parallel Example: User Story 1

```bash
# Write the five failing test files together:
Task: "formatter_test.cpp in tests/unit/logging/"
Task: "redaction_test.cpp in tests/unit/logging/"
Task: "queue_test.cpp in tests/unit/logging/"
Task: "filter_test.cpp in tests/unit/logging/"
Task: "lifecycle_test.cpp in tests/unit/logging/"

# Then verify they all fail for the right reason:
ctest --test-dir build -L logging-unit --output-on-failure   # expect assertion failures, not crashes
```

---

## Implementation Strategy

### MVP (US1 only)

1. Phase 1 Setup, Phase 2 Foundational
2. Phase 3 US1
3. **STOP and VALIDATE**: `ctest -L logging-unit` green, plus the ASan run
4. At this point `tcu_logging` already closes project 03's deferred logging shortcut for any *new*
   consumer (R-19), which is the single most reusable thing this project produces

### Incremental delivery

1. Setup + Foundational → both libraries compile as shells
2. US1 → a usable logger (MVP)
3. US2 → real sinks with real failure behaviour; tickets TCU-LOG-* closed
4. US3 → startup configuration; a process can boot on a validated file
5. US4 → safe reload; last-known-good guaranteed
6. US5 → observers; modules can react
7. US6 → hands-off change detection
8. US7 → the numbers add up
9. US8 → the demo; the project is showable
10. US9 → the evidence; the project is defensible
11. Polish → the gate

### Commit policy

One commit per story at its checkpoint, message prefixed `project 04:` with the tickets it closes, as
projects 01-03 did. A story whose checkpoint command fails is not committed as done.

---

## Notes

- `[P]` tasks touch different files and depend on nothing incomplete
- Every test name in this file also appears in plan.md's traceability tables; if a name changes,
  change it in both
- Verify tests fail before implementing — a test that passes against an empty implementation is
  testing nothing
- No test may sleep to wait for an event; use `tests/support/test_gate.hpp`
- No test may fill a real disk or change a real permission; use `FaultyFileOps`
- Do not add an enum value, a public method or a `LoggerOptions` field that `contracts/` does not
  list. That is a spec change, not an implementation detail
