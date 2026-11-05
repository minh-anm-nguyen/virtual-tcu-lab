# Quickstart: validating the Timer & Task Scheduler

Unlike project 02, project 03 builds on **both** Linux and Windows — it uses only the standard
library (research R-00). Sanitizer runs are Linux/WSL only, because that is a toolchain limit.

## Prerequisites

Linux / WSL2 Ubuntu 22.04+:

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build gdb clang
cmake --version   # >= 3.20
g++ --version     # >= 11
```

Windows: Visual Studio 2022 Build Tools (or MSVC 19.3x) plus CMake >= 3.20 and Ninja. GoogleTest is
fetched by CMake at the pin project 01 set (`v1.14.0`); nothing to install.

---

## 1. Build and run the tests

```bash
cd /mnt/d/TCU            # D:\TCU on Windows
cmake -S . -B build-wsl -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-wsl
ctest --test-dir build-wsl --output-on-failure
```

Expected: the project 01 and 02 suites plus the three project 03 suites, all passing. The project 03
cases are labelled so they can be run alone:

```bash
ctest --test-dir build-wsl -L scheduler-unit        --output-on-failure
ctest --test-dir build-wsl -L scheduler-integration --output-on-failure
ctest --test-dir build-wsl -L scheduler-stress      --output-on-failure
```

The unit suite is the one to run while implementing: every deadline assertion uses the fake clock, so
it finishes in well under a second and cannot flake on timing (research R-17).

The stress suite runs 5 seconds by default. For a soak:

```bash
TCU_SCHEDULER_STRESS_SECONDS=600 ctest --test-dir build-wsl -L scheduler-stress --output-on-failure
```

### Seeing the structured log

Only INFO and above reach stderr by default, so the per-schedule DEBUG line is opt-in:

```bash
TCU_SCHEDULER_LOG_LEVEL=debug ./build-wsl/apps/telemetry_timer_demo/telemetry_timer_demo --samples 2
```

### Sanitizers (Linux only)

```bash
# ASan + UBSan
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan
TCU_SCHEDULER_STRESS_SECONDS=3 ctest --test-dir build-asan -L scheduler --output-on-failure

# TSan — clang only, and setarch is required (root CMakeLists option)
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++ -DTCU_SANITIZER_SETARCH=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer"
cmake --build build-tsan
ctest --test-dir build-tsan -L scheduler --output-on-failure
```

Never combine ASan and TSan in one build. The acceptance criterion in spec section 21 ("no data race
or memory error in supported sanitizer builds") is evaluated by these two runs.

---

## 2. Demo 1 — telemetry timer (spec section 20, steps 1-2)

```bash
./build-wsl/apps/telemetry_timer_demo/telemetry_timer_demo --interval-ms 1000 --samples 5
```

Expected output: five periodic samples roughly a second apart, published onto project 01's Event Bus
and received by a subscriber, then a clean stop:

```text
scheduler.started workers=2
telemetry.sample tick=1 vehicle_speed=0
telemetry.sample tick=2 vehicle_speed=0
...
scheduler.stopped mode=drain
```

Then the slow-callback case, which is what makes `periods_skipped_total` and the lateness histogram
move (spec section 20, step 5):

```bash
./build-wsl/apps/telemetry_timer_demo/telemetry_timer_demo \
  --interval-ms 100 --callback-ms 250 --samples 5 --mode fixed-rate
```

Expected: starts stay aligned to 100 ms boundaries, some slots are skipped, and the printed metrics
show a non-zero `periods_skipped_total` with no burst of back-to-back callbacks
([contracts/timing_semantics.md](./contracts/timing_semantics.md) section 4).

Switch `--mode fixed-delay` on the same arguments and the starts drift to ~350 ms apart with
`periods_skipped_total == 0` — the observable difference between the two modes.

---

## 3. Demo 2 — modem timeout race (spec section 20, steps 3-4)

```bash
# Reply at 2 s beats the 3 s timeout: the timer is cancelled
./build-wsl/apps/modem_timeout_demo/modem_timeout_demo --timeout-ms 3000 --reply-ms 2000

# Reply at 4 s loses: the timeout fires and the late reply is ignored
./build-wsl/apps/modem_timeout_demo/modem_timeout_demo --timeout-ms 3000 --reply-ms 4000
```

Expected, matching spec section 20's illustrative log:

```text
modem.request id=17 timeout_ms=3000
modem.reply id=17 latency_ms=2000
scheduler.cancel id=42 result=cancelled
modem.request id=18 timeout_ms=3000
modem.timeout id=18
modem.late_reply id=18 ignored=true
```

The point of this demo is spec section 12.13: `cancel()` alone does not guarantee a single
completion. The request state is an `std::atomic` flipped with `compare_exchange_strong`, and
`ignored=true` is the losing side observing that it lost.

To make the race actually race rather than being decided by a 1 s margin:

```bash
./build-wsl/apps/modem_timeout_demo/modem_timeout_demo --timeout-ms 50 --reply-ms 50 --repeat 300
```

Expected: exactly `300` completions in total across `modem.reply` and `modem.timeout`, never 299 and
never 301 - the demo exits non-zero if the totals disagree. That count is the assertion; which side
wins each round is not. Measured 2026-11-04: `replied=91 timed_out=209 completed=300`.

---

## 4. Shutdown semantics (spec section 20, steps 6-7)

```bash
./build-wsl/apps/telemetry_timer_demo/telemetry_timer_demo --interval-ms 100 --samples 3 --stop drain
./build-wsl/apps/telemetry_timer_demo/telemetry_timer_demo --interval-ms 100 --samples 3 --stop discard
```

| `--stop drain` | `--stop discard` |
|---|---|
| callbacks already due or queued at stop time run | nothing not already running starts |
| future timers are cancelled and counted | everything not running is cancelled |
| `stop()` returns once that bounded set finishes | `stop()` returns once in-flight callbacks finish |

Both print the final metrics snapshot; in both cases `ready_queue_depth` is 0 and
`callbacks_started_total == callbacks_completed_total + callbacks_failed_total` (M1, M5).

---

## 5. Benchmark PT-01 (spec section 19.4)

```bash
cmake --build build-wsl --target scheduler_bench
./build-wsl/benchmarks/scheduler_bench/scheduler_bench --timers 10000 --duration-s 30
```

Prints the table spec section 19.4 asks for: host, cores, OS/kernel, compiler and flags, timer count,
callback rate, worker count, queue limits, p50/p95/p99 lateness derived from the histogram buckets,
and skip counts. Peak RSS is reported by the surrounding shell (`/usr/bin/time -v`), not by the
library.

This is a measurement, not a gate. No CTest case fails on a latency threshold — spec section 6 calls
the p99 < 50 ms figure a benchmark target and section 19.4 forbids reading a workstation run as a
real-time guarantee (research R-18).

---

## 6. Acceptance checklist (spec section 21)

| Criterion | How this quickstart shows it |
|---|---|
| FR-01..FR-12 implemented | `ctest -L scheduler-unit`; plan.md's traceability table maps each FR to a named test |
| All 20 behavioural contracts covered | the same table maps B-01..B-20 to unit, integration or stress cases |
| No callback runs under an internal lock | section 1's unit suite — `Lock_CallbackCanScheduleAndCancel` would deadlock (S1) |
| Cancellation and shutdown contracts tested | `-L scheduler-unit` (UT-05..07, 12, 13) and `-L scheduler-integration` (IT-02, IT-03) |
| Timer and ready queues bounded; compaction tested | `Capacity_RejectsBeyondMaxTimers`, `ReadyQueue_NeverExceedsCapacity`, `DeadlineQueue_CompactionBoundsSize` |
| No data race or memory error | the two sanitizer runs in section 1 |
| Metrics correct | the M1-M6 conservation tests, plus the snapshot each demo prints |
| Compiles under C++17, no hardware | sections 1-4; nothing opens a device or a socket except the Linux-only IPC integration case |
| Limitations documented | [contracts/timing_semantics.md](./contracts/timing_semantics.md) section 1 ("Not guaranteed") and the shortcut ledger in plan.md |
