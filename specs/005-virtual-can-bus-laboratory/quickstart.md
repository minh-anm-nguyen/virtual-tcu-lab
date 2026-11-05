# Quickstart: Virtual CAN Bus Laboratory

**Feature**: `005-virtual-can-bus-laboratory` | **Date**: 2026-11-05

Runnable validation for project 05. Every scenario below ends in an observable outcome tied to a
spec acceptance criterion, so this file doubles as the manual half of the section 26 Definition of
Done. Implementation detail belongs in [tasks.md](./tasks.md); contracts belong in
[contracts/](./contracts/).

> **Lab only.** Every CAN identifier here is invented. Never point this at a physical vehicle,
> gateway or production ECU. The service refuses any interface not matching `vcan[0-9]*`, and
> `lab_only` is the only mode V1 parses (R-16).

---

## 0. Can this host run it?

| Layer | Needs | This repo's dev host (Windows + WSL2) |
|---|---|---|
| Build + unit tests + stress tests | Linux, GCC/Clang, CMake 3.20+ | **Works** under WSL |
| Integration tests on `vcan0` | a kernel with `CONFIG_CAN_VCAN` and `CAP_NET_ADMIN` | **Does not work** — the stock WSL2 kernel has no `vcan` module |

Verify in one command:

```bash
modprobe -n vcan && echo "vcan available" || echo "vcan NOT available on this kernel"
```

On the WSL2 kernel `6.6.87.2-microsoft-standard-WSL2` this prints
`modprobe: FATAL: Module vcan not found`. That is expected and **not** a project defect: the
integration tests skip with a printed reason rather than failing (R-01). To run the integration
layer locally, use a native Linux install or a full Linux VM; GitHub Actions' `ubuntu-latest`
runner loads `vcan` and runs them for real.

---

## 1. Install tools

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build \
  iproute2 can-utils libgtest-dev libgmock-dev
```

`can-utils` gives `candump`, `cansend`, `cangen` and `canplayer` — the independent reference the
decoder is checked against, so a decoder bug cannot hide behind the simulator that produced the
frame (section 24's "same code assumptions" risk row).

---

## 2. Create `vcan0`

```bash
scripts/setup_vcan.sh          # idempotent: checks before creating
```

Equivalent by hand:

```bash
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set dev vcan0 up
ip -details link show vcan0
```

Teardown:

```bash
scripts/teardown_vcan.sh
```

Both scripts are idempotent, which is a Definition-of-Done item. They never touch an interface
named `can0` or `can1`.

`vcan` has no wire, so a `bitrate` setting is meaningless here — do not draw bus-load conclusions
from this lab (section 21.7).

---

## 3. Verify SocketCAN before trusting any C++

Two terminals. This step exists so that a later failure can be attributed to the code rather than
to the environment.

**Terminal A — observer**

```bash
candump vcan0
```

**Terminal B — inject the golden vectors**

```bash
cansend vcan0 100#0100000000000000     # ignition ON
cansend vcan0 101#7017000000000000     # speed 60.00 km/h
cansend vcan0 102#B004550000000000     # 12.00 V, SOC 85 %
cansend vcan0 103#1000000000000000     # doors closed, locked
cansend vcan0 110#0100000000000000     # heartbeat 1, ECU OK
```

Terminal A must echo all five. Expected decoded meaning is in
[contracts/frame_catalog.md](./contracts/frame_catalog.md) section 3.

---

## 4. Build and test

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected: all `can_unit_*` and `can_stress_*` tests pass. `can_integration_*` tests pass where
`vcan0` exists and **skip with a printed reason** where it does not.

Sanitizer runs, matching the three existing CI jobs:

```bash
# ASan + UBSan
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DTCU_BUILD_BENCHMARKS=OFF -DTCU_SANITIZER_SETARCH=ON
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure

# TSan (clang; never combined with ASan)
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DTCU_BUILD_BENCHMARKS=OFF -DTCU_SANITIZER_SETARCH=ON
cmake --build build-tsan && ctest --test-dir build-tsan --output-on-failure
```

TSan matters most here and works **without** `vcan`: the concurrency evidence comes from
`FakeCanTransport`, so it is available on every host (NFR-03, R-25).

---

## 5. Run the lab

Four terminals.

```bash
# 1. the TCU service under test
./build/bin/tcu_can_service --config config/can_lab.json

# 2. the vehicle ECUs
./build/bin/vehicle_simulator --config config/can_lab.json \
                              --scenario config/scenarios/normal_drive.json

# 3. the body control module
./build/bin/bcm_simulator --interface vcan0

# 4. an independent observer
candump vcan0
```

---

## 6. Validation scenarios

Each row is a thing to do and the exact outcome to look for. The right column names the spec
criterion it discharges.

### 6.1 Startup reports *unknown*, not zero

```bash
./build/bin/tcu_can_service --config config/can_lab.json &
./build/bin/can_control_cli health
```

**Expect:** every signal `"quality": "NEVER_RECEIVED"` with `"value": null`. Specifically **not**
ignition `false` and **not** speed `0`. → B-02, UT-22, section 13.1.

### 6.2 A frame becomes a decoded, fresh signal

Start `vehicle_simulator`, then:

```bash
./build/bin/can_control_cli health
```

**Expect:** `speed_kmh` with a value, `"quality": "FRESH"`, `"source_can_id": "0x101"`, a small
`age_ms`. `candump` shows the same frames. → FR-02, FR-05, FR-07, B-03.

### 6.3 Zero is a value, not a gap

```bash
./build/bin/can_control_cli set-signal speed_kmh 0
./build/bin/can_control_cli health
```

**Expect:** `"value": 0.0` with `"quality": "FRESH"` — distinguishable from 6.1's `null`. → B-04.

### 6.4 One signal goes stale; the others do not

```bash
./build/bin/can_control_cli pause-id 0x101
./build/bin/can_control_cli health     # run it again after the 500 ms deadline passes
```

**Expect:** `speed_kmh` becomes `STALE` after its 500 ms deadline while `heartbeat` and `ignition`
stay `FRESH` on their 3000 ms deadlines. The last good speed is still present. Exactly **one**
`signal.stale` log line appears, not one per 50 ms health tick. → B-07, B-08, B-18, FR-08.

### 6.5 Recovery happens once

```bash
./build/bin/can_control_cli resume-id 0x101
./build/bin/can_control_cli health
```

**Expect:** `FRESH` again, `generation` higher than before, exactly **one** `signal.recovered` line.
→ B-09, FR-09.

### 6.6 An invalid frame does not corrupt a good value

```bash
./build/bin/can_control_cli inject-invalid 0x102     # emits SOC = 120
./build/bin/can_control_cli health
```

**Expect:** both `battery_voltage_v` and `battery_soc_pct` keep their previous values — the whole
frame was rejected, not half of it (atomic reject, R-14). `can_reject_range_total` rose by one and
the signals read `INVALID`. `last_good_at` did not move. → B-11, UT-10, section 13.4.

### 6.7 Command and acknowledgment correlate

```bash
./build/bin/can_control_cli lock --sequence 0x2A
```

**Expect:** `candump` shows `200#012A...` then `201#2A00...`; the service logs `command.acked` with
`sequence=0x2A result=OK`; `0x103` follows with the locked bit set. Then send an ACK for a sequence
nobody is waiting on and confirm `can_ack_unmatched_total` rises while nothing else changes.
→ B-22, B-23, FR-14.

### 6.8 Interface failure degrades and recovers without a busy loop

```bash
sudo ip link set dev vcan0 down
./build/bin/can_control_cli health     # expect "DEGRADED", signals LINK_UNAVAILABLE
top -p "$(pgrep -f tcu_can_service)"   # expect near-zero CPU: backoff, not a spin
sudo ip link set dev vcan0 up
./build/bin/can_control_cli health
```

**Expect:** state returns to `RUNNING` and `can_link_reconnect_total` rises by one. Signals stay
non-fresh until a **new valid frame** arrives — transport recovery alone never restores `FRESH`.
→ B-19, B-20, FR-13, section 13.6.

Error order and errno vary across kernels, so assert the final observable state rather than a
specific errno sequence (section 21.6).

### 6.9 Overload is bounded and counted

```bash
cangen vcan0 -I 101 -L 8 -g 0 &        # flood; stop it after a few seconds
./build/bin/can_control_cli metrics
```

**Expect:** `can_queue_depth <= can_queue_capacity` throughout, `can_rx_queue_drop_total` rising,
`can_rx_enqueued_total + can_rx_queue_drop_total == can_rx_read_total`, and resident memory flat.
Drop warnings are rate-limited, not one line per frame. → B-16, FR-10, NFR-02, section 13.7.

### 6.10 Shutdown is clean and idempotent

```bash
kill -TERM "$(pgrep -f tcu_can_service)"
```

**Expect:** the process exits on its own with no `SIGKILL` needed, `can_service.stopped` is logged
with a `discarded_frames` count, and a second `stop()` in the repeated-shutdown unit test does
nothing. No sanitizer finding. → B-25, B-26, UT-17, FR-11, NFR-11.

---

## 7. Performance characterization

```bash
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-rel
./build-rel/bin/can_bench --rate 100 --duration 60
```

Prints host CPU model, kernel release, compiler and flags, queue capacity, frame rate and duration,
then p50/p95/p99 decode latency, peak RSS and total drops.

**No target is asserted and none should be inferred.** NFR-05 and section 18.4 forbid turning one
laptop's numbers into a performance claim; the recorded conditions are the point, not the figures.

---

## 8. Known limitations

These are design boundaries, not defects. The README repeats them, which is a Definition-of-Done
item (section 26).

- `vcan` cannot reproduce bus arbitration timing, transceiver behaviour, electrical faults or
  authentic bus-off recovery. Those need HIL or real hardware (section 15).
- A successful `send()` means the local socket accepted the frame — never that a peer processed it
  (section 10.2).
- `can_rx_read_total` does not prove the kernel dropped nothing; `SO_RXQ_OVFL` is a V2 item (R-24).
- Some malformed frames cannot be placed on a real `vcan0` at all, so they are injected through
  `FakeCanTransport`. An impossible on-wire test is never claimed as passed (section 18.1).
- The decoder is hard-coded to this synthetic seven-frame catalog. General DBC decoding is project
  06 (ADR-02).
- Classical CAN only. CAN FD is a V2 extension (section 10.5).
