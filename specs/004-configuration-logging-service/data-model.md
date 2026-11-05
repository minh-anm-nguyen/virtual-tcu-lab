# Phase 1 Data Model: Configuration & Logging Service

**Feature**: `004-configuration-logging-service` | **Date**: 2026-11-04

Entities, states and invariants. Invariant ids are referenced by the tests named in
[plan.md](./plan.md): `C*` configuration snapshot, `S*` service lifecycle and publication, `N*`
notification, `L*` log record, `Q*` queue, `F*` file sink, `M*` metrics.

---

## 1. Configuration entities

### 1.1 `ConfigSnapshot` — the immutable unit of consistency

Spec section 7.3, with the four sub-structs spelled out.

| Field | Type | Notes |
|---|---|---|
| `generation` | `std::uint64_t` | 0 before the first publish; 1 after startup; +1 per effective change only |
| `source` | `std::string` | absolute path the bytes came from |
| `loadedAt` | `std::chrono::system_clock::time_point` | wall clock, for human output |
| `connectivity` | `ConnectivityConfig` | `retryBaseMs`, `retryMaxMs`, `maxAttempts` |
| `telemetry` | `TelemetryConfig` | `periodMs`, `offlineQueueCapacity` |
| `logging` | `LoggingConfig` | `level`, `queueCapacity`, `fileMaxBytes`, `fileCount`, `consoleEnabled` |
| `network` | `NetworkConfig` | `cloudEndpoint`, `tlsRequired` |

Integer fields are `std::int64_t` so a range check happens on the parsed value rather than after a
narrowing conversion. `level` is `tcu::logging::Severity`, parsed from the enum name.

`ConfigPtr = std::shared_ptr<const ConfigSnapshot>`.

**Invariants**

| ID | Invariant | Enforced by |
|---|---|---|
| C1 | A published snapshot is never mutated. Readers hold `shared_ptr<const>` and no non-const accessor exists | type system; `Snapshot_HeldPointerStaysValidAfterReload` |
| C2 | Every field of a published snapshot passed validation. There is no code path from parse to publish that skips the validator | `configuration_service.cpp` single pipeline; `Reload_InvalidKeepsGeneration` |
| C3 | Semantic equality ignores formatting: two files differing only in whitespace, key order or integer spelling produce equal snapshots. Comparison is field-by-field on the typed struct, never on the JSON text | `ConfigSnapshot::contentEquals`; `Reload_NoOpDoesNotBumpGenerationOrNotify` |
| C4 | `generation` is strictly increasing and never reused; a rejected candidate does not consume a number | `Reload_InvalidKeepsGeneration`, `Reload_ConcurrentCallsAreSerialized` |
| C5 | `contentEquals` compares every field except `generation`, `source` and `loadedAt` | `Snapshot_ContentEqualsIgnoresProvenance` |

### 1.2 `FieldSpec` and `kFieldSpecs` — the schema table (R-04)

One `constexpr` array of 13 rows, each `{pointer, type, min, max, policy}`. It is the only place a
key, its range and its reload policy are written. Full rows in
[contracts/config_schema.md](./contracts/config_schema.md).

| ID | Invariant | Enforced by |
|---|---|---|
| C6 | Every key accepted by the validator appears in `kFieldSpecs`; any other key is `SchemaError` | `Validator_RejectsUnknownKey` |
| C7 | Every row has a `ReloadPolicy`; there is no default and no "unspecified" value | `Schema_EveryFieldHasReloadPolicy` |
| C8 | No pointer appears twice | `Schema_PointersAreUnique` |
| C9 | `configs/tcu.json` contains exactly the 13 pointers, no more and no fewer | `Schema_SampleFileMatchesTable` |

### 1.3 `LoadError` and `ReloadResult`

```cpp
enum class LoadError {
  None, FileNotFound, ReadError, TooLarge, ParseError, SchemaError,
  InvalidValue, RestartRequired, UnsupportedVersion, ServiceStopped
};

struct ReloadResult {
  LoadError error{LoadError::None};
  std::uint64_t generation{0};   // the generation in force after the call, committed or kept
  bool changed{false};
  std::string publicReason;      // redacted; safe for logs (R-10)
};
```

`TooLarge` and `UnsupportedVersion` are the two additions to spec section 8's enum, both required by
section 13 and both recorded in [plan.md](./plan.md) Complexity Tracking.

| ID | Invariant | Enforced by |
|---|---|---|
| C10 | `error == None` implies a snapshot is in force whose generation equals `result.generation` | `Reload_HotChangeCommitsAndNotifies` |
| C11 | `error != None` implies `changed == false` and `generation` is the *previous* generation, unchanged | `Reload_InvalidKeepsGeneration` |
| C12 | `changed == true` implies `generation` increased by exactly 1 since the call started | `Reload_HotChangeCommitsAndNotifies` |
| C13 | `publicReason` is non-empty exactly when `error != None`, and is built from the closed template set of R-10 | `Redaction_ReasonStringsNeverQuoteInput` |

---

## 2. Configuration service

### 2.1 Lifecycle states (spec section 11)

| State | Entered by | Permitted operations |
|---|---|---|
| `Created` | construction | `start()` |
| `Loading` | `start()` | — (transient, inside `start()`) |
| `Running` | valid initial config published as generation 1 | `snapshot()`, `reload()`, `subscribe()`, `unsubscribe()`, `metrics()`, `stop()` |
| `Failed` | initial config invalid | `metrics()`, `stop()`; `snapshot()` returns `nullptr` |
| `Stopping` | `stop()` | — (transient) |
| `Stopped` | poller cancelled, in-flight reload finished, registry cleared | `metrics()`, `stop()` (no-op) |

| ID | Invariant | Enforced by |
|---|---|---|
| S1 | `start()` either publishes generation 1 or leaves the service in `Failed` with no snapshot. There is no state in which the service is `Running` with an unvalidated or absent snapshot | `Lifecycle_StartPublishesGenerationOne`, `Lifecycle_MissingRequiredFieldFailsStartup` |
| S2 | Publication is a single `std::atomic_store(&current_, candidate)` (AS-03). A reader performs one `std::atomic_load` and gets a complete old or new snapshot, never a mix | `Snapshot_ReaderSeesOldOrNewNeverTorn` (TSan) |
| S3 | `reload()` is serialized by one reload mutex. Two concurrent calls produce deterministic generations and no interleaved publication | `Reload_ConcurrentCallsAreSerialized` |
| S4 | No observer callback runs while the reload mutex or the registry mutex is held (section 14). Lock order is reload -> registry, one way | `Lock_ObserverCanReadSnapshotAndLog` — a nested lock deadlocks the test |
| S5 | `reload()` after `stop()` returns `ServiceStopped` and touches nothing | `Service_ReloadAfterStop` |
| S6 | `stop()` is idempotent; it cancels the poller timer, waits for an in-flight reload, and clears the registry so no captured observer outlives the service | `Service_StopIsIdempotent` (BC-30) |
| S7 | `snapshot()` is `noexcept` and never blocks on file I/O, parsing, validation or a mutex held by a reload | `Snapshot_ReadDuringReloadDoesNotBlock` |

### 2.2 Reload pipeline (spec section 9)

Stages, in order, each a function with one responsibility:

1. `probeSize` — `file_size` > `maxFileBytes` -> `TooLarge` (BC-15), before any read.
2. `readAll` — into a `std::string` sized by the probe; failure -> `FileNotFound` or `ReadError`.
3. `parse` — strict JSON, depth <= 8, duplicate keys rejected -> `ParseError` (BC-03, BC-14).
4. `checkVersion` — `schema_version != 1` -> `UnsupportedVersion` (BC-28).
5. `validate` — `kFieldSpecs` walk, then cross-field rules -> `SchemaError` or `InvalidValue`.
6. `diff` — against the snapshot in force: no change -> `changed = false`, success, no publish, no
   notification (BC-06); any `Restart` field changed -> `RestartRequired` (BC-08).
7. `publish` — `atomic_store`, generation + 1.
8. `notify` — one `scheduleAfter(0ms, ...)` per published generation (R-02).

| ID | Invariant | Enforced by |
|---|---|---|
| S8 | Stages 1-6 produce no observable effect on the snapshot in force. Only stage 7 mutates it | `Reload_InvalidKeepsGeneration` for every error kind |
| S9 | Stage 6 runs before stage 7, so a no-op reload never bumps the generation and never notifies | `Reload_NoOpDoesNotBumpGenerationOrNotify` |
| S10 | Stage 8 runs after stage 7 returns, never before, so an observer that calls `snapshot()` sees at least the generation it was notified about | `Notify_RunsAfterCommit` |

### 2.3 Cross-field rules (spec section 7.2, BC-05)

| Rule | Error | Reason template |
|---|---|---|
| `connectivity.retry_max_ms >= connectivity.retry_base_ms` | `InvalidValue` | `cross-field: /connectivity/retry_max_ms must be >= /connectivity/retry_base_ms` |
| `network.tls_required == true` unless the `cloud_endpoint` host is loopback or `.local` | `InvalidValue` | `cross-field: /network/tls_required must be true for a nonlocal endpoint` |
| `network.cloud_endpoint` scheme in {`http`, `https`}, host non-empty, length <= 255 | `InvalidValue` | `out of range at /network/cloud_endpoint: allowed scheme http/https, host <= 255 bytes` |

---

## 3. Notification

### 3.1 Subscriber registry

```cpp
using SubscriptionId = std::uint64_t;             // strictly increasing, 0 never issued
using ConfigObserver = std::function<void(ConfigPtr current, ConfigPtr previous)>;
```

The registry is a `std::vector<Entry>` of `{id, observer, alive}` under its own mutex. A
notification task copies the live entries under the lock, releases it, then calls them in
registration order.

| ID | Invariant | Enforced by |
|---|---|---|
| N1 | Notifications for generations g and g+1 are delivered in that order (one scheduler worker, FIFO ready queue — R-02) | `Notify_OrderedAcrossGenerations` |
| N2 | An observer that throws is caught at the notifier boundary, counted in `config_notify_failures_total`, logged with its subscription id, and the remaining observers still run | `Notify_ObserverThrowIsIsolatedAndCounted` (BC-11) |
| N3 | `unsubscribe(id)` prevents any callback for that id from *starting* after the call returns; a callback already running may finish | `Notify_UnsubscribeStopsFutureCallbacks` (BC-12) |
| N4 | On notification-queue overflow nothing is published twice and nothing is rolled back: `notifyDegraded` latches, and the next accepted notification is a resync carrying `previous == nullptr` (R-06) | `Notify_OverflowMarksDegradedAndResyncs` (BC-25) |
| N5 | `previous` is `nullptr` for the first notification after startup and for a resync; otherwise it is the snapshot that was in force before the published one | `Notify_PreviousIsNullOnFirstAndResync` |
| N6 | An observer may call `snapshot()`, `subscribe()`, `unsubscribe()` and the logger; none of these deadlocks (S4) | `Lock_ObserverCanReadSnapshotAndLog` |

---

## 4. Logging entities

### 4.1 `LogRecord`

| Field | Type | Notes |
|---|---|---|
| `timestampUtc` | `system_clock::time_point` | formatted as `2026-10-30T01:00:00.000Z` |
| `timestampMono` | `steady_clock::time_point` | durations only, never printed |
| `severity` | `Severity` | `Trace, Debug, Info, Warn, Error, Fatal` |
| `component` | `std::string` | owned copy (section 14) |
| `eventName` | `std::string` | owned copy |
| `message` | `std::string` | owned copy, first to be truncated (R-12) |
| `correlationId` | `std::string` | opaque; never a VIN, phone number or token |
| `fields` | `std::vector<Field>` | typed key/value plus classification |

```cpp
enum class DataClass { Public, VehicleSensitive, Secret };

struct Field {
  std::string key;
  std::variant<std::int64_t, double, bool, std::string> value;
  DataClass cls{DataClass::Public};
};
```

| ID | Invariant | Enforced by |
|---|---|---|
| L1 | A queued record owns every byte it references. No `string_view` into caller memory survives the `log()` call | `Queue_RecordOwnsItsStrings` under ASan |
| L2 | A field whose class is not `Public` holds the literal `[REDACTED]` from ingestion onward; the formatter checks again (R-11) | `Redaction_AppliedAtIngestionAndFormatting`, `Redaction_SecretAbsentFromEverySink` (BC-19) |
| L3 | Every emitted line is valid JSON on one line: control characters, quotes and backslashes escaped, no raw newline | `Formatter_EmitsWellFormedJsonl`, `Formatter_EscapesControlCharacters` |
| L4 | A line never exceeds `maxRecordBytes`; over-long records are truncated in the documented order and marked `"truncated":true` | `Record_OversizeTruncatedAndCounted` (BC-29) |
| L5 | Numbers are emitted unquoted, booleans as `true`/`false`, strings quoted — the variant's type decides, not the caller | `Formatter_FieldTypesRoundTrip` |

### 4.2 Queue states and admission policy (spec section 10.3)

| Severity | Queue-full behaviour | Counter |
|---|---|---|
| Trace, Debug, Info | drop the incoming record immediately, no wait | `logger_records_dropped_total{severity}` |
| Warn, Error | wait up to `highPriorityWaitMs` (default 20) for space, then drop | `logger_high_priority_drops_total` |
| Fatal | synchronous best-effort write to the pre-opened stderr, then a bounded flush request | `logger_fatal_emergency_total` |

| ID | Invariant | Enforced by |
|---|---|---|
| Q1 | Queue depth never exceeds `queueCapacity` | `Queue_DepthNeverExceedsCapacity` |
| Q2 | A producer never waits unboundedly. Trace..Info do not wait at all; Warn/Error wait at most `highPriorityWaitMs` | `Queue_DropsInfoWhenFull` (BC-17), `Queue_BoundedWaitForError` (BC-18) |
| Q3 | A record below the current level is discarded before formatting and before enqueue | `Logger_FilterBelowLevelNeverFormats` (BC-16) |
| Q4 | A `log()` call made from the writer thread (a sink that logs) never enqueues; it takes the emergency path. Detected with a `thread_local` guard, so no recursion and no deadlock | `Logger_ReentrantLogFromSinkDoesNotDeadlock` (BC-26) |
| Q5 | After `stop()`, `log()` is a counted no-op and never throws | `Logger_RejectsAfterStop` (BC-23) |

### 4.3 Logger lifecycle

`Created -> Running (start) -> Stopping (stop) -> Stopped`. `stop()` stops admission, drains the
queue within `shutdownDeadline` (default 2 s, spec section 18), flushes and closes the sinks, and
joins the writer thread.

| ID | Invariant | Enforced by |
|---|---|---|
| L6 | `stop()` is idempotent and never self-joins, including when called from a sink callback | `Logger_StopIsIdempotent` (BC-30) |
| L7 | Records queued at `stop()` are written unless the deadline expires; the number not written is reported, never silently lost | `Logger_StopDrainsWithinDeadline` (BC-23) |
| L8 | Records still queued when the process dies are lost, and the documentation says so. No durability is claimed anywhere in the API | `Logger_QueueLossOnCrashIsDocumented` (BC-24) |
| L9 | No exception escapes `log()`, `flush()` or `stop()`. A formatting failure increments `logger_format_failures_total` and fails closed | `Logger_FormatFailureIsCountedNotThrown` |

### 4.4 Sink health (spec section 10.4, scenario F)

| State | Meaning | Transition |
|---|---|---|
| `Healthy` | last write succeeded | -> `Unhealthy` on open/write/rotate failure |
| `Unhealthy` | writes rejected; records go to the fallback or are counted as dropped | -> `Healthy` when a probe at or after `nextProbeAt` succeeds |

| ID | Invariant | Enforced by |
|---|---|---|
| F1 | A sink failure never propagates to the application. `log()` stays `noexcept` and the business thread is unaffected | `FileSink_DiskFullMarksUnhealthyAppSurvives` (BC-21) |
| F2 | Rotation happens before a write that would exceed `fileMaxBytes`, so the active file never exceeds it | `FileSink_WritesAndRotates` (BC-20) |
| F3 | At most `fileCount` rotated files plus the active file exist; the oldest is removed | `FileSink_RetainsAtMostFileCount` (BC-20) |
| F4 | The active file and every rotated file are created with owner read/write only; the parent directory is created when missing | `FileSink_PermissionsAreOwnerOnly` |
| F5 | An unwritable path at startup leaves the sink `Unhealthy` and falls back to stderr; the service still starts | `FileSink_UnwritablePathFallsBackToStderr` (BC-27) |
| F6 | Recovery probing is bounded and never spins: at most one probe per `recoveryProbeInterval` (default 30 s), performed on the writer thread's existing timed wait (R-14) | `FileSink_RecoveryProbeIsRateLimited` |

---

## 5. Metrics entities

Two plain snapshot structs, following project 03's `SchedulerMetrics` pattern: atomics inside, a
by-value snapshot out, no registry and no label strings at runtime. Definitions, label sets and
conservation equations are in [contracts/metrics.md](./contracts/metrics.md).

| ID | Invariant | Enforced by |
|---|---|---|
| M1 | `config_reload_attempts_total == config_reload_success_total + sum(config_reload_failures_total{reason})` | `Metrics_ReloadAccountingBalances` |
| M2 | `config_generation` equals the generation of the snapshot `snapshot()` returns | `Metrics_GenerationMatchesSnapshot` |
| M3 | `logger_records_submitted_total == logger_records_written_total + sum(logger_records_dropped_total{severity})` | `Metrics_ConservationAfterFlood` |
| M4 | Every metric label comes from a closed set — severity name, sink name, or `LoadError` name — so cardinality is bounded by construction | `Metrics_LabelsAreClosedSets` |
