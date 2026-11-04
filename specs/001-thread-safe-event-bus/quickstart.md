# Quickstart: validating the Thread-Safe Event Bus

How to prove the feature works end to end. Commands only - no implementation code. The API surface is
in [contracts/event_bus_api.md](./contracts/event_bus_api.md); counter meanings are in
[contracts/metrics.md](./contracts/metrics.md).

## Prerequisites

Per research R-00 the build target is Linux. On this Windows machine that means WSL2 Ubuntu.

```bash
# in WSL2 Ubuntu 22.04+
sudo apt update
sudo apt install -y build-essential cmake git
cmake --version   # must be >= 3.20
g++ --version     # must be >= 11, or clang++ >= 14
cd /mnt/d/TCU
```

If `cmake` is older than 3.20, install it from Kitware's apt repository rather than the distro
package. Network access is needed on the first configure, because GoogleTest is fetched by
`FetchContent` (R-14).

## 1. Build and run the tests

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j
ctest --test-dir build/debug --output-on-failure
```

Expected: every test passes, and the count is at least 25 - the 15 unit tests of spec section 9.1
plus the 10 behaviour scenarios B01-B10 of section 18.2. A pass count below that means a required
test was never written, not that the build is clean.

```bash
ctest --test-dir build/debug -N   # list names; check UT-01..UT-15 and B01..B10 are all present
```

## 2. Run the demo (spec section 20)

```bash
./build/debug/apps/event_bus_demo/event_bus_demo
```

Expected behaviour, in order:

1. A bus with `capacity=3`; `PowerManager` and `Telemetry` subscribe to `IgnitionChanged` in that
   order.
2. The simulator publishes `OFF -> ON -> OFF` with sequence ids.
3. Each line carries `event_id`, `event_type`, `publisher`, `subscriber`, `queue_depth` and
   `publish_status`, and nothing else (see the log-field list in `contracts/metrics.md`).
4. For each event, `PowerManager` is logged before `Telemetry` - registration order, G9.
5. The slow-handler phase fills the queue; `Reject` yields `QueueFull`, and `BlockUntil` with a short
   deadline yields `TimedOut`.
6. `stop(Drain)` prints a summary in which `accepted == dispatched` and `discarded == 0`.
7. A second run with `stop(Discard)` prints `accepted == dispatched + discarded`, with
   `discarded > 0`.

The log shape to expect is the example in spec section 20. Treat the numbers there as illustrative,
not as expected values.

## 3. Sanitizers

Two separate builds. ASan and TSan must never be combined (spec section 9.2).

```bash
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
cmake --build build/asan -j && ctest --test-dir build/asan --output-on-failure

cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g"
cmake --build build/tsan -j && ctest --test-dir build/tsan --output-on-failure
```

Expected: no ASan/UBSan report, no leak, no TSan data-race or lock-order-inversion report. A TSan
warning is a failure, not noise - NFR-02 is "no data race", and the concurrency stress test B09 is
the case that exercises it.

## 4. Benchmark

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
./build/release/benchmarks/event_bus_bench/event_bus_bench
```

Expected: the harness prints throughput and p50/p95/p99 enqueue-to-dispatch latency **together with**
CPU model, core count, OS and kernel version, compiler and version, build type, and queue capacity.

NFR-06 forbids inventing numbers: whatever this prints on your machine is the result. Paste the output
into the README as measured, with its environment block. Do not carry numbers over from another
machine, and do not treat a good benchmark as evidence of correctness (spec section 21).

## 5. Acceptance checklist

Maps to spec section 10. Verified 2026-11-04; every line is checked.

- [x] Builds on Ubuntu with C++17 and CMake (step 1)
- [x] Public API documents ownership, state transitions and callback/thread rules (`contracts/event_bus_api.md` plus Doxygen comments in the headers)
- [x] Multiple publishers, multiple subscribers, bounded queue (UT-02, UT-03, B09)
- [x] `Reject` and `BlockUntil` return the documented statuses (UT-04, UT-05, B03, B04)
- [x] `Drain` and `Discard` honour their contracts (UT-10, UT-11, B07, B08)
- [x] No callback invoked while an internal lock is held; no deadlock in supported usage (TSan run, UT-15)
- [x] An RAII subscription outliving the bus does not dangle (UT-13, ASan run)
- [x] All 15 unit tests of section 9.1 pass (step 1)
- [x] CI runs unit tests plus both sanitizer jobs (step 3 wired into CI)
- [x] README has the quickstart, the design trade-offs, and a reproducible benchmark (step 4)

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `ctest` reports 0 tests | `TCU_BUILD_TESTS` is off, or GoogleTest `FetchContent` failed - check network on the first configure |
| A test hangs instead of failing | a `Drain` stop waiting on a handler that never returns (G17). Every test needs its own timeout guard; this is the failure mode section 9.1 warns about |
| TSan reports a race on the subscription `active` flag | the dispatcher snapshotted the list but did not re-check `active` under the per-subscription mutex before invoking (R-13) |
| `stop()` never returns | `stop()` was called from inside a handler - unsupported, G20 |
| A flaky timing test | the test used `sleep` instead of the condition-variable test gate (section 9.1 forbids sleep-based synchronization) |
