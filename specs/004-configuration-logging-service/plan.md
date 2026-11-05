# Implementation Plan: Configuration & Logging Service

**Branch**: `004-configuration-logging-service` | **Date**: 2026-11-04 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/004-configuration-logging-service/spec.md`

## Summary

Build two C++17 libraries plus one demo.

`tcu_logging` is a bounded asynchronous structured logger: an MPSC queue with a configurable
capacity, one writer thread that alone owns the sinks, severity filtering before formatting,
newline-delimited JSON output to a console sink and a size-rotating file sink, a severity-aware
overload policy (drop INFO and below immediately, bounded wait for WARN/ERROR, synchronous
emergency stderr for FATAL), redaction of every non-public field at ingestion *and* again at
formatting, and a sink that goes unhealthy instead of taking the application down. It depends on
the standard library only.

`tcu_config` loads one JSON file into an immutable typed snapshot, validates types, ranges,
cross-field invariants, the schema version and unknown keys before publishing anything, hands the
snapshot over at a single atomic point, increments a generation only on an effective change, rejects
the whole candidate when any restart-only key moved, and notifies observers asynchronously in
publication order with each callback's exception isolated and counted. It reuses project 03's
`TimerScheduler` for both of its background jobs — the notifier and the file poller — so it adds no
thread and no queue of its own.

`apps/config_logging_demo` runs the spec's section 21 sequence: valid startup, valid hot reload,
rejected reload, restart-only rejection, log flood, and a sink error, with telemetry and
connectivity worker threads reading the same generation.

This project also pays off project 03's deferred shortcut R-16 *for new code*: `tcu_config` logs
through `tcu_logging` rather than stderr. Project 03's own `src/log.cpp` is **not** migrated here —
see [research.md](./research.md) R-19.

The spec already fixes the design: architecture (section 6), the data model and schema table
(section 7), the illustrative API (section 8), the reload pipeline and its nine rules (section 9),
the logging contract (section 10), the lifecycle (section 11), six end-to-end scenarios
(section 12), 30 behavioural contracts (section 13), the concurrency and lock-order rules
(section 14), the failure matrix (section 15), the metrics and log lines (section 16), the testing
strategy (section 17), the performance budgets (section 18), the layout (section 19), a 15-ticket
backlog (section 20), the demo workflow (section 21), the integration points (section 22) and the
Definition of Done (section 24). This plan redesigns none of it. It closes the nine open questions
of section 25 plus the mechanisms the spec states as a goal without naming an implementation; those
closures are [research.md](./research.md) R-00..R-21.

## Technical Context

**Language/Version**: C++17, no compiler extensions in library code.

**Primary Dependencies**: `tcu_logging` — standard library only (`<thread>`, `<mutex>`,
`<condition_variable>`, `<chrono>`, `<deque>`, `<variant>`, `<atomic>`, `<fstream>`,
`<filesystem>`). `tcu_config` — `nlohmann/json` v3.11.3, header-only, pinned via FetchContent
(spec AS-02; justified in Complexity Tracking), plus `tcu_scheduler` (project 03) for the notifier
and the file poller, plus `tcu_logging` for its own diagnostics. `tcu_eventbus` (project 01) is
linked by the demo and one integration test only, never by either library (R-20).

**Storage**: the configuration JSON file is read, never written, by these libraries. Log files are
written and rotated; durability across crash or power loss is an explicit non-goal (spec section 3,
BC-24).

**Testing**: GoogleTest + CTest, four labels — `config-unit`, `logging-unit`,
`config-logging-integration`, `config-logging-stress`. Reuses the shared
`tests/support/test_gate.hpp` rendezvous instead of sleeping. Every filesystem test runs inside a
unique directory under `std::filesystem::temp_directory_path()` and removes it in fixture teardown.
Disk-full, permission-denied, short-write and `EINTR` faults are injected through a `FileOps` seam,
never by filling a real disk (R-14, Principle IV). Stress duration is `TCU_CONFIG_STRESS_SECONDS`,
default 5 (R-21). Sanitizers: `-fsanitize=address,undefined` and a separate `-fsanitize=thread`
build (clang + `TCU_SANITIZER_SETARCH=ON`), never combined.

**Target Platform**: Linux/Ubuntu 22.04+ x86-64 (GCC 11+ or Clang 14+) **and** Windows/MSVC 19.3x.
Both libraries are portable: file permissions are set through `std::filesystem::permissions` and
file change detection is polling, so there is no `inotify` and no POSIX call anywhere (R-00). Both
targets therefore sit outside the root `CMakeLists.txt` Linux guard, as project 03 does. Sanitizer
jobs stay Linux-only, a toolchain limit, so spec section 24's sanitizer criterion is evaluated on
Linux/WSL.

**Project Type**: two static libraries + one demo executable + four test suites, inside the Virtual
TCU Lab monorepo.

**Performance Goals**: measured, never asserted. Spec section 18 labels its budgets "engineering
exercise budgets, not automotive OEM requirements". Snapshot read p99 < 100 us, reload of a 100 KiB
file p99 < 100 ms, >= 10,000 small records/s, graceful stop <= 2 s. The stress suite asserts the
*shape* of the result — no loss, no unbounded growth, no indefinite wait — and reports throughput
and percentiles without failing on them (R-21).

**Constraints**: bounded memory and bounded time everywhere. Config file capped at 1 MiB before
parsing (BC-15); log record capped at 16 KiB with truncation (BC-29); log queue capped at
`logging.queue_capacity`; notification queue capped by the scheduler's `maxReadyTasks`. Snapshot
readers never block on file I/O, parsing or validation. No user callback and no sink write runs
under the reload mutex, the subscriber-registry mutex or the queue mutex (spec section 14). Lock
order is fixed and one-way: reload mutex -> registry mutex, never the reverse, and neither is held
while entering the logger. No exception escapes `ILogger::log`. No secret or vehicle-sensitive value
reaches a sink, a metric label or an error string. Every owned thread is joined; nothing is detached.

**Scale/Scope**: 8 concurrent log producers and 100 sequential reloads in the stress suite, 10,000
records/s target, 4 config observers in the demo, two libraries of roughly 1,800 lines plus about
2,200 lines of tests.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Gate | Status |
|---|---|---|
| I. Spec Fidelity and Scope Discipline | Deliver goals 1-9 of section 3; nothing from the section 3 non-goals (cloud config sync, remote command channel, OTA rollout, multi-process atomic config, consensus, crash durability, hiding in-memory credentials from a compromised host, real-time logging from interrupt context, runtime path or crypto policy changes) | PASS — R-01..R-09 close all nine ADR candidates of section 25 with an explicit v1 boundary, and [contracts/config_api.md](./contracts/config_api.md) section 6 plus [contracts/logging_api.md](./contracts/logging_api.md) section 7 each end with a not-in-v1 list. Two `LoadError` values are added beyond section 8's enum — `UnsupportedVersion`, which BC-28 names outright, and `TooLarge`, which section 9 rule 1 and BC-15 require to be distinguishable. Both are recorded in Complexity Tracking. |
| II. Library-First Reuse | One CMake target per library with a public header surface; consume earlier projects rather than copy them | PASS — two targets, `tcu_config` and `tcu_logging`, matching the spec's two deliverable libraries. Project 03's `TimerScheduler` is genuinely reused for the notifier executor and the file poller (R-02, R-03), so no second thread pool, no second bounded ready queue and no debounce timer are written. Project 01's `EventBus` is reused by the demo for `ConfigChanged` (R-20). GoogleTest is reused at the root pin. `tests/support/test_gate.hpp` is reused, not re-written. The reverse direction — replacing project 03's private `src/log.cpp` with `tcu_logging` — is deliberately *not* done here, with reasons in R-19. |
| III. Test-First | Every spec requirement has a unit test naming it; every BDD scenario has a test; every acceptance criterion is observable | PASS — the traceability tables below map goals 1-9 and BC-01..BC-30 onto named tests. Each task writes its test first. The conservation equations in [contracts/metrics.md](./contracts/metrics.md) section 4 turn "no record was lost" and "no reload went uncounted" into arithmetic a test can assert rather than a claim a reviewer has to believe. |
| IV. Simulation-Only | No real hardware; time and faults injectable; deterministic | PASS — nothing but files, threads and memory. Disk-full, permission-denied, short-write and `EINTR` arrive through the `FileOps` seam; a throwing observer, a slow sink and a queue-overflow burst arrive through test-controlled gates. No test sleeps to wait for an event, and no test fills a real filesystem. Sink timestamps come from an injectable clock function so golden-line assertions are exact (R-17). |
| V. Safe Observability | Structured logs and metrics where the spec asks; no secrets; failures isolated and counted | PASS — this is the project whose subject *is* safe observability. Redaction happens twice, at ingestion and at formatting, and a dedicated test asserts a `Secret` value appears in no sink, no error string and no metric (BC-19). Parse errors report the JSON pointer of the offending key and the error kind, never the value and never the file contents (R-10). Metric labels are closed sets — severity, sink name, bounded reason — so no path, VIN or correlation id can become a label (section 16). Every observer exception is caught at the notifier boundary, counted in `config_notify_failures_total`, and logged with the subscription id, never swallowed. |

**Post-Phase 1 re-check**: still PASS. Phase 1 added one third-party dependency, recorded below; no
abstraction with a single implementation other than `ILogSink`, which has two implementations
(console, file) plus the test fault seam; and no public API beyond spec sections 8 and 10 plus the
additions listed in Complexity Tracking. The notifier and poller reuse decision *removed* public
surface rather than adding any.

### Requirement to test traceability

| Spec goal (section 3) | Covered by |
|---|---|
| 1. Load and validate a JSON file | `Loader_ParsesSampleConfig`, `Validator_RejectsWrongType`, `Validator_RejectsOutOfRange`, `Validator_RejectsCrossFieldRetryOrder`, `Validator_RejectsUnknownKey`, `Loader_RejectsOversizeBeforeParse` |
| 2. Typed reads from one immutable snapshot | `Snapshot_TypedFieldsMatchFile`, `Snapshot_ReaderSeesOldOrNewNeverTorn` (BC-10), `Snapshot_HeldPointerStaysValidAfterReload` |
| 3. Reload without publishing partial or invalid state | `Reload_InvalidKeepsGeneration` (BC-03, BC-04), `Reload_TruncatedFileRejected` (BC-14), `Poller_DetectsAtomicRename` |
| 4. Dynamic vs restart-required settings | `Reload_RestartOnlyKeyRejectsWholeCandidate` (BC-08), `Schema_EveryFieldHasReloadPolicy` |
| 5. Notify after commit, isolate callbacks | `Notify_RunsAfterCommit`, `Notify_ObserverThrowIsIsolatedAndCounted` (BC-11), `Notify_UnsubscribeStopsFutureCallbacks` (BC-12), `Notify_OverflowMarksDegradedAndResyncs` (BC-25) |
| 6. Structured severity-filtered console and file output | `Formatter_EmitsWellFormedJsonl`, `Formatter_EscapesControlCharacters`, `Logger_FilterBelowLevelNeverFormats` (BC-16), `FileSink_WritesAndRotates` (BC-20) |
| 7. Bound queue, memory, disk and shutdown | `Queue_DropsInfoWhenFull` (BC-17), `Queue_BoundedWaitForError` (BC-18), `Record_OversizeTruncatedAndCounted` (BC-29), `FileSink_RetainsAtMostFileCount` (BC-20), `Logger_StopDrainsWithinDeadline` (BC-23) |
| 8. Redaction, counters, health | `Redaction_SecretAbsentFromEverySink` (BC-19), `Redaction_AppliedAtIngestionAndFormatting`, `Metrics_ConservationAfterFlood`, `Health_ReportsSinkAndConfigState` |
| 9. Runs with no hardware; deterministic tests, fault injection | the whole suite: no sleeps, injected clock, injected `FileOps`, `TCU_CONFIG_STRESS_SECONDS` |

### BDD contract traceability (spec section 13)

| ID | Covered by | Kind |
|---|---|---|
| BC-01 | `Lifecycle_StartPublishesGenerationOne` | unit |
| BC-02 | `Lifecycle_MissingRequiredFieldFailsStartup` | unit |
| BC-03 | `Reload_InvalidJsonKeepsSnapshot` | unit |
| BC-04 | `Reload_OutOfRangeKeepsSnapshot` | unit |
| BC-05 | `Validator_RejectsCrossFieldRetryOrder` | unit |
| BC-06 | `Reload_NoOpDoesNotBumpGenerationOrNotify` | unit |
| BC-07 | `Reload_HotChangeCommitsAndNotifies` | unit |
| BC-08 | `Reload_RestartOnlyKeyRejectsWholeCandidate` | unit |
| BC-09 | `Reload_ConcurrentCallsAreSerialized` | integration |
| BC-10 | `Snapshot_ReaderSeesOldOrNewNeverTorn` | stress + TSan |
| BC-11 | `Notify_ObserverThrowIsIsolatedAndCounted` | unit |
| BC-12 | `Notify_UnsubscribeStopsFutureCallbacks` | unit |
| BC-13 | `Poller_DebouncesRepeatedEvents` | unit |
| BC-14 | `Reload_TruncatedFileRejected` | unit |
| BC-15 | `Loader_RejectsOversizeBeforeParse` | unit |
| BC-16 | `Logger_FilterBelowLevelNeverFormats` | unit |
| BC-17 | `Queue_DropsInfoWhenFull` | unit |
| BC-18 | `Queue_BoundedWaitForError` | unit |
| BC-19 | `Redaction_SecretAbsentFromEverySink` | unit |
| BC-20 | `FileSink_WritesAndRotates`, `FileSink_RetainsAtMostFileCount` | unit |
| BC-21 | `FileSink_DiskFullMarksUnhealthyAppSurvives` | unit (injected `ENOSPC`) |
| BC-22 | `Stress_EightProducersWellFormedJsonl` | stress + TSan |
| BC-23 | `Logger_StopDrainsWithinDeadline`, `Logger_RejectsAfterStop` | integration |
| BC-24 | `Logger_QueueLossOnCrashIsDocumented` — asserts the documented claim, not durability | integration |
| BC-25 | `Notify_OverflowMarksDegradedAndResyncs` | unit |
| BC-26 | `Logger_ReentrantLogFromSinkDoesNotDeadlock` | unit |
| BC-27 | `FileSink_UnwritablePathFallsBackToStderr` | unit |
| BC-28 | `Loader_RejectsUnsupportedSchemaVersion` | unit |
| BC-29 | `Record_OversizeTruncatedAndCounted` | unit |
| BC-30 | `Logger_StopIsIdempotent`, `Service_StopIsIdempotent` | unit |

BC-10 and BC-22 are listed as sanitizer targets too: part of what they assert is the *absence* of a
race, which only TSan observes. The tests themselves assert that every read returns a coherent
snapshot and that every emitted line parses as JSON.

Five cases exist beyond the spec's 30 contracts, each because a Phase 1 invariant would otherwise be
unobservable: `Lock_ObserverCanReadSnapshotAndLog` (section 14's lock-order rule — a nested lock
deadlocks the test), `Schema_EveryFieldHasReloadPolicy` (R-04's one table is only a single source of
truth if nothing can be added without a policy), `Notify_RunsAfterCommit` (section 9 rule 6 and the
section 13 test-oracle note), `Redaction_AppliedAtIngestionAndFormatting` (section 10.5 asks for
both, and either one alone passes BC-19) and `Metrics_ConservationAfterFlood`.

## Project Structure

### Documentation (this feature)

```text
specs/004-configuration-logging-service/
  plan.md                      # This file
  research.md                  # Phase 0: closes the spec's open decisions (R-00..R-21)
  data-model.md                # Phase 1: entities, states, invariants C*/S*/N*/L*/Q*/F*
  quickstart.md                # Phase 1: build, test, demo sequence, acceptance checklist
  contracts/
    config_api.md              # Config public API pre/postconditions, thread safety, not-in-v1
    config_schema.md           # The field table: type, range, mandatory, reload policy, errors
    logging_api.md             # Logger API, severity policy, overload policy, redaction, rotation
    metrics.md                 # Metric definitions, conservation equations, log field vocabulary
  tasks.md                     # Phase 2 output (/speckit-tasks - NOT created here)
```

### Source Code (repository root)

Spec section 19's layout, re-rooted into the monorepo:

```text
CMakeLists.txt                                   # root: nlohmann pin + project 04 block, no platform guard
README.md                                        # extended with the config and logging sections

modules/logging/
  CMakeLists.txt                                 # target tcu_logging
  include/tcu/logging/log_types.hpp              # Severity, DataClass, Field, LogRecord
  include/tcu/logging/logger.hpp                 # ILogger
  include/tcu/logging/log_sink.hpp               # ILogSink, SinkHealth
  include/tcu/logging/logger_metrics.hpp         # LoggerMetrics snapshot
  include/tcu/logging/async_logger.hpp           # AsyncLogger, LoggerOptions
  include/tcu/logging/console_sink.hpp
  include/tcu/logging/file_sink.hpp              # FileSinkOptions, FileOps seam
  src/json_formatter.hpp                         # internal: record -> one JSONL line
  src/json_formatter.cpp
  src/log_queue.hpp                              # internal: bounded MPSC, severity-aware admission
  src/log_queue.cpp
  src/redaction.hpp                              # internal: classification -> [REDACTED]
  src/async_logger.cpp                           # writer thread, emergency path, lifecycle
  src/console_sink.cpp
  src/file_sink.cpp                              # rotation, permissions, health, recovery probe

modules/config/
  CMakeLists.txt                                 # target tcu_config
  include/tcu/config/config_types.hpp            # LoadError, ReloadResult, ConfigPtr, SubscriptionId, snapshot structs
  include/tcu/config/config_schema.hpp           # FieldSpec, kFieldSpecs, ReloadPolicy
  include/tcu/config/configuration_service.hpp   # IConfigurationService, ConfigurationService, ServiceOptions
  include/tcu/config/config_metrics.hpp          # ConfigMetrics snapshot
  src/json_document.hpp                          # internal: nlohmann wrapper, duplicate-key rejection, pointer paths
  src/json_document.cpp
  src/loader.hpp                                 # internal: capped read + parse -> JsonDocument
  src/loader.cpp
  src/validator.hpp                              # internal: kFieldSpecs walk + cross-field rules
  src/validator.cpp
  src/snapshot_diff.hpp                          # internal: changed-field set, hot/restart classification
  src/file_poller.hpp                            # internal: mtime+size probe on the scheduler
  src/file_poller.cpp
  src/configuration_service.cpp                  # reload pipeline, atomic publish, subscriber registry, notifier

apps/config_logging_demo/main.cpp                # section 21 sequence; publishes ConfigChanged on project 01's bus

configs/tcu.json                                 # the vetted baseline (R-16)
configs/invalid_retry.json                       # BC-05 fixture
configs/invalid_schema.json                      # BC-28 fixture
configs/restart_only.json                        # BC-08 fixture

tests/unit/logging/
  support/fake_clock.hpp                         # injectable timestamp source (R-17)
  support/faulty_file_ops.hpp                    # ENOSPC, EACCES, short write, EINTR seam
  support/recording_sink.hpp                     # captures lines for assertion
  formatter_test.cpp                             # JSONL shape, escaping, field types, size cap
  redaction_test.cpp                             # BC-19, ingestion + formatting
  queue_test.cpp                                 # BC-17, BC-18, BC-29, Q1..Q4
  filter_test.cpp                                # BC-16, hot level change
  file_sink_test.cpp                             # BC-20, BC-21, BC-27, F1..F5
  console_sink_test.cpp                          # console on/off hot change
  lifecycle_test.cpp                             # BC-26, BC-30, flush, stop idempotence
  metrics_test.cpp                               # conservation equations M1..M4

tests/unit/config/
  support/temp_config_dir.hpp                    # unique temp dir fixture + atomic rename helper
  loader_test.cpp                                # BC-14, BC-15, BC-28, duplicate keys, UTF-8
  validator_test.cpp                             # BC-02, BC-04, BC-05, unknown keys, schema table
  snapshot_test.cpp                              # BC-01, BC-06, typed reads, C1..C4
  reload_test.cpp                                # BC-03, BC-07, BC-08, generation rules
  subscriber_test.cpp                            # BC-11, BC-12, BC-25, N1..N4
  poller_test.cpp                                # BC-13, mtime/size probe, atomic rename
  lock_test.cpp                                  # section 14 lock order, observer reads and logs
  metrics_test.cpp                               # config counters and reasons

tests/integration/config_logging/
  startup_test.cpp                               # scenario A
  hot_reload_test.cpp                            # scenario B, telemetry period applied
  rejection_test.cpp                             # scenarios C and D
  shutdown_test.cpp                              # BC-23, BC-24, section 11 shutdown order
  serialized_reload_test.cpp                     # BC-09
  event_bus_test.cpp                             # R-20, ConfigChanged carries generation only

tests/stress/config_logging/
  log_flood_test.cpp                             # BC-22, scenario E, 8 producers
  reload_storm_test.cpp                          # BC-10, 100 reloads against live readers
  soak_test.cpp                                  # bounded memory and file count, TCU_CONFIG_STRESS_SECONDS

scripts/run_config_logging_demo.sh               # the section 21 sequence end to end
```

**Structure Decision**: spec section 19's tree is adopted with five deltas, each justified:

1. **Re-rooted into the monorepo.** `modules/config` and `modules/logging` sit beside
   `modules/core/event_bus`, `modules/ipc` and `modules/scheduler`; demos go to `apps/`, tests to
   `tests/{unit,integration,stress}/...`. Same re-rooting projects 01-03 recorded.
2. **`docs/architecture/04_...md` is not created.** The design document already exists twice, as
   `Virtual_TCU_Lab_30_System_Designs/04_Configuration_Logging_Service_System_Design.md` and as this
   feature's `spec.md`. A third copy would drift. `config/schema.md`, which section 5 lists as a
   deliverable, becomes [contracts/config_schema.md](./contracts/config_schema.md) — one schema
   document, in the place the other three projects keep their contracts.
3. **`scripts/test_disk_failure.sh` is not created.** Disk failure is injected through `FileOps` in
   `file_sink_test.cpp`, which is deterministic and runs in CI; a shell script that fills a real
   filesystem is neither (Principle IV). `scripts/run_config_logging_demo.sh` *is* created, because
   the section 21 sequence is six steps across two terminals.
4. **Private headers under `src/` beyond section 19's `.cpp` list.** Section 6 names Loader,
   Validator, Snapshot Store, Reload Coordinator, Notification Queue, Formatter, Queue and Sinks as
   components; the eight `.cpp` files of section 19 cannot hold them with one declaration each. The
   extra headers stay under `src/`, so the surface projects 05-30 depend on is exactly the public
   headers listed above. There is deliberately **no** `watcher.cpp` class hierarchy: the poller is
   one function plus two `std::filesystem` calls on a scheduler timer (R-03).
5. **`configs/` at the repository root**, as section 19 writes it, rather than `config/`. The
   fixtures are shared by the demo and three test suites, so they are not test data of any one suite.

The root `CMakeLists.txt` is extended, not restructured: the `nlohmann_json` FetchContent pin is
declared unconditionally (the library needs it even when `TCU_BUILD_TESTS=OFF`), and project 04's
`add_subdirectory` lines go **outside** the Linux guard, like project 03's.

## Complexity Tracking

> Three entries. The first is a new third-party dependency, which the Constitution's Technology
> Constraints require to be justified here. The other two are public API values beyond the spec's
> own enums.

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| New dependency: `nlohmann/json` v3.11.3, header-only, FetchContent-pinned | Goal 1 is strict JSON parsing with typed errors, a depth limit and duplicate-key rejection. Spec AS-02 names this dependency explicitly and requires the version be pinned in the build. | Writing a strict JSON parser: 350-450 lines of new library code plus its own test suite, to re-solve a solved problem — and every parser bug would be ours. `apps/vehicle_service/json_lite.hpp` from project 02 was the other candidate and is **not** reusable here: it is a 94-line structural well-formedness checker (balanced braces, terminated strings, depth), not a parser — it extracts no values, and it lives in `apps/` by project 02's deliberate choice (its research R-01 keeps JSON out of the transport). Reusing it would mean writing the value extraction anyway. |
| `LoadError::UnsupportedVersion` beyond spec section 8's enum | BC-28 requires exactly this: "Reject with UnsupportedVersion, no blind parsing". The enum in section 8 is labelled illustrative; section 13 is the contract. | Reporting a version mismatch as `SchemaError` makes the single most likely upgrade failure indistinguishable from a typo, and BC-28 names the value. |
| `LoadError::TooLarge` beyond spec section 8's enum | Section 9 rule 1 and BC-15 require oversize input to be rejected *before* parsing; an operator needs to tell "file too big" from "cannot read file". | Folding it into `ReadError` loses the distinction at exactly the point where the fix differs (shrink the file vs fix permissions). `publicReason` carries detail, but metrics are labelled by reason and `config_reload_failures_total{reason}` must stay a closed, meaningful set. |

Three decisions that came closest to needing an entry, and why they do not:

| Candidate | Why it is not a violation |
|---|---|
| `tcu_config` depending on `tcu_scheduler` for two background jobs | It looks like coupling a config library to a timer library. But spec section 6 states the choice outright — "the reload and notifier workers can be implemented as dedicated threads or integrated with Project 03; contract stays the same" — and section 22 lists scheduler integration as expected. Principle II makes reuse the default: the alternative is a second thread, a second bounded queue and a second debounce implementation, which is the duplication the principle exists to prevent (R-02, R-03). |
| Polling the config file instead of `inotify` | Section 6 says "file watcher", and polling is the humbler mechanism. AS-08 settles it: "filesystem watcher events are hints; content is re-read and validated" — the contract is *re-read on signal*, not *receive kernel events*. Polling satisfies it, keeps both libraries POSIX-free and therefore Windows-capable, and makes debounce free (one probe per interval). Section 23 already prefers explicit reload as the deterministic path (R-03). |
| No defaults-merge layer: every schema key is mandatory | Section 7.2 mentions "Defaults: loaded from a vetted baseline". BC-02 requires a missing required field to fail startup with no implicit defaults. Making every key mandatory and shipping `configs/tcu.json` as that vetted baseline satisfies both, and deletes a precedence-merge layer that would otherwise need its own tests to prove it cannot silently substitute a zero (R-16). |

## Deferred shortcuts

Every deliberate shortcut carries a `shortcut: <limit>, <when to upgrade>` comment at the code that
takes it. Planned set, to be confirmed after implementation:

| Where | Limit accepted | Upgrade when |
|---|---|---|
| `modules/config/src/file_poller.hpp` | change detection is a 500 ms mtime+size probe, so a modification preserving both is missed, and detection latency is up to one interval | a consumer needs sub-interval latency or same-size-same-mtime detection; then add a content hash or a platform watcher behind the same interface (R-03) |
| `modules/config/src/json_document.cpp` | duplicate keys are detected by a parser callback holding one key set per open object, so detection costs memory proportional to the largest object | a config file grows objects large enough for that to matter (R-01) |
| `modules/config/src/configuration_service.cpp` | notification coalescing is off, as section 9 rule 7 requires for v1, so a reload storm enqueues one notification per generation until the queue is full | spec section 9 rule 7 changes; the overflow path already degrades safely and triggers resync (R-06) |
| `modules/logging/include/tcu/logging/file_sink.hpp` | rotation is size-based only; no time-based retention, no compression, and the rename happens on the writer thread, which briefly stalls writes | spec section 10.4's v1 boundary moves (R-13) |
| `modules/logging/src/log_queue.cpp` | a mutex + condition variable queue, not lock-free, as section 25.5 fixes for v1 | a profile shows the queue mutex is the bottleneck at the 10,000 records/s target (R-08) |
| `modules/scheduler/src/log.cpp` (project 03, unchanged) | project 03 keeps writing to stderr with no level filter | project 03's R-16 is closed by a separate change that re-tests project 03; doing it inside project 04 would edit a finished project's internals with project 04's tests as the only evidence (R-19) |

All six sit inside the envelope spec sections 1 and 3 draw, so none is a Constitution Check
violation — they are recorded here so a later project finds them without reading the source.

## Post-implementation reconciliation

The Constitution gate is re-checked after implementation, and this is what it found. Every item
below is a deviation from what the Phase 1 documents said, recorded here rather than left to be
discovered in the code.

| # | Deviation | Why | Status |
|---|---|---|---|
| 1 | `ILogSink` gained `kind()`, `periodic()` and `nextPeriodicIn()` | Every per-sink metric is an array indexed by sink (invariant M4), so a string-to-index lookup on every write would be waste; the two periodic members are what keep the bounded recovery probe on the writer thread existing wait instead of a second thread (R-14) | contracts/logging_api.md section 1 updated |
| 2 | `log_types.hpp` ships `f(...)` and `classified(...)` field makers | A call site reads as data instead of a `Field` aggregate initialisation. No behaviour | contracts/logging_api.md section 1 updated |
| 3 | `FileSink` exposes `open()`, `rotations()`, `currentSize()`, `lastError()` | `open()` is the documented start-up call; the three observers are what make BC-20, BC-21 and F6 assertable without reaching into the implementation | contracts/logging_api.md section 4 describes the behaviour; the accessors are additive |
| 4 | Two reason templates were missing: `restart-required key changed: <pointer>` and `service stopped` | BC-08 needs the first (the test asserts the pointer) and C13 needs the second (a non-empty reason whenever `error != None`) | contracts/config_schema.md section 4 updated |
| 5 | `startup_failed` was missing from the event vocabulary | BC-02 needs a line for a refused startup | contracts/metrics.md section 6 updated |
| 6 | The logger does not log its own `sink_error`, `sink_recovered`, `rotated` or `queue_drops_detected` lines | It cannot: logging from the writer thread is the reentrancy the guard exists to stop (BC-26). The demo turns metric deltas into those events instead, one line per change rather than one per record | contracts/metrics.md section 6 updated; demo `LoggerWatch` implements it |
| 7 | The notification queue is bounded by the scheduler `maxTimers`, not `maxReadyTasks` | Project 03 guarantees a scheduled one-shot is never lost to ready-queue pressure (its IT-01), so `CapacityExceeded` - the overflow signal section 9 rule 7 needs - is what a full timer registry returns | research R-02 reasoning holds; the knob named is corrected in the tests and the fixture |
| 8 | `ConfigurationService::Impl` is held by `shared_ptr`, and the notifier and poller hold weak references | A task already handed to the scheduler can run after the service is destroyed. ASan caught it as a heap-use-after-free; a weak handle makes a late task inert instead of dangling | private member, no contract change |
| 9 | `tcu::eventbus::ConfigChanged` was appended to project 01 `EventPayload` | Spec section 22 asks for the event, and project 01 `event.hpp` documents that extension point for exactly this. The three existing indices did not move; project 01 own assertion was updated from a frozen count of 3 to 4 | cross-project change, sanctioned by project 01 |

Complexity Tracking is unchanged: still exactly three entries - the `nlohmann/json` dependency and
the two `LoadError` values. Nothing from the section 3 non-goals was built.

### Test-first compliance, stated honestly

Principle III was followed for US1, US2, US5, US6, US7, US8 and US9: the test file was written,
seen to fail, and then made to pass. For US3 and US4 the order slipped in two files -
`modules/config/src/loader.cpp` and `validator.cpp` were written before `loader_test.cpp` and
`validator_test.cpp`. The tests that followed did find real defects (the endpoint port handling),
but the sequence was not test-first and is recorded here rather than claimed otherwise.

### What the sanitizers found

ASan found one real defect (item 8 above). TSan found two real test defects: a `TestGate` and an
`EventBus` each declared after the stack whose callbacks captured them, so they were destroyed
while a notification could still touch them. All three are fixed, and both builds are clean across
all four labels.
