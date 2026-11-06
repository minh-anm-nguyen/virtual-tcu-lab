# Contract: `tcu_signal` Public API

**Feature**: `006-dbc-signal-decoder` | **Spec**: [../spec.md](../spec.md) sections 8, 9, 10, 12

`tcu_signal` owns everything with a lifecycle: the frame source seam, the bounded RX queue, the one
decoder worker, the quality cache, the freshness scan and the reload coordinator. It links
`tcu_dbc`, `tcu_scheduler` (project 03) and `tcu_logging` (project 04). It does **not** link
`tcu_eventbus` (research R-18).

Headers: `include/tcu/signal/{frame_adapter,signal_quality,signal_cache,decoder_service,decoder_config,decoder_metrics}.hpp`.
Namespace `tcu::signal`.

## 1. The frame source: project 05's transport, unchanged

This project defines **no** frame-source interface. It consumes project 05's:

```cpp
// modules/can/include/tcu/can/can_transport.hpp - project 05, not redeclared here
class ICanTransport {
public:
  virtual ReadResult receive(std::chrono::milliseconds timeout) = 0;   // one thread only
  virtual WriteStatus send(const CanMessage& msg) = 0;                 // any thread
  virtual void requestStop() noexcept = 0;                             // any thread, idempotent
};
```

`SocketCanTransport` gives the `vcan0` path; `FakeCanTransport` (public on purpose, per project
05's own note) gives every test path. `tcu::can::CanMessage` is the frame type, and it already owns
its 8-byte payload, so the RX queue stores it directly (research R-22).

The one adapter this project adds, in `include/tcu/signal/frame_adapter.hpp`:

```cpp
// A view over msg.data - valid only while msg is alive. The queue owns the CanMessage,
// so the view is constructed per frame inside the worker and never outlives it.
dbc::CanFrameView toFrameView(const tcu::can::CanMessage& msg, uint32_t busId);
```

- `format` maps `tcu::can::FrameFormat` one to one.
- `isRemote` / `isError` come from `tcu::can::FrameKind` (`Remote` / `Error`).
- `rxTime` is `msg.received_at`, which project 05 stamped from the injected clock; this project
  never re-stamps it (research R-06, spec section 10.5).
- `size` is `msg.length`, which project 05 has already validated to be 0..8.

**Deliberately not reused** (research R-24): `tcu::can::CanService`, `tcu::can::SignalCache` and
`IDemoSignalDecoder`. Their `SignalSample`/`DecodeResult` cannot carry a raw `uint64_t`, a signed
raw, a unit, an enum label or a per-batch `(dbcVersion, dbcHash)`, which is the data this project
exists to produce.

## 2. The quality cache

```cpp
struct SignalKey { uint32_t busId; bool isExtended; uint32_t arbitrationId; std::string signalName; };
struct SignalSnapshot {
  SignalQuality lastObservationQuality;
  std::optional<dbc::SignalValue> lastGood;
  std::chrono::steady_clock::time_point lastGoodRxTime;
  bool lastGoodIsStale;
  uint64_t epoch;
  uint64_t dbcVersion;
};

class SignalCache {
public:
  std::optional<SignalSnapshot> get(const SignalKey&) const;
  std::vector<QualityEvent> commit(const dbc::DecodedBatch&);     // returns events, publishes none
  std::vector<QualityEvent> scanFreshness(std::chrono::steady_clock::time_point now);
  void migrate(const dbc::CompiledDbc& next);                     // research R-09
};
```

**Postconditions**:
- `get` returns both axes in one read; `lastGoodIsStale` is derived at call time, never stored
  (S-05, S-06, research R-12).
- `commit` updates `lastGood`, `lastGoodRxTime` and `epoch` only for signals whose quality is
  `Valid` (S-01, S-03, S-04), ignores a batch whose `rxTime` is older than the stored one
  (S-02), and returns the event list rather than dispatching it (S-08).
- `scanFreshness` emits a stale transition only when the entry's epoch is unchanged since it
  sampled it (S-10, research R-13).
- `migrate` keeps a signal whose semantic hash is unchanged, resets a changed one to `NotReceived`
  and marks a removed one `ConfigUnavailable` (V-12).

**Thread safety**: every method is safe to call concurrently. One mutex, held for the computation
only. **No method invokes a caller-supplied callback** (S-07) — that is what makes the "no callback
under lock" invariant structural rather than a review rule.

## 3. The service

```cpp
struct DecoderServiceDeps {
  std::unique_ptr<tcu::can::ICanTransport> transport;          // project 05 (R-22)
  std::shared_ptr<tcu::scheduler::TimerScheduler> scheduler;   // project 03, required
  std::shared_ptr<tcu::logging::ILogger>   logger;             // project 04, required
  std::function<std::chrono::steady_clock::time_point()> now;  // injectable (research R-06)
};

class DecoderService {
public:
  DecoderService(DecoderConfig cfg, DecoderServiceDeps deps);
  ~DecoderService();                                   // calls stop()

  StartResult start();                                 // Ready or NotReady + diagnostics
  State       state() const;
  void        stop(std::chrono::milliseconds deadline = std::chrono::milliseconds{2000});

  SubscriptionId addObserver(std::weak_ptr<ISignalObserver>);
  void           removeObserver(SubscriptionId);

  std::optional<SignalSnapshot> query(const SignalKey&) const;
  ReloadHandle requestReload();                        // ReloadAccepted | ReloadBusy | ReloadDisabled
  SchemaIdentity schema() const;                       // {version, hash}
  Metrics        metrics() const;
};
```

**Preconditions**: every dependency in `DecoderServiceDeps` is non-null. The scheduler is already
started; the service registers jobs on it and cancels exactly those jobs on stop.

**Postconditions**:
- `start()` reaches `Ready` only with a valid database; a cold-start failure leaves `NotReady`, and
  in that state `query` answers `ConfigUnavailable` and no batch is ever published (V-05, B-01).
- Observers are stored as `weak_ptr`, so a destroyed observer is skipped and unregistered rather
  than called (FR-16). An observer that throws is caught, counted and rate-limit-logged (V-08, B-14).
- `stop()` follows the fixed order of V-06, is idempotent, never runs a callback after it returns
  (V-07), and reports `StopIncomplete` instead of blocking past `deadline`.
- `requestReload()` returns `ReloadBusy` when one is pending and `ReloadDisabled` when
  `reload.enabled` is false; the handle resolves to `Committed{version}` or `Rejected{diagnostics}`
  (V-09, V-11, B-11, B-12).
- `metrics()` is a consistent snapshot; counters are monotonic (M-05).

**Thread safety**: every public method is callable from any thread. `query` never blocks on I/O,
on the decoder worker or on a reload.

**Threads owned**: exactly one (the decoder worker). Everything periodic is a scheduler job
(research R-15).

## 4. The observer contract

`ISignalObserver` is spec section 8 verbatim. Rules the implementation guarantees:

- Called from the decoder worker (batches, quality changes from a commit) or from a scheduler worker
  (stale transitions), never with any lock of this library held (S-07, section 12 rule 5).
- `onBatch` is called once per committed batch, after the cache commit (section 10.2 step 7).
- An exception is caught at the boundary; a slow observer delays the worker but is accounted for in
  `decoder_observer_failures_total` and in the publish policy of `event_publish.on_full`.
- Ordering: per bus, batches arrive in queue order. Cross-bus order is not promised
  (section 12 rule 4).

## 5. Demo adapter (not part of either library)

`apps/virtual_dbc_demo` implements `ISignalObserver` and republishes onto project 01's Event Bus as
`SignalsDecoded`, `SignalQualityChanged`, `DbcReloaded` and `DbcReloadFailed`. One integration test
does the same, so FR-18 is demonstrated without either library depending on the bus (research R-18).
The demo takes `--iface vcan0` for a `SocketCanTransport` or `--fake` for a scripted
`FakeCanTransport`, so the nine-step sequence runs with or without a configured interface.

## 6. Not in v1

More than one decoder worker; multi-bus routing across several transports; historical last-good;
per-signal subscription filters; transmitting frames (project 05's `send` is not wrapped here); a
reload triggered by an OS file-change notification (the scheduler-driven poll of
`reload.debounce_ms` is what v1 has); any API that returns a physical value without its quality.
