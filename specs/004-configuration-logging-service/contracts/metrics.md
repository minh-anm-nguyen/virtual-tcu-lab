# Contract: Metrics, Health and Log Fields

**Feature**: `004-configuration-logging-service` | Spec section 16

Two by-value snapshot structs, following project 03's `SchedulerMetrics`: atomics inside, a plain
struct out, no registry, no runtime label strings. Every label below comes from a closed set, so
cardinality is bounded by construction (M4) and no path, VIN or correlation id can become one.

---

## 1. `ConfigMetrics`

```cpp
struct ConfigMetrics {
  // gauges
  std::uint64_t generation{0};
  std::uint64_t notificationQueueDepth{0};
  bool notifyDegraded{false};
  bool configValid{false};

  // counters
  std::uint64_t reloadAttemptsTotal{0};
  std::uint64_t reloadSuccessChangedTotal{0};
  std::uint64_t reloadSuccessNoopTotal{0};
  std::array<std::uint64_t, kLoadErrorCount> reloadFailuresTotal{};  // indexed by LoadError
  std::uint64_t notifyFailuresTotal{0};
  std::uint64_t notifyDeliveredTotal{0};
  std::uint64_t resyncNotificationsTotal{0};
};
```

| Spec name (section 16) | Field | Meaning |
|---|---|---|
| `config_generation` | `generation` | the generation currently in force; 0 before the first publish |
| `config_reload_attempts_total` | `reloadAttemptsTotal` | every `reload()` that reached stage 1, poller-driven or explicit |
| `config_reload_success_total` | `reloadSuccessChangedTotal` + `reloadSuccessNoopTotal` | split, because section 16 asks to "tag separately" |
| `config_reload_failures_total{reason}` | `reloadFailuresTotal[reason]` | indexed by `LoadError`; the label is the enumerator name |
| `config_notify_failures_total` | `notifyFailuresTotal` | observer exceptions **plus** notification-queue overflows |
| `config_notification_queue_depth` | `notificationQueueDepth` | pending notifications, read from the scheduler's ready-queue depth |
| — | `notifyDegraded` | latches on the first overflow; section 9 rule 7's "degraded health" |
| — | `configValid` | scenario A's health line; true iff a snapshot is in force |
| — | `resyncNotificationsTotal` | resync deliveries (R-06); separable from normal ones for diagnosis |

`config_generation_applied{component}` is **not** a library metric. It belongs to the consumer that
applied the change — the library cannot know when a component finished. The demo exposes it for
telemetry and connectivity, and section 13's test-oracle note is the reason it exists at all.

---

## 2. `LoggerMetrics`

```cpp
struct LoggerMetrics {
  // gauges
  std::uint64_t queueDepth{0};
  std::array<SinkHealth, kSinkCount> sinkHealth{};     // indexed by sink: console, file

  // counters
  std::uint64_t recordsSubmittedTotal{0};
  std::uint64_t recordsFilteredTotal{0};
  std::uint64_t recordsEnqueuedTotal{0};
  std::array<std::uint64_t, kSeverityCount> recordsDroppedTotal{};   // indexed by Severity
  std::uint64_t highPriorityDropsTotal{0};
  std::uint64_t recordsTruncatedTotal{0};
  std::uint64_t formatFailuresTotal{0};
  std::uint64_t fatalEmergencyTotal{0};
  std::uint64_t reentrantEmergencyTotal{0};
  std::uint64_t rejectedAfterStopTotal{0};
  std::uint64_t notDrainedAtStopTotal{0};
  std::array<std::uint64_t, kSinkCount> recordsWrittenTotal{};
  std::array<std::uint64_t, kSinkCount> sinkErrorsTotal{};
  std::uint64_t rotationsTotal{0};
  Histogram writeLatencyMs{};                                        // tcu_logging's own; see note
};
```

| Spec name (section 16) | Field |
|---|---|
| `logger_queue_depth` | `queueDepth` |
| `logger_records_written_total{sink}` | `recordsWrittenTotal[sink]` |
| `logger_records_dropped_total{severity}` | `recordsDroppedTotal[severity]` |
| `logger_sink_errors_total{sink}` | `sinkErrorsTotal[sink]` |
| `logger_write_latency_ms` | `writeLatencyMs` |
| `logger_rotations_total` | `rotationsTotal` |

The rest are the counters the behavioural contracts need in order to be observable: BC-18 needs
`highPriorityDropsTotal`, BC-29 needs `recordsTruncatedTotal`, BC-26 needs
`reentrantEmergencyTotal`, BC-23 needs `rejectedAfterStopTotal` and `notDrainedAtStopTotal`, and L9
needs `formatFailuresTotal`.

`writeLatencyMs` uses a `Histogram` **declared in `logger_metrics.hpp`**, with the same shape as
project 03's (nine fixed buckets, `count`, `sum_ms`) so the stress report's percentile code works on
both. It is not included from `tcu/scheduler/scheduler_metrics.hpp`, even though that header is
public: doing so would make `tcu_logging` — the one target every other project should be able to
link freely — depend on the timer library, and the dependency runs the wrong way, since project 03
is a future *consumer* of this logger (R-19). Duplicating a three-field POD is the cheaper of the
two, and Principle II's concern is re-implementing behaviour, of which this struct has none.

---

## 3. Label sets

| Label | Closed set | Size |
|---|---|---|
| `severity` | `TRACE, DEBUG, INFO, WARN, ERROR, FATAL` | 6 |
| `sink` | `console, file` | 2 |
| `reason` | the `LoadError` enumerator names except `None` | 9 |
| `component` (only on `config_generation_applied`, demo-side) | the demo's fixed component names | 2 |

Anything else — a file path, a JSON pointer, a correlation id, an endpoint — is **never** a label.
Pointers appear inside a log line's `fields`, which is not a metric dimension.
`Metrics_LabelsAreClosedSets` asserts each array's length equals its enum's size, so a new
enumerator without a new slot fails that test.

---

## 4. Conservation equations

These turn "nothing was lost" into arithmetic a test can assert. All are checked after the stress
cases, where the counts are large and the races are real.

**Config**

```text
M1  reloadAttemptsTotal
      == reloadSuccessChangedTotal + reloadSuccessNoopTotal + sum(reloadFailuresTotal)

M2  generation == snapshot()->generation            (and 0 when snapshot() is null)

M5  reloadSuccessChangedTotal == generation - 1     (after a successful start, with no restart)

M6  notifyDeliveredTotal + notifyFailuresTotal
      >= reloadSuccessChangedTotal                  (>= because startup and resync also deliver)
```

**Logger**

```text
M3  recordsSubmittedTotal
      == recordsFilteredTotal + recordsEnqueuedTotal + sum(recordsDroppedTotal)
          + rejectedAfterStopTotal + reentrantEmergencyTotal

M7  recordsEnqueuedTotal
      == records handed to the sinks + notDrainedAtStopTotal + formatFailuresTotal

M8  sum(recordsWrittenTotal) counts successful sink writes only; a failed write
    increments sinkErrorsTotal[sink] and not recordsWrittenTotal[sink]

M9  queueDepth == 0 after flush() returns, and after stop() completes
```

`highPriorityDropsTotal` is a subset of `recordsDroppedTotal[WARN] + recordsDroppedTotal[ERROR]`,
not an addition to it, so it does not appear in M3.

---

## 5. Health line

The demo prints one JSONL health record per status request, built from both snapshots. Scenario A
requires `config_valid=true`, `logger_healthy=true`, `generation=1`:

```jsonl
{"timestamp_utc":"2026-10-30T01:00:00.000Z","severity":"INFO","component":"demo","event":"health","fields":{"config_valid":true,"generation":1,"notify_degraded":false,"logger_healthy":true,"queue_depth":0,"dropped_total":0,"file_sink":"healthy"}}
```

`logger_healthy` is true when every sink is `Healthy`. No configuration *value* appears in the
health line (section 16: "do not leak sensitive config values") — only the generation and flags.

---

## 6. Log field vocabulary

Structured field keys the two libraries emit. A closed set, so a query can rely on them and no
secret can hide in an ad-hoc key:

| Key | Type | Emitted by |
|---|---|---|
| `generation` | int | config, on `startup_loaded`, `reload_committed`, `resync` |
| `reason` | string | config, on `reload_rejected`; a `LoadError` name |
| `field` | string | config, on `reload_rejected`; a JSON pointer, never a value |
| `subscription_id` | int | config, on `observer_failed` |
| `count` | int | logger, on `queue_drops_detected` |
| `sink` | string | logger, on `sink_error`, `sink_recovered`, `rotated` |
| `truncated` | bool | logger, on any truncated record (top level, not in `fields`) |

Event names, also a closed set: `startup_loaded`, `startup_failed`, `reload_committed`,
`reload_rejected`, `reload_noop`, `resync`, `observer_failed`, `notify_overflow`,
`queue_drops_detected`, `sink_error`, `sink_recovered`, `rotated`, `shutdown_drained`, `health`.

Who emits what. `tcu_config` emits its own lines through the injected logger. The **logger does not
emit its own**: logging from the writer thread is the reentrancy the guard exists to stop (BC-26),
so it reports through metrics and the application turns the deltas into `queue_drops_detected`,
`sink_error`, `sink_recovered` and `rotated` - one line per change, never one per record. The demo
`LoggerWatch` is that component, and `health` and `shutdown_drained` are the demo too.

There is **no** `payload`, `value` or `config` field. A log line can carry a generation, a pointer
and a reason, and that is deliberately all the configuration content it can carry.
