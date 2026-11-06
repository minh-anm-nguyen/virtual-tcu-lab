# Contract: `decoder.json` Schema

**Feature**: `006-dbc-signal-decoder` | **Spec**: [../spec.md](../spec.md) sections 3.4 and 17

One table, read by three consumers: the validator checks presence, type and range; the loader fills
`DecoderConfig`; this document is the same rows for human readers. A test asserts that
`configs/decoder.json` agrees with the table (V-14), following project 04's precedent.

This sidecar is defined by this project, not by the DBC standard. It is parsed by `tcu_signal`'s own
loader rather than by `tcu_config`, for the reason recorded in [../research.md](../research.md) R-16.

## 1. Fields

| JSON pointer | Type | Range / legal values | Mandatory | Default in `configs/decoder.json` |
|---|---|---|---|---|
| `/schema_version` | int | 1..1 | yes | 1 |
| `/dbc_path` | string | 1..4096 chars | yes | `./configs/virtual_tcu.dbc` |
| `/strict` | bool | `true` only in v1 (R-02) | yes | true |
| `/max_dbc_bytes` | int | 1024..16777216 | yes | 2097152 |
| `/frame_mode` | enum | `classical_can` only in v1 (R-01) | yes | `classical_can` |
| `/payload_length_policy` | enum | `exact` only in v1 | yes | `exact` |
| `/rx_queue_capacity` | int | 16..65536 | yes | 4096 |
| `/decoder_workers` | int | 1..1 (ADR-05) | yes | 1 |
| `/reload/enabled` | bool | | yes | true |
| `/reload/debounce_ms` | int | 0..60000 | yes | 300 |
| `/reload/max_attempts_per_min` | int | 1..600 | yes | 6 |
| `/signal_freshness/default_timeout_ms` | int | 1..3600000 | yes | 500 |
| `/signal_freshness/scan_period_ms` | int | 1..60000 | yes | 50 |
| `/signal_freshness/overrides` | object of int | values 1..3600000 | yes (may be empty) | empty |
| `/invalid_raw_codes` | object of int array | each value fits uint64 | yes (may be empty) | VehicleSpeed: [65535] |
| `/range_enforcement` | object of bool | | yes (may be empty) | VehicleSpeed: true |
| `/event_publish/mode` | enum | `sync`, `async` | yes | `async` |
| `/event_publish/on_full` | enum | `drop_and_count`, `block_bounded` | yes | `drop_and_count` |
| `/logging/rate_limit_per_reason_per_min` | int | 0..100000 | yes | 60 |

Every key is mandatory: there is no defaults-merge layer, so a missing key fails the load. This is
project 04's R-16 rule, kept for the same reason - a typo in a key name must not silently restore a
default.

## 2. Keys of the signal-name maps

`/signal_freshness/overrides`, `/invalid_raw_codes` and `/range_enforcement` are keyed by
`MESSAGE.Signal`, the fully qualified name as spec section 17 shows. A key that matches no signal in
the loaded DBC produces a startup warning and increments a counter; it is never a silent no-op
(V-16). When two buses carry the same message name, the key must be written
`bus<id>:MESSAGE.Signal`; without the prefix the entry applies to bus 0 only.

`/invalid_raw_codes` values are **raw**, pre-scaling integers (research R-05).

## 3. Validation errors

```text
decoder.json: /rx_queue_capacity: OutOfRange (allowed 16..65536)
decoder.json: /frame_mode: UnsupportedValue
decoder.json: /telemetry: UnknownKey
decoder.json: /signal_freshness/default_timeout_ms: WrongType (expected int)
```

- The error names the JSON pointer and the kind, never the value (V-17, Principle V).
- Unknown keys are rejected, at any depth (V-14).
- Errors are reported for the whole file, not only the first, so one run fixes many problems.
- A config load failure at startup leaves the service `NotReady`; it never starts with a partial
  configuration.

## 4. Relationship to the DBC file

`decoder.json` is validated first, then `dbc_path` is loaded. `max_dbc_bytes` is checked against
the file size before the file is read, and the path is checked with `symlink_status` and rejected
unless it is a regular file (research R-08).

A reload re-reads **only** the DBC file. `decoder.json` itself is read once at startup in v1;
changing a limit or a timeout requires a restart, and the quickstart says so.

## 5. Not in v1

Environment-variable overrides; command-line overrides of individual fields; a defaults layer; hot
reload of the sidecar; per-bus sections; multiple DBC paths; secrets of any kind (the file holds
none, and none may be added - Principle V).
