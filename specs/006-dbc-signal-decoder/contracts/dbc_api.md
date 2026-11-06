# Contract: `tcu_dbc` Public API

**Feature**: `006-dbc-signal-decoder` | **Spec**: [../spec.md](../spec.md) section 8

`tcu_dbc` is pure: no thread, no file watcher, no clock, no logging dependency on the hot path. It
parses a DBC file into an immutable database and decodes one frame against it. Everything with a
lifecycle lives in `tcu_signal` ([signal_api.md](./signal_api.md)).

Headers: `include/tcu/dbc/{dbc_types,frame_view,dbc_parser,dbc_database,bit_extractor,signal_decoder}.hpp`.
Namespace `tcu::dbc`. The types of spec section 8 are provided as written; the additions below are
listed in [../plan.md](../plan.md) Complexity Tracking.

## 1. Types added beyond spec section 8

| Type | Why it is needed |
|---|---|
| `ByteOrder { Intel, Motorola }` | section 6.1 distinguishes `@1` and `@0`; the enum keeps `bool littleEndian` out of every signature |
| `MuxRole { None, Selector, Branch }` | FR-09 and D-08 need the three cases named |
| `DbcErrorCode` | section 6.2 defines the codes; FR-15 requires them to be machine-classifiable (NFR-10) |
| `ParseDiagnostic` | section 6.3 names the exact shape |
| `LoadResult { std::shared_ptr<const CompiledDbc> db; std::vector<ParseDiagnostic> diagnostics; }` | `bool` cannot carry diagnostics, and `loadInitial` must report all of them |
| `SignalMeta` | unit, bounds, enum map and semantic hash have to be queryable without re-parsing (R-09) |

`DecodeError` gains no value; `SignalQuality` gains no value.

## 2. Parsing and compiling

```cpp
struct ParseLimits { /* section 3.4 defaults */ };

// Reads the file, validates it, compiles bit plans, computes the SHA-256 of the bytes.
// Never throws for a malformed or hostile file: every failure is a diagnostic.
LoadResult loadDbcFile(const std::string& path, const ParseLimits& limits = {});

// Same, for bytes already in memory: the fuzz targets and every unit test use this one.
LoadResult loadDbcBytes(std::string_view text, std::string_view fileLabel,
                        const ParseLimits& limits = {});
```

**Preconditions**: none; any `path` and any byte sequence are legal inputs.

**Postconditions**:
- `db != nullptr` if and only if no diagnostic has `Severity::Error` (D-03).
- `db->version == 1` and `db->hash` is the SHA-256 hex digest of the exact bytes read (P-09).
- Diagnostics are ordered by `(line, column)`.
- No allocation exceeds the `ParseLimits` bound that governs it (D-12).
- `loadDbcFile` rejects a non-regular file or a symlink with `DBC_SOURCE` before opening it (R-08).

**Thread safety**: both are pure functions of their input; any number may run concurrently.

## 3. Querying the database

```cpp
const CompiledMessage* CompiledDbc::find(uint32_t busId, FrameFormat, uint32_t arbitrationId) const;
const SignalMeta*      CompiledDbc::findSignal(const SignalKey&) const;
Stats                  CompiledDbc::stats() const;
```

- `find` is O(1) and never conflates `Standard11` with `Extended29` for the same number (P-08, B-07).
- Every accessor is `const`; `CompiledDbc` has no non-`const` member function at all (P-07).
- Returned pointers are valid for as long as the caller holds its `shared_ptr`.

## 4. Decoding

```cpp
class SignalDecoder : public ISignalDecoder {
public:
  explicit SignalDecoder(std::shared_ptr<const CompiledDbc> db, DecodePolicy policy);
  DecodeResult decode(const CanFrameView& frame) const override;
};
struct DecodePolicy {
  PayloadLengthPolicy length = PayloadLengthPolicy::Exact;
  const InvalidRawTable*   invalidRaw   = nullptr;   // research R-05, borrowed, may be null
  const RangeEnforcement*  rangeEnforce = nullptr;   // research R-04, borrowed, may be null
};
```

**Preconditions**: `frame.data` is non-null when `frame.size > 0`, and addresses at least
`frame.size` readable bytes. A null `data` with a non-zero `size` is the one programming error this
API does not absorb; it is an assertion in a debug build, and the fuzz target never produces it.

**Postconditions**, in evaluation order:

1. `isError` -> `ErrorFrame`; `isRemote` -> `RemoteFrame`; neither reads the payload (P-11).
2. `size > 8` -> `WrongLength` (R-01).
3. no message for `(busId, format, id)` -> `UnknownMessage`, `batch` empty, nothing counted against
   any signal (FR-13, B-07).
4. `Exact` policy and `size != expectedLength` -> `WrongLength`, no signal decoded (P-12, B-06).
5. selector present and undecodable -> unconditional signals are emitted, the branch is skipped,
   and the batch is marked `muxUnresolved` (P-15, B-20).
6. otherwise a `DecodedBatch` with the unconditional signals plus the matching branch, each signal
   carrying `raw`, `signedRaw` when signed, `physical`, `unit`, `enumLabel` when the raw value has
   a label, and a per-signal `SignalQuality` (P-14).
7. a non-finite physical value makes that one signal `DecodeError`; the rest of the batch stands
   (P-05, spec section 10.3 partial-batch rule).
8. `batch->dbcVersion` and `batch->dbcHash` come from the database this decoder holds, so a batch
   has exactly one schema identity (P-13).

**Thread safety**: `decode` is `const` and reads only its arguments and the immutable database, so
any number of threads may call it on one instance. V1 calls it from one worker anyway (ADR-05).

**Exceptions**: none for any frame content. `std::bad_alloc` from the result vector is the only
exception that can propagate, and the caller treats it as `InternalError`.

**Allocation**: one vector of signals per batch. No allocation in the bit loop. No string is copied
per frame; `messageName` and `unit` are `std::string` copies in v1, with the interning option from
spec section 8 left for after the benchmark (R-07).

## 5. CLI surface

- `dbc_validate <file.dbc> [--limits-json <file>]` -> prints every diagnostic in the section 5
  format of [dbc_subset.md](./dbc_subset.md); exit 0 clean, 1 rejected, 2 usage/IO error.
- `dbc_decode --dbc <file.dbc> (--hex <id>:<bytes> | --candump <file>) [--json]` -> prints one line
  per signal, or a JSON batch with `--json`; exit 0 all frames decoded, 1 if any frame produced a
  `DecodeError`, 2 usage/IO error.

Both are deterministic for identical input: no wall-clock, no map iteration order, no locale in the
output (NFR-09).

## 6. Not in v1

Encoding signals into frames; mutating a loaded database; multiple DBC files merged into one
database; a C API; exceptions as an error channel; any form of decode that returns a value without
a quality.
