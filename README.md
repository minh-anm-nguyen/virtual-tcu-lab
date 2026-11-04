# Virtual TCU Lab

A 30-project lab that builds a simulated Telematics Control Unit in C++17. No real hardware, no
real vehicle network, no real cloud: everything is simulated in-process or over loopback, so the
whole thing runs on a developer machine and in CI with reproducible results.

The design documents for all 30 projects live in `Virtual_TCU_Lab_30_System_Designs/`, and the
spec-driven artifacts for the project being built live in `specs/`. Neither is tracked in git.

## Status

| Project | Target | State |
|---|---|---|
| 01 Thread-Safe Event Bus | `tcu_eventbus` | complete - 51 tests, clean under ASan/UBSan and TSan |
| 02-30 | - | not started |

## Requirements

Linux is the build and test target. The sanitizer jobs need GCC's and Clang's sanitizer runtimes,
and projects 26 and 27 are Linux-only, so a Linux toolchain is needed regardless. On Windows this
means WSL2.

```bash
sudo apt install -y build-essential clang cmake git
cmake --version   # >= 3.20
g++ --version     # >= 11, or clang++ >= 14
```

The first configure fetches GoogleTest v1.14.0 over the network; after that the build is offline.

## Build and test

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j
ctest --test-dir build/debug --output-on-failure
```

Expect 51 tests, covering UT-01..UT-15 from the design document's test table and the behaviour
scenarios B01..B10, plus the demo smoke test.

### Demo

```bash
./build/debug/apps/event_bus_demo/event_bus_demo
```

Shows a bus with capacity 3, `PowerManager` and `Telemetry` subscribed in that order, the vehicle
simulator sending `OFF -> ON -> OFF`, a deliberately slow handler filling the queue so `Reject`
returns `QueueFull` and `BlockUntil` returns `TimedOut`, and then the same scenario run twice -
once ending in `stop(Drain)` and once in `stop(Discard)` - so the two conservation equations are
visible side by side.

### Sanitizers

Two separate builds; ASan and TSan are never combined.

```bash
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug \
      "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer" \
      -DTCU_BUILD_BENCHMARKS=OFF -DTCU_SANITIZER_SETARCH=ON
cmake --build build/asan -j && ctest --test-dir build/asan --output-on-failure

cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ \
      "-DCMAKE_CXX_FLAGS=-fsanitize=thread -fno-omit-frame-pointer" \
      -DTCU_BUILD_BENCHMARKS=OFF -DTCU_SANITIZER_SETARCH=ON
cmake --build build/tsan -j && ctest --test-dir build/tsan --output-on-failure
```

Two flags there are not optional, and both were found the hard way:

- **`-DTCU_SANITIZER_SETARCH=ON`** runs each binary under `setarch -R`. ThreadSanitizer's fixed
  shadow mapping clashes with the high-entropy ASLR of recent kernels; without it the binary aborts
  at startup with `FATAL: ThreadSanitizer: unexpected memory mapping`.
- **`-DCMAKE_CXX_COMPILER=clang++` for TSan.** GCC 11's `libtsan.so` has no interceptor for
  `pthread_cond_clockwait`, which glibc 2.35 uses for every `steady_clock` timed wait. TSan then
  never learns the mutex was released during the wait, and reports a `double lock of a mutex` plus
  a spurious data race for each test that uses a timeout - six of them here. Clang's TSan does
  intercept it and reports nothing. Suppressions were deliberately not used: they would have masked
  genuine races in the same functions.

## Project 01: Thread-Safe Event Bus

An in-process event bus: many publisher threads, a bounded FIFO queue, one dispatcher worker, RAII
subscriptions, two backpressure policies, two shutdown modes, and metrics.

```text
modules/core/event_bus/
  include/tcu/eventbus/   event.hpp, event_bus.hpp, metrics.hpp - the whole public surface
  src/                    bounded_queue, subscription_registry, event_bus - private
apps/event_bus_demo/      the demo scenario, as a library plus a main()
benchmarks/event_bus_bench/
tests/unit/event_bus/
```

### Design trade-offs

| Decision | Why | What it costs |
|---|---|---|
| One dispatcher worker | Ordering and correctness are easy to reason about and to prove | A slow subscriber delays every other subscriber |
| Bounded FIFO queue | Predictable memory, explicit backpressure | A producer can be refused |
| RAII subscription over weak bus state | A handle outliving the bus is inert, not dangling | In-flight callbacks need their own admission control |
| Result-based `publish` | Full, stopped and timed-out are ordinary outcomes, not exceptions | Every caller has to check the status |
| `std::variant` payload | Type and payload cannot disagree; no unchecked casts | Adding an event type recompiles consumers |
| In-process only | Light, testable, no hardware | Not a replacement for IPC or a broker |

### What v1 does not promise

- Nothing survives the process. No persistence, no delivery acknowledgement, no exactly-once.
- No real-time deadline guarantee.
- No protection of one subscriber from another slow subscriber on the same bus.
- No callback timeout: a C++ callback cannot be interrupted safely, so a stuck handler stalls the
  dispatcher and hangs a `Drain` shutdown.
- No isolation from a handler that crashes the process. That needs a supervisor, which is a later
  project.
- The bus does not manage the lifetime of whatever a callback captured.

### Benchmark

Measured on this machine, not copied from anywhere. A benchmark is not evidence of correctness.

```text
cpu_model=Intel(R) Core(TM) i5-10300H CPU @ 2.50GHz
hardware_concurrency=8
kernel=Linux 6.6.87.2-microsoft-standard-WSL2
compiler=gcc 11.4
build_type=release-like (NDEBUG defined)
queue_capacity=4096
publisher_threads=4
events_per_thread=20000

wall_clock_seconds=0.236515
accepted=80000 dispatched=80000 queue_high_watermark=4096
throughput_events_per_second=338244
latency_us_p50=8801
latency_us_p95=32840
latency_us_p99=47791
```

Reproduce with:

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
./build/release/benchmarks/event_bus_bench/event_bus_bench 4 20000 4096
```

Read the latency figures together with the high watermark: four publishers outrun a single
dispatcher, so the queue sits at its 4096-event capacity and the percentiles are mostly queueing
delay, not dispatch cost. Lower the capacity and the latencies fall while publishers block instead.
That is the backpressure design working, not a defect - but it does mean these numbers say little
about per-event overhead.

## Logging

The library emits structured `key=value` lines only when `TCU_EVENTBUS_LOG` is set, so test output
stays clean:

```bash
TCU_EVENTBUS_LOG=1 ./build/debug/apps/event_bus_demo/event_bus_demo
```

Only the keys listed in the project's metrics contract are ever emitted. Payload contents, keys,
tokens and credentials never appear in a log line.
