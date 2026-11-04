# Phase 1 Data Model: Thread-Safe Event Bus

Entities are taken from spec section 5 (API design), section 18.1 (payload schema) and section 19
(metrics), with the Phase 0 decisions applied. The field names here are the contract that
[contracts/event_bus_api.md](./contracts/event_bus_api.md) and the tests refer to.

---

## 1. Payload types

Concrete, independently owned, immutable after construction. No pointer or reference into a
producer's stack frame (spec section 18.1).

| Type | Fields | Validation |
|---|---|---|
| `IgnitionChanged` | `bool on` | none |
| `VehicleSpeedUpdated` | `double kmh` | must be finite; whether a negative speed is meaningful is the producer's concern, not the bus's |
| `NetworkStatusChanged` | `bool online` | none |

```cpp
using EventPayload = std::variant<
    IgnitionChanged,
    VehicleSpeedUpdated,
    NetworkStatusChanged>;
```

**Invariant P1**: a payload is copyable and movable, owns all its data, and holds no raw pointer or
reference.

**Extension rule**: projects 05-30 add alternatives to `EventPayload`, appending only. Reordering
existing alternatives changes every `EventType` value and is a breaking change (R-01).

---

## 2. `EventType`

```cpp
using EventType = std::uint32_t;
```

Derived from the variant alternative index, so type and payload cannot disagree (R-01):

```cpp
template <typename T>
constexpr EventType event_type_of() noexcept;   // index of T in EventPayload
```

**Invariant T1**: `event_type_of<T>()` is stable within a build and equals
`static_cast<EventType>(event.payload.index())` for any event carrying a `T`.

---

## 3. `Event`

| Field | Type | Meaning |
|---|---|---|
| `id` | `std::uint64_t` | monotonic per bus instance, assigned by `publish()` on acceptance; this is the `event_id` of the section 20 log lines |
| `type` | `EventType` | redundant with `payload.index()`, kept so the registry lookup need not visit the variant |
| `created_at` | `std::chrono::steady_clock::time_point` | set by the producer; the benchmark uses it for enqueue-to-dispatch latency |
| `payload` | `EventPayload` | the data |

**Invariant E1**: an `Event` is not modified after `publish()` returns `Accepted`; every subscriber of
that event observes identical data (spec section 17.3).

**Invariant E2**: `type == static_cast<EventType>(payload.index())`. Checked on publish; a mismatch is
`PublishStatus::InvalidArgument`.

**Invariant E3**: `id` is assigned only to accepted events and is strictly increasing in enqueue
order, so FIFO is assertable by id (B09). Rejected, timed-out and stopped publishes consume no id.

---

## 4. Configuration and options

| Type | Field | Default | Validation |
|---|---|---|---|
| `BusConfig` | `std::size_t queue_capacity` | `1024` | must be `> 0`; `0` throws `std::invalid_argument` from the constructor (spec section 17.2) |
| `PublishOptions` | `FullQueuePolicy on_full` | `Reject` | - |
| `PublishOptions` | `std::chrono::milliseconds timeout` | `0` | ignored when `on_full == Reject`; with `BlockUntil`, `0` means poll (R-11); negative gives `InvalidArgument` |

---

## 5. Enumerations

```cpp
enum class PublishStatus { Accepted, QueueFull, TimedOut, Stopped, InvalidArgument };
enum class FullQueuePolicy { Reject, BlockUntil };
enum class StopMode { Drain, Discard };
```

`PublishStatus` has exactly these five values - FR-06, and R-02 declined to add a sixth.

| Status | Returned when |
|---|---|
| `Accepted` | the event is in the queue and has an `id` |
| `QueueFull` | `Reject` met a full queue |
| `TimedOut` | `BlockUntil` reached its deadline, including the zero-timeout poll on a full queue (R-11) |
| `Stopped` | the state is not `Running`: before `start()` (R-02), during `Stopping`, after `Stopped`, or a waiting publisher woken by `stop()` |
| `InvalidArgument` | `type`/`payload` mismatch (E2), negative timeout, or `BlockUntil` called from the dispatcher thread (R-10) |

---

## 6. Bus lifecycle state

```text
Created --start()--> Running --stop(Drain|Discard)--> Stopping --worker joined--> Stopped
Created --stop()----------------------------------------------------------------> Stopped
```

| From | Event | To | Notes |
|---|---|---|---|
| `Created` | `start()` | `Running` | spawns exactly one worker |
| `Created` or `Running` | `start()` again | - | `std::logic_error`, no second worker (R-08) |
| `Created` | `stop()` | `Stopped` | no worker was ever started |
| `Running` | `stop(mode)` | `Stopping` | the linearization point; from here every `publish()` returns `Stopped` (section 17.7) |
| `Stopping` | worker joined | `Stopped` | Drain: queue empty and callbacks complete. Discard: queue cleared, current callback completed |
| `Stopping` or `Stopped` | `subscribe()` | - | rejected; no new subscription once shutdown has begun (section 17.2) |
| any | `stop()` again | unchanged | idempotent; concurrent callers wait for `Stopped` (R-09) |

**Invariant S1**: the transition `Running -> Stopping` happens under the lock, and it is the single
point after which no publish can be accepted.

**Invariant S2**: exactly one worker thread exists over the bus's whole lifetime, and it is joined
exactly once.

---

## 7. Subscription

| Entity | Field | Meaning |
|---|---|---|
| `SubscriptionState` (internal, held by `shared_ptr`) | `std::atomic<bool> active` | cleared by `unsubscribe()`; re-checked by the dispatcher immediately before invoking (R-13) |
| | `std::mutex in_flight` | held across the callback; makes `unsubscribe()` wait out a running invocation |
| | `std::thread::id owner` | the thread currently running the callback, so self-`unsubscribe()` does not deadlock (R-13 caveat) |
| | `EventType type` | registry key |
| | `Handler handler` | `std::function<void(const Event&)>` |
| | `std::uint64_t id` | registration sequence number; defines invocation order (R-04) |
| `Subscription` (public handle) | `std::weak_ptr<SubscriptionState>` | weak, so a handle outliving the bus is safe (spec section 17.5) |

**Invariant U1**: `Subscription` is move-only. Destruction or move-assignment unsubscribes the
previous registration.

**Invariant U2**: `unsubscribe()` is idempotent. After it returns, no new invocation of that handler
starts; an invocation already running may finish.

**Invariant U3**: a `Subscription` whose `EventBus` has been destroyed is inert - `unsubscribe()` on
it is a no-op, never a use-after-free (weak ownership).

**Invariant U4**: within one event type, handlers run in ascending `id`, i.e. registration order
(R-04).

---

## 8. Bounded queue (internal)

| Field | Meaning |
|---|---|
| `std::deque<Event> items` | FIFO storage |
| `std::size_t capacity` | fixed, `> 0` |
| `std::condition_variable not_empty` | the worker waits here |
| `std::condition_variable not_full` | `BlockUntil` publishers wait here |
| `bool closed` | set at `Running -> Stopping`; wakes every waiter |

**Invariant Q1**: `0 <= items.size() <= capacity` at all times; depth never goes negative and never
exceeds capacity (section 17.4).

**Invariant Q2**: FIFO by successful enqueue order, not by the wall-clock time at which publishers
called the API (section 17.1).

**Invariant Q3**: every wait uses `std::chrono::steady_clock` with an absolute deadline inside a
predicate loop, so a spurious wakeup cannot shorten a timeout (section 17.4).

**Invariant Q4**: no callback is ever invoked while the queue mutex is held (section 6).

**Note**: wake-up fairness among several blocked publishers is explicitly not guaranteed in v1
(section 17.4).

---

## 9. `MetricsSnapshot`

Returned by value; every field is a plain integer read from an atomic counter. Definitions and the
conservation equations live in [contracts/metrics.md](./contracts/metrics.md).

| Field | Type |
|---|---|
| `accepted_total` | `std::uint64_t` |
| `rejected_queue_full_total` | `std::uint64_t` |
| `publish_timeout_total` | `std::uint64_t` |
| `dispatched_total` | `std::uint64_t` |
| `callback_invocations_total` | `std::uint64_t` |
| `callback_failures_total` | `std::uint64_t` |
| `discarded_on_stop_total` | `std::uint64_t` |
| `queue_depth` | `std::size_t` |
| `queue_high_watermark` | `std::size_t` |

**Invariant M1**: `queue_depth <= queue_high_watermark <= capacity`.

**Invariant M2**: a snapshot taken while the bus is running is a consistent read of each counter but
not an atomic read across counters; the conservation equations hold only after `stop()` has returned.
