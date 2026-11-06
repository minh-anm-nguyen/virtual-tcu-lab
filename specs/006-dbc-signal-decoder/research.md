# Phase 0 Research: DBC Signal Decoder

**Feature**: `006-dbc-signal-decoder` | **Date**: 2026-11-05 | **Spec**: [spec.md](./spec.md)

Purpose: close every decision the spec leaves open so implementation has no remaining ambiguity.
The spec already fixes the scope boundary (section 3), the requirements (section 4), the
architecture (section 5), the DBC subset and validation contract (section 6), the reference bit
extraction algorithm and the hand-computed goldens (section 7), the illustrative API (section 8),
the quality state machine and its transition matrix (section 9), the operational behaviour
(section 10), the sequences (section 11), the concurrency rules and the property invariants
(section 12), the 20 behavioural contracts (section 13), the what-if matrix (section 14), the test
harness (section 15), the metrics (section 16), the config sidecar (section 17), the layout
(section 18), the 22-ticket backlog (section 19) and the Definition of Done (section 23). Nothing
below redesigns any of that.

Entries R-01..R-10 answer spec section 24's ten open questions. R-00 and R-11..R-24 answer
decisions the spec states as a goal without naming a mechanism.

**Revision, 2026-11-06**: R-00, R-22 and the new R-24 were rewritten after project 05 landed
(commit `fe31dac`). The first draft of this document was written while project 05 was specified but
unimplemented and therefore deferred its integration; project 06 now reuses project 05's transport
instead of defining a frame-source abstraction of its own.

---

## R-00: Target platform — `tcu_dbc` portable, `tcu_signal` Linux-only

**Decision**: `modules/dbc` builds and tests on Linux **and** Windows, outside any platform guard.
`modules/signal` sits **inside** the root `CMakeLists.txt` Linux guard, next to projects 02 and 05,
because it links `tcu_can` (R-22) and that target compiles `socketcan_transport.cpp`.

**Rationale**: the parser, the validator, the bit extractor and the decoder need no POSIX call —
`std::ifstream`, arithmetic on a `const uint8_t*`, `<filesystem>` for the stat — and they carry the
bulk of the test value (the whole golden matrix, every validation class, the fuzz targets). Keeping
`tcu_dbc` portable keeps all of that runnable on Windows. `tcu_signal` consumes project 05's
transport rather than re-implementing one, and project 05 is Linux-only by its own design, so
`tcu_signal` inherits that. The Constitution asks Linux to build and test with Windows
best-effort, and the spec names Ubuntu with `vcan`; this is the split where best-effort is free and
nothing is faked to buy it.

**Alternative considered**: having project 05 export a header-only `tcu_can_types` INTERFACE target
so `tcu_signal` could stay portable. Rejected for now — `FakeCanTransport` lives in a `.cpp` inside
`tcu_can`, so every test would still need the real library, and splitting a finished project's
target to buy best-effort Windows support for half of project 06 is a refactor nobody asked for.

**Consequence**: sanitizer and fuzzing jobs stay Linux/clang-only — a toolchain limit, not a code
limit — so the spec section 23 criteria for ASan/UBSan/TSan and fuzzing are evaluated on
Linux/WSL and that limitation is stated in the quickstart rather than hidden.

---

## R-01: Frame mode — Classical CAN only, CAN FD rejected at load

**Spec question**: section 24 Q1 — "Classical CAN only or CAN FD? Which DBC subset?"

**Decision**: V1 is Classical CAN: payload 0..8 bytes, 11-bit and 29-bit identifiers. The sidecar
key `frame_mode` accepts exactly one value in V1, `"classical_can"`; any other value is a config
load error. A `BO_` whose declared length exceeds 8 is rejected with `DBC_MESSAGE`, and a frame
whose `size > 8` is rejected with `DecodeError::WrongLength` before any bit is read.

**Rationale**: spec sections 3.1 and 3.2 put FD in V2, and section 17 states the rule outright.
Keeping the key in the schema with a single legal value means the FD switch arrives later as a new
enum value plus tests, not as a schema change that invalidates every existing `decoder.json`.

**Alternatives considered**: accepting FD lengths in the parser while rejecting FD frames at
decode. Rejected — a DBC that declares a 64-byte message would load "successfully" and then fail
every frame, which is precisely the silent half-support ADR-07 forbids.

---

## R-02: Unsupported DBC constructs — whole file rejected, never ignored

**Spec question**: section 24 Q2 — "Does the DBC contain `SIG_VALTYPE_`, extended multiplexing or
OEM-specific attributes?"

**Decision**: three tiers, fixed at parse time:

| Tier | Constructs | Behaviour |
|---|---|---|
| Supported | `VERSION`, `NS_`, `BS_`, `BU_`, `BO_`, `SG_`, `VAL_`, one-level mux `M`/`mN` | parsed |
| Ignorable allowlist | `CM_`, `BA_DEF_`, `BA_DEF_DEF_`, `BA_`, `VAL_TABLE_`, `SIG_GROUP_`, `EV_` | skipped to end of statement, counted, one warning diagnostic per kind |
| Rejected | `SIG_VALTYPE_`, `SG_MUL_VAL_`, `BO_TX_BU_`, any unknown token at statement position | `UNSUPPORTED_FEATURE` diagnostic with line/column, **whole database rejected** |

**Rationale**: spec section 6.1 draws exactly this line — "unknown optional metadata may be ignored
only if it is on an allowlist and recorded as a warning; an unknown directive can affect decode:
reject the file". `SIG_VALTYPE_` changes the *meaning* of bits already covered by a `SG_`, so
ignoring it would decode a float as an integer and report it as `Valid`: the one outcome section 1
calls unacceptable. `VAL_TABLE_` is on the ignore list because V1 resolves no table references;
a `VAL_` that references a table name instead of inlining its pairs is a `DBC_ENUM` rejection.

**Alternatives considered**: a `strict: false` mode that downgrades rejections to warnings.
Rejected — `strict` stays in the schema (spec section 17) but in V1 its only legal value is `true`,
for the same reason as R-01. A lenient mode is a V2 decision that needs its own test matrix.

---

## R-03: Signal timeout comes from the sidecar only

**Spec question**: section 24 Q3 — "Is signal timeout supplied by DBC attributes, an OEM spec or
sidecar config?"

**Decision**: `decoder.json` only. `signal_freshness.default_timeout_ms` applies to every signal;
`signal_freshness.overrides` is an optional map from fully qualified signal name
(`MESSAGE.Signal`) to a per-signal timeout. `GenMsgCycleTime` and every other DBC attribute is on
the R-02 ignore list and **never** converted into a timeout.

**Rationale**: spec section 6.1 forbids deriving cycle time from DBC attributes without a formal
contract, and section 3.4 lists `signal_timeout_default` as a config variable. A cycle time is
also not a timeout: the deadline is a multiple of it plus jitter allowance, and that multiplier is
an engineering decision the DBC does not carry.

**Consequence**: a signal present in the DBC but absent from `overrides` is still monitored, at the
default. There is no "unmonitored" signal, so no signal can silently never go stale.

---

## R-04: `[min|max]` is metadata; enforcement is opt-in per signal

**Spec question**: section 24 Q4 — "Is DBC min/max a hard validity criterion or display
metadata?"

**Decision**: metadata by default. The parsed bounds are retained and exposed, and an
out-of-bounds physical value is counted in `decoder_signals_total{quality="out_of_range"}` and
reported as a warning-level diagnostic, but the signal is still `Valid`. Enforcement is per signal
via `range_enforcement: {"VEHICLE_STATUS.VehicleSpeed": true}`; an enforced violation becomes
quality `OutOfRange`, leaves last-good untouched, and is **never clamped**.

**Rationale**: spec section 6.2's strictness nuance says exactly this, and gives the reason —
`[0|0]` is widely used to mean "range not declared", so treating bounds as validity would reject
correct traffic. Section 7.2 adds the no-clamp rule.

**Alternatives considered**: a single global `range_enforcement: true`. Rejected — a per-signal map
is the same amount of code (one lookup either way) and is what section 17's sample file shows.

---

## R-05: Invalid raw codes come from the sidecar; no E2E profile in V1

**Spec question**: section 24 Q5 — "Where are invalid/NA raw values defined? Are there alive
counter / CRC profiles?"

**Decision**: `invalid_raw_codes` in `decoder.json`, keyed by fully qualified signal name, holding
a list of **raw** (pre-scaling) values. A match yields quality `InvalidRaw`, keeps last-good, and
does not refresh freshness. No rolling counter, no CRC, no source authenticity check in V1; such a
message decodes normally and its counter/CRC signals are ordinary signals the consumer may check.

**Rationale**: FR-10 says invalid values are "applied from config, not inferred", and section 3.2
puts E2E profiles in V2. Raw-domain matching rather than physical-domain is deliberate: `0xFFFF`
is exact in `uint64_t` and inexact after `× 0.01`, so a physical comparison would need a tolerance
and could match a legitimate value.

**Consequence**: the decoder must not advertise data-integrity guarantees. The quickstart and the
ADR state that a successful decode says nothing about whether the sender was genuine.

---

## R-06: Timestamps — monotonic, supplied by the frame source, injectable

**Spec question**: section 24 Q6 — "Is a hardware/kernel timestamp needed? What accuracy does
freshness require?"

**Decision**: `CanFrameView::rxTime` is a `std::chrono::steady_clock::time_point` captured by
whatever produces the frame, and the decoder never re-stamps it (spec section 10.5). All freshness
arithmetic uses that value against an injected `now()`. No `SO_TIMESTAMPING`, no
`SOF_TIMESTAMPING_RX_HARDWARE` in V1. Wall-clock time appears in log lines only.

**Decision on the boundary case**: `now >= lastGoodRxTime + timeout` is stale. `isFresh(now)` is an
exact predicate evaluated on call; the timer only decides *when the event fires*, never *what the
answer is* (spec section 10.5, B-09).

**Accuracy claim**: the lab claims scan-period resolution for *stale event latency*
(`scan_period_ms`, default 50) and exactness for *stale queries*. It claims nothing about
sub-millisecond frame arrival accuracy, because `vcan` has none to give.

---

## R-07: Performance — measured and reported, never asserted in a test

**Spec question**: section 24 Q7 — "Decode throughput SLA and CPU/memory budget on the target
SoC?"

**Decision**: unknown and out of scope. NFR-03 (p99 < 200 µs per classical frame) and NFR-04
(`dbc_load` < 500 ms) are reported by `benchmarks/dbc_decoder_bench` with the CPU model, compiler
and build type printed alongside, and are **not** CTest assertions. The stress suite asserts the
*shape* of the result — bounded queue depth, monotonic drop counters, no RSS trend, no lost
accounting — which is what NFR-02 actually requires.

**Rationale**: the spec says the numbers are "goals for the lab, measure a baseline and adjust by
CPU/OS; no hard real-time promise". A latency assertion on a shared CI runner fails for reasons
that have nothing to do with the code, and a flaky gate teaches people to ignore gates. This
follows project 04's R-21 precedent.

---

## R-08: Hot reload — enabled, path from config, no signature

**Spec question**: section 24 Q8 — "Is DBC reload enabled in the vehicle or compile-time only? Is
there a signature or secure provisioning?"

**Decision**: enabled in the lab, `reload.enabled` defaults to true. Before reading, the path is
checked with `std::filesystem::symlink_status`: a symlink, a directory, a FIFO or anything that is
not a regular file is rejected with `DBC_SOURCE` and the active snapshot is kept. Size is checked
against `max_dbc_bytes` **before** the read. No signature, no certificate, no provisioning chain
in V1 — and the ADR says so, so nobody mistakes "reload worked" for "the file was authentic".

**Rationale**: spec section 12 rule 9 asks for no path injection and no symlink by default;
section 3.3 puts security certification out of scope. A rate limit of
`reload.max_attempts_per_min` (default 6) plus `debounce_ms` (default 300) bounds thrash, and a
second pending request while one is in flight is rejected rather than queued (spec section 10.4
rule 1).

---

## R-09: Consumers see latest last-good plus quality; schema change resets quality

**Spec question**: section 24 Q9 — "Do consumers need historical last-good or only latest-valid?
How are schema changes versioned?"

**Decision**: latest only. The cache holds one `LastObservation` and one `LastGoodValue` per signal
key, no ring buffer and no history. Each signal gets a **semantic hash** computed at compile time
over `(messageId, isExtended, busId, signalName, startBit, width, byteOrder, isSigned, factor,
offset, muxRole, muxValue, unit)`. On a successful swap, a signal whose semantic hash is unchanged
keeps its last-good and its freshness; a signal whose hash changed, or which no longer exists, is
reset to `NotReceived` / `ConfigUnavailable` and its old last-good is retained for diagnostics
only (spec section 9.3).

**Rationale**: section 9.3 recommends exactly this reset, and the reason is the one that matters —
after a factor or signedness change, the old number is a different physical quantity. Keeping it
as `Valid` would be the "silent semantics drift" the spec names.

**Alternatives considered**: version-namespaced history (`speed@v1`, `speed@v2`). Rejected as V2:
unbounded growth across reloads, and no consumer in projects 01-30 asks for it yet.

---

## R-10: Decode-only, and the safety boundary is written down

**Spec question**: section 24 Q10 — "Do CAN signals influence safety decisions?"

**Decision**: in this lab, no. The libraries expose read-only decoded data; nothing in this
project transmits a CAN frame, and no API accepts an actuation request. The CLIs read files and
print; the demo reads a replay file and prints. The boundary is repeated in the ADR, the quickstart
and the module README, in the spec's own words: a successful decode is not evidence of a
trustworthy sender, and `vcan` is not a HIL rig.

---

## R-11: No third-party DBC parser — a hand-written lexer and recursive-descent parser

**Decision**: write it. `src/dbc_lexer.cpp` produces tokens with line/column; `src/dbc_parser.cpp`
is a statement-level recursive-descent parser over the R-02 grammar; `src/schema_validator.cpp`
checks what the grammar cannot (section 6.2's eleven check classes); `src/compiled_bitplan.cpp`
turns the validated AST into immutable lookup tables. Estimated 900-1,100 lines plus tests.

**Rationale**: the Constitution's dependency rule prefers the standard library, and the alternatives
are worse here. `libcanardbc`/`dbcppp`-class libraries are *permissive* by design — they accept the
full Vector grammar and silently tolerate what they do not model — which is the opposite of the
fail-closed contract in sections 1, 6.3 and ADR-01. We also need `ParseDiagnostic{file,line,column,
code,message}` for every rejection (FR-15, B-18), a documented resource limit on every dimension
(`DBC_LIMIT`), and a fuzz target over the whole surface (NFR-05); retrofitting those onto a
permissive parser is more work than writing a strict one for a 7-statement subset.

**Alternatives considered**: generating a parser (ANTLR, re2c). Rejected — a build-time code
generator is a new toolchain dependency for a grammar with seven statement kinds, and the generated
error messages are worse than the hand-written ones the spec requires.

---

## R-12: Quality is two axes, not one enum

**Decision**: `SignalQuality` from spec section 8 is kept verbatim as the *observation* axis, and
the cache entry carries freshness separately:

```text
SignalState {
  SignalQuality lastObservationQuality;   // NotReceived | Valid | InvalidRaw | OutOfRange
                                          // | DecodeError | NotActive | ConfigUnavailable
  std::optional<SignalValue> lastGood;    // the last Valid observation
  steady_clock::time_point  lastGoodRxTime;
  bool lastGoodIsStale;                   // derived: now >= lastGoodRxTime + timeout
  uint64_t epoch;                         // bumped on every last-good commit (R-13)
}
```

`SignalQuality::Stale` remains in the enum for the reported/event surface, but it is a *derived*
label — `lastGoodIsStale && lastObservationQuality == Valid` — not a stored state.

**Rationale**: spec section 9.2's "Important" note states a single enum cannot express
"invalid *and* timed out" and requires both views. Deriving `Stale` instead of storing it removes
the whole class of bug where a stored flag disagrees with the timestamps.

---

## R-13: Stale-vs-valid race — epoch recheck under the cache mutex

**Decision**: every last-good commit increments `epoch`. The freshness scan runs in two steps:
(1) under the cache mutex, collect `(key, epoch, lastGoodRxTime)` for entries whose deadline has
passed; (2) re-acquire, and emit the stale transition **only if** the entry's epoch still matches
what step 1 saw. Events are published after the mutex is released.

**Rationale**: spec section 12 rule 6 and race recipe B in section 15.3 describe this exact
hazard — a timer that computed "stale" before a valid frame committed must not overwrite `Valid`.
Comparing a counter is cheaper and clearer than comparing timestamps, which can be equal.

**Testability**: the epoch is observable through the cache's diagnostic snapshot, so race B is a
unit test with a test gate, not a sleep-and-hope integration test.

---

## R-14: Snapshot handover — `shared_ptr<const CompiledDbc>`, pinned once per frame

**Decision**: the repository holds `std::shared_ptr<const CompiledDbc>` published with
`std::atomic_store` and read with `std::atomic_load` (the free functions; C++17 has no
`std::atomic<std::shared_ptr<T>>`). The decoder worker pins it **once** at the top of each frame
into a local, uses that local for the whole frame, and releases it when the batch is committed.
`DecodedBatch` carries the pinned snapshot's `dbcVersion` and `dbcHash`.

**Rationale**: spec section 12 rules 2-3 require an O(1) load and forbid holding a lock across the
decode loop; FR-06/NFR-06 and B-12 require one and only one schema version per batch, which a
single pin guarantees by construction. Project 04 R-03 already established this idiom in the repo.

**Consequence**: a frame in flight during a swap completes against the old version and is labelled
with it. That is the specified behaviour (section 10.4 rule 5), not a defect, and consumers get the
version and the epoch to filter on.

---

## R-15: Threading — one decoder worker, and project 03's scheduler for everything periodic

**Decision**: one RX queue (bounded, `rx_queue_capacity`), one decoder worker thread owned by
`DecoderService`, and **no other owned thread**. The freshness scan (`scan_period_ms`), the reload
debounce and the reload job itself run as `TimerScheduler` tasks from project 03.

**Rationale**: Constitution Principle II, and ADR-05 fixes the single worker for deterministic
ordering. The scheduler already provides a monotonic clock source, a bounded ready queue, a
cooperative stop and an exception-isolating task boundary; writing a second timer thread here would
duplicate project 03's tested code and add a second shutdown order to get wrong.

**Consequence**: reload validation runs on a scheduler worker, off the decode hot path, which is
what section 9.3 and section 10.4 require. A long parse delays other scheduler tasks but never the
decoder.

---

## R-16: `decoder.json` gets its own loader; `tcu_config` is not reused for it

**Decision**: `modules/signal/src/decoder_config.cpp` parses and validates `decoder.json` using
`nlohmann/json` (already pinned at the repository root) behind the same kind of wrapper project 04
uses, with a schema table of its own and unknown-key rejection in strict mode.

**Rationale — and this is a deliberate exception to Principle II**: `tcu_config` is not a generic
configuration engine. Its `kFieldSpecs` table and its `ConfigSnapshot` struct *are* the TCU
application's configuration (connectivity, telemetry, logging, network), by design and by its own
research R-04 "one schema table as the single source of truth". Reusing it would mean either
adding a `/dbc/...` section to the TCU config snapshot — coupling the decoder's sidecar to an
unrelated file and editing project 04's tested schema — or extracting a generic schema engine out
of project 04, which is a refactor of a finished project that no other project has asked for.
Spec section 17 also states that `decoder.json` is a separate, project-defined sidecar.

What *is* reused: the validation **pattern** (one table, type + range + mandatory, reject unknown
keys, report a JSON pointer and an error kind but never the value) and `tcu_logging` for
diagnostics. Recorded in [plan.md](./plan.md) Complexity Tracking.

---

## R-17: Logging — `tcu_logging`, with per-reason rate limiting at the call site

**Decision**: both libraries log through project 04's `ILogger`. Unknown-ID, parse-error and
frame-rejection lines pass through a small token-bucket keyed by `(event, reason)` with
`logging.rate_limit_per_reason_per_min` (default 60); suppressed lines are counted and the next
emitted line carries `suppressed_since_last` so nothing disappears silently.

**Rationale**: spec sections 10.3 and 16.1 require sampled logs for unknown IDs and repeated parse
errors; a poisoned bus can otherwise turn one malformed frame per millisecond into a disk-filling
log. The bucket lives at the call site rather than inside `tcu_logging` because this is a decoder
policy, not a logger feature — adding it to project 04 would change a finished library for one
consumer.

---

## R-18: Event Bus — an observer interface here, the adapter in the demo

**Decision**: `tcu_signal` defines `ISignalObserver` (spec section 8) and calls it with no lock
held. It does **not** link `tcu_eventbus`. The mapping to `SignalsDecoded`, `SignalQualityChanged`,
`DbcReloaded` and `DbcReloadFailed` lives in `apps/virtual_dbc_demo` and in one integration test.

**Rationale**: FR-18 is SHOULD, not MUST, and section 5 keeps the decoder independent of its
consumers. This is project 04's R-20 precedent: the library stays dependency-light and the
integration is still demonstrated and tested. Observer exceptions are caught at the call boundary,
counted in `decoder_observer_failures_total`, and logged with the observer id (B-14, Principle V).

---

## R-19: `dbc_hash` — SHA-256 implemented in-tree, ~110 lines, NIST vectors

**Decision**: `modules/dbc/src/sha256.cpp`, a straightforward FIPS 180-4 implementation over the
raw DBC bytes, with the three NIST short-message vectors plus the 1,000,000-`a` vector as unit
tests. The hex digest is what section 6.3 calls `dbc_hash`.

**Rationale**: spec section 6.3 names SHA-256 specifically, so `std::hash` is not an option (not
stable across runs, 64-bit, and not a digest). OpenSSL/libsodium would add a linked system
dependency to a library that otherwise has none, for one call. A hash is also not a security
boundary here (R-08: no signature), so the usual "never roll your own crypto" argument does not
apply — this is a content fingerprint, and it is verified against published vectors.

**Alternatives considered**: hashing the *parsed* model rather than the bytes. Rejected — two
byte-different files that parse identically should be distinguishable in a log, and the semantic
comparison that R-09 needs is the per-signal semantic hash, which is a separate thing.

---

## R-20: Differential testing against `cantools` — opt-in, generated, checked in

**Decision**: `scripts/generate_dbc_vectors.py` uses `cantools` to produce
`tests/fixtures/dbc/golden/*.json` from the checked-in `.dbc` fixtures and a seeded random
generator (10,000 vectors: Intel/Motorola × signed/unsigned × widths 1..64 × mux branches). The
**generated files are committed**, so CTest compares against them with no Python and no `cantools`
at test time. Regenerating is a developer action behind `scripts/`, and the script records
`cantools` version and seed in the output header.

**Rationale**: section 15.1 wants `cantools` as an independent oracle and section 15.5 requires
stored seeds and reproducible fixtures; making CI depend on a pip install at test time would make
the C++ suite fail for Python reasons. The 50+ hand-computed goldens of section 7.3 are written
by hand and never generated — they are the check on the oracle itself.

**Intentional divergences** (section 15.5 asks for these to be recorded): `cantools` accepts
`SIG_VALTYPE_`, extended multiplexing and out-of-range `BO_` lengths that V1 rejects. The
comparison script skips those files and the divergence list lives next to the fixtures.

---

## R-21: Fuzzing — clang libFuzzer, off by default, Linux CI only

**Decision**: `TCU_BUILD_FUZZERS` (default OFF) adds `fuzz/fuzz_dbc_parser.cpp` and
`fuzz/fuzz_frame_decoder.cpp`, built only with clang and `-fsanitize=fuzzer,address,undefined`.
A seed corpus of the valid and invalid `.dbc` fixtures is checked in. The NFR-05 budget — 1,000,000
executions, or the documented CI time box, whichever comes first — is a script
(`scripts/run_dbc_fuzz.sh`), not a CTest case.

**Rationale**: libFuzzer needs a `main` the test framework also wants and a compiler the default
build does not require. Keeping it opt-in means `cmake --build` stays green on GCC and on Windows,
while the acceptance evidence for NFR-05 is still reproducible by one command.

---

## R-22: The transport is project 05's `ICanTransport` — no new source abstraction

**Decision**: `tcu_signal` consumes frames through project 05's existing
`tcu::can::ICanTransport { ReadResult receive(milliseconds); WriteStatus send(const CanMessage&);
void requestStop() noexcept; }`, and project 05's `CanMessage` is the frame type on the wire into
this project. `SocketCanTransport` gives the `vcan0` path and `FakeCanTransport` gives every test
path. **Project 06 writes no transport, no frame source interface and no fake.**

**Rationale**: project 05 was completed and committed (`fe31dac`, project 05: virtual CAN bus
laboratory, CAN-001..019) and owns exactly this boundary. Principle II forbids re-implementing what
an earlier project provides, and project 05 anticipated this consumer in two places: the comment on
`FakeCanTransport` says it is "PUBLIC on purpose ... project 06 replaces the decoder behind this
same transport", and `can_types.hpp` is deliberately free of every Linux header so a consumer gets
an OS-neutral `CanMessage`. An `ICanSource` of our own would have been a second name for the same
seam.

**What this removes from project 06**: `ICanSource`, `ReplaySource` as a transport, and
`FakeCanSource` — roughly 250 lines plus their tests. `CanMessage` already owns its 8-byte payload,
so the RX queue stores it directly instead of copying a payload out of a borrowed view.

**What survives of the replay idea**: FR-17 asks for CLI replay, so `apps/dbc_decode` keeps a
small `candump`-format reader that produces `CanMessage` values for the CLI only. It is not a
transport and is not used by the service.

**Consequence**: backlog ticket **DBC-013 is delivered, not deferred** — the `vcan` integration
test uses `scripts/setup_vcan.sh` plus `SocketCanTransport`, and `virtual_dbc_demo` runs the
section 20 demo over `vcan0` for real. The Definition of Done item "standard demo on `vcan0`
documented and repeatable" is **met**.

*History note, kept deliberately*: the Phase 1 plan for this project was written while project 05
was still unimplemented and deferred DBC-013 on that basis. The premise changed; this entry and
R-24 record the correction rather than hiding it.

---

## R-24: Reuse project 05's transport, not its `CanService`, `SignalCache` or decoder

**Decision**: project 06 consumes `tcu_can` at the transport layer only. It does **not** plug a
`DbcSignalDecoder` into `tcu::can::CanService`, does not extend `tcu::can::SignalCache`, and does
not modify project 05. `tcu_signal` keeps its own worker, its own bounded queue and its own cache,
driven directly by an `ICanTransport`.

**Rationale**: project 05's decode seam is `IDemoSignalDecoder::decode(const CanMessage&) ->
DecodeResult{accepted, RejectReason, std::vector<SignalSample>}`, and `SignalSample` carries
`std::optional<double> value` plus a five-value `SignalQuality`
(`NeverReceived|Fresh|Stale|Invalid|LinkUnavailable`). Project 06's spec requires per signal the
exact `uint64_t` raw, the signed raw, the unit, the enum label and an eight-value quality including
`InvalidRaw`, `OutOfRange`, `NotActive` and `ConfigUnavailable` (section 8), and per batch exactly
one `(dbcVersion, dbcHash)` identity (NFR-06). None of that fits through `DecodeResult`, so plugging
into `CanService` would mean either throwing away the data the project exists to produce, or
widening a finished project's public types and its tested quality semantics. `CanServiceDeps` also
has no decoder field today, so the plug-in route needs a project 05 change regardless.

**What this costs, stated honestly**: two signal caches now exist in the repository.
`tcu::can::SignalCache` serves project 05's seven hard-coded identifiers and its own demo;
`tcu::signal::SignalCache` serves DBC-decoded signals with the richer model. They are not
duplicates in behaviour — 05's computes quality from a deadline and a link flag, 06's adds the
observation/last-good split, the epoch recheck and semantic-hash migration — but they are adjacent,
and a reader deserves to be told. Project 05's `DemoSignalDecoder` is **superseded for new
consumers** by `tcu_dbc`; it is not deleted, because project 05's own tests and demo depend on it
and deleting it would break a finished project for no benefit.

**When to revisit**: if project 07 (Vehicle Gateway) needs one cache across both buses, the right
move is to retire `tcu::can::SignalCache` in favour of this one *in project 07*, with project 05's
tests migrated — one change, in the project that needs it, rather than a speculative refactor here.

---

## R-23: Test layout, labels and determinism

**Decision**: `tests/unit/dbc`, `tests/unit/signal`, `tests/integration/dbc_decoder`,
`tests/stress/dbc_decoder`, fixtures in `tests/fixtures/dbc/`. CTest labels: `dbc-unit`,
`signal-unit`, `dbc-integration`, `dbc-stress`. The existing `tests/support/test_gate.hpp`
rendezvous is reused for every race recipe; no test sleeps. Stress duration comes from
`TCU_DBC_STRESS_SECONDS` (default 5), matching project 04's `TCU_CONFIG_STRESS_SECONDS`.

**Fault injection points** (spec section 15.2 names nine): implemented as a single
`FaultHooks` struct of `std::function<void()>` members, default-empty, settable only through a
`src/`-private header that tests include directly — so the hooks are not part of either library's
public API, and a release build calls through empty `std::function`s at the same places the spec
lists: `before_validate`, `before_bit_read`, `after_pin_snapshot`, `before_cache_commit`,
`after_cache_commit`, `before_event_publish`, `after_reload_parse`, `before_reload_commit`,
`on_stop_requested`.

**Rationale**: section 15.2 says "do not put test hooks into the public production API unless
necessary". A `src/`-private header gives the tests the seam and gives consumers no way to reach
it, with no `#ifdef TESTING` build variant to keep in sync.
