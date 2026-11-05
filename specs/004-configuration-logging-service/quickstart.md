# Quickstart: Configuration & Logging Service

**Feature**: `004-configuration-logging-service` | Spec sections 17, 18, 21 and 24

How to build it, how to run every test, how to drive the six demo scenarios, and the acceptance
checklist with the command that proves each line. Nothing here is a claim about code that does not
exist yet: until `/speckit-implement` has run, this is the validation plan.

---

## 1. Build and test

```bash
# Configure and build everything (projects 01-04)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Everything
ctest --test-dir build --output-on-failure

# Just this project, by label
ctest --test-dir build -L config-unit --output-on-failure
ctest --test-dir build -L logging-unit --output-on-failure
ctest --test-dir build -L config-logging-integration --output-on-failure
ctest --test-dir build -L config-logging-stress --output-on-failure
```

The unit suites use no sleeps and no real timers, so both finish in about a second. The stress suite
honours `TCU_CONFIG_STRESS_SECONDS` (default 5):

```bash
TCU_CONFIG_STRESS_SECONDS=60 ctest --test-dir build -L config-logging-stress --output-on-failure
```

A long soak is the same test with the variable raised; section 17's 1-24 hour run is a manual job,
not a CI one:

```bash
TCU_CONFIG_STRESS_SECONDS=3600 ctest --test-dir build -R Soak --output-on-failure
```

### Sanitizers (Linux or WSL only)

```bash
# ASan + UBSan
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure

# TSan: clang, and setarch, per project 03's research R-00 addendum
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++ -DTCU_SANITIZER_SETARCH=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread"
cmake --build build-tsan && ctest --test-dir build-tsan --output-on-failure
```

Never combine ASan and TSan. On Windows the libraries build and all four suites run; the sanitizer
jobs do not, which is the toolchain limitation spec section 24's criterion is read against (R-00).
Both sanitizer builds were run for this project under WSL2 (Ubuntu 22.04, gcc 11 and clang 14) and
both are clean across all four labels.

With the MSYS2/UCRT64 toolchain on Windows, put `C:/msys64/ucrt64/bin` first on `PATH` before
building: `gtest_discover_tests` runs each test binary at build time, and another toolchain earlier
on `PATH` shadows the runtime DLLs, which surfaces as exit code `0xc0000139`.

---

## 2. Run the demo

```bash
mkdir -p logs
cp configs/tcu.json /tmp/tcu-live.json
./build/apps/config_logging_demo/config_logging_demo --config /tmp/tcu-live.json --log logs/tcu.log
```

The demo starts a scheduler (one worker), a logger (console + rotating file at `logs/tcu.log`), the
configuration service, and two worker threads — telemetry (periodic sample on the scheduler) and
connectivity (retry backoff). Console output is JSONL; pipe through `jq` to read it:

```bash
./build/apps/config_logging_demo/config_logging_demo --config /tmp/tcu-live.json | jq -c '{event,severity,fields}'
```

`scripts/run_config_logging_demo.sh` performs the whole section 21 sequence unattended and prints
what to expect at each step.

### Scenario A — normal startup (spec section 12.A)

Expected, in order:

```jsonl
{"severity":"INFO","component":"config","event":"startup_loaded","fields":{"generation":1}}
{"severity":"INFO","component":"demo","event":"health","fields":{"config_valid":true,"generation":1,"logger_healthy":true}}
```

### Scenario B — valid hot reload (12.B)

Always write with temp file plus rename; that is what section 25.4 recommends and what the poller is
built for:

```bash
jq '.telemetry.period_ms = 2000' /tmp/tcu-live.json > /tmp/tcu-live.json.new
mv /tmp/tcu-live.json.new /tmp/tcu-live.json     # atomic on the same filesystem
```

Expected within one poll interval (500 ms):

```jsonl
{"severity":"INFO","component":"config","event":"reload_committed","fields":{"generation":2}}
{"severity":"INFO","component":"demo","event":"telemetry_rescheduled","fields":{"generation":2,"period_ms":2000}}
```

Telemetry samples then arrive every 2 s instead of 5 s.

### Scenario C — invalid reload (12.C)

```bash
jq '.connectivity.retry_max_ms = 500' /tmp/tcu-live.json > /tmp/tcu-bad.json
mv /tmp/tcu-bad.json /tmp/tcu-live.json
```

Expected — generation stays 2, and the reason names the relation, not the values:

```jsonl
{"severity":"WARN","component":"config","event":"reload_rejected","fields":{"reason":"InvalidValue","field":"/connectivity/retry_max_ms"}}
```

### Scenario D — restart-only change (12.D)

```bash
jq '.logging.queue_capacity = 4096 | .logging.level = "DEBUG"' /tmp/tcu-live.json > /tmp/tcu-rs.json
mv /tmp/tcu-rs.json /tmp/tcu-live.json
```

Expected — rejected whole. The `DEBUG` change is **not** applied either (BC-08):

```jsonl
{"severity":"WARN","component":"config","event":"reload_rejected","fields":{"reason":"RestartRequired","field":"/logging/queue_capacity"}}
```

Restart the demo to adopt it.

### Scenario E — log pressure (12.E)

```bash
./build/apps/config_logging_demo/config_logging_demo --config /tmp/tcu-live.json --flood 8x50000 --duration 5
```

Expected: `logger_records_dropped_total{DEBUG}` rises, WARN and ERROR records still appear, the
health line reports a non-zero `dropped_total`, and telemetry keeps sampling on schedule. The
process stays responsive — that is the assertion, not the drop count.

### Scenario F — sink failure (12.F)

A real `ENOSPC` is not needed and is not used in tests (Principle IV). For the demo, make the log
directory unwritable:

```bash
chmod 500 logs                        # Linux; on Windows use an ACL or a read-only directory
# ... let the demo run, then:
chmod 700 logs
```

Expected: `sink_error` with `fields.sink = "file"`, the health line shows `file_sink: "unhealthy"`,
console logging continues, and within 30 s of permissions being restored a `sink_recovered` line
appears. No spinning in between (F6).

---

## 3. Inspect the output

```bash
# Every line is valid JSON - this prints nothing if one is not.
# jq ships with WSL and most Linux images but not with MSYS2, so the python
# equivalent is given for the check that matters.
jq -e . logs/tcu.log > /dev/null && echo "all lines well-formed"
python -c "import json; [json.loads(l) for l in open('logs/tcu.log') if l.strip()]; print('all lines well-formed')"

# Rotation: at most file_count rotated files plus the active one
ls -l logs/tcu.log*

# Permissions: owner read/write only
stat -c '%a %n' logs/tcu.log*

# No secret ever reaches a sink
! grep -r -i -E 'password|token|private[_-]key|bearer' logs/ && echo "no secret material in logs"
```

---

## 4. Acceptance checklist (spec section 24)

Every line has a command or a test that decides it. Nothing is marked done by inspection.

| Criterion | Proven by |
|---|---|
| Typed schema covers every sample field and the version policy | `ctest -R Schema_` — `Schema_SampleFileMatchesTable`, `Schema_EveryFieldHasReloadPolicy`, `Loader_RejectsUnsupportedSchemaVersion` |
| Invalid startup fails safely; invalid reload keeps last-known-good | `ctest -R 'Lifecycle_MissingRequiredFieldFailsStartup|Reload_Invalid'` |
| Generation, no-op, hot/restart-only and observer behaviours match contracts | `ctest -L config-unit` — `reload_test.cpp`, `subscriber_test.cpp` |
| No partial config reads under simultaneous reloads | `ctest -R Snapshot_ReaderSeesOldOrNewNeverTorn` plus the TSan build |
| Logging JSONL is well-formed under concurrent producers | `ctest -R Stress_EightProducersWellFormedJsonl`, then `jq -e . logs/tcu.log` |
| All secrets classified and redacted, including on error paths | `ctest -R 'Redaction_'` — `Redaction_SecretAbsentFromEverySink`, `Redaction_AppliedAtIngestionAndFormatting`, `Redaction_ReasonStringsNeverQuoteInput` |
| Bounded queue, max record size and file retention enforced | `ctest -R 'Queue_|Record_Oversize|FileSink_RetainsAtMostFileCount'` |
| Disk failures cannot crash normal telemetry processing | `ctest -R FileSink_DiskFullMarksUnhealthyAppSurvives` and demo scenario F |
| Shutdown does not deadlock or leave owned threads running | `ctest -R 'Logger_Stop|Service_Stop|shutdown'` plus the ASan build (a leaked thread shows up at exit) |
| BC-01..BC-30 mapped to automated or documented manual tests | the traceability table in [plan.md](./plan.md); every row names a test |
| ASan/UBSan/TSan run in supported configurations; limits recorded | section 1 above; the Windows limitation is stated, not hidden |
| Integration demo shows valid reload, invalid reload, rotation and overload | `scripts/run_config_logging_demo.sh` |
| README documents architecture, build, limitations and metrics | `README.md` config and logging sections |

### Known limitations to state, not to hide

1. A config change that preserves both mtime and size is not detected by the poller; use the
   explicit reload path (R-03).
2. Detection latency is up to one poll interval (500 ms default).
3. Records still queued when the process dies are lost. No durability is claimed (BC-24).
4. Rotation briefly stalls the writer thread; producers are unaffected but queue depth rises.
5. Performance numbers from a workstation run are measurements of that workstation, not guarantees
   (section 18, R-21).
6. Sanitizer coverage is Linux/WSL only.
7. Project 03's scheduler still logs to stderr through its own private logger; it was not migrated
   here (R-19).

---

## 5. Performance report

The stress suite prints, and never asserts:

```text
[config-logging] host: <cpu> / <cores> cores / <ram> GB / <os>
[config-logging] log throughput:        <N> records/s over <S> s, <D> dropped
[config-logging] reload latency ms:     p50 <a>  p95 <b>  p99 <c>  (n=<count>)
[config-logging] snapshot read us:      p50 <a>  p95 <b>  p99 <c>  (n=<count>)
[config-logging] rotations: <N>   sink errors: <N>   peak queue depth: <N>
```

Compare against section 18's budgets by reading the numbers, not by watching a test go red. A CI
failure caused by a busy runner teaches nobody anything.
