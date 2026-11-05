# Contract: Configuration Schema

**Feature**: `004-configuration-logging-service` | Source of truth in code:
`modules/config/include/tcu/config/config_schema.hpp`

This is the deliverable spec section 5 calls `config/schema.md`. It and `kFieldSpecs` hold the same
13 rows (R-04); `Schema_SampleFileMatchesTable` asserts `configs/tcu.json` contains exactly these
pointers, and `Schema_EveryFieldHasReloadPolicy` asserts no row can exist without a policy.

---

## 1. The field table

Schema version 1. Every key is **mandatory**: there is no defaults layer, so a missing key fails
startup and fails a reload (R-16, BC-02).

| JSON pointer | Type | Allowed values | Reload policy |
|---|---|---|---|
| `/schema_version` | int | exactly `1` | Restart (migration) |
| `/connectivity/retry_base_ms` | int | 100 .. 60000 | Hot |
| `/connectivity/retry_max_ms` | int | 100 .. 300000, and >= `retry_base_ms` | Hot |
| `/connectivity/max_attempts` | int | 0 .. 100 (`0` = unlimited policy-controlled attempts, not a busy loop) | Hot |
| `/telemetry/period_ms` | int | 200 .. 3600000 | Hot |
| `/telemetry/offline_queue_capacity` | int | 16 .. 100000 | Restart |
| `/logging/level` | enum | `TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`, `FATAL` | Hot |
| `/logging/queue_capacity` | int | 128 .. 65536 | Restart |
| `/logging/file_max_bytes` | int | 1048576 .. 104857600 (1 MiB .. 100 MiB) | Restart |
| `/logging/file_count` | int | 1 .. 20 | Restart |
| `/logging/console_enabled` | bool | `true`, `false` | Hot |
| `/network/cloud_endpoint` | string | scheme `http` or `https`, non-empty host, total length <= 255 | Restart (v1) |
| `/network/tls_required` | bool | `true`, `false`; must be `true` unless the host is loopback or `.local` | Restart |

Hot keys: 6. Restart keys: 7. A candidate that changes any Restart key is rejected whole, including
its Hot changes (R-09, BC-08).

### Reload policy meanings

| Policy | Meaning |
|---|---|
| `Hot` | The value may change in a running process. A committed change is published as a new generation and observers are notified. |
| `Restart` | The value is read once at startup. A reload whose candidate differs in this key is rejected with `RestartRequired`; the operator restarts the process to adopt it. |

Who applies a Hot change: `logging.level` and `logging.console_enabled` are applied by the demo's
observer through `AsyncLogger::setLevel` and `setConsoleEnabled` (R-15); `telemetry.period_ms` by
cancelling and rescheduling the telemetry timer; `connectivity.*` by the connectivity worker on its
next retry decision. The library applies nothing on a consumer's behalf.

---

## 2. Reference file

`configs/tcu.json` — the vetted baseline (R-16). Spec section 7.1, with no secret in it and none
ever added:

```json
{
  "schema_version": 1,
  "connectivity": {
    "retry_base_ms": 1000,
    "retry_max_ms": 30000,
    "max_attempts": 0
  },
  "telemetry": {
    "period_ms": 5000,
    "offline_queue_capacity": 2048
  },
  "logging": {
    "level": "INFO",
    "queue_capacity": 8192,
    "file_max_bytes": 10485760,
    "file_count": 5,
    "console_enabled": true
  },
  "network": {
    "cloud_endpoint": "https://mock-cloud.local",
    "tls_required": true
  }
}
```

Test fixtures, each differing from the baseline in exactly one way so the rejected reason is
unambiguous:

| File | Difference | Expected result |
|---|---|---|
| `configs/invalid_retry.json` | `retry_max_ms` 500 with `retry_base_ms` 1000 | `InvalidValue`, cross-field reason (BC-05) |
| `configs/invalid_schema.json` | `schema_version` 2 | `UnsupportedVersion` (BC-28) |
| `configs/restart_only.json` | `queue_capacity` 4096 **and** `level` `DEBUG` | `RestartRequired`; the `DEBUG` change is not applied (BC-08) |

---

## 3. Parse rules (R-01)

| Rule | Violation |
|---|---|
| Size is probed before reading; > `maxFileBytes` (1 MiB default) is rejected unparsed | `TooLarge` (BC-15) |
| Strict JSON: no comments, no trailing commas, no NaN/Infinity, no single quotes, no trailing data | `ParseError` |
| Nesting depth <= 8 | `ParseError` |
| A duplicate key inside one object is rejected | `ParseError` |
| A truncated or partially written file is rejected; the previous snapshot stays in force | `ParseError` (BC-14) |
| Input must be valid UTF-8 | `ParseError` |
| Numbers must be integers where the table says int; `5.0` is not an int | `SchemaError` |
| Any key absent from the table | `SchemaError` (R-05) |
| Any table key missing from the file | `SchemaError` |

Order of checks: size, read, parse, `schema_version`, per-field type, per-field range, cross-field.
The first failure wins and reporting stops there, so one reason is returned, not a list.

---

## 4. Reason templates (R-10)

`ReloadResult::publicReason` is built only from these. **No template interpolates a value read from
the file**, and the JSON library's own message is never forwarded, because it quotes input.

| Template | Used for |
|---|---|
| `missing required key <pointer>` | absent mandatory key |
| `wrong type at <pointer>: expected <type>` | type mismatch |
| `out of range at <pointer>: allowed <min>..<max>` | range violation |
| `unknown key <pointer>` | key not in the table |
| `cross-field: <pointer> must be >= <pointer>` | retry ordering |
| `cross-field: <pointer> must be true for a nonlocal endpoint` | TLS policy |
| `unsupported schema_version: expected 1` | version mismatch |
| `file exceeds <N> bytes` | oversize |
| `parse error at byte offset <N>` | malformed JSON |
| `duplicate key <pointer>` | duplicate key |
| `cannot read configured config path` | open or read failure |
| `restart-required key changed: <pointer>` | a Restart-policy key moved, so the whole candidate is rejected (BC-08) |
| `service stopped` | reload() called before start() or after stop() |

Adding a template is a change to this document, not a local decision;
`Redaction_ReasonStringsNeverQuoteInput` asserts the set.

---

## 5. Error to metric mapping

`config_reload_failures_total{reason}` uses the `LoadError` enumerator name, so the label set is
closed and bounded: `FileNotFound`, `ReadError`, `TooLarge`, `ParseError`, `SchemaError`,
`InvalidValue`, `RestartRequired`, `UnsupportedVersion`, `ServiceStopped`. `None` never appears as a
failure label.

---

## 6. Changing the schema later

1. Add or edit the row in `config_schema.hpp` **and** in section 1 of this document.
2. Choose a `ReloadPolicy` deliberately; there is no default (C7).
3. Update `configs/tcu.json`, because every key is mandatory and `Schema_SampleFileMatchesTable`
   compares the two.
4. A change to the *meaning* of an existing key needs a `schema_version` bump, which is a Restart key
   by definition, so running processes reject the new file instead of misreading it (BC-28).
