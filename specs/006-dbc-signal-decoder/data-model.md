# Phase 1 Data Model: DBC Signal Decoder

**Feature**: `006-dbc-signal-decoder` | **Date**: 2026-11-05 | **Spec**: [spec.md](./spec.md)

Entities, states and invariants. Spec section 8 fixes the public types; this document fills in the
internal ones, states every invariant a test must be able to check, and gives each invariant an ID
so [plan.md](./plan.md) and `tasks.md` can point at it. Decisions referenced as `R-xx` live in
[research.md](./research.md).

Invariant prefixes: **D** parse and database, **P** bit plan and decode, **S** signal state and
cache, **V** service lifecycle and reload, **M** metrics and accounting.

---

## 1. Parse-time entities (`tcu_dbc`, private to `src/`)

### 1.1 `Token` and `ParseDiagnostic`

```text
Token      { Kind kind; std::string text; uint32_t line; uint32_t column; }
Severity   { Warning, Error }
ParseDiagnostic {
  std::string file; uint32_t line; uint32_t column;
  DbcErrorCode code;        // section 6.2's codes
  Severity severity;
  std::string message;      // never contains the file's payload bytes
}
```

`DbcErrorCode` is the closed set of spec section 6.2 plus two sources named by R-02 and R-08:
`DBC_SYNTAX`, `DBC_LIMIT`, `DBC_MESSAGE`, `DBC_SIGNAL`, `DBC_OUT_OF_BOUNDS`, `DBC_OVERLAP`,
`DBC_MUX`, `DBC_ENUM`, `DBC_RANGE`, `UNSUPPORTED_FEATURE`, `DBC_SOURCE`.

- **D-01** Every rejection carries a line and a column that point inside the offending statement.
- **D-02** A diagnostic message never embeds signal payload or more than 64 characters of the
  offending token; `DBC_SOURCE` never embeds the resolved absolute path.
- **D-03** `Severity::Warning` diagnostics (R-02 ignore list, unenforced range metadata) never
  prevent a load; `Severity::Error` always does.

### 1.2 `RawMessage` / `RawSignal` — the AST

```text
RawSignal {
  std::string name;
  uint32_t startBit; uint8_t width;
  ByteOrder byteOrder;              // Intel | Motorola
  bool isSigned;
  double factor; double offset; double minimum; double maximum;
  std::string unit;
  MuxRole muxRole;                  // None | Selector | Branch
  uint32_t muxValue;                // meaningful only when muxRole == Branch
  std::vector<std::pair<int64_t, std::string>> valueDescriptions;  // from VAL_
  uint32_t line;                    // for diagnostics
}
RawMessage {
  uint32_t rawId;                   // exactly as written in BO_
  uint32_t arbitrationId; bool isExtended;   // resolved per section 6.1
  std::string name; uint8_t length;
  std::vector<RawSignal> signals;
  uint32_t line;
}
```

- **D-04** `rawId` bit 31 set means extended; `arbitrationId` is then `rawId & 0x1FFFFFFF`, else
  `rawId & 0x7FF`. Any reserved bit set, or an `arbitrationId` wider than the resolved format
  allows, is `DBC_MESSAGE` — never truncated silently.
- **D-05** `(busId, isExtended, arbitrationId)` is unique across the database; a standard and an
  extended message may share a numeric id and are distinct entries (FR-07, B-07).
- **D-06** Signal names are unique within a message. Across messages they may repeat, so the cache
  key is `(busId, isExtended, arbitrationId, signalName)` (spec section 8).
- **D-07** `width` is 1..64; `startBit + span` stays inside `length * 8` for the message's byte
  order; otherwise `DBC_OUT_OF_BOUNDS`.
- **D-08** At most one `MuxRole::Selector` per message; a `Branch` signal in a message with no
  selector is `DBC_MUX`.
- **D-09** Two signals overlap illegally when their bit sets intersect **and** they can be active
  in the same frame: both unconditional, both in the same branch, or one unconditional and one in
  any branch. Two signals in *different* branches may overlap (B-18, B-19).
- **D-10** `factor`, `offset`, `minimum`, `maximum` are finite and `factor != 0`; `minimum <=
  maximum`; otherwise `DBC_RANGE` (B-17).
- **D-11** `valueDescriptions` has no duplicate number and no duplicate label within one signal
  (`DBC_ENUM`). A raw value with no label is not invalid — the numeric value is always available
  (spec section 6.1).

### 1.3 `ParseLimits`

Spec section 3.4, enforced *while* parsing so a hostile file cannot allocate first and fail later:
`dbc_max_bytes` 2 MiB, `max_messages` 2000, `max_signals_per_message` 128, `max_total_signals`
10000, `max_message_length` 8, plus a token length cap (1 KiB) and a statement count cap.

- **D-12** Every limit is checked before the allocation it bounds, and exceeding one yields
  `DBC_LIMIT` with the limit's name in the message.
- **D-13** Parsing is a single pass with no recursion over file-controlled depth, so no input can
  cause unbounded stack growth (NFR-05).

---

## 2. Compiled entities (`tcu_dbc`, public, immutable)

### 2.1 `BitPlan` — one per signal, computed once at load

```text
BitPlan {
  uint16_t signalIndex;             // into CompiledMessage::signals
  uint32_t startBit; uint8_t width;
  ByteOrder byteOrder; bool isSigned;
  double factor; double offset;
  uint8_t  lastByteTouched;         // for the cheap length pre-check
  MuxRole  muxRole; uint32_t muxValue;
}
```

- **P-01** A `BitPlan` is produced only from an AST that passed every **D** check, so decode
  performs no layout validation — the plan is correct by construction (ADR-02).
- **P-02** Extraction reads only byte indices in `[0, frame.size)` and never shifts by 64 or more
  (spec section 15.4 P-01, P-02; UBSan-observable).
- **P-03** Intel reads bit `start + i` into raw bit `i`. Motorola starts at the MSB and steps
  `p % 8 == 0 ? p + 15 : p - 1`. Both match spec section 7.1's reference loop exactly; the
  section 7.3 goldens are the oracle, and `tests/fixtures/dbc/golden` is the second one (R-20).
- **P-04** `rawSigned` is present if and only if `isSigned`, and equals the two's-complement
  interpretation for every width 1..64, with `INT64_MIN` handled without UB (spec section 7.2).
- **P-05** `physical = (isSigned ? rawSigned : raw) * factor + offset`. If the result is not
  finite, quality is `DecodeError` with `NonFinitePhysical` and last-good is untouched.
- **P-06** `raw` is always the exact `uint64_t`, whatever `physical` rounds to. For unsigned widths
  above 53 bits the batch carries a precision note; the raw value is never converted through
  `double` or through `int64_t` (spec section 7.2).

### 2.2 `CompiledMessage` and `CompiledDbc`

```text
CompiledMessage {
  uint32_t busId; uint32_t arbitrationId; bool isExtended;
  std::string name; uint8_t expectedLength;
  std::optional<BitPlan> selector;
  std::vector<BitPlan> unconditional;
  std::unordered_map<uint32_t, std::vector<BitPlan>> branches;   // muxValue -> plans
  std::vector<SignalMeta> signals;   // name, unit, bounds, enum map, semantic hash
}
CompiledDbc {
  uint64_t version;                  // 1 at cold start, +1 per committed swap
  std::string hash;                  // SHA-256 hex of the file bytes (R-19)
  std::string sourcePath;
  std::unordered_map<MessageKey, CompiledMessage> byKey;
  Stats stats;                       // messages, signals, ignored statements, warnings
}
```

- **P-07** `CompiledDbc` is reachable only as `shared_ptr<const CompiledDbc>`; nothing mutates it
  after publication (FR-01, section 12 rule 2).
- **P-08** Lookup by `(busId, format, arbitrationId)` is O(1) and distinguishes STD from EFF.
- **P-09** `version` increases strictly; `hash` is the digest of the bytes that produced it.
- **P-10** Every `SignalMeta` carries the R-09 semantic hash, computed at compile time, so a reload
  can classify each signal as unchanged, changed or removed without re-parsing anything.

### 2.3 `CanFrameView` and decode results

Spec section 8 verbatim. `CanFrameView` stays a borrowed view declared by `tcu_dbc`, so the pure
library depends on nothing; `tcu::signal::toFrameView` builds one from project 05's `CanMessage`
(research R-22), which is what every real frame arrives as. The invariant list:

- **P-11** `isError` or `isRemote` yields `ErrorFrame` / `RemoteFrame` before any lookup; such a
  frame never touches the cache (B-08).
- **P-12** With `payload_length_policy = exact`, `frame.size != expectedLength` yields
  `WrongLength` and no signal is decoded (B-06, section 15.4 P-04).
- **P-13** A `DecodedBatch` carries exactly one `(dbcVersion, dbcHash)` pair, taken from the single
  pinned snapshot (NFR-06, B-12, section 15.4 P-07).
- **P-14** A batch contains only *active* signals: the unconditional set plus the branch matching
  the decoded selector. An inactive branch signal never appears (FR-09, B-04, B-05,
  section 15.4 P-03).
- **P-15** An undecodable selector rejects the branch half only: unconditional signals are still
  emitted, `decoder_unknown_mux_total` increments, and no branch is guessed (B-20, section 10.3).
- **P-16** `decode()` allocates only for the result, touches no file, takes no lock, and throws
  nothing for any expected-invalid input (spec section 8 API contract).

---

## 3. Runtime state (`tcu_signal`)

### 3.1 `SignalState` — the cache entry

Two axes, per R-12: `lastObservationQuality` + `lastGood` + `lastGoodRxTime` + `epoch`, with
`Stale` derived. `NotActive` is recorded as an observation but is **not** an error and **not** a
last-good update (spec section 9.2 row 7).

- **S-01** `lastGood` changes only on a `Valid` active observation.
- **S-02** `lastGoodRxTime` never decreases (section 15.4 P-08). A frame whose `rxTime` is older
  than the stored one updates nothing and increments `decoder_out_of_order_total`.
- **S-03** `epoch` increments on every `lastGood` commit and never decreases.
- **S-04** `InvalidRaw`, `OutOfRange`, `DecodeError`, `NotActive`, `WrongLength`, an unknown id and
  an unknown mux value all leave `lastGood`, `lastGoodRxTime` and `epoch` untouched (FR-12, B-08,
  section 15.4 P-04, P-09).
- **S-05** `isFresh(now) == (lastGood.has_value() && now < lastGoodRxTime + timeout)`; the boundary
  `now == deadline` is stale (B-09, R-06).
- **S-06** A query returns both axes in one read; no caller can observe one without the other.
- **S-07** No observer callback runs while the cache mutex is held (section 12 rule 5,
  section 15.4 P-10). Enforced by a test whose observer re-enters the cache.

### 3.2 Quality transitions

Spec section 9.2's matrix is the specification. Implementation notes only:

- **S-08** Transitions are computed inside the cache under its mutex; the resulting event list is
  returned to the caller, which publishes after unlocking.
- **S-09** `SignalRecovered` fires on `Stale -> Valid`; `SignalQualityChanged` fires on any other
  observed-quality change; `SignalUpdated` fires per committed batch. A repeated identical
  observation emits no quality event (only the batch event).
- **S-10** A stale transition is emitted only if the entry's `epoch` is unchanged since the scan
  sampled it (R-13).

### 3.3 `RxQueue`

Bounded MPSC, `rx_queue_capacity`, mutex + condition variable, holding `tcu::can::CanMessage`
values. Project 05's frame type already owns its payload as a `std::array<uint8_t, 8>`, so there is
nothing to copy out and no borrowed pointer to outlive its source (research R-22). The worker builds
a `CanFrameView` over the queued message for the duration of one decode.

- **V-01** Depth never exceeds capacity (section 15.4 P-11); `decoder_queue_depth <= capacity`
  holds at every observation point.
- **V-02** On full, the configured policy applies — V1 is drop-newest plus
  `decoder_queue_drops_total++` — and the producer never blocks indefinitely (B-13).
- **V-03** Drop counters are monotonic.
- **V-04** The worker consumes in queue order; per-bus ordering is preserved, cross-bus ordering is
  explicitly not promised (section 12 rule 4).

### 3.4 `DecoderService` lifecycle

```text
Created -> Starting -> Ready -> Stopping -> Stopped
                 \-> NotReady (cold-start load failed; no valid signal is ever published)
```

- **V-05** `Ready` is reachable only with a valid `CompiledDbc`; in `NotReady` every query returns
  `ConfigUnavailable` and no batch is published (FR-14, section 6.3, B-01).
- **V-06** Stop order is fixed: stop accepting frames -> `transport->requestStop()` -> drain or
  discard the queue per policy -> cancel scheduler jobs -> join the worker -> detach observers ->
  flush metrics (section 12 rule 7, B-15). The transport's descriptors stay owned by the receiving
  thread, as project 05 requires: `requestStop` wakes it, it closes nothing.
- **V-07** No callback runs after `stop()` returns; `stop()` is idempotent and bounded by
  `shutdown_deadline`, after which it reports `StopIncomplete` rather than blocking forever
  (section 10.6, race recipe C).
- **V-08** An observer that throws is caught at the publish boundary, counted, logged once per
  rate-limit window, and does not stop the worker (B-14, FR-16).

### 3.5 `ReloadCoordinator`

- **V-09** At most one reload in flight; a second request while one is pending is rejected with
  `ReloadBusy`, not queued (section 10.4 rule 1).
- **V-10** Parse, validate and compile happen entirely off the decoder thread; the swap is a single
  `atomic_store` (R-14, R-15).
- **V-11** A failed reload leaves `version`, `hash` and every cache entry exactly as they were, and
  the service stays `Ready` (FR-14, NFR-07, B-11, section 15.4 P-06).
- **V-12** A committed reload increments `version` by one, then migrates the cache by semantic hash
  (R-09) before the first frame is decoded against the new snapshot.
- **V-13** Reload attempts are rate limited to `reload.max_attempts_per_min`; a rejected attempt is
  counted and logged, and does not touch the active snapshot.

---

## 4. Config entity (`tcu_signal`)

`DecoderConfig` is an immutable struct produced by the R-16 loader from `decoder.json`:
`dbcPath`, `strict`, `maxDbcBytes`, `frameMode`, `payloadLengthPolicy`, `rxQueueCapacity`,
`decoderWorkers`, `reload{enabled, debounceMs, maxAttemptsPerMin}`,
`signalFreshness{defaultTimeoutMs, scanPeriodMs, overrides}`, `invalidRawCodes`,
`rangeEnforcement`, `eventPublish{mode, onFull}`, `logging{rateLimitPerReasonPerMin}`.

- **V-14** Unknown keys are rejected in strict mode; every known key has a type, a range and a
  mandatory flag in one table, and the sample `configs/decoder.json` is asserted against that table
  by a test (project 04's `Schema_SampleFileMatchesTable` precedent).
- **V-15** `decoderWorkers` accepts only 1 in V1 (ADR-05); `frameMode` only `classical_can` (R-01);
  `strict` only `true` (R-02).
- **V-16** `invalidRawCodes` and `rangeEnforcement` keys that match no signal in the loaded DBC are
  a startup warning with a count, not a silent no-op — a typo in a signal name would otherwise
  disable a check invisibly.
- **V-17** An error message names the JSON pointer and the error kind, never the value.

---

## 5. Metrics entities

Spec section 16.1's table is the specification; `decoder_observer_failures_total` (R-18) and
`decoder_out_of_order_total` (S-02) are added because S-02 and V-08 are otherwise unobservable.

Label sets are closed, per section 16.1's cardinality rule:
`result` is one of {ok, unknown_id, wrong_length, wrong_format, error_frame, remote_frame,
decode_error}, `quality` is one of the `SignalQuality` names, `bus` is one of the configured bus
ids. **No metric is labelled with a signal or message name taken from the DBC**; per-signal stale
and recovered counts are keyed by the compiled numeric signal index and exposed through a
diagnostics snapshot, not as labels.

Conservation equations a test can assert (project 04's `Metrics_ConservationAfterFlood`
precedent):

- **M-01** `can_frames_received_total = decoder_frames_total{all results} + decoder_queue_drops_total`
- **M-02** `decoder_frames_total{result="ok"} = batches published + batches dropped by publish policy`
- **M-03** `dbc_load_attempts_total{result="ok"} = dbc_active_version` when no rollback occurred,
  and `dbc_active_version` never decreases
- **M-04** `decoder_signals_total{quality="valid"}` equals the number of last-good commits, which
  equals the sum of per-signal epoch increments
- **M-05** every counter is monotonic, and `decoder_queue_depth <= rx_queue_capacity` always
