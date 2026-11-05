# Phase 0 Research: Configuration & Logging Service

**Feature**: `004-configuration-logging-service` | **Date**: 2026-11-04 | **Spec**: [spec.md](./spec.md)

Purpose: close every decision the spec leaves open so implementation has no remaining ambiguity.
The spec already fixes the architecture (section 6), the data model (section 7), the illustrative
API (section 8), the reload pipeline and its nine rules (section 9), the logging contract
(section 10), the lifecycle (section 11), the six scenarios (section 12), the 30 behavioural
contracts (section 13), the concurrency rules (section 14), the failure matrix (section 15) and the
metrics (section 16). Nothing below redesigns any of that.

Entries R-01..R-09 answer spec section 25's nine open questions. R-00 and R-10..R-21 answer
decisions the spec states as a goal without naming a mechanism.

---

## R-00: Target platform — Linux **and** Windows, like project 03

**Decision**: no platform guard in CMake. `modules/config` and `modules/logging` build and test on
both Linux and Windows.

**Rationale**: neither library needs a POSIX call. File reads use `std::ifstream`, change detection
uses `std::filesystem::last_write_time` and `file_size` (R-03), file permissions use
`std::filesystem::permissions` with `owner_read | owner_write` (R-13), and everything else is
threads, memory and `<chrono>`. The reason project 02 is Linux-only (`AF_UNIX`, `poll`,
`SO_PEERCRED`) does not apply. The Constitution asks Linux to build and test with Windows
best-effort; here best-effort is free.

**Alternatives considered**: an `inotify` watcher with a Windows `ReadDirectoryChangesW` twin.
Rejected — two platform implementations of a component the spec itself calls a *hint* source
(AS-08), when a portable 20-line poll satisfies the contract.

**Consequence**: sanitizer jobs stay Linux-only (toolchain limit, not code limit), so spec
section 24's ASan/UBSan/TSan criterion is evaluated on Linux/WSL, and that limitation is recorded
in the quickstart rather than hidden.

---

## R-01: JSON parser — `nlohmann/json` v3.11.3, pinned

**Spec question**: AS-02 "JSON parsing via a selected dependency (e.g. nlohmann/json); version
pinned in build."

**Decision**: `nlohmann/json` v3.11.3, header-only, fetched by the root `CMakeLists.txt` with
`FetchContent` at an exact tag, and wrapped by `modules/config/src/json_document.hpp` so the
dependency appears in no public header of `tcu_config`.

**Rationale**: the spec names it, and the Constitution's "standard library and already-present
dependencies first" rule has a stated escape hatch — justification in `plan.md` Complexity
Tracking, which [plan.md](./plan.md) records. The wrapper matters more than the choice: projects
05-30 include `tcu/config/config_types.hpp` and get typed structs, never a `json` object, so
swapping the parser later touches one `.cpp` file.

**Strictness settings**: `json::parse` with exceptions enabled, `ignore_comments = false`,
`allow_exceptions = true`, and a `parser_callback_t` that (a) rejects a duplicate key inside the
same object by tracking one key set per open object, and (b) rejects nesting deeper than 8, the same
limit project 02 chose for payloads. Trailing data, NaN/Infinity and single quotes are rejected by
the library's default strict mode.

**Alternatives considered**: writing a strict parser (350-450 new lines plus a test suite, every bug
ours); reusing `apps/vehicle_service/json_lite.hpp` from project 02 (a 94-line *structural* checker —
balanced braces, terminated strings, depth — that extracts no values, so value extraction would
still have to be written, and it deliberately lives in `apps/`, not in a library).

---

## R-02: Notification delivery — asynchronous, ordered, on project 03's scheduler

**Spec question**: 25.1 "Should config notifications be synchronous or asynchronous? **V1:
asynchronous, ordered, with resync on overflow.**"

**Decision**: `ConfigurationService` takes a `tcu::scheduler::TimerScheduler&` and delivers each
`ConfigChanged` notification with `scheduleAfter(0ms, ...)`. The scheduler instance the service is
given **must** be configured with `workerThreads = 1`; the service checks nothing at runtime, the
requirement is stated in [contracts/config_api.md](./contracts/config_api.md) section 2 and asserted
by `Notify_OrderedAcrossGenerations`.

**Rationale**: project 03 already provides exactly the three properties section 9 rule 7 demands.
Its `ReadyQueue` is strict FIFO, so one worker gives publication order for free. Its
`maxReadyTasks` is a hard cap, so the bounded notification queue of section 6 is already built. Its
`Error::CapacityExceeded` is the overflow signal section 9 rule 7 needs, with no new error type.
Writing a notifier thread instead would mean a second bounded queue, a second shutdown handshake and
a second set of race tests, for behaviour that already exists one target away — the duplication
Principle II exists to prevent.

**Alternatives considered**: a dedicated notifier thread (rejected above); synchronous notification
inside `reload()` (rejected by 25.1, and it would make a slow observer stall the reload path, which
section 14 forbids).

**Consequence**: `tcu_config` links `tcu_scheduler`. The owner of both — the demo, or a test —
constructs and starts the scheduler, passes it in, and stops it *after* the config service, which
section 11's shutdown order already requires.

---

## R-03: Change detection — polling `mtime` + `size` on a scheduler timer

**Spec question**: section 6's "Reload trigger: command or file watcher", AS-08's "watcher events
are hints", and section 23's "watcher may be unreliable across rename/mount behavior".

**Decision**: `FilePoller` is a fixed-delay periodic timer on the same `TimerScheduler`, default
interval 500 ms, that reads `last_write_time` and `file_size` and calls `reload()` when either
differs from the values recorded at the last successful or rejected load. `pollInterval == 0`
disables polling; explicit `reload()` remains available and is the deterministic path used by every
unit test.

**Rationale**: AS-08 fixes the contract as *re-read and validate on a hint*, not *consume kernel
events*. A poll is a hint source that is portable (R-00), needs no debounce logic (one probe per
interval collapses any burst of writes, which is BC-13), survives the temp-file + rename pattern
section 25.4 recommends because it compares the *path's* stat, not an inode's, and cannot deliver an
event for a half-written file more than once.

**Alternatives considered**: `inotify` with `IN_MOVED_TO | IN_CLOSE_WRITE` plus an explicit debounce
timer (platform-specific, needs the debounce project 03's timer would provide anyway, and still
requires the content re-read AS-08 mandates); a content hash per poll (reads the whole file every
interval to detect a change that mtime already reports).

**Limit accepted**: a write that preserves both mtime and size is missed, and detection latency is
up to one interval. Recorded as a `shortcut:` comment in `file_poller.cpp` and in
[plan.md](./plan.md) Deferred shortcuts.

---

## R-04: One schema table as the single source of truth

**Spec question**: section 7.2 gives a table of key, type, constraint and reload policy; AS-07 says
"only a defined allowlist of keys supports hot reload".

**Decision**: `include/tcu/config/config_schema.hpp` declares

```cpp
enum class FieldType { Int, Bool, String, Enum };
enum class ReloadPolicy { Hot, Restart };

struct FieldSpec {
  std::string_view pointer;      // RFC 6901 pointer, e.g. "/connectivity/retry_base_ms"
  FieldType type;
  std::int64_t min;              // Int only
  std::int64_t max;              // Int only
  ReloadPolicy policy;
};

inline constexpr std::array<FieldSpec, 13> kFieldSpecs{ /* section 7.2, in order */ };
```

The validator walks `kFieldSpecs` to check presence, type and range; the diff walks it to classify a
changed field as hot or restart-only; `contracts/config_schema.md` holds the same rows for human
readers, and `Schema_EveryFieldHasReloadPolicy` asserts the array covers every key the sample file
contains and that no key appears twice.

**Rationale**: three consumers of one table means one place to add a key. Writing the presence
checks, the range checks and the hot/restart classification as three hand-written functions would
let them disagree, which is precisely the failure AS-07 warns about.

**Alternatives considered**: a JSON Schema document validated by a schema library (a second
dependency for 13 fields, and the hot/restart policy is not expressible in JSON Schema anyway); a
table generated at build time from `contracts/config_schema.md` (a code generator for 13 rows).

---

## R-05: Unknown keys — reject, with no `extensions` escape hatch in v1

**Spec question**: section 7.2 "Unknown keys: reject by default; optionally provide explicit
`extensions` object for future vendor extensions."

**Decision**: reject. Any key not in `kFieldSpecs` fails validation with `SchemaError` and a
`publicReason` naming the pointer. No `extensions` object is accepted in v1.

**Rationale**: "optionally" is not a requirement, and an accepted-but-ignored object is a silent
no-op waiting to be mistaken for a working setting. Principle I: not required, not built. Adding it
later is one row in `kFieldSpecs` plus a passthrough field.

---

## R-06: Notification overflow — degrade, count, resync

**Spec question**: section 9 rule 7 "Bounded notification queue overflow marks degraded health and
triggers a resync notification; consumers must always re-read the newest generation."

**Decision**: when `scheduleAfter` returns `Error::CapacityExceeded`, the service increments
`config_notify_failures_total`, sets a sticky `notifyDegraded` flag visible in `ConfigMetrics` and
in the health line, and sets a `resyncPending` flag. The next notification that *is* accepted is a
resync notification: it carries the newest snapshot as `current` and `nullptr` as `previous`, which
is the documented signal to re-read everything rather than diff. Commit is never rolled back, and
`reload()` still reports success — a committed generation is committed whether or not anyone was
told (section 13's test-oracle note).

**Rationale**: the spec requires every part of this except the shape of the resync signal.
`previous == nullptr` reuses the observer signature rather than adding a second callback type, and it
is already the shape of the first notification after startup, so observers that handle startup
handle resync with no new branch.

**Consequence**: `ConfigObserver` must tolerate `previous == nullptr`. Stated in
[contracts/config_api.md](./contracts/config_api.md) section 3 and asserted by
`Notify_OverflowMarksDegradedAndResyncs`.

---

## R-07: Secrets — no secret in the config file, no credential provider in v1

**Spec question**: 25.2 "Should secrets use environment variables? **V1: use a separate credential
provider; avoid CLI/env leakage when possible.**"

**Decision**: the configuration file contains no secret, and `tcu_config` ships no credential
provider. `kFieldSpecs` has no secret-valued field, so there is nothing for the loader to classify;
`network.cloud_endpoint` is validated for scheme and host and is not a secret. The logging library
*does* ship `DataClass::Secret` because callers outside this project will have secrets to keep out
of logs (R-11).

**Rationale**: goal 8 is about not *leaking* sensitive data, and the strongest way to satisfy it for
v1 is to have none in the file. A credential provider with no credential to provide is an interface
with one unimplemented implementation. The OTA and connectivity projects that genuinely hold
credentials can define the provider when they have one to hand it.

**Alternatives considered**: an `ISecretProvider` with an env-var implementation (25.2 explicitly
wants to avoid env leakage, so the only implementation would be the one the spec discourages).

---

## R-08: Log queue — bounded mutex + condition variable, MPSC

**Spec question**: 25.5 "Should logging use lock-free queue? **V1: bounded mutex/CV queue first;
optimize only after profiling.**"

**Decision**: `std::deque<LogRecord>` behind one mutex, capacity `logging.queue_capacity`, one
`condition_variable` for the writer and one for producers waiting under the WARN/ERROR policy. The
writer swaps the whole deque into a local one under the lock and then formats and writes with the
lock released, so formatting and I/O never block a producer.

**Rationale**: 25.5 fixes it. The batch swap is the one optimisation taken up front because it is
three lines and removes the only real contention point at the 10,000 records/s target.

**Consequence**: recorded as a `shortcut:` comment — upgrade to a lock-free ring when a profile
shows the queue mutex is the bottleneck, not before.

---

## R-09: Partial hot reload when a restart-only key also changed — rejected whole

**Spec question**: 25.3 "**V1: no, reject all.**"

**Decision**: the diff classifies every changed field; if any is `ReloadPolicy::Restart`, the
candidate is rejected with `RestartRequired` and `publicReason` naming the first restart-only
pointer that changed. Nothing is published, the generation does not move, and the hot fields in that
same candidate are **not** applied (BC-08).

**Rationale**: 25.3 fixes it, and section 9 rule 5 repeats it. Partial application would publish a
generation whose content matches no file on disk, which makes every later "what is running?"
question unanswerable.

---

## R-10: Error reporting — JSON pointer and error kind, never the value

**Spec question**: section 10.5 "Configuration parse errors expose key path and error type, not
secret values or full file content."

**Decision**: `ReloadResult::publicReason` is built from a closed set of templates:
`"missing required key <pointer>"`, `"wrong type at <pointer>: expected <type>"`,
`"out of range at <pointer>: allowed <min>..<max>"`, `"unknown key <pointer>"`,
`"cross-field: <pointer> must be >= <pointer>"`, `"unsupported schema_version: expected 1"`,
`"file exceeds <N> bytes"`, `"parse error at byte offset <N>"`, `"duplicate key <pointer>"`. No
template interpolates a *value* from the file. The nlohmann exception's `what()` is never forwarded,
because it quotes input.

**Rationale**: a reason string reaches logs, metric labels (as a bounded reason) and any future
status API, so it is a leak path. Bounded templates make the leak impossible by construction rather
than by review, and they keep `config_reload_failures_total{reason}` cardinality fixed.

**Consequence**: `Redaction_ReasonStringsNeverQuoteInput` asserts the template set, and a review
rule applies: a new reason template is a contract change to
[contracts/config_schema.md](./contracts/config_schema.md) section 4.

---

## R-11: Redaction policy — both non-public classes become `[REDACTED]`, twice

**Decision**: at `log()` ingestion, every `Field` whose `DataClass` is `VehicleSensitive` or
`Secret` has its value replaced by the literal `[REDACTED]` before the record enters the queue. The
formatter checks the class again and replaces any value that survived. No masking, hashing or
partial reveal (no "last 4 of the VIN") in v1.

**Rationale**: section 10.5 asks for redaction "at ingestion and again validated in sink
formatting", so both are required. Replacing at ingestion means a secret never sits in a queue that
a crash dump could contain. The second check is cheap and makes `Redaction_SecretAbsentFromEverySink`
true even if a future sink is added that formats records itself. Masking policies are not in the
spec (section 10.5 makes precise GPS and full VIN conditional on "explicit policy/legal basis",
which v1 does not have), so none is built.

**Alternatives considered**: dropping the whole record (loses the diagnostic the caller wanted);
redacting only at formatting (leaves plaintext secrets in the queue).

---

## R-12: Oversized records — truncate with a marker, count, keep the record

**Spec question**: BC-29 "Truncate or reject per documented policy with counter".

**Decision**: truncate. `LoggerOptions::maxRecordBytes` defaults to 16 KiB as section 10.1
suggests. The formatter writes the record, and if the line would exceed the cap it truncates the
`message` field first, then the `fields` array from the end, appends `"truncated":true`, and
increments `logger_records_truncated_total`. The line is always valid JSON.

**Rationale**: a 17 KiB diagnostic truncated to 16 KiB still answers the question that caused it to
be written; a rejected record answers nothing and loses the fact that something large happened.
Truncating the message before the fields keeps the structured keys, which are what a query filters
on.

---

## R-13: Rotation, naming and permissions

**Decision**: size-based rotation, checked *before* each write: if the current size plus the line
length would exceed `logging.file_max_bytes`, rotate first. Naming is section 10.4's scheme —
`tcu.log` is active, `tcu.log.1` the newest rotated file, up to `tcu.log.<file_count>`, and the
oldest is removed. Rotation is a rename chain from oldest to newest, performed on the writer thread.
Permissions are set with `std::filesystem::permissions(path, perms::owner_read | perms::owner_write,
perm_options::replace)` on create, for the active file and every rotated name; the parent directory
is created with `owner_all` when missing.

**Rationale**: all of it is section 10.4, and `std::filesystem` covers the whole requirement without
a POSIX call (R-00). Checking before the write is what keeps "at most `file_max_bytes`" true rather
than approximately true.

**Not in v1**, per section 10.4: time-based retention, compression, and symlink-attack hardening
beyond refusing to follow a symlink at the active path (an `std::filesystem::is_symlink` check on
open, which is cheap and is the only hardening section 10.4 scopes into the "production-hardening
phase").

**Limit accepted**: the rename chain runs on the writer thread, so a rotation briefly stalls writes.
Producers do not block — they enqueue — so the stall shows up as queue depth, which is already a
metric.

---

## R-14: Sink health and recovery — writer-thread timed wait, no extra thread

**Spec question**: section 10.4 "Disk full / permission denied: file sink becomes unhealthy, drop or
redirect according to configured fallback policy" and section 12 scenario F's "probe every 30 s; no
spin".

**Decision**: a sink that fails a write or an open records the error, increments
`logger_sink_errors_total{sink}`, sets its state to `Unhealthy` with the time of the next probe, and
returns failure. The writer thread already waits on its condition variable with a timeout; that
timeout becomes `min(remaining probe interval, idle wait)`, so recovery probing costs no thread and
no spin. While the file sink is unhealthy, records go to the emergency stderr path if
`fallbackToStderr` is set (default true), and are counted as dropped for that sink otherwise. The
application never sees an error: `log()` is `noexcept` and returns `void`.

**Fault injection**: `FileOps` is a three-function seam (`open`, `write`, `rename`) that the
production build satisfies with `std::ofstream` and `std::filesystem`, and tests satisfy with
`FaultyFileOps`, which can return `ENOSPC`, `EACCES`, a short write or `EINTR` on the Nth call. No
test fills a real filesystem (Principle IV).

**Rationale**: the spec asks for a bounded recovery probe, not a recovery thread. Folding it into
the wait the writer already performs is the whole implementation.

---

## R-15: Hot-reloadable logger settings — two atomic setters

**Decision**: `logging.level` and `logging.console_enabled` are `Hot` in the schema table, so
`AsyncLogger` exposes `setLevel(Severity)` and `setConsoleEnabled(bool)`, both backed by
`std::atomic` and both safe from any thread. Every other logging field — `queue_capacity`,
`file_max_bytes`, `file_count` — is `Restart`, read once in `start()`, and changing it is rejected by
the reload pipeline (R-09).

**Rationale**: section 7.2's reload-policy column requires exactly these two to change at runtime.
The level is read before formatting, so the filter is one relaxed atomic load on the producer's
thread (BC-16 asserts no formatting happens below the level).

**Consequence**: the demo's config observer calls those two setters and nothing else; it does not
rebuild the logger on reload. That wiring is what makes scenario B observable.

---

## R-16: No defaults-merge layer — every schema key is mandatory

**Spec question**: section 7.2 "Defaults: loaded from a vetted baseline; malformed or missing
mandatory fields never silently become arbitrary zero values." BC-02 "Missing required field →
startup fails; no unsafe implicit defaults."

**Decision**: all 13 keys in `kFieldSpecs` are mandatory. There is no defaults layer and no
precedence merge in code. `configs/tcu.json` *is* the vetted baseline: an operator copies it and
edits it.

**Rationale**: the two requirements together say a missing mandatory field must fail and no default
may be substituted silently. With every key mandatory, both hold by construction and a whole merge
layer — plus the tests that would have to prove it cannot substitute a zero — does not exist.

**Not in v1**: command-line key overrides. Section 7.2's precedence sentence mentions
"command-line permitted overrides", but no goal in section 3 and no contract in section 13 requires
them, and an override path that bypasses the file is a second source of truth to validate and log.
The demo accepts `--config <path>` only.

---

## R-17: Timestamps — wall clock for output, monotonic for durations, injectable for tests

**Decision**: `LogRecord` carries both stamps, as section 10.1 requires. The JSON line's
`timestamp_utc` is ISO-8601 UTC with milliseconds (`2026-10-30T01:00:00.000Z`), formatted from
`system_clock`; durations and the recovery probe use `steady_clock` (AS-04). `LoggerOptions` holds a
`std::function<std::chrono::system_clock::time_point()>` clock, defaulting to `system_clock::now`,
so formatter tests assert exact lines instead of matching a timestamp pattern.

**Rationale**: AS-04 fixes the two clocks. The injectable one exists because a golden-line assertion
is stronger than a regex, and Principle IV asks for injectable time.

---

## R-18: Console output format — the same JSONL as the file

**Decision**: one formatter, used by both sinks. The console sink writes the same newline-delimited
JSON to stdout that the file sink writes to disk, with `console_enabled` as the only control.

**Rationale**: section 10.1 recommends NDJSON as "the structured output" without distinguishing
sinks, and the spec's own example lines (section 16) are JSONL. A second human-readable formatter
would be a second escaping implementation to keep correct, for a demo convenience nobody asked for.
Readability in the demo comes from `jq`, which `scripts/run_config_logging_demo.sh` pipes through.

---

## R-19: Project 03's `src/log.cpp` is not migrated in this project

**Context**: project 03's research R-16 records that its private stderr logger exists because
project 02's logging internals were unreachable, and names project 04 as the fix.

**Decision**: `tcu_logging` is built and is used by `tcu_config` and the demo. Project 03's
`modules/scheduler/src/log.cpp` is **left unchanged**.

**Rationale**: the migration is a change to a finished project's internals whose only evidence of
correctness would be project 04's tests. Project 03 has its own suite, its own sanitizer jobs and
its own acceptance criteria, and a change to its logging should be gated on those. It is also not in
project 04's scope: spec section 22 asks for scheduler integration as "debounce watcher and retry
sink recovery; update telemetry interval", not a logging retrofit.

**Consequence**: project 03's shortcut row stays open, now with a named replacement to adopt.
Carried forward in [plan.md](./plan.md) Deferred shortcuts so it is not lost.

---

## R-20: Event Bus and IPC integration — demo only, and only the generation number

**Spec question**: section 22's integration table.

**Decision**: `tcu_config` links neither `tcu_eventbus` nor `tcu_ipc`. The demo subscribes a config
observer that publishes a `ConfigChanged{generation}` event on project 01's bus, and one integration
test asserts the published event carries the generation and **no** configuration values. The
optional `GetConfigStatus` IPC request of section 22 is **not** built in v1: section 5's deliverables
do not list it, and it would make a Linux-only dependency out of a portable library (R-00).

**Rationale**: same pattern projects 02 and 03 used — the library stays dependency-light, the demo
does the wiring. Publishing only a generation is section 22's own rule ("do not publish sensitive
config payloads; consumers re-read snapshots") and keeps the bus free of a second copy of the config
that could go stale.

---

## R-21: Stress duration, soak scope and performance reporting

**Decision**: `TCU_CONFIG_STRESS_SECONDS`, default 5, bounds every stress case, as project 03 does.
`soak_test.cpp` runs the same loop and asserts bounded memory growth and a bounded file count; the
1-24 hour soak of section 17 is a manual run of that test with the environment variable raised, and
the quickstart says so. Throughput, reload latency percentiles and drop counts are **reported**, and
no CTest case fails on a latency or throughput threshold.

**Rationale**: section 18 calls its own numbers "engineering exercise budgets, not automotive OEM
requirements" and section 17 asks for a soak with a reported host profile. A CI job that fails
because a shared runner was busy teaches the team to re-run CI, not to fix performance. The
assertions that *do* run are the ones that cannot be explained away by a slow host: nothing lost,
nothing unbounded, no indefinite wait.
