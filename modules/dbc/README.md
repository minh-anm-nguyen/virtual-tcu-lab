# `tcu_dbc` — DBC parsing and signal decoding (project 06)

Two libraries make up project 06. This is the pure half.

| Target | What it is | Links |
|---|---|---|
| `tcu_dbc` | lexer, parser, validator, compiled bit plans, frame decoder, SHA-256 | **nothing** |
| `tcu_signal` | transport glue, bounded queue, decoder worker, quality cache, freshness, hot reload | `tcu_dbc`, `tcu_can`, `tcu_scheduler`, `tcu_logging` |

`tcu_dbc` links nothing on purpose — not even the logger. A parser that can be
fuzzed and property-tested with no lifecycle at all is the reason the project is
split in two, and diagnostics are *returned* from here, never logged. It is also
why `tcu_dbc` builds on Windows while `tcu_signal` does not.

## What it accepts

A documented DBC subset, and nothing else:

- `VERSION`, `NS_`, `BS_`, `BU_`, `BO_`, `SG_`, `VAL_`, one-level multiplexing
  (`M` and `mN`)
- Intel (`@1`) and Motorola (`@0`), signed and unsigned, widths 1..64
- Classical CAN only: 0..8 payload bytes, 11-bit and 29-bit identifiers kept
  distinct

`CM_`, `BA_*`, `VAL_TABLE_`, `SIG_GROUP_` and `EV_` are skipped with a warning
and **never interpreted** — in particular `GenMsgCycleTime` does not become a
timeout. Everything else, including `SIG_VALTYPE_` and `SG_MUL_VAL_`, is
rejected and the whole file goes with it. A construct that changes what the bits
mean cannot be ignored: doing so would decode the wrong number and report it as
`Valid`.

Full grammar and the eleven validation classes:
[contracts/dbc_subset.md](../../specs/006-dbc-signal-decoder/contracts/dbc_subset.md).

## Using it

```cpp
#include "tcu/dbc/dbc_parser.hpp"
#include "tcu/dbc/signal_decoder.hpp"

const tcu::dbc::LoadResult loaded = tcu::dbc::loadDbcFile("configs/virtual_tcu.dbc");
if (!loaded.ok()) {
  for (const auto& d : loaded.diagnostics) {
    // file:line:column: CODE: message - navigable, and never echoes payload
  }
  return;
}

const tcu::dbc::SignalDecoder decoder(loaded.db, tcu::dbc::DecodePolicy{});

tcu::dbc::CanFrameView frame;        // a BORROWED view; it must not outlive its bytes
frame.arbitrationId = 0x100;
frame.data = payload;                // const uint8_t*
frame.size = 8;
frame.rxTime = whenItArrived;        // monotonic, stamped by whoever received it

const auto result = decoder.decode(frame);
if (result.error == tcu::dbc::DecodeError::None) {
  for (const auto& value : result.batch->signals) {
    // value.raw is the exact uint64_t; value.physical may round above 2^53
    // value.quality says how much to trust it - there is no accessor that
    // hands back a number without one
  }
}
```

Every `decode()` carries exactly one `(dbcVersion, dbcHash)` pair, so a batch
can never mix two schema versions. `decode()` takes no lock, touches no file,
and throws for no frame content.

## Command line

```bash
dbc_validate configs/virtual_tcu.dbc        # 0 clean, 1 rejected, 2 I/O or usage
dbc_decode --dbc configs/virtual_tcu.dbc --hex 100:1027010050000000
dbc_decode --dbc configs/virtual_tcu.dbc --candump capture.log --json
```

Output is byte-identical for identical input: no wall clock, no map iteration
order, no locale.

## What it is not

- **Not an encoder.** Nothing here builds a frame, and nothing transmits one.
- **Not a trust boundary.** A successful decode proves bytes matched a layout.
  It says nothing about whether the sender was genuine: there is no CRC, rolling
  counter or signature in v1.
- **Not full DBC compatibility.** See the accept list above; a real-world file
  will often be refused, and `dbc_validate` says exactly where.

## Tests

```bash
ctest --test-dir build -L dbc-unit            # parser, bits, mux, database (portable)
ctest --test-dir build-wsl -L dbc-integration  # pipeline, races, golden matrix (Linux)
```

The six hand-computed vectors of the spec's section 7.3 are the first tests in
`bit_extractor_test.cpp`, written by hand before any generated vector existed.
`tests/fixtures/dbc/golden/matrix.txt` holds 10,000 committed vectors generated
with `cantools` as an independent oracle; regenerate with
`scripts/generate_dbc_vectors.py` and commit the result with its seed.

Design decisions: [docs/adr/006-dbc-signal-decoder.md](../../docs/adr/006-dbc-signal-decoder.md).
