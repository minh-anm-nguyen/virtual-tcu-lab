# Contract: metrics and observable acceptance

Counter definitions come from spec section 19; the counting semantics were settled in research R-12.
These counters are not decoration - the conservation equations below are the cheapest end-to-end
correctness check the bus has, and the tests assert them.

## Counters

| Field | Increments | Does **not** count |
|---|---|---|
| `accepted_total` | once per `publish()` that returned `Accepted` | anything about whether a handler ran |
| `rejected_queue_full_total` | once per `QueueFull` return | `TimedOut`, `Stopped` |
| `publish_timeout_total` | once per `TimedOut` return, including the `timeout == 0` poll (R-11) | `QueueFull`, publishers woken by `stop()` |
| `dispatched_total` | once per event whose dispatch round **completed** - including an event with no subscriber, and an event whose handler threw | callback invocations; discarded events |
| `callback_invocations_total` | once per handler call attempted | - |
| `callback_failures_total` | once per handler call that threw and was caught | a handler that crashed the process |
| `discarded_on_stop_total` | once per event still queued when `stop(Discard)` cleared the queue | the event whose handler was already running |
| `queue_depth` | gauge: events waiting in the queue | the event currently being dispatched |
| `queue_high_watermark` | gauge: largest `queue_depth` observed | - |

A publish that returns `Stopped` or `InvalidArgument` increments nothing. That is deliberate - those
are a shutdown race and a programming error, not backpressure, and folding them into the
backpressure counters would hide both.

## Conservation equations

Valid only after `stop()` has returned, i.e. the worker is joined and no counter can move (M2).

**After `stop(Drain)`:**

```text
accepted_total == dispatched_total
discarded_on_stop_total == 0
```

**After `stop(Discard)`:**

```text
accepted_total == dispatched_total + discarded_on_stop_total
```

**Always:**

```text
queue_depth == 0                                  (after stop)
queue_depth <= queue_high_watermark <= capacity   (M1)
callback_failures_total <= callback_invocations_total
```

`callback_invocations_total` has no fixed relation to `dispatched_total`: it is larger when events
have several subscribers and smaller when they have none. Any test that assumes otherwise is wrong.

## Tests that assert these

| Assertion | Test |
|---|---|
| Drain equation | UT-10, B07 |
| Discard equation, `discarded_on_stop_total == 2` | UT-11, B08 |
| `callback_failures_total == 1` while the second handler still runs | UT-07, B06 |
| `rejected_queue_full_total` moves and `publish_timeout_total` does not | UT-04, B03 |
| `publish_timeout_total` moves and `rejected_queue_full_total` does not | UT-05, B04 |
| `accepted_total == dispatched_total` over 8 threads and 8000 events | B09 |
| `queue_depth` never exceeds capacity; high watermark `<=` capacity | UT-04, B03 |
| `dispatched_total` still increments for an event with no subscriber | B02 |

## Structured log fields

The demo and library logs use exactly these keys (spec section 20); nothing else, and never payload
contents, keys, tokens or credentials (constitution Principle V):

```text
bus_state, capacity, mode,
event_id, event_type, publisher, subscriber,
queue_depth, publish_status, callback_status,
accepted, dispatched, discarded, callback_failures
```

Logging must not recurse through the bus itself and must not block the dispatcher for long (spec
section 8).
