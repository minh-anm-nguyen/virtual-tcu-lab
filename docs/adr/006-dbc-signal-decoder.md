# ADRs: Project 06 — DBC Signal Decoder

Spec section 21 proposes seven decisions. All seven are **accepted**, each with
the limitation it carries. Two more were forced by what the repository looked
like when the project was built; they are recorded here rather than left in a
commit message.

Decision records referenced as `R-xx` live in
[../../specs/006-dbc-signal-decoder/research.md](../../specs/006-dbc-signal-decoder/research.md).

---

## ADR-01 — A small strict DBC subset, not pretended full compatibility

**Accepted.** `tcu_dbc` parses seven statement kinds, ignores an allowlist of
seven more with a warning, and rejects everything else outright.

**Limitation**: real-world DBC files from a tool chain will often be refused.
That is the intended outcome — a parser that accepts a construct it does not
model will decode the wrong number and report it as `Valid`, which spec
section 1 names as the one unacceptable failure. `dbc_validate` exists so the
refusal is diagnosable in one run.

## ADR-02 — Compiled bit plans, not ad hoc bit twiddling

**Accepted.** Every signal becomes a `BitPlan` at load time, from an AST that
already passed every validation check, so `decode()` performs no layout
validation at all.

**Limitation**: a bug in the validator becomes a wrong decode rather than a
runtime error, because decode trusts the plan. The eleven validation classes
are therefore tested one case at a time, each asserting the error code *and*
the reported line.

## ADR-03 — Exact DLC, not partial decode

**Accepted.** `payload_length_policy` has one legal value, `exact`: a frame
whose length differs from the declared length decodes nothing.

**Limitation**: a sender that legitimately truncates trailing bytes will have
every frame refused. The alternative — decoding the signals that happen to fit
— cannot distinguish "the rest is absent" from "the rest is garbage", and the
spec's freshness model has no way to express a half-updated message.

## ADR-04 — Preserve last-good, never mislabel invalid as fresh

**Accepted.** The cache keeps the last observation and the last good value on
separate axes, and freshness is computed from the last-good timestamp at query
time. No invalid, out-of-range, wrong-length, unknown or out-of-order frame
touches last-good, its timestamp or its epoch.

**Limitation**: a consumer that reads only the value and ignores the quality
will use a stale number. The API makes that awkward on purpose — every query
returns both axes in one read, and there is no accessor that returns a physical
value alone.

## ADR-05 — A single decoder worker

**Accepted.** `decoder_workers` has one legal value, 1. Decode order is the
queue order, which makes a replayed sequence reproducible.

**Limitation**: decode throughput is one core's worth. That is not measured yet
(see the Verification gap below), so the limitation is stated rather than
quantified.

## ADR-06 — Atomic immutable DBC hot reload

**Accepted.** A reload parses, validates and compiles on a scheduler worker,
then publishes a new `shared_ptr<const CompiledDbc>` with a single atomic
store. The worker pins the snapshot once per frame, so a batch carries exactly
one `(version, hash)` pair even if a reload commits mid-decode.

**Limitation**: a frame in flight during a swap completes against the old
version. That is the specified behaviour, not a defect — consumers get the
version on every batch and can discard what they consider too old.

## ADR-07 — CAN FD and E2E features explicitly unsupported in v1

**Accepted.** `SIG_VALTYPE_`, `SG_MUL_VAL_`, `BO_TX_BU_` and any unknown
statement are rejected at load. `frame_mode` accepts only `classical_can`.
There is no CRC, rolling counter, signature or provenance check.

**Limitations, stated plainly**:

- `vcan` is not a hardware-in-the-loop rig. It simulates no arbitration, no
  bus-off, no bit timing and no transceiver behaviour. Frames the kernel
  refuses to put on `vcan0` — a wrong length, an RTR, an error frame — are
  reachable only through `FakeCanTransport`, and the demo says so on screen
  rather than implying they went on the wire.
- **A successful decode is not evidence that the sender was genuine.** The
  decoder proves that bytes matched a layout, nothing more. Data integrity,
  authenticity and any safety policy belong to layers this project does not
  provide (R-05, R-08).
- Nothing here controls a vehicle. The libraries are read-only; no API accepts
  an actuation request and nothing in project 06 transmits a frame.

---

## ADR-08 — Reuse project 05's transport, not its service or cache

**Accepted** (R-22, R-24). `tcu_signal` consumes `tcu::can::ICanTransport`,
`CanMessage`, `SocketCanTransport` and `FakeCanTransport` as project 05 ships
them, and writes no transport, no frame-source interface and no CAN fake. It
does **not** plug into `tcu::can::CanService` or extend
`tcu::can::SignalCache`, because their `SignalSample` carries one `double` and
a five-value quality, which cannot express this project's raw `uint64_t`,
signed raw, unit, enum label, eight-value quality or per-batch DBC identity.

**Limitation, and it is a real cost**: two signal caches now exist in the
repository. `tcu::can::SignalCache` serves project 05's seven hard-coded
identifiers and its own demo; `tcu::signal::SignalCache` serves DBC-decoded
signals. They are not duplicates in behaviour, but they are adjacent, and a
reader deserves to be told rather than left to discover it. Project 05's
`DemoSignalDecoder` is superseded for new consumers and deliberately not
deleted, because project 05's own tests and demo depend on it.

**When to revisit**: project 07 (Vehicle Gateway) is the first consumer that
needs one cache across two buses. Retiring `tcu::can::SignalCache` belongs
there, with project 05's tests migrated — one change, in the project that needs
it, rather than a speculative refactor here.

## ADR-09 — SHA-256 implemented in-tree

**Accepted** (R-19). Spec section 6.3 names SHA-256 for `dbc_hash`, so
`std::hash` is not an option: it is not a digest and is not stable across runs.
OpenSSL would add a linked system dependency to a library that otherwise has
none, for one call.

**Limitation**: this is hand-written cryptographic code, which is normally a
bad idea. It is acceptable here for one reason — it is a *content fingerprint*,
not a security boundary. R-08 states there is no signature and no provenance
check in v1, so nothing trusts this digest to resist an attacker. It is
verified against the four published NIST vectors, including the
one-million-character case.

---

## Verification gap, as of 2026-11-06

The decisions above are implemented and unit-tested, but the evidence the
Definition of Done asks for is **not yet collected**: no sanitizer run, no
fuzzing, no differential matrix against `cantools`, no soak and no benchmark.
NFR-01..NFR-05 are therefore unproven, and ADR-05's throughput limitation is
unquantified. Until those run, treat the parser as untested against hostile
input — which is exactly what a fuzz target exists to settle.
