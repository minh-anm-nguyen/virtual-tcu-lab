# Quickstart: validating the IPC Service Framework

Every command runs **inside WSL2 Ubuntu** (research R-00). Project 02 does not build on Windows: it
needs `AF_UNIX`, `poll`, `SO_PEERCRED` and `MSG_NOSIGNAL`, none of which Win32 provides.

## Prerequisites

```bash
# in WSL2 Ubuntu 22.04+
sudo apt update
sudo apt install -y build-essential cmake ninja-build gdb clang
cmake --version   # >= 3.20
g++ --version     # >= 11
```

GoogleTest is fetched by CMake at the pin project 01 set (`v1.14.0`); nothing to install.

**One WSL2-specific rule**: the Unix socket must live on the Linux filesystem, not on `/mnt/d`.
Binding a socket on DrvFs fails `EOPNOTSUPP` (measured 2026-11-04). The defaults already use
`/tmp/virtual-tcu/`; do not move them next to the build directory.

## 1. Build and run the tests

```bash
cd /mnt/d/TCU
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected: the project 01 suite plus the project 02 suites, all passing. The project 02 cases are
labelled so they can be run alone:

```bash
ctest --test-dir build -L ipc-unit --output-on-failure
ctest --test-dir build -L ipc-integration --output-on-failure
ctest --test-dir build -L ipc-fault --output-on-failure
```

The stress case runs 5 seconds by default (research R-14). For the section 12.3 soak:

```bash
TCU_IPC_STRESS_SECONDS=1800 ctest --test-dir build -R Stress --output-on-failure
```

## 2. Run the demo (spec section 16)

The scripted version does the whole section 16 sequence, including the kill and the restart:

```bash
./scripts/run_ipc_demo.sh
```

By hand, in three terminals:

```bash
# Terminal 1
./build/apps/vehicle_service/vehicle_service --socket /tmp/virtual-tcu/vehicle.sock

# Terminal 2
./build/apps/tcu_client/tcu_client --socket /tmp/virtual-tcu/vehicle.sock \
  --scenario status-and-subscribe

# Terminal 3, once the status request has succeeded
pkill -9 -f 'vehicle_service'     # controlled demo environment only
./build/apps/vehicle_service/vehicle_service --socket /tmp/virtual-tcu/vehicle.sock
```

Expected client output, in this order (spec section 16.1):

```text
[client] state=Connected protocol=v1
[client] request_id=1 method=vehicle.getStatus outcome=success
[client] subscription=vehicle.ignitionChanged outcome=active
[client] state=Backoff reason=PeerClosed pending_failed=1
[client] state=Connected reconnect_count=1
[client] subscription=vehicle.ignitionChanged outcome=restored
[client] notification topic=vehicle.ignitionChanged ignition=false
```

The three lines that matter are the last three: they prove the client detected the peer loss,
reconnected on its own, re-subscribed and received a notification afterwards - which is the demo
goal stated in spec section 1.

## 3. Fault injection by hand

Each case corresponds to a row of spec section 10; all of them also run as tests.

```bash
# Slow handler: the client must time out at 2 s and discard the late response
./build/apps/vehicle_service/vehicle_service --socket /tmp/virtual-tcu/vehicle.sock --slow-ms 3000
./build/apps/tcu_client/tcu_client --socket /tmp/virtual-tcu/vehicle.sock --scenario status

# Unauthorized peer: a UID not on the allowlist is rejected before any frame is read
./build/apps/vehicle_service/vehicle_service --socket /tmp/virtual-tcu/vehicle.sock --allow-uid 0
./build/apps/tcu_client/tcu_client --socket /tmp/virtual-tcu/vehicle.sock --scenario status
# expect (measured 2026-11-04): the client never reaches Connected and exits non-zero with
#   [client] state=Backoff reason=not_connected
# and the server logs one rejected_peer line per attempt, each carrying the peer identity:
#   event=rejected_peer reason=uid=1000 gid=1000 pid=20023
# The rejection happens after accept() and before any frame is read, so no handler runs.

# Slow consumer: the client stops reading; the server's queue must stay capped
./build/apps/tcu_client/tcu_client --socket /tmp/virtual-tcu/vehicle.sock --scenario slow-reader
```

## 4. Sanitizers

ASan/UBSan and TSan are separate builds and must never be combined (spec section 12.3):

```bash
# ASan + UBSan
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure

# TSan: clang only, and under setarch -R (project 01 research R-00)
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++ -DTCU_SANITIZER_SETARCH=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread"
cmake --build build-tsan
ctest --test-dir build-tsan --output-on-failure
```

GCC's TSan is not usable here - project 01 measured that `g++ 11.4.0`'s `libtsan` has no
interceptor for `pthread_cond_clockwait`, which produces false "double lock of a mutex" reports on
every timed wait. The IPC client uses timed waits too, so the same rule applies.

**Measured 2026-11-04**, WSL2 Ubuntu-22.04:

| Build | Result |
|---|---|
| g++ 11.4.0, Ninja, Debug | 137/137 tests pass (51 from project 01, 86 from project 02) |
| ASan + UBSan (g++) | 86/86 project 02 tests pass, no sanitizer report |
| TSan (clang++ 14, `setarch -R`) | 86/86 project 02 tests pass, no ThreadSanitizer report |

The sanitizers earned their place here - each found something the other builds did not:

- **ASan** exposed a racy *assertion*, not a product defect:
  `FaultPartialFrame.OversizePrefixIsRejectedBeforeAllocation` checked `sessionCount() == 0` the
  instant its peer saw EOF. The peer observes EOF when the descriptor closes, a few instructions
  before `connections_active` is restored, so under ASan's slowdown the check could read the old
  value. It now waits for the count with a deadline.
- **TSan** found a real data race in the client. `wake()` is callable from any thread, and it wrote
  the self-pipe's `UnixSocket::last_errno_`, so `subscribe()` on a user thread raced
  `sendSubscribe()` on the I/O thread. The write end is now a bare `std::atomic<int>` descriptor
  with an ignored `::send` result. The review checklist of spec section 19 would not have caught
  this: the shared state was hidden inside a type that looked single-owner.

## 5. FD leak check

```bash
./build/apps/vehicle_service/vehicle_service --socket /tmp/virtual-tcu/vehicle.sock &
SRV=$!
ls /proc/$SRV/fd | wc -l          # baseline
for i in $(seq 1 20); do
  ./build/apps/tcu_client/tcu_client --socket /tmp/virtual-tcu/vehicle.sock --scenario status
done
ls /proc/$SRV/fd | wc -l          # must equal the baseline
kill $SRV
```

The same comparison runs automatically in `tests/fault_injection/ipc/rapid_restart_test.cpp`.

## 6. Acceptance checklist (spec section 13)

Verified 2026-11-04 on WSL2 Ubuntu-22.04, g++ 11.4.0, cmake 3.30.5, Ninja generator.
136 tests pass in total: 51 from project 01 and 85 from project 02
(66 `ipc-unit`, 11 `ipc-integration`, 8 `ipc-fault`).

- [x] `cmake -S . -B build && cmake --build build` succeeds on Ubuntu. Verified with the Ninja
      generator; the default Make generator was not exercised.
- [x] Two or more processes exchange request/response over a Unix domain socket -
      `./scripts/run_ipc_demo.sh` and `IntegrationRequestResponse.*`, which forks the real
      `vehicle_service` binary.
- [x] Framing is versioned, size-bound and partial-I/O safe - `FrameCodec.*` (10 cases) and
      `FaultPartialFrame.*` (4 cases), including a frame delivered one byte at a time.
- [x] All 20 BDD scenarios of section 9 are covered by tests - the traceability matrix in
      [plan.md](./plan.md) names a test for each of B-01..B-20, and every one of those tests exists
      and passes.
- [x] Kill and restart: the client reconnects without crashing and replays no RPC -
      `IntegrationReconnect.KillWithRequestInflightFailsItOnceThenReconnects` (real `SIGKILL`),
      `ClientLifecycle.NeverReplaysRequest`, and `FaultRapidRestart` over 20 cycles.
- [x] Timeouts and pending promises resolve exactly once -
      `ClientTimeout.TimeoutCompletesExactlyOnce` plus equation E1 asserted in every client test.
- [x] Queues are bounded; a slow client does not grow memory -
      `Session.SlowConsumerDoesNotGrowMemory`, `Session.OneSlowPeerDoesNotStarveAnother`.
- [x] Shutdown is clean; no FD leak is visible in the tests -
      `ClientLifecycle.StopIsIdempotentAndClosesEveryFdOnce`, `FaultRapidRestart` and
      `FaultStress` all compare `/proc/self/fd` before and after.
- [x] Logs carry connection and request IDs and no secrets - `Log.*` (5 cases), including
      `Log.RejectedPeerIsTheOnlyPlaceWithPeerIds`, which captures stderr and asserts that peer
      identity appears only on the `rejected_peer` audit line.
- [x] `README.md` documents the run script, the architecture, the protocol version and the
      limitations - see its "Project 02: IPC Service Framework" section.

**Not delivered, deliberately**: FR-13 (a D-Bus adapter) is marked P2 and "Khong nam trong MVP" by
spec section 3.1, so it is not implemented. Everything else in section 3.1 is.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `bind: Operation not supported` | the socket path is on `/mnt/...` (DrvFs) | use a path under `/tmp` |
| `bind: Address already in use` and `start()` returns false | a live server already owns the path | stop it, or pick another `--socket` |
| server exits with a permission error on start | `/tmp/virtual-tcu` exists with the wrong owner or loose permissions | `rm -rf /tmp/virtual-tcu` and restart; the server recreates it `0700` |
| client stays `Backoff` forever | no server is listening, or the UID is not on the allowlist | check the server's `rejected_peer` log line |
| client stops immediately with `UNSUPPORTED_VERSION` | peer is not protocol v1 | expected and terminal by design (research R-06) |
| `FATAL: ThreadSanitizer: unexpected memory mapping` | ASLR entropy vs TSan's shadow map | configure with `-DTCU_SANITIZER_SETARCH=ON` |
| false TSan "double lock of a mutex" on timed waits | GCC's `libtsan` lacks a `pthread_cond_clockwait` interceptor | build the TSan job with `clang++` |
