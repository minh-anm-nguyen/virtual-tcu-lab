# Quickstart: DBC Signal Decoder

**Feature**: `006-dbc-signal-decoder` | **Date**: 2026-11-05 | **Spec**: [spec.md](./spec.md)

Everything here runs on a developer machine with no CAN hardware. Frames arrive through project
05's `ICanTransport` — `FakeCanTransport` for every test, `SocketCanTransport` over `vcan0` for the
demo ([research.md](./research.md) R-22). `tcu_dbc` and its unit suite also build on Windows;
`tcu_signal` is Linux-only because it links `tcu_can` (R-00). Commands describe the implementation
this plan proposes; they work once `/speckit-implement` has run.

## 1. Build and test

```bash
# Configure and build everything (projects 01-04 plus 06)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Everything
ctest --test-dir build --output-on-failure

# Just this project, by label
ctest --test-dir build -L dbc-unit --output-on-failure
ctest --test-dir build -L signal-unit --output-on-failure
ctest --test-dir build -L dbc-integration --output-on-failure
ctest --test-dir build -L dbc-stress --output-on-failure
```

The unit suites use no sleeps and no real timers: the clock is injected wherever freshness is
involved (R-06), so they finish in about a second. The stress suite honours
`TCU_DBC_STRESS_SECONDS` (default 5):

```bash
TCU_DBC_STRESS_SECONDS=60 ctest --test-dir build -L dbc-stress --output-on-failure
```

Spec section 23's one-hour soak is the same test with the variable raised — a manual job, not a CI
one:

```bash
TCU_DBC_STRESS_SECONDS=3600 ctest --test-dir build -R Soak --output-on-failure
```

### Sanitizers (Linux or WSL only)

```bash
# ASan + UBSan - UBSan is the one that observes P-02 (no out-of-range shift or read)
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure

# TSan: clang, and setarch, per project 03's research R-00 addendum
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++ -DTCU_SANITIZER_SETARCH=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread"
cmake --build build-tsan && ctest --test-dir build-tsan --output-on-failure
```

Never combine ASan and TSan. On Windows `tcu_dbc` builds and the `dbc-unit` suite runs in full;
`tcu_signal`, the integration suite and the stress suite are Linux-only because they link `tcu_can`
(R-00). The sanitizer and fuzzing jobs are Linux-only too, which is the toolchain limitation that
spec section 23's sanitizer and fuzzing criteria are read against (R-21).

### Fuzzing (clang only, opt-in)

```bash
cmake -S . -B build-fuzz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER=clang++ -DTCU_BUILD_FUZZERS=ON
cmake --build build-fuzz

# The NFR-05 budget: 1,000,000 executions per target, seeded from the committed corpus
scripts/run_dbc_fuzz.sh build-fuzz
```

### Regenerating the differential vectors (optional, needs Python)

```bash
python -m pip install "cantools==39.4.4"
python scripts/generate_dbc_vectors.py --seed 20261105 --count 10000 \
  --out tests/fixtures/dbc/golden
```

The generated files are committed, so CTest never needs Python (R-20). Regenerate only when a
fixture changes, and commit the result together with the seed printed in each file's header.

---

## 2. The CLIs

```bash
# Validate a DBC file: every diagnostic, with file:line:column and a code
./build/apps/dbc_validate/dbc_validate configs/virtual_tcu.dbc
echo $?        # 0 clean, 1 rejected, 2 usage or IO error

# Decode one frame given as hex (id:payload)
./build/apps/dbc_decode/dbc_decode --dbc configs/virtual_tcu.dbc \
  --hex 100:1027010050000000

# Expected (spec section 7.3 goldens):
#   VEHICLE_STATUS.VehicleSpeed  raw=0x2710  physical=100.00 km/h   quality=Valid
#   VEHICLE_STATUS.Ignition      raw=0x1     physical=1             quality=Valid
#   VEHICLE_STATUS.CabinTemp     raw=0x00    physical=-40 degC      quality=Valid

# Motorola signed, cross-byte
./build/apps/dbc_decode/dbc_decode --dbc configs/virtual_tcu.dbc --hex 101:ABC0000000000000
#   TORQUE_STATUS.Torque  raw=0xABC  signed=-1348  physical=-674 Nm  quality=Valid

# Multiplexed: page 1 yields BatteryVoltage only, page 2 MotorTemp only
./build/apps/dbc_decode/dbc_decode --dbc configs/virtual_tcu.dbc --hex 102:017FD20400000000
./build/apps/dbc_decode/dbc_decode --dbc configs/virtual_tcu.dbc --hex 102:027F9CFF00000000

# A whole candump capture, as JSON batches
./build/apps/dbc_decode/dbc_decode --dbc configs/virtual_tcu.dbc \
  --candump tests/fixtures/dbc/capture_normal.log --json
```

Output is byte-identical for identical input: no wall clock, no map iteration order, no locale
(NFR-09, `Cli_DecodeOutputStable`).

---

## 3. Run the demo

```bash
scripts/run_dbc_decoder_demo.sh
```

or by hand, over a real virtual bus (project 05's script creates it):

```bash
sudo scripts/setup_vcan.sh                      # project 05, reused unchanged
./build/apps/virtual_dbc_demo/virtual_dbc_demo \
  --config configs/decoder.json --iface vcan0
# in another terminal, inject the lab frames:
cansend vcan0 100#1027010050000000
cansend vcan0 101#ABC0000000000000
cansend vcan0 102#017FD20400000000
candump vcan0                                   # watch what the decoder is reading
sudo scripts/teardown_vcan.sh
```

Without an interface, or on a machine where `vcan` is not loaded, the same sequence runs over a
scripted `FakeCanTransport`:

```bash
./build/apps/virtual_dbc_demo/virtual_dbc_demo --config configs/decoder.json --fake
```

`dbc_decode --candump` still reads a `candump` capture file, so a session recorded from `vcan0`
replays through the CLI unchanged:

```text
(1730793600.000000) vcan0 100#1027010050000000
(1730793600.050000) vcan0 101#ABC0000000000000
(1730793600.100000) vcan0 102#017FD20400000000
```

The demo covers the section 20.4 what-if sequence, in order:

| Step | What is injected | What to look for |
|---|---|---|
| A — normal | the three lab messages at 20 Hz | `decoder_ready` with version 1 and a hash, then `SignalsDecoded` batches with the section 7.3 values |
| B — wrong length | a 7-byte `VEHICLE_STATUS` | `frame_rejected reason=WRONG_LENGTH`; the cached speed keeps its previous value and its previous timestamp (B-06, B-08) |
| C — unknown id | an undefined id, then the same number as an extended frame | `unknown_message` counted, no cache mutation, and the extended frame is **not** aliased onto the standard message (B-07) |
| D — stale | the source stops sending `TORQUE_STATUS` | after `signal_freshness` elapses, `signal_quality_changed from=Valid to=Stale`, and last-good is still readable (B-09) |
| E — recovery | the message resumes | `SignalRecovered`, new timestamp (B-10) |
| F — bad reload | `configs/virtual_tcu.dbc` replaced by a file with an overlap in branch `m1` | `reload_rejected reason=DBC_OVERLAP` with a line number; the active version and hash are unchanged and the service stays Ready (B-11) |
| G — good reload | a valid edit (a new signal added) | `reload_committed from_version=1 to_version=2`; signals whose semantic hash is unchanged keep their last-good, changed ones reset to NotReceived (B-12, R-09) |
| H — overload | a burst far above `rx_queue_capacity` | `queue_overflow` with a monotonic `dropped_total`, queue depth never above capacity, no deadlock (B-13) |
| I — bad consumer | one observer throws, another blocks | `observer_failed` counted, the decoder keeps running, and stop still completes inside its deadline (B-14, B-15) |

Edit the DBC the way a real tool does — write a temporary file and rename it over the target — so
the reload sees a complete file rather than a truncated one.

---

## 4. What this does **not** demonstrate

- No CAN arbitration, bus-off, bit timing or transceiver behaviour — `vcan` does not simulate those
  (spec section 3.3). Malformed frames the kernel refuses to put on `vcan0` are reachable only
  through `FakeCanTransport`, exactly as project 05 documented.
- No second signal cache retired. Project 05's `SignalCache` and `DemoSignalDecoder` still serve its
  own demo and tests; this project supersedes them for DBC-decoded signals without deleting them
  (R-24).
- No data-integrity guarantee. A successful decode says nothing about whether the sender was
  genuine: there is no CRC, rolling counter or signature in v1 (R-05, R-08).
- No hard real-time claim. The benchmark reports latency; nothing asserts it (R-07).

---

## 5. Acceptance checklist (spec section 23)

| DoD item | How to verify |
|---|---|
| Builds with CMake, all tests pass, warnings enabled | `cmake --build build` then `ctest --test-dir build` |
| Strict subset documented; decode-affecting directives rejected | [contracts/dbc_subset.md](./contracts/dbc_subset.md), `Parser_Rejects*` |
| STD/EFF mapping and DLC rules validated | `Database_LookupDistinguishesFormat`, `Decode_WrongLengthRejected` |
| Intel and Motorola verified against hand vectors and an independent oracle | `BitExtractor_*Goldens`, `Golden_GeneratedMatrix` |
| Signed 1..64, scaling, NaN/Inf, overflow without UB | `SignExtend_*`, `Scaling_*`, UBSan run |
| One-level mux and `VAL_` work; nested rejected | `Mux_*`, `Decode_EnumLabelResolved`, `Parser_RejectsExtendedMux` |
| No invalid/unknown/wrong-DLC frame refreshes last-good | `Cache_InvalidKeepsLastGood`, `Cache_OutOfOrderFrameIgnored` |
| Monotonic freshness; reload cannot silently reinterpret | `Freshness_ExactDeadlineIsStale`, `Reload_MigrationResetsChangedSignals` |
| Atomic reload tested against decode, timer and shutdown | `RaceA_*`, `RaceB_*`, `RaceC_*`, 10,000 iterations each |
| Bounded queue and load-shedding metrics; no growth in soak | `Queue_*`, `Soak_HundredThousandFramesBounded`, `Soak_MetricConservation` |
| Every P0 what-if mapped to a test with CI evidence | the traceability tables in [plan.md](./plan.md) plus the section 23.1 table filled in at implementation |
| Parser and frame fuzz clean for the agreed budget | `scripts/run_dbc_fuzz.sh` (Linux + clang) |
| Standard demo on `vcan0` documented and repeatable | section 3 above, over project 05's `SocketCanTransport` plus `scripts/setup_vcan.sh`; `Vcan_EndToEnd` covers it in CI when the interface exists and skips with a message when it does not |
| Limitations explicit | section 4 above, plus the "Not in v1" section of each contract |
| ADR, docs, test report, benchmark retained | spec section 21 ADRs, `benchmarks/dbc_decoder_bench` output committed with CPU/compiler noted |
