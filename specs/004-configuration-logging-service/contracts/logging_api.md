# Contract: Logging API

**Feature**: `004-configuration-logging-service` | Target: `tcu_logging` | Header root:
`modules/logging/include/tcu/logging/`

Spec section 10, with the preconditions, the overload policy and the failure behaviour made
explicit. This library depends on the standard library only, so any project from 01 to 30 can link
it without inheriting a dependency.

---

## 1. Types

```cpp
namespace tcu::logging {

enum class Severity { Trace, Debug, Info, Warn, Error, Fatal };
enum class DataClass { Public, VehicleSensitive, Secret };

struct Field {
  std::string key;
  std::variant<std::int64_t, double, bool, std::string> value;
  DataClass cls{DataClass::Public};
};

struct LogRecord {
  std::chrono::system_clock::time_point timestampUtc;
  std::chrono::steady_clock::time_point timestampMono;
  Severity severity{Severity::Info};
  std::string component;
  std::string eventName;
  std::string message;
  std::string correlationId;
  std::vector<Field> fields;
};

class ILogger {
 public:
  virtual ~ILogger() = default;

  virtual void log(Severity level,
                   std::string_view component,
                   std::string_view event,
                   std::string_view message) noexcept = 0;

  virtual void log(Severity level,
                   std::string_view component,
                   std::string_view event,
                   std::string_view message,
                   std::vector<Field> fields) noexcept = 0;

  virtual bool enabled(Severity level) const noexcept = 0;

  virtual void flush() = 0;   // may block; never call from a sink callback
  virtual void stop() = 0;
};

}  // namespace tcu::logging
```

`enabled()` is the one addition to spec section 10.2's sketch, and section 10.2 asks for it in
words: "avoid building expensive payloads when a severity is disabled". A caller writes
`if (log.enabled(Severity::Debug)) { ... }` around any field vector that costs something to build.

### Sinks

```cpp
enum class SinkHealth { Healthy, Unhealthy };

enum class SinkKind { Console, File };   // the closed set; indexes the metrics

class ILogSink {
 public:
  virtual ~ILogSink() = default;
  virtual std::string_view name() const noexcept = 0;     // "console" or "file"
  virtual SinkKind kind() const noexcept = 0;             // index into the metric arrays
  virtual bool write(std::string_view line) = 0;          // one complete JSONL line, no newline
  virtual void flush() = 0;
  virtual SinkHealth health() const noexcept = 0;

  // Bounded periodic work on the writer thread existing wait, so a sink can
  // run its recovery probe without a thread of its own (R-14). Default: none.
  virtual void periodic() {}
  virtual std::chrono::steady_clock::duration nextPeriodicIn() const;
};
```

`kind()`, `periodic()` and `nextPeriodicIn()` were added during implementation and are recorded in
plan.md under Post-implementation reconciliation. `kind()` exists because every per-sink metric is
an array indexed by it, and a string-to-index lookup on every write would be waste; the two
periodic members are what keep the recovery probe off a second thread.

`log_types.hpp` also ships `f(key, value)` overloads and `classified(key, value, cls)`, so a call
site reads as data rather than as a `Field` aggregate initialisation. They add no behaviour.

A sink is only ever touched by the writer thread (section 14), so implementations need no internal
locking. `write` returns false instead of throwing; the writer records the failure.

### Options

```cpp
struct LoggerOptions {
  Severity minLevel{Severity::Info};                               // hot (R-15)
  bool consoleEnabled{true};                                       // hot (R-15)
  std::size_t queueCapacity{8192};                                 // restart
  std::size_t maxRecordBytes{16 * 1024};                           // restart (section 10.1)
  std::chrono::milliseconds highPriorityWaitMs{20};                // restart (section 10.3)
  std::chrono::milliseconds shutdownDeadline{2000};                // restart (section 18)
  std::chrono::seconds recoveryProbeInterval{30};                  // restart (scenario F)
  std::function<std::chrono::system_clock::time_point()> clock{};  // default: system_clock::now (R-17)
};

struct FileSinkOptions {
  std::filesystem::path path{"logs/tcu.log"};
  std::size_t fileMaxBytes{10 * 1024 * 1024};
  std::size_t fileCount{5};
  bool fallbackToStderr{true};
};
```

`AsyncLogger` is constructed with `LoggerOptions` and a vector of sinks, started with `start()`, and
exposes `setLevel(Severity)`, `setConsoleEnabled(bool)` and `metrics()`.

---

## 2. `log()` contract

**Never throws, never blocks unboundedly, never fails the caller.** It returns `void` because there
is nothing useful a business thread can do about a logging failure; the failure is counted instead
(spec section 3's principle "failure of optional logging cannot crash business logic").

Order of operations, so the cost of a filtered call is one atomic load:

1. Compare `level` against the current `minLevel` (relaxed atomic). Below it: return. Nothing is
   formatted, nothing is copied, nothing is enqueued (BC-16).
2. Check the `thread_local` reentrancy guard. Set means this is the writer thread logging from
   inside a sink: take the emergency stderr path and return. No enqueue, no recursion (BC-26, Q4).
3. Copy `component`, `event`, `message` and every field value into an owned `LogRecord`. No
   `string_view` into caller memory survives the call (L1).
4. Redact: every field whose class is not `Public` has its value replaced by `[REDACTED]` (R-11).
5. Admit to the queue per the policy below.

### Overload policy (section 10.3)

| Severity | Queue full | Counter |
|---|---|---|
| Trace, Debug, Info | drop the incoming record immediately, no wait | `logger_records_dropped_total{severity}` |
| Warn, Error | wait up to `highPriorityWaitMs` for space, then drop | `logger_high_priority_drops_total` |
| Fatal | synchronous best-effort write to the pre-opened stderr, then a bounded flush request | `logger_fatal_emergency_total` |

The record *dropped* under pressure is the incoming one, not the oldest queued one: the queue keeps
the earlier context, which is usually what explains the flood.

**Never** hold an application lock across a `log()` call at Warn or Error. The bounded wait is
bounded, but 20 ms with a lock held is 20 ms other threads also wait (section 10.3).

---

## 3. Output format

One JSONL line per record, the same for every sink (R-18):

```jsonl
{"timestamp_utc":"2026-10-30T01:00:00.000Z","severity":"INFO","component":"config","event":"startup_loaded","correlation_id":"","message":"","fields":{"generation":1}}
```

| Rule | Detail |
|---|---|
| Key order is fixed | `timestamp_utc`, `severity`, `component`, `event`, `correlation_id`, `message`, `fields`, then `truncated` when present. Fixed order makes golden-line tests possible. |
| `timestamp_utc` | ISO-8601 UTC, millisecond precision, trailing `Z` |
| `severity` | upper-case enumerator name |
| `fields` | a JSON object; numbers unquoted, booleans bare, strings quoted, by variant type (L5) |
| Escaping | `"`, `\`, and every control character below 0x20 are escaped; no raw newline can appear, so one record is always one line (L3) |
| Size | the line never exceeds `maxRecordBytes`; over-long records truncate `message` first, then fields from the end, and gain `"truncated":true` (R-12, BC-29) |
| Duplicate field keys | last one wins; the formatter does not deduplicate, and no test depends on the order of equal keys |

---

## 4. Rotation and file handling (section 10.4, R-13)

| Rule | Detail |
|---|---|
| When | before a write that would push the active file past `fileMaxBytes` |
| Naming | `tcu.log` active, `tcu.log.1` newest rotated, ... `tcu.log.<fileCount>` oldest; the oldest is deleted |
| Count | at most `fileCount` rotated files plus the active file exist at any time (F3) |
| Permissions | active and rotated files are created owner read/write only; a missing parent directory is created owner-only (F4) |
| Symlinks | an active path that is a symlink is refused and the sink starts `Unhealthy` |
| Where | the rename chain runs on the writer thread, so a rotation briefly stalls writes but never blocks a producer |

### Failure and recovery

| Failure | Behaviour |
|---|---|
| Open fails at startup (`EACCES`, a directory it cannot create) | sink starts `Unhealthy`; records go to stderr if `fallbackToStderr`; the application still starts (BC-27, F5) |
| Write fails (`ENOSPC`, I/O error) | `logger_sink_errors_total{sink}` increments, sink becomes `Unhealthy`, the record falls back or is counted dropped; the application is unaffected (BC-21, F1) |
| Short write | treated as a failure for that record; no partial line is left behind, because the sink writes line-at-a-time and reports failure rather than retrying forever |
| `EINTR` | retried, bounded by a small retry count, then treated as a failure |
| Recovery | at most one probe per `recoveryProbeInterval`, on the writer thread's existing timed wait — no probe thread, no spin (F6) |

---

## 5. Lifecycle

| Call | Contract |
|---|---|
| `start()` | opens the sinks, starts the writer thread. A sink that cannot open does not fail `start()`; it starts `Unhealthy`. |
| `flush()` | blocks until every record queued at the time of the call has been handed to the sinks and the sinks have flushed. **Must not** be called from a sink callback or from the writer thread; doing so returns immediately rather than self-deadlocking. |
| `stop()` | stops admission, drains within `shutdownDeadline`, flushes and closes the sinks, joins the writer thread. Idempotent. Safe from any thread. Never self-joins (L6, BC-30). |
| after `stop()` | `log()` is a counted no-op; `flush()` returns immediately (Q5, BC-23). |

**Shutdown order for an application** (spec section 11): stop producers first, then the config
service, then the logger. Stopping the logger while producers still log is permitted — those records
are counted as rejected, not lost silently — but it throws away the diagnostics of your own
shutdown.

**Durability**: none. Records still in the queue when the process dies are gone, and nothing in this
API claims otherwise (BC-24, L8).

---

## 6. Privacy rules (section 10.5)

| Rule | Mechanism |
|---|---|
| Credentials, tokens, private keys, SIM identifiers, full VIN and precise GPS must not be logged | callers classify such fields `Secret` or `VehicleSensitive`, and the value never reaches a sink (R-11) |
| Redaction happens at ingestion **and** at formatting | both checks exist; either alone would pass BC-19, which is why a separate test asserts both |
| `correlationId` is opaque | documented as such; the library does not inspect it, so a caller that puts a VIN in it has defeated the policy — the demo uses a random hex id |
| Error messages never quote input | the config side's reason templates (R-10); the logger's own internal failures report a counter and a reason code, never the offending content |
| Rotated files inherit restrictive permissions | F4 |

There is **no** API that takes a secret value and promises to handle it safely. The only safe thing
to do with a secret is not to pass it (section 10.2).

---

## 7. Not in v1

1. No lock-free queue (25.5, R-08).
2. No time-based retention and no compression (section 10.4, R-13).
3. No human-readable console formatter; console output is the same JSONL (R-18).
4. No remote or cloud sink, no log upload (section 3, 25.6).
5. No masking policy — no "last 4 of the VIN", no hashing. Non-public is `[REDACTED]`, whole (R-11).
6. No per-component level overrides; one `minLevel` for the process.
7. No sampling or rate limiting beyond the queue policy of section 2.
8. No interrupt-context or signal-handler-safe logging (section 3).
9. No structured sink other than console and rotating file.
10. No guarantee that a log line survives a crash, a power loss or a full disk (section 3, BC-24).
