# Contract: Metrics, Logs and Health

**Feature**: `006-dbc-signal-decoder` | **Spec**: [../spec.md](../spec.md) section 16

## 1. Metrics

Spec section 16.1's table is normative. Two additions, each because an invariant is otherwise
unobservable:

| Metric | Type | Added for |
|---|---|---|
| `decoder_observer_failures_total` | counter | V-08, B-14: an observer threw and was isolated |
| `decoder_out_of_order_total` | counter | S-02: a frame older than the stored last-good arrived |

## 2. Label sets (closed, per section 16.1 cardinality rule)

| Label | Values |
|---|---|
| `result` on `decoder_frames_total` | ok, unknown_id, wrong_length, wrong_format, error_frame, remote_frame, decode_error |
| `result` on `dbc_load_attempts_total` | ok, rejected, busy, rate_limited, source_error |
| `quality` on `decoder_signals_total` | not_received, valid, invalid_raw, out_of_range, stale, decode_error, not_active, config_unavailable |
| `bus` | the bus ids named in the configuration |

**No metric carries a signal or message name as a label.** A DBC is untrusted input with up to
10,000 signals, so a name label is unbounded cardinality (section 16.1). Per-signal stale and
recovered counts exist, keyed by the compiled numeric signal index, and are exposed through
`DecoderService::metrics()` as a bounded vector - not as labels.

## 3. Log events

| `event` | Severity | Fields beyond the common set |
|---|---|---|
| `decoder_ready` | INFO | `dbc_version`, `dbc_hash`, `messages`, `signals` |
| `decoder_not_ready` | ERROR | `reason`, first 3 diagnostics |
| `frame_rejected` | WARN | `reason`, `can_id`, `format`, `expected_len`, `actual_len` |
| `unknown_message` | WARN | `can_id`, `format`, `bus` |
| `unknown_mux_value` | WARN | `message`, `selector_raw` |
| `signal_quality_changed` | INFO | `signal_id`, `from`, `to` |
| `reload_requested` | INFO | `trigger` |
| `reload_committed` | INFO | `from_version`, `to_version`, `dbc_hash`, `signals_reset` |
| `reload_rejected` | WARN | `reason`, `path`, `line` |
| `observer_failed` | ERROR | `subscription_id`, `what` |
| `queue_overflow` | WARN | `dropped_total`, `capacity` |
| `decoder_stopped` | INFO | `drained`, `discarded`, `forced` |

Common set: `ts` (wall clock, ISO-8601 UTC, for humans only), `severity`, `module`
(`dbc_decoder`), `event`. The format is the newline-delimited JSON of `tcu_logging`, unchanged.

- Freshness and timeout logic never reads a wall clock (research R-06); `ts` is for reading, not
  for deciding.
- `frame_rejected`, `unknown_message`, `unknown_mux_value` and `reload_rejected` pass through the
  per-reason token bucket of research R-17. A line emitted after suppression carries
  `suppressed_since_last`, so the gap is visible.
- No log line contains a payload byte, a file's contents, or a path beyond the configured
  `dbc_path` as written (D-02, Principle V).

## 4. Conservation equations

Assertable arithmetic, not prose (project 04's `Metrics_ConservationAfterFlood` precedent):

- **M-01** `can_frames_received_total == sum(decoder_frames_total) + decoder_queue_drops_total`
- **M-02** `decoder_frames_total{result="ok"} == batches_published + batches_dropped_by_policy`
- **M-03** `dbc_active_version` never decreases, and equals
  `dbc_load_attempts_total{result="ok"}` when no reload was rejected after a commit
- **M-04** `decoder_signals_total{quality="valid"} == sum of per-signal epoch increments`
- **M-05** every counter is monotonic and `decoder_queue_depth <= rx_queue_capacity`

A stress run asserts all five after a burst, so "no frame went unaccounted for" is arithmetic a
test checks rather than a claim a reviewer has to believe.

## 5. Health

`DecoderService::state()` plus `schema()` is the health surface: `Ready` with a version and a hash,
or `NotReady` with the reason. There is no HTTP endpoint and no remote status in v1 - spec
section 3.3 keeps those out, and projects 13/14 own telemetry export.
