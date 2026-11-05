# Contract: Configuration Service API

**Feature**: `004-configuration-logging-service` | Target: `tcu_config` | Header root:
`modules/config/include/tcu/config/`

This is the surface projects 05-30 compile against. It is spec section 8 with the preconditions,
postconditions and thread-safety rules made explicit, plus the two `LoadError` values section 13
requires. Anything not stated here is not promised.

---

## 1. Types

```cpp
namespace tcu::config {

using ConfigPtr       = std::shared_ptr<const ConfigSnapshot>;
using SubscriptionId  = std::uint64_t;                       // strictly increasing; 0 is never issued
using ConfigObserver  = std::function<void(ConfigPtr current, ConfigPtr previous)>;

enum class LoadError {
  None, FileNotFound, ReadError, TooLarge, ParseError, SchemaError,
  InvalidValue, RestartRequired, UnsupportedVersion, ServiceStopped
};

struct ReloadResult {
  LoadError error{LoadError::None};
  std::uint64_t generation{0};
  bool changed{false};
  std::string publicReason;
};

struct ServiceOptions {
  std::filesystem::path path;                                   // required
  std::size_t maxFileBytes{1024 * 1024};                        // 1 MiB (spec section 9 rule 1)
  std::chrono::milliseconds pollInterval{500};                  // 0 disables polling (R-03)
};

class IConfigurationService {
 public:
  virtual ~IConfigurationService() = default;
  virtual ConfigPtr snapshot() const noexcept = 0;
  virtual ReloadResult reload() = 0;
  virtual SubscriptionId subscribe(ConfigObserver observer) = 0;
  virtual bool unsubscribe(SubscriptionId id) = 0;
};

class ConfigurationService final : public IConfigurationService {
 public:
  ConfigurationService(ServiceOptions options,
                       tcu::scheduler::TimerScheduler& scheduler,
                       tcu::logging::ILogger& logger);
  ~ConfigurationService() override;                             // calls stop()

  LoadError start();
  void stop();
  ConfigMetrics metrics() const;
  // ... IConfigurationService overrides
};

}  // namespace tcu::config
```

`LoadError::TooLarge` and `LoadError::UnsupportedVersion` extend the enum of spec section 8; BC-15
and BC-28 require both to be distinguishable. Adding any further value is a spec change, not an
implementation detail (Constitution Principle I).

---

## 2. Construction and ownership

| Rule | Detail |
|---|---|
| The caller owns the scheduler and the logger | Both are references. Both must outlive the service. |
| The scheduler must be started before `start()` | The poller timer and the first notification are scheduled during or right after `start()`. |
| The scheduler must have `workerThreads == 1` | Publication-order delivery (spec section 9 rule 7) relies on one worker draining a FIFO ready queue (R-02). With more workers, order is not promised and N1 does not hold. |
| The scheduler must be stopped **after** the service | Spec section 11's shutdown order. Stopping it first leaves the poller cancelled but a notification possibly undelivered — safe, but it silently loses the last callback. |
| `maxReadyTasks` bounds the notification queue | A burst beyond it is the overflow path of section 3 below, not an error for the caller to handle. |

---

## 3. Operations

### `LoadError start()`

**Preconditions**: state is `Created`; the scheduler is running.

**Postconditions**

- Returns `None`: generation 1 is published, `snapshot()` returns it, state is `Running`, the poller
  timer is armed when `pollInterval > 0`, and the first notification (with `previous == nullptr`) is
  enqueued for every observer registered before the call.
- Returns anything else: state is `Failed`, `snapshot()` returns `nullptr`, no timer is armed, and no
  notification is enqueued. The service does **not** start with implicit defaults (BC-02).

**Idempotence**: a second call returns `ServiceStopped` if stopped, otherwise the current state's
error. It never re-publishes generation 1.

### `ConfigPtr snapshot() const noexcept`

**Preconditions**: none. Callable from any thread, at any time, including concurrently with
`reload()`.

**Postconditions**: returns the snapshot in force at a single atomic read, or `nullptr` if the
service never published (states `Created` and `Failed`). The returned pointer keeps that snapshot
alive for as long as the caller holds it; a later reload never mutates it (C1).

**Does not**: block on file I/O, parsing, validation, the reload mutex or the registry mutex; throw;
allocate beyond the `shared_ptr` copy.

### `ReloadResult reload()`

**Preconditions**: state is `Running` for any effect; in `Created`, `Failed`, `Stopping` or
`Stopped` it returns `ServiceStopped` with the current generation and no side effect.

**Postconditions**: exactly one of

| Outcome | `error` | `changed` | `generation` | Notification |
|---|---|---|---|---|
| committed change | `None` | `true` | previous + 1 | one, after commit |
| no effective change | `None` | `false` | unchanged | none (BC-06) |
| rejected | the reason | `false` | unchanged | none |

**Serialization**: concurrent calls are serialized by the reload mutex. Each returns the result of
its own attempt, and generations are handed out in completion order with no gaps (BC-09).

**Blocking**: performs file I/O and parsing on the calling thread. Do not call it from a thread that
must stay responsive, and never from an observer callback — a nested `reload()` from an observer is
not a deadlock (the mutex is released before notification) but it re-enters the pipeline for a file
that has probably not changed. Call it from the poller, an operator command, or a test.

**Success does not mean applied.** A committed generation says the snapshot is in force, not that any
observer has finished acting on it (spec section 13's test-oracle note). Use
`config_generation_applied{component}` for that.

### `SubscriptionId subscribe(ConfigObserver observer)`

**Preconditions**: `observer` is non-empty. Callable from any thread, including from inside another
observer callback.

**Postconditions**: returns a fresh non-zero id. The observer will be called for every generation
published *after* the call. It is **not** called for the generation already in force — a new
subscriber reads `snapshot()` itself, which is the resync pattern section 8 describes.

### `bool unsubscribe(SubscriptionId id)`

**Postconditions**: returns true if the id was live and is now removed; false if it was never issued
or already removed. After it returns, no callback for that id will *start*. A callback already
running may finish, so the observer's captured state must stay valid until the caller knows no
callback is in flight — the usual way is to unsubscribe, then destroy the owner on the thread that
owns it, with no borrowed references in the callback (BC-12).

**From inside a callback**: permitted. The callback finishes normally and no further ones start.

### `ConfigMetrics metrics() const`

A by-value snapshot of the counters in [metrics.md](./metrics.md). Lock-free, safe from any thread,
never throws.

### `void stop()`

**Postconditions**: the poller timer is cancelled, an in-flight `reload()` completes (it is not
interrupted), the registry is cleared so no observer is called again and no captured object is
referenced after the call, and state is `Stopped`. Idempotent; safe from any thread; safe from
inside an observer callback (BC-30). Does not stop the scheduler or the logger — the caller owns
both.

---

## 4. Observer contract

What an observer **must** tolerate:

| Condition | Why |
|---|---|
| `previous == nullptr` | First notification after startup, and every resync after notification overflow (R-06, N5). |
| Running on the scheduler's worker thread, not the caller's | Section 6's thread model. |
| Being called after a later generation was already published | Delivery is asynchronous; `current->generation` is authoritative. |
| Its own exceptions being swallowed | Caught, counted, logged with the subscription id, and the next observer still runs (BC-11). |

What an observer **must not** do:

| Prohibited | Consequence |
|---|---|
| Block indefinitely | It occupies the single notifier worker, so every later notification waits behind it; the queue fills and the service degrades to resync (BC-25). Section 15's last row calls this a component bug. |
| Assume `reload()` has not returned yet | It has. The commit happened before the callback was enqueued (S10). |
| Hold a lock that any producer also takes while logging | Section 14's lock-order rule. |
| Keep a raw pointer to the snapshot | Keep the `ConfigPtr`; the snapshot stays alive as long as it does. |

---

## 5. Thread safety summary

| Member | Safe from any thread | Blocks | Notes |
|---|---|---|---|
| `snapshot()` | yes | no | one atomic load |
| `reload()` | yes | yes — I/O, parse, validate | serialized |
| `subscribe()` | yes | briefly on the registry mutex | callable from a callback |
| `unsubscribe()` | yes | briefly on the registry mutex | callable from a callback |
| `metrics()` | yes | no | relaxed atomic loads |
| `start()` | caller's thread, once | yes | |
| `stop()` | yes | yes — waits for an in-flight reload | idempotent |

---

## 6. Not in v1

Stated so no consumer waits for it:

1. No cloud configuration synchronisation, remote command channel or OTA rollout (section 3).
2. No multi-process atomic configuration; a second process has its own instance and may run a
   different generation (AS-05).
3. No partial application of a candidate that touches a restart-only key (25.3, BC-08).
4. No `extensions` object and no unknown-key passthrough (R-05).
5. No defaults merge, no environment-variable layer and no command-line key override. The file is the
   only source (R-16).
6. No credential or secret provider. No key in the schema holds a secret (R-07).
7. No notification coalescing (section 9 rule 7).
8. No `GetConfigStatus` IPC endpoint (R-20).
9. No file-watcher kernel integration; change detection is a bounded poll (R-03).
10. No rollback of a committed generation. A component that cannot accept a change keeps its last
    working state, reports a failure metric, and escalates to the application owner (section 9).
