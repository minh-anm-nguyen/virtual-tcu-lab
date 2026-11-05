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
| 02 IPC Service Framework | `tcu_ipc` | complete - unit, integration and fault suites; Linux only |
| 03 Timer & Task Scheduler | `tcu_scheduler` | complete - 87 unit, 20 integration, 9 stress tests; Linux and Windows |
| 04 Configuration & Logging | `tcu_config`, `tcu_logging` | complete - 118 unit, 17 integration, 3 stress tests; clean under ASan/UBSan and TSan; Linux and Windows |
| 05-30 | - | not started |

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

## Project 02: IPC Service Framework

`tcu_ipc` carries request/response and notifications **between processes** over a Unix domain
socket. Project 01's event bus moves events inside one process; this moves them across a process
boundary. The two are not interchangeable: they have different fault models and different delivery
guarantees.

```text
tcu_client  ---- AF_UNIX / SOCK_STREAM ---->  vehicle_service
   IpcClient                                      IpcServer
   request/response with deadlines                handlers on an executor
   bounded TX queue                               per-session bounded TX queue
   reconnect with backoff                         SO_PEERCRED authorization
   subscriptions restored on reconnect            notifications to subscribers
```

**Protocol v1** is a length-prefixed binary frame - `body_length`, `version`, `type`, `flags`,
`request_id`, `name_length`, name, payload - all big-endian. The payload is opaque: the library
never parses it, so there is no JSON dependency. The byte layout, the reserved `ipc.*` control
methods (`ipc.hello`, `ipc.subscribe`, `ipc.unsubscribe`) and the nine wire error codes are
specified in `specs/002-ipc-service-framework/contracts/wire_protocol.md`.

### Run the demo

```bash
./scripts/run_ipc_demo.sh
```

It starts the service, runs a status request, subscribes to `vehicle.ignitionChanged`, kills the
service with `SIGKILL`, restarts it, and shows the client detecting the loss, backing off,
reconnecting, re-subscribing and receiving notifications again - the goal stated in the design
document's section 1. By hand, in three terminals:

```bash
./build/debug/apps/vehicle_service/vehicle_service --socket /tmp/virtual-tcu/vehicle.sock
./build/debug/apps/tcu_client/tcu_client --socket /tmp/virtual-tcu/vehicle.sock \
  --scenario status-and-subscribe
pkill -9 -f vehicle_service      # controlled demo environment only
```

The socket must live on the Linux filesystem. Binding an `AF_UNIX` socket on a DrvFs path such as
`/mnt/d/...` fails `EOPNOTSUPP` under WSL2, so keep the path under `/tmp`.

### Test labels

```bash
ctest --test-dir build/debug -L ipc-unit            # codec, client, server, session, metrics
ctest --test-dir build/debug -L ipc-integration     # forks the real service binary
ctest --test-dir build/debug -L ipc-fault           # the design document's fault matrix
TCU_IPC_STRESS_SECONDS=1800 ctest --test-dir build/debug -R FaultStress   # the 30-minute soak
```

### IPC design trade-offs

| Decision | v1 choice | Cost |
|---|---|---|
| Transport | `AF_UNIX` / `SOCK_STREAM` | Linux-local only; framing is ours to get right |
| Readiness | `poll()`, not `epoll` | O(n) per wake, which is free at 32 sessions |
| Payload | opaque bytes, no JSON library | the apps compose and validate their own payloads |
| Concurrency | one I/O loop plus a one-thread executor | more moving parts than thread-per-client, far more control |
| Backpressure | bounded queues, fail fast | callers must handle `QueueFull` |
| Slow consumer | drop the frame and count it | a notification can be lost; no session is ever starved |
| Retry | reconnect only, never replay an RPC | a side-effecting call is the caller's to repeat |
| Authorization | directory permissions plus `SO_PEERCRED` | local Linux only; no cryptographic identity |

### What IPC v1 does not promise

No multi-host transport, no shared memory, no POSIX message queues. No exactly-once delivery, no
durable replay after a crash, no transactions. No encryption on the socket. No automatic retry of a
request that has side effects. No `flags` bits - any nonzero value is a protocol error, which is
what reserves them for v2. Notifications produced while a client is disconnected are lost.

Windows is not a target for this project at all: it needs `SO_PEERCRED`, `poll` and `MSG_NOSIGNAL`,
so the CMake targets are guarded and a Windows configure prints `Project 02 (IPC) skipped`.

## Project 03: Timer & Task Scheduler

`tcu_scheduler` schedules delayed and periodic work inside one process. One coordinator thread sleeps
on the earliest deadline of a min-heap; a bounded FIFO queue and a fixed worker pool run the
callbacks. No user callback is ever invoked while a scheduler lock is held, so a callback may
schedule and cancel freely.

Design document: `Virtual_TCU_Lab_30_System_Designs/03_Timer_Task_Scheduler_System_Design.md`.
Spec and contracts: `specs/003-timer-task-scheduler/`.

```cpp
#include "tcu/scheduler/timer_scheduler.hpp"

tcu::scheduler::TimerScheduler scheduler(tcu::scheduler::Config{});
scheduler.start();

// A timeout.
auto id = scheduler.scheduleAfter(std::chrono::seconds(3), [] { /* ... */ }).id;
scheduler.cancel(id);

// Telemetry sampling that stays on its nominal cadence and skips what it misses.
scheduler.scheduleEvery(std::chrono::seconds(1),
                        tcu::scheduler::PeriodicMode::FixedRateSkipMissed,
                        [] { /* ... */ });

scheduler.stop(tcu::scheduler::StopMode::Drain);
```

Unlike project 02 this one is **portable**: it uses only the standard library, so it builds and runs
on Linux and on Windows. Only one integration test is Linux-gated, because it drives a real project
02 IPC server.

### Run the demos

```bash
./scripts/run_scheduler_demo.sh build-wsl        # the whole spec section 20 workflow

# Or one at a time:
./build-wsl/apps/telemetry_timer_demo/telemetry_timer_demo --interval-ms 1000 --samples 5
./build-wsl/apps/modem_timeout_demo/modem_timeout_demo --timeout-ms 3000 --reply-ms 2000
./build-wsl/apps/modem_timeout_demo/modem_timeout_demo --timeout-ms 50 --reply-ms 50 --repeat 300
```

The last one is the interesting one: with the deadline equal to the reply latency the race is
genuinely undecided, and the demo asserts that `replied + timed_out` equals the round count exactly.
Cancelling the timer is **not** what makes the outcome single - the caller owns an atomic request
state, because `cancel()` can return `RunningCancelRequested` when the timeout callback has already
started.

### Test labels

```bash
ctest --test-dir build-wsl -L scheduler-unit          # fake clock; every deadline case, no sleeps
ctest --test-dir build-wsl -L scheduler-integration   # real clock; backpressure, shutdown, project 01 and 02
ctest --test-dir build-wsl -L scheduler-stress        # concurrency, churn, 10k timers
TCU_SCHEDULER_STRESS_SECONDS=600 ctest --test-dir build-wsl -L scheduler-stress   # a soak
```

### Scheduler design trade-offs

| Decision | v1 choice | Cost |
|---|---|---|
| Deadline clock | `steady_clock` only | no wall-clock or calendar scheduling; a suspend/resume may leave timers overdue |
| Locking | one mutex for registry, heap, states and queue | coarser than two mutexes, but no lock order to get wrong and a callback can call back in |
| Periodic overload | explicit mode: drift (`FixedDelay`) or skip (`FixedRateSkipMissed`) | the caller must choose; there is no implicit catch-up |
| Ready queue full | a due one-shot stays scheduled and retries; a fixed-rate tick is skipped and counted | lateness grows under overload; it is measured, not bounded |
| Lifecycle | single-use: `start()` after `Stopped` is `InvalidState` | a restart means a fresh instance, which avoids thread-resurrection races |
| `cancel()` of a running callback | cooperative: `RunningCancelRequested`, no interruption | the caller needs its own completion state for a race (see the demo above) |
| `stop()` | no timeout, no force-kill | a callback that never returns blocks `stop()` for ever, by design |

### Benchmark

```bash
./build-wsl/benchmarks/scheduler_bench/scheduler_bench --timers 10000 --duration-s 30
```

Measured on this repository's WSL2 Ubuntu host, 8 hardware threads, GCC 11.4, **debug build with
assertions on**, 10,000 fixed-rate timers at a 1 s cadence and 2 workers: about 5,900 callback starts
per second, mean start lateness 1.8 ms, p50 under 1 ms, p95 under 10 ms, p99 under 25 ms, no skipped
periods. A release build and a dedicated host would do better.

That is a measurement, not a promise. Nothing in the test suite asserts a latency threshold: the
design document calls its p99 target a benchmark, not a guarantee, and a workstation number says
nothing about real-time behaviour.

### What scheduler v1 does not promise

No hard real-time scheduling, no OS real-time priorities, no sub-millisecond precision. No durable
jobs: nothing survives a process restart, and there is no exactly-once delivery across a crash. No
cron or calendar expressions. No callback priorities and no runtime resizing of the worker pool. No
way to interrupt a running callback. Nothing executes while the host is suspended.

## Logging

The library emits structured `key=value` lines only when `TCU_EVENTBUS_LOG` is set, so test output
stays clean:

```bash
TCU_EVENTBUS_LOG=1 ./build/debug/apps/event_bus_demo/event_bus_demo
```

Only the keys listed in the project's metrics contract are ever emitted. Payload contents, keys,
tokens and credentials never appear in a log line.

## Project 04: Configuration & Logging Service

Two libraries and one demonstration.

`tcu_logging` is a bounded asynchronous structured logger: a capped MPSC queue, one writer thread
that alone owns the sinks, severity filtering before formatting, newline-delimited JSON to a
console sink and a size-rotating file sink, a severity-aware overload policy, and a sink that goes
unhealthy instead of taking the application down. It links **nothing** - not even project 03 - so
any project from 01 to 30 can use it without inheriting a dependency.

`tcu_config` loads one JSON file into an immutable typed snapshot, validates types, ranges,
cross-field invariants, the schema version and unknown keys before publishing anything, hands the
snapshot over at a single atomic point, bumps a generation only on an effective change, rejects the
whole candidate when any restart-only key moved, and notifies observers asynchronously in
publication order with each callback failure isolated and counted. It reuses project 03's
`TimerScheduler` for both of its background jobs - the notifier and the file poller - so it adds no
thread and no queue of its own.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build

ctest --test-dir build -L logging-unit --output-on-failure
ctest --test-dir build -L config-unit --output-on-failure
ctest --test-dir build -L config-logging-integration --output-on-failure
ctest --test-dir build -L config-logging-stress --output-on-failure

# A longer soak: same test, more seconds.
TCU_CONFIG_STRESS_SECONDS=3600 ctest --test-dir build -R Soak --output-on-failure

# The six scenarios of the design document, unattended:
scripts/run_config_logging_demo.sh build
```

The first configure downloads one pinned header, `nlohmann/json` v3.11.3, verified by SHA256. It is
fetched by URL rather than through the upstream CMake project, which declares
`cmake_minimum_required(VERSION 3.1)` and so is refused outright by CMake 4. After the first
configure the build is offline.

### Metrics

`ConfigurationService::metrics()` reports the committed generation, the notification queue depth, a
sticky degraded flag, and counters for reload attempts, committed changes, no-ops, failures by
reason, notification deliveries, notification failures and resyncs. `AsyncLogger::metrics()`
reports queue depth, per-sink health, and counters for records submitted, filtered, enqueued,
dropped by severity, truncated, written per sink, sink errors, rotations and write latency.

Every label comes from a closed set - a severity name, a sink name, or a `LoadError` name - so
metric cardinality is bounded by construction. No path, correlation id or endpoint is ever a label,
and no configuration **value** appears in a log line: a generation, a JSON pointer and a bounded
reason are all the configuration content the vocabulary allows.

### Failure behaviour

Every row of the design document's failure matrix has a test:

| Failure | Covered by |
|---|---|
| Missing file at startup | `Lifecycle.StartOnAMissingFileFails` |
| Bad JSON on reload | `Reload.InvalidJsonKeepsSnapshot` |
| Unsupported schema version | `Loader.RejectsUnsupportedSchemaVersion` |
| Temporary read failure | `Loader.MissingFileIsFileNotFound`, `Poller.MissingFileDoesNotSpin` |
| Notification failure or overflow | `Notify.ObserverThrowIsIsolatedAndCounted`, `Notify.OverflowMarksDegradedAndResyncs` |
| Disk full | `FileSinkTest.DiskFullMarksUnhealthyAppSurvives` |
| Permission denied | `FileSinkTest.UnwritablePathFallsBackToStderr` |
| Slow log disk | `Queue.DropsInfoWhenFull`, `Queue.BoundedWaitForError` |
| Oversized or classified field | `Record_OversizeTruncatedAndCounted`, `Redaction.SecretAbsentFromEverySink` |
| Shutdown with a blocked consumer | `Shutdown.QueueLossIsReportedNotHidden`, `Notify.OverflowMarksDegradedAndResyncs` |

Disk-full, permission-denied, short-write and `EINTR` are injected through a `FileOps` seam. No test
fills a real disk or changes a real permission.

### What project 04 v1 does not promise

- No cloud configuration sync, no remote command channel, no OTA rollout.
- No multi-process consistency: a second process has its own instance and may run a different
  generation.
- No partial application of a candidate that touches a restart-only key - the whole file is refused.
- No defaults layer, no environment overrides, no command-line key overrides. Every key is
  mandatory and the file is the only source.
- No credential provider, and no key in the schema holds a secret.
- No durability. Records still queued when the process dies are lost; nothing in the API pretends
  otherwise.
- Change detection is a 500 ms mtime-and-size poll, so a write that preserves both is missed and
  detection latency is up to one interval. Explicit `reload()` is the deterministic path.
- Rotation is size-based only: no time-based retention, no compression.
- Performance numbers from a workstation run are measurements of that workstation, not guarantees.

### Platform notes

Both targets build and test on Linux and on Windows. Two Windows caveats, stated rather than hidden:

- `std::filesystem::permissions` only toggles the read-only attribute, so the owner-only permission
  assertion is skipped there (it runs on Linux).
- The filesystem timestamp has one-second resolution, which is the limit the poller shortcut
  documents.

With the MSYS2/UCRT64 toolchain, put `C:/msys64/ucrt64/bin` first on `PATH` before building or
running tests: `gtest_discover_tests` runs each test binary at build time, and another toolchain
earlier on `PATH` can shadow the runtime DLLs, which shows up as exit code `0xc0000139`.
