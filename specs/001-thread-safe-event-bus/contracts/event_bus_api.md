# Contract: `tcu::eventbus` public API

The interface that projects 05, 10, 13, 16, 18 and 30 compile against. Everything not listed here is
implementation detail and lives under `modules/core/event_bus/src/`.

Public headers:

```text
include/tcu/eventbus/event.hpp      # EventType, payload structs, EventPayload, Event, event_type_of
include/tcu/eventbus/event_bus.hpp  # PublishStatus, FullQueuePolicy, StopMode, BusConfig,
                                    # PublishOptions, Handler, Subscription, EventBus
include/tcu/eventbus/metrics.hpp    # MetricsSnapshot
```

Field-level definitions are in [../data-model.md](../data-model.md). Counter semantics are in
[metrics.md](./metrics.md).

---

## `EventBus::EventBus(BusConfig)`

| | |
|---|---|
| Precondition | `config.queue_capacity > 0` |
| Postcondition | state is `Created`; no worker thread exists |
| Throws | `std::invalid_argument` if `queue_capacity == 0`; whatever allocation throws |
| Thread safety | not callable concurrently with any other method on the same object |

Copy and assignment are deleted.

## `void EventBus::start()`

| | |
|---|---|
| Precondition | state is `Created` |
| Postcondition | state is `Running`; exactly one worker thread is running |
| Throws | `std::logic_error` from any other state; no second worker is created (R-08) |
| Thread safety | safe from any non-worker thread; only one call ever succeeds |

## `PublishStatus EventBus::publish(Event event, PublishOptions opts = {})`

Returns a status; it does not throw for any expected condition (spec section 8).

| Returns | When |
|---|---|
| `Accepted` | the event is in the queue and has been assigned an `id` |
| `QueueFull` | `on_full == Reject` and the queue was full |
| `TimedOut` | `on_full == BlockUntil` and the deadline passed with no free slot; includes `timeout == 0` on a full queue (R-11) |
| `Stopped` | state is not `Running` - before `start()` (R-02), during `Stopping`, after `Stopped`, or a blocked publisher woken by `stop()` |
| `InvalidArgument` | `event.type != event.payload.index()`; `opts.timeout < 0`; or `on_full == BlockUntil` called from the dispatcher thread (R-10) |

Guarantees:

- **G1** `Accepted` means *enqueued*, nothing more. It does not mean any handler ran, succeeded, or
  will run (spec section 17.1).
- **G2** A non-`Accepted` return means the event is **not** in the queue. v1 never drops an accepted
  event silently (FR-05).
- **G3** Safe from any number of threads concurrently (FR-01).
- **G4** Never invokes a handler on the calling thread (FR-03).
- **G5** Calling `publish` with `Reject` from inside a handler is supported; the new event is
  processed after the current one (spec section 17.6). `BlockUntil` from inside a handler is
  `InvalidArgument`, never a deadlock (R-10).
- **G6** The queue never exceeds `queue_capacity` (Q1).
- **G7** Accepted events are dispatched FIFO by enqueue order. No ordering is promised between
  concurrent publishers by wall-clock call time (Q2).

## `Subscription EventBus::subscribe(EventType type, Handler callback)`

| | |
|---|---|
| Precondition | state is `Created` or `Running`; `callback` is non-empty |
| Postcondition | the handler is registered and will receive events of `type` |
| Throws | `std::logic_error` if state is `Stopping` or `Stopped` (spec section 17.2); `std::invalid_argument` on an empty `callback` |
| Thread safety | safe from any thread, including from inside a handler |

Guarantees:

- **G8** Many handlers may subscribe to one type; each receives each matching event exactly once
  (FR-02).
- **G9** Handlers of one type are invoked in registration order, and this is a contract in v1 (R-04).
- **G10** The returned `Subscription` is move-only; letting it go out of scope unsubscribes (U1).
- **G11** A handler is never invoked while the bus holds its queue or registry lock (Q4).

**Caller obligation C1**: the handler must not capture a raw `this` that can be destroyed before the
handler can no longer run. `Subscription` manages the *registration*, not the lifetime of whatever
the callback captured. Use `weak_ptr`, or order shutdown so the handler can no longer run, before
destroying the captured object (spec section 17.5).

**Caller obligation C2**: a handler should not block for long. There is no callback timeout, and a
slow handler delays every later event (R-07). Hand slow or network work to another thread or queue.

## `void Subscription::unsubscribe()`

| | |
|---|---|
| Postcondition | no new invocation of this handler begins |
| Throws | nothing |
| Thread safety | safe from any thread, including from inside this subscription's own handler |

Guarantees:

- **G12** Idempotent (U2).
- **G13** An invocation already in progress is allowed to finish; `unsubscribe()` does not kill it,
  and v1 has no wait-for-in-flight variant (R-03).
- **G14** Calling it from inside its own handler does not deadlock: the flag is cleared, the current
  invocation completes, and no further one starts (R-13 caveat).
- **G15** Safe to call after the `EventBus` has been destroyed; it is a no-op (U3).

`~Subscription()` calls `unsubscribe()`.

## `void EventBus::stop(StopMode mode = StopMode::Drain)`

| | |
|---|---|
| Precondition | not called from the dispatcher thread |
| Postcondition | state is `Stopped`; the worker is joined |
| Throws | nothing in supported usage |
| Thread safety | safe from any non-worker thread; concurrent callers all wait for `Stopped` (R-09) |

Guarantees:

- **G16** From the instant the state leaves `Running`, every `publish()` returns `Stopped`, and every
  publisher blocked in `BlockUntil` is woken and returns `Stopped` (S1, spec section 17.7).
- **G17** `Drain`: every event accepted before the stop is dispatched before `stop()` returns.
  **This has no time limit** - a handler that never returns hangs `stop()` (spec section 17.7).
- **G18** `Discard`: queued events are dropped and counted in `discarded_on_stop_total`; the handler
  already running is allowed to finish.
- **G19** Idempotent; the worker is joined exactly once, never twice (S2).
- **G20** Calling `stop()` from inside a handler is **not supported** (spec section 17.6); v1 has no
  non-blocking `requestStop()`.

## `MetricsSnapshot EventBus::metrics() const`

| | |
|---|---|
| Postcondition | returns a by-value snapshot |
| Throws | nothing |
| Thread safety | safe from any thread at any time |

Guarantees:

- **G21** Each field is a consistent read of its own counter, but the snapshot is not atomic across
  fields. The conservation equations in [metrics.md](./metrics.md) hold only after `stop()` has
  returned (M2).

## `~EventBus()`

**Caller obligation C3**: no other thread may be calling any method on the bus while it is being
destroyed. Ordinary C++ object lifetime rules apply and the bus does not defend against this (spec
section 17.7). The destructor stops the bus if it is still running.

A `Subscription` handle outliving the bus is the one exception that **is** handled - it becomes inert
rather than dangling (U3, G15).

---

## Explicitly not in this contract (v1)

Spec "Ngoai pham vi v1", restated so no later project assumes otherwise: cross-process IPC,
distributed messaging, durable or persisted queues, delivery acknowledgement, exactly-once delivery,
hard real-time deadlines, event priority, automatic retry, zero-copy payloads, a thread-pool
dispatcher, `unsubscribeAndWait()`, a non-blocking `requestStop()`, callback timeouts, and any
metrics exporter.

Isolation from a handler that crashes the process (segfault, `std::terminate`) is **not** provided;
that needs the process supervisor of a later project (spec section 17.6).
