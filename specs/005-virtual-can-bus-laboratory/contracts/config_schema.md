# Contract: `can_lab.json` Configuration Schema

**Feature**: `005-virtual-can-bus-laboratory` | **Date**: 2026-11-05 | **Source**: spec sections 13.1, 22, 24

Loaded once at startup by `modules/can/src/config_loader.cpp` and validated against the `constexpr`
table in `src/can_config_schema.hpp` before any socket or thread exists (section 13.1 step 2). There
is no hot reload in V1.

**Why this project loads its own config instead of using `tcu_config`:** see [research.md](../research.md)
R-06 and [plan.md](../plan.md) Complexity Tracking. In short, project 04's `ConfigSnapshot` is
hard-typed to its own four structs and its schema table is a flat 13-row array of its own keys;
`signals` here is a map of per-signal objects, which that table cannot express.

---

## 1. Key table

Every key is **mandatory**. There is no defaults layer, and an unknown key is a load error, not an
ignored one — the same discipline project 04 adopted in its R-05 and R-16.

| JSON pointer | Type | Range / allowed values | Rule |
|---|---|---|---|
| `/schema_version` | int | `1` | Any other value is `UnsupportedVersion`, not `InvalidValue` — the two must be distinguishable |
| `/mode` | enum | `"lab_only"` | The **only** value V1 parses. `"unrestricted"` does not exist in V1, so there is no runtime path to a physical interface (R-16) |
| `/can/interface` | string | 1-15 chars, must match `vcan[0-9]*` | 15 is `IFNAMSIZ - 1`. The pattern check is what the section 24 risk table asks for |
| `/can/format` | enum | `"classic_standard_11bit"` | CAN FD is section 10.5's V2 extension (R-24) |
| `/can/rx_queue_capacity` | int | `1 .. 65536` | NFR-02 forbids 0 and forbids unbounded |
| `/can/health_check_ms` | int | `1 .. 10000` | The `FixedDelay` period of the freshness timer (R-04) |
| `/can/drain_deadline_ms` | int | `0 .. 5000` | Shutdown drain budget (R-19) |
| `/can/reconnect/initial_ms` | int | `1 .. 300000` | Must be `<= reconnect/max_ms` — cross-field rule |
| `/can/reconnect/max_ms` | int | `1 .. 300000` | Must be `>= reconnect/initial_ms` |
| `/can/reconnect/jitter_percent` | int | `0 .. 50` | Applied as plus-or-minus that percentage on each backoff interval |
| `/can/allowed_rx_ids` | int array | 1-16 entries, each `0x000 .. 0x7FF` | Every entry must be a `ToTcu` row of the frame catalog. An ID not in the catalog is a load error, because a filter for a frame no decoder understands is a configuration mistake, not a feature |
| `/signals/<name>/can_id` | int | `0x000 .. 0x7FF` | Must equal the catalog's `source_id` for that signal. A mismatch is a load error — the catalog, not the config, defines where a signal lives |
| `/signals/<name>/timeout_ms` | int | `1 .. 600000` | Overrides the catalog default for this signal |
| `/simulator/speed_period_ms` | int | `10 .. 10000` | `vehicle_simulator` only |
| `/simulator/heartbeat_period_ms` | int | `100 .. 60000` | `vehicle_simulator` only |
| `/simulator/fixed_seed` | int | `0 .. 2147483647` | Seeds `std::mt19937` for reproducibility (NFR-09, R-21) |

`<name>` must be one of the seven signal names: `ignition`, `speed_kmh`, `battery_voltage_v`,
`battery_soc_pct`, `door_state`, `heartbeat`, `ecu_status`. A name not in the catalog is a load
error. All seven must be present.

`ecu_status` is not in the spec's section 22 sample, which lists six signals; the reason it exists
is in [data-model.md](../data-model.md) section 3.2 and it is recorded in Complexity Tracking.

---

## 2. Sample file

`config/can_lab.json`, which `ConfigLoader_SampleFileMatchesTable` asserts agrees with the schema
table above — the same test project 04 wrote for its own sample.

```json
{
  "schema_version": 1,
  "mode": "lab_only",
  "can": {
    "interface": "vcan0",
    "format": "classic_standard_11bit",
    "rx_queue_capacity": 1024,
    "health_check_ms": 50,
    "drain_deadline_ms": 200,
    "reconnect": {
      "initial_ms": 250,
      "max_ms": 5000,
      "jitter_percent": 10
    },
    "allowed_rx_ids": [256, 257, 258, 259, 272, 513]
  },
  "signals": {
    "ignition":          { "can_id": 256, "timeout_ms": 3000 },
    "speed_kmh":         { "can_id": 257, "timeout_ms": 500  },
    "battery_voltage_v": { "can_id": 258, "timeout_ms": 3000 },
    "battery_soc_pct":   { "can_id": 258, "timeout_ms": 3000 },
    "door_state":        { "can_id": 259, "timeout_ms": 3000 },
    "heartbeat":         { "can_id": 272, "timeout_ms": 3000 },
    "ecu_status":        { "can_id": 272, "timeout_ms": 3000 }
  },
  "simulator": {
    "speed_period_ms": 100,
    "heartbeat_period_ms": 1000,
    "fixed_seed": 42
  }
}
```

The decimal identifiers are the section 22 values: `0x100 = 256`, `0x101 = 257`, `0x102 = 258`,
`0x103 = 259`, `0x110 = 272`, `0x201 = 513`. These timeouts and ranges are **lab example
configuration**, not production requirements (section 6.3).

---

## 3. Scenario files

`config/scenarios/*.json`, read by `vehicle_simulator`. Three ship with the project, named by
section 20: `normal_drive.json`, `speed_signal_loss.json`, `invalid_battery.json`.

| JSON pointer | Type | Rule |
|---|---|---|
| `/schema_version` | int | `1` |
| `/name` | string | 1-64 chars, used in logs |
| `/fixed_seed` | int | `0 .. 2147483647`; overrides `/simulator/fixed_seed` |
| `/steps` | array | 1-1024 entries, ordered by `at_ms` |
| `/steps[]/at_ms` | int | `0 .. 3600000`, monotonically non-decreasing across the array. An offset in milliseconds from scenario start on the monotonic clock — not a date |
| `/steps[]/action` | enum | `set_signal`, `pause_id`, `resume_id`, `inject_invalid` |
| `/steps[]/signal` | string | required for `set_signal`; a catalog signal name |
| `/steps[]/value` | number | required for `set_signal`; **not** range-checked, so a scenario can deliberately emit an out-of-range value for B-11 |
| `/steps[]/can_id` | int | required for `pause_id`, `resume_id`, `inject_invalid` |

```json
{
  "schema_version": 1,
  "name": "speed_signal_loss",
  "fixed_seed": 42,
  "steps": [
    { "at_ms": 0,    "action": "set_signal", "signal": "ignition",  "value": 1  },
    { "at_ms": 500,  "action": "set_signal", "signal": "speed_kmh", "value": 60 },
    { "at_ms": 2000, "action": "pause_id",   "can_id": 257 },
    { "at_ms": 4000, "action": "resume_id",  "can_id": 257 }
  ]
}
```

The assertion B-29 makes about a replay is on the **value sequence per CAN ID**, not on wall-clock
arrival times (R-21).

The same four actions are the IPC methods `can_control_cli` calls at runtime (R-08), so a scenario
file and the CLI drive one code path rather than two.

---

## 4. Error reporting

```cpp
enum class LoadError {
  None, FileNotFound, ReadError, TooLarge, ParseError,
  SchemaError,          // unknown key, missing key, wrong type
  InvalidValue,         // out of range, or a cross-field rule failed
  UnsupportedVersion,   // schema_version != 1
};

struct LoadResult {
  LoadError error{LoadError::None};
  std::string pointer;        // RFC 6901, e.g. "/can/rx_queue_capacity"
  std::string publicReason;   // from a closed template set; never the offending value
};
```

**`publicReason` never echoes a value from the file.** It is built from a fixed set of templates
(`out_of_range`, `wrong_type`, `unknown_key`, `missing_key`, `cross_field`, `not_in_catalog`,
`interface_not_lab`, `unsupported_version`), which is project 04's R-10 rule applied here: an error
message is a log line, and a log line must not carry unvalidated input (Constitution Principle V).

`TooLarge` caps the file at 256 KiB so a malformed or hostile file cannot be read into memory
unbounded.

**Failure behaviour:** `CanService::start()` returns `false`, the state stays `Stopped`, and no
socket or thread was ever created. A partial configuration is never applied (section 12.1 policy).
