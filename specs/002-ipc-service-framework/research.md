# Phase 0 Research: IPC Service Framework

The spec leaves a fixed, enumerated set of decisions open: section 4.2 (`SOCK_STREAM` vs
`SOCK_SEQPACKET`, level- vs edge-triggered readiness), section 6 "API notes", section 7
(`pending_requests` ownership - "chon **mot** mo hinh va tai lieu hoa"), section 8.6 (server policy
for a slow consumer), section 8.9 (inflight policy on stop), section 17.1 (seven questions before
production) and the review checklist in section 19. Nothing here invents new scope; each entry
closes one of those questions so no `NEEDS CLARIFICATION` remains before Phase 1.

---

## R-00 Build and test platform

**Decision**: Linux/Ubuntu 22.04 or later, built from WSL2 Ubuntu-22.04 under `/mnt/d/TCU`, exactly
as project 01 settled in its R-00. Project 02 inherits that decision unchanged and adds one
constraint: project 02 has **no** Windows build at all, not even best-effort.

**Rationale**: the spec's transport is `AF_UNIX` + `SOCK_STREAM` (section 5), its readiness
mechanism is `poll`/`epoll` (section 7), its authorization mechanism is `SO_PEERCRED` (section 8.8)
and its signal handling is `MSG_NOSIGNAL` (section 8.3). None of these exist on Win32. Windows 10+
has an `AF_UNIX` stream socket, but no `SO_PEERCRED`, no `epoll`, no `MSG_NOSIGNAL` and no `EPIPE`
semantics, so FR-09 and section 8.3 could not be satisfied. The constitution's Technology
Constraints already say "must build and test on Linux; Windows is best-effort unless a spec states
otherwise" - this spec states otherwise.

**Consequence for the root build**: `add_subdirectory(modules/ipc)` and the project 02 test and app
targets are guarded by a Linux check, so the existing project 01 targets keep configuring on
Windows. A Windows configure prints one status line saying project 02 is skipped; it is not an
error.

**Alternatives considered**:
- *Make project 02 portable to Win32 `AF_UNIX`*: drops peer credentials (FR-09), so the local trust
  boundary of section 8.8 disappears. Rejected - the spec's security behaviour is not optional.
- *Abstract the socket layer behind a platform interface now*: an abstraction with exactly one
  implementation, forbidden by Principle I and by the spec's own "Ngoai pham vi v1" ("cross-platform
  Windows"). Rejected.

**Measured 2026-11-04** (WSL2 Ubuntu-22.04, inherited from project 01 R-00): cmake 3.30.5,
g++ 11.4.0, clang++ 14.0.0. ThreadSanitizer needs clang **and** `setarch -R`; the root
`CMakeLists.txt` already exposes that as `TCU_SANITIZER_SETARCH`, so project 02 reuses it and adds
nothing.

---

## R-01 Payload encoding and the JSON question

**Decision**: `libtcu_ipc` **never parses JSON**. A payload is an opaque `std::string` of bytes that
the library length-prefixes, bounds-checks and hands over untouched. No JSON library is added to the
repository. The demo apps compose the few small JSON documents of section 5.2 as string literals
with a 30-line writer, and read the two fields they care about with a depth-limited scanner in
`apps/` - not in the library.

**Rationale**: section 5.1 states the rule itself - "JSON chi la **encoding application**, framing
khong phu thuoc JSON" - and the section 6 API already types every payload as `std::string`
`payload_json`. The library's protocol invariants (version, type, flags, `name_length`,
`max_body_length`) are all byte-level. Adding `nlohmann/json` would put a 25k-line header into the
dependency closure of projects 02-30 to serve four literal documents, against the constitution's
"standard library and already-present dependencies first".

**What still holds the line**: B-18 ("malformed JSON ... Error ro rang, khong kill I/O loop") is a
*handler* contract, not a transport one. The demo handler returns `INVALID_ARGUMENT` when its
scanner rejects the input, and the I/O loop never sees the payload at all. Section 5.1's "gioi han
JSON depth o application layer" is satisfied by the scanner's depth cap of 8.

**Alternatives considered**:
- *`nlohmann/json` via FetchContent*: would need a Complexity Tracking entry. Rejected for the
  reason above; revisit at project 04 (Configuration & Logging) or 15 (MQTT), where real schema
  validation is in scope.
- *Protobuf*: section 17.1 question 3 defers it explicitly. Rejected as out of v1 scope.

---

## R-02 Readiness mechanism: `poll()`, not `epoll`

**Decision**: one `poll()` loop per process, non-blocking FDs, level-triggered semantics by
construction.

**Rationale**: section 7 names "nonblocking `poll`/`epoll`" as equally acceptable, so this is a
choice inside the spec, not a deviation from it. NFR-02 sizes the system at "1 server, toi thieu 8
concurrent clients", and the stress target of section 12.3 is 8 clients (fault matrix: 8-32).
`poll()` is O(n) per wake on an array of at most 34 FDs, needs no FD registry to keep in sync with
the session map, and has no edge-triggered starvation mode to get wrong. `epoll`'s advantage appears
in the hundreds of connections, which section 3.3 excludes ("high-performance broker hang tram nghin
clients").

**Consequence**: the loop's `timeout` argument doubles as the timer mechanism - see R-04. Each
session contributes exactly one `pollfd`; `POLLOUT` is requested only while its TX buffer is
non-empty, so an idle session costs one `POLLIN` slot.

**Alternatives considered**:
- *`epoll` level-triggered, as section 4.2 suggests*: correct but adds an FD-to-session registry and
  an `epoll_ctl` call on every TX state change, for no measurable gain at 32 FDs. Recorded as the
  documented upgrade path: a `shortcut:` comment in the loop names the client count at which to
  switch.
- *Thread per client*: rejected by section 17's own trade-off table.

---

## R-03 `pending_requests` ownership

**Decision**: a `std::unordered_map<uint64_t, PendingRequest>` owned by `IpcClient` and guarded by
one `std::mutex`. The mutex is never held while calling `promise.set_value()`, a user callback, or
any syscall. The I/O thread and user threads both take it, for short, allocation-free critical
sections.

**Rationale**: section 7 requires picking one model. `request()` is called from arbitrary user
threads and must allocate a `request_id` and register a deadline **before** returning a future
(section 8.2), so the map has to be reachable from those threads. The alternative - exclusive I/O
loop ownership - would need a command queue plus a wakeup per request, and `request()` would have to
either block on a reply or return a future resolved later by the loop, which is strictly more
machinery for the same guarantee.

**Resolution discipline** (section 7, last bullet; B-07, B-08, B-12, B-15): resolving a request is a
two-step operation - under the lock, `erase` the entry and move the `std::promise` out; after
releasing the lock, `set_value()`. Because only the thread that won the `erase` holds the promise,
exactly one resolution is possible and `std::future_error` cannot occur.

---

## R-04 Deadlines and the timer path

**Decision**: deadlines are `std::chrono::steady_clock::time_point` stored in the pending map. The
I/O loop computes `poll()`'s timeout as the time to the earliest deadline, clamped to
`[0 ms, 1000 ms]`, and sweeps expired entries after every wake. No `timerfd`, no timer thread.

**Rationale**: section 3.2 mandates `steady_clock` and forbids busy-spin; section 7.2 requires the
deadline to run from request creation, "ke ca van nam trong TX queue". A single sweep over at most
`max_pending_requests` (128) entries is cheaper than a second FD and keeps "resolve exactly once" in
one place. The 1000 ms clamp bounds shutdown latency and gives the loop a heartbeat for reconnect
backoff without a second timer source.

**Wakeup**: a `socketpair` self-pipe is the only other FD in the client loop. `request()`,
`subscribe()` and `stop()` write one byte to it so a sleeping `poll()` re-computes its timeout
immediately. This is what makes `stop()` bounded (section 8.9 step 3, B-16).

---

## R-05 Callback executor, and why not project 01's event bus

**Decision**: a private `Executor` inside `modules/ipc/src/` - one worker thread, one bounded queue
of `std::function<void()>`, drain-on-stop, exceptions caught and counted. Not a public header. Used
for user response callbacks, notification callbacks and server method handlers.

**Rationale**: section 4.1 requires handlers to run off the I/O loop and section 7 asks for a
"callback executor 1-N threads". Principle II forbids re-implementing what an earlier project
provides, so project 01's `tcu_eventbus` was evaluated first and does not fit: its `publish()` takes
an `Event` holding a closed `std::variant` of concrete payload structs fixed by project 01's spec
section 18.1, and its subscribers are keyed by `EventType`. Carrying an arbitrary `std::function`
through it would mean adding a payload alternative to project 01's public variant - widening the API
that projects 05-30 depend on, to make a type-safe event bus pretend to be a task queue. That is a
worse violation of Principle II's rationale than a 60-line private queue.

**Where project 01 *is* reused**: in the demo. `apps/vehicle_service` publishes its internal
`IgnitionChanged` onto a `tcu::eventbus::EventBus` and an in-process subscriber forwards it to
`IpcServer::publishNotification`, which is exactly the `WORK <--> EB` edge of the spec's section 4.1
diagram and the boundary section 18 insists on. The library itself does not link `tcu_eventbus`, so
the dependency stays "tuy chon" as the spec header states.

**Alternatives considered**:
- *N worker threads*: section 7 allows 1-N; one thread makes notification order per session
  observable and deterministic (section 8.7) and is enough for the demo. The queue is the part that
  matters; the thread count is a one-line change if a later project needs it.
- *Run handlers on the I/O thread*: forbidden by section 4.1 ("Tuyet doi khong lam: goi blocking
  handler tren I/O loop") and would break B-11 and the 3 s slow-handler fault case.

---

## R-06 Handshake and control messages on the wire

**Decision**: `HELLO`/`HELLO_ACK` and subscribe/unsubscribe are ordinary `Request`/`Response` frames
with reserved method names, not new frame types:

| Method | Direction | Payload | Reply |
|---|---|---|---|
| `ipc.hello` | client -> server | `{"version":1,"client_id":"<id>"}` | `{"version":1,"capabilities":[]}` |
| `ipc.subscribe` | client -> server | `{"topic":"<t>"}` | `{"topic":"<t>","active":true}` |
| `ipc.unsubscribe` | client -> server | `{"topic":"<t>"}` | `{"topic":"<t>","active":false}` |

**Rationale**: section 5.1 fixes `type` to exactly `1=Request, 2=Response, 3=Notification, 4=Error`
and `flags` to 0 in v1, so a fifth type would break the wire format the spec defines. Section 6's
API notes already describe the mechanism this way: "server ho tro control request
`ipc.subscribe`/`ipc.unsubscribe`". Control methods therefore cost no new framing, no new parser
branch and no new state - they reuse the correlation, timeout and error paths that FR-04 and FR-06
already require.

**Reserved namespace**: method and topic names beginning with `ipc.` are reserved for the transport.
`registerHandler("ipc.…")` is rejected at registration time, so an application cannot shadow the
handshake. The allowlist of section 5.1 ("method/topic theo allowlist") is therefore
`{registered handlers}` plus `{ipc.hello, ipc.subscribe, ipc.unsubscribe}`.

**Ordering**: a session that has not completed `ipc.hello` may send only `ipc.hello`; anything else
is answered `UNAUTHENTICATED` and the session is closed (B-01, B-19). The client reaches `Connected`
only after `HELLO_ACK` (section 8.1), and a version mismatch yields `UNSUPPORTED_VERSION` plus a
**terminal** client state, not a reconnect loop - section 8.1's "khong reconnect tight loop voi loi
cau hinh co dinh".

---

## R-07 Subscriptions and restore after reconnect

**Decision**: the client keeps its subscription list (topic -> callback) for the lifetime of the
`IpcClient`, independent of the connection. On reaching `Connected`, the reconnect path re-sends
`ipc.subscribe` for every topic and marks each one active only when its ACK arrives. The server
stores subscriptions **per session** and drops them when the session closes.

**Rationale**: section 8.4 requires restore-after-ACK and section 6's API notes say the same. Server
state is per session because a restarted server has no memory of the old one (section 8.4,
"notifications trong khoang mat ket noi **co the bi mat**"), and because `publishNotification` must
be able to enumerate live sessions only.

**Observable in tests**: B-09 asserts a notification arrives after a restart, and only after the ACK
(the test subscribes, kills the server, restarts it and waits for `subscription=restored` before
publishing).

---

## R-08 Slow consumer policy on the server

**Decision**: per-session bounded TX queue with the same two caps as the client
(`max_tx_messages` 256, `max_tx_bytes` 1 MiB). When enqueuing would exceed either cap the frame is
**dropped**, `ipc_queue_rejects_total` is incremented and one structured log line is written per
session per transition into the full state. The session is not closed and no other session is
affected.

**Rationale**: section 8.6 offers the choice - "ngung push hoac dong slow connection theo chinh sach
duoc ghi log" - and requires only that memory stays bounded and other clients keep working. Dropping
is the lesser action and is already legal for notifications under section 8.7's "best-effort
at-most-once trong mot session". A dropped response is not a stranded client either: the requester's
own deadline (R-04) resolves its future with `Timeout`, which is the documented outcome of section
8.5.

**Not done**: closing a session after a time at the cap. It needs a second timer, a new config knob
and a new error code to express, and no acceptance criterion or BDD row asks for it. Recorded as the
upgrade path in a `shortcut:` comment next to the cap check.

---

## R-09 Inflight policy on `stop()`

**Decision**: `Cancel`. `stop()` flips the state to `Stopping`, stops accepting new work
(`request()` then returns a future already resolved `ShuttingDown`), resolves every pending request
exactly once with `Cancelled`, wakes the loop through the self-pipe, closes FDs, drains and stops the
executor, joins both threads, and on the server also unlinks the socket path it created. `stop()` is
idempotent through a `std::atomic` state plus a `std::once_flag` around the join-and-close block, so
the second call returns without touching an FD (B-15) and a call racing with reconnect cannot
deadlock (B-16).

**Rationale**: section 8.9 names `Cancel` as the default and fixes the five steps; B-17 fixes the
post-stop error. `~IpcClient()`/`~IpcServer()` call `stop()`, so RAII covers the forgotten case.

**No self-join**: the executor and I/O threads never call `stop()`; a handler that calls `stop()`
schedules it and returns, which is what section 8.9 step 4 means by "khong self-join".

---

## R-10 Socket path, stale sockets and peer authorization

**Decision**:
- Runtime directory: `/tmp/virtual-tcu`, created `0700` and owned by the server's UID. If it exists
  with different ownership or with any group/other permission bit set, `start()` fails
  `PermissionDenied` rather than fixing it.
- Socket file: `bind()` first; on `EADDRINUSE`, `connect()` to the path. If the connect succeeds a
  live server owns it and `start()` fails; only `ECONNREFUSED` (nobody listening) authorises
  `unlink()` and one retry.
- Peer check on accept: `getsockopt(SO_PEERCRED)`, then a UID allowlist that defaults to
  `{geteuid()}` and is extended by `--allow-uid`. A rejected peer is closed before any frame is read
  and logged once with its UID, PID and GID.

**Rationale**: this is section 8.8 made executable. The connect-probe is the part worth stating: the
spec says "`unlink` stale socket chi sau khi xac dinh an toan; tranh xoa socket dang phuc vu process
hop le", and a probe is the only check that distinguishes the two cases without a lock file or a
pidfile. `0700` on the directory is what makes the UID allowlist meaningful - a world-writable
directory would let anyone replace the socket, which section 8.8 calls out directly.

**Alternatives considered**:
- *Abstract socket namespace* (a NUL-prefixed name): no filesystem permissions at all, so the trust
  boundary would rest on the UID check alone, and `unlink` semantics vanish. Rejected - section 8.8
  says "pathname socket".
- *`unlink()` unconditionally before `bind()`*: the exact foot-gun the spec forbids. Rejected.

---

## R-11 Error codes

**Decision**: one closed set, shared by the wire `Error` frame (`type=4`) and `IpcError`, defined
once in `contracts/wire_protocol.md`. Wire codes are the screaming-snake strings already used in the
spec: `INVALID_ARGUMENT`, `UNKNOWN_METHOD`, `PERMISSION_DENIED`, `UNAUTHENTICATED`,
`UNSUPPORTED_VERSION`, `FRAME_TOO_LARGE`, `PROTOCOL_ERROR`, `INTERNAL_ERROR`, `SHUTTING_DOWN`.
`IpcError` keeps exactly the ten values of section 6 - nothing is added to the enum.

**Rationale**: FR-06 asks for "error code chuan hoa". The spec already uses `INVALID_ARGUMENT`
(5.2), `FRAME_TOO_LARGE` (5.1), `UNSUPPORTED_VERSION` (8.1), `INTERNAL_ERROR` (B-11) and
`PermissionDenied` (fault matrix). `UNKNOWN_METHOD` and `UNAUTHENTICATED` are the two that the
spec's behaviour implies (section 4.1 "Trust payload khong kiem tra", R-06's ordering rule) but
never names; both map to `IpcError::RemoteError` on the client, so the public enum is untouched.

**Mapping rule**: a remote `Error` frame becomes `IpcError::RemoteError` with the wire code in
`IpcResponse::detail`. Local failures use the specific enumerator (`Timeout`, `QueueFull`,
`NotConnected`, `Cancelled`, `PeerClosed`, `ProtocolError`, `ShuttingDown`, `PermissionDenied`).

---

## R-12 Request IDs and session generations

**Decision**: `request_id` is a per-`IpcClient` `uint64_t` counter starting at 1, incremented per
request, never reused while an entry with that ID is pending (a counter that only increases cannot
collide within one process lifetime). `request_id == 0` is reserved for notifications, as section 5.1
requires. Each successful connect increments a `generation` counter; a frame decoded from socket
generation *g* is discarded unless *g* equals the current generation.

**Rationale**: the review checklist (section 19) asks for both: "`request_id` unique tren moi
connection va khong reuse khi con pending" and "Reconnect tao generation moi; response session cu
khong duoc accept". A monotonic 64-bit counter satisfies the first by construction. The generation
check is what makes B-12 (late response after timeout) and the kill/restart path of B-08 safe at the
same time: a late response from the *same* generation is dropped and counted as
`ipc_late_responses_total`, while anything from an older generation is dropped as a stale-session
frame.

---

## R-13 Fault injection without Python

**Decision**: the partial-frame, oversize-frame and corrupt-header cases are driven from C++ test
code that owns a raw `AF_UNIX` FD and writes chosen byte sequences, not from a Python script.

**Rationale**: the fault matrix suggests "Python test gui tung byte" as a *method*, and section 12.3
lists no Python requirement in the acceptance criteria. A C++ helper runs inside the same CTest
invocation, under the same sanitizer build, with no interpreter on the dependency list and no
cross-language synchronisation to get flaky. `tests/fault_injection/ipc/raw_peer.hpp` is 40 lines and
is reused by six cases.

---

## R-14 Stress test scale and duration

**Decision**: the stress test is one CTest case at 8 clients x 10 requests/s for **5 seconds** by
default, and reads `TCU_IPC_STRESS_SECONDS` to run longer. The 30-minute figure of section 12.3 is
reached with `TCU_IPC_STRESS_SECONDS=1800` and is not part of the default suite.

**Rationale**: section 12.3 marks its own numbers as "muc tieu demo, dieu chinh theo may", and
NFR-05's determinism plus the constitution's CI gate both require a suite that finishes. The
assertions are invariants, not timings - no deadlock, every request resolved exactly once,
`ipc_tx_queue_bytes` returns to 0, FD count unchanged - so they hold identically at 5 s and 1800 s.

---

## R-15 `configs/ipc_demo.json` and the run script

**Decision**: no config file. Both demo apps take CLI flags (`--socket`, `--scenario`,
`--allow-uid`, `--timeout-ms`), and `scripts/run_ipc_demo.sh` drives the section 16 demo end to end,
including the kill and restart.

**Rationale**: section 15 lists `configs/ipc_demo.json` in the layout, but section 16's target
commands pass everything on the command line, and no acceptance criterion in section 13 mentions a
config file. A JSON config would also re-open R-01 by needing a parser in the apps for settings that
are already flags. The script is kept because section 13 does require "README mo ta run scripts".
Recorded as a deliberate layout delta in plan.md.

---

## R-16 GoogleTest acquisition

**Decision**: reuse the existing root `CMakeLists.txt` block - `FetchContent` with the pinned
`v1.14.0` tag from project 01's R-14. Project 02 adds `add_subdirectory` lines only.

**Rationale**: Principle II. The pin exists for reproducibility; a second declaration of the same
dependency at a different tag is how a monorepo gets two GoogleTests.

---

## R-17 Section 17.1's seven production questions

All seven are explicitly **out of v1** and are recorded here so no task picks them up by accident:

| # | Question | v1 answer |
|---|---|---|
| 1 | Per-service socket or central broker? | Per-service socket. A broker is "high-performance broker" territory, excluded by 3.3. |
| 2 | QNX compatibility? | No. Linux-only (R-00). |
| 3 | Protobuf schema and backward compatibility? | No. Opaque payload (R-01); the `version` field reserves the upgrade. |
| 4 | Idempotency key / retry contract? | None. v1 never replays an RPC (section 8.4, B-20). |
| 5 | systemd-managed runtime dir? | No. The server creates `/tmp/virtual-tcu` itself (R-10). Project 26 owns systemd. |
| 6 | Sensitive payloads, per-method/topic permissions? | Method allowlist only (R-06); no payload classification, no encryption (3.3). |
| 7 | Throughput/latency targets? | None asserted. Section 3.2's numbers are test targets; no benchmark harness is in scope for 02. |

---

## Open questions remaining

None blocking. Two notes for the operator:

1. **Inherited from project 01 R-00**: if Windows/MSVC is ever meant to be a first-class target, this
   project is the counter-example - it cannot be, and the constitution's Technology Constraints
   would need an exception for it rather than the reverse.
2. **`/tmp/virtual-tcu` on WSL2**: the path lives in the WSL2 filesystem, not on `/mnt/d`. Unix
   sockets cannot be bound on a DrvFs path - measured 2026-11-04, `bind()` on
   `/mnt/d/TCU/build/x.sock` fails `EOPNOTSUPP` ("Operation not supported") while `/tmp/x.sock`
   succeeds - so the demo and the integration tests must use a `/tmp` path even though the
   repository is on `/mnt/d/TCU`. The default in
   `ClientOptions` already is `/tmp/virtual-tcu/vehicle.sock`, so nothing changes - but a future task
   must not "helpfully" move the socket next to the build directory.
