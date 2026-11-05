#!/usr/bin/env bash
# The spec section 21 demonstration, unattended: six scenarios, each printing
# what to expect before it runs, so the output can be checked by reading it.
#
# Usage: scripts/run_config_logging_demo.sh [path-to-build-dir]
set -u

BUILD="${1:-build}"
DEMO="${BUILD}/apps/config_logging_demo/config_logging_demo"
[ -x "${DEMO}" ] || DEMO="${DEMO}.exe"
LIVE="$(mktemp -t tcu-live-XXXXXX.json)"
LOGDIR="$(mktemp -d -t tcu-demo-XXXXXX)"
LOG="${LOGDIR}/tcu.log"
DEMO_PID=""

if [ ! -x "${DEMO}" ]; then
  echo "demo binary not found: ${DEMO}" >&2
  echo "build it first: cmake --build ${BUILD} --target config_logging_demo" >&2
  exit 1
fi

cleanup() {
  if [ -n "${DEMO_PID}" ]; then
    kill "${DEMO_PID}" 2>/dev/null
    wait "${DEMO_PID}" 2>/dev/null
  fi
  rm -f "${LIVE}"
  rm -rf "${LOGDIR}"
}
trap cleanup EXIT

step() { printf '\n=== %s ===\n' "$1"; }
expect() { printf 'expect: %s\n' "$1"; }

# A hot edit, written the way an operator should: temp file plus rename, so the
# service never sees a half-written file (spec section 25.4).
edit() {
  if command -v jq >/dev/null 2>&1; then
    jq "$1" "${LIVE}" > "${LIVE}.new" && mv "${LIVE}.new" "${LIVE}"
  else
    echo "jq not installed: skipping this edit" >&2
    return 1
  fi
}

cp configs/tcu.json "${LIVE}"

step "Scenario A - normal startup"
expect "startup_loaded with generation 1, then a health line with config_valid=true"
"${DEMO}" --config "${LIVE}" --log "${LOG}" --duration 30 &
DEMO_PID=$!
sleep 2

step "Scenario B - valid hot reload"
expect "reload_committed generation=2, then telemetry_rescheduled with period_ms=2000"
edit '.telemetry.period_ms = 2000'
sleep 2

step "Scenario C - invalid reload"
expect "reload_rejected reason=InvalidValue field=/connectivity/retry_max_ms; generation stays 2"
edit '.connectivity.retry_max_ms = 500'
sleep 2
expect "the previous value is restored so the run can continue"
edit '.connectivity.retry_max_ms = 30000'
sleep 2

step "Scenario D - restart-only change"
expect "reload_rejected reason=RestartRequired field=/logging/queue_capacity; the level change is NOT applied either"
edit '.logging.queue_capacity = 4096 | .logging.level = "DEBUG"'
sleep 2
edit '.logging.queue_capacity = 8192 | .logging.level = "INFO"'
sleep 2

step "Scenario F - sink error and recovery"
# An already-open file descriptor survives a directory permission change, so
# removing write access does NOT make the running sink fail: it would only fail
# on the next open, which happens at rotation. A live demonstration therefore
# needs a rotation to coincide with the unwritable directory, which this script
# does not force.
#
# The behaviour itself is covered deterministically by the unit suite, where
# the failure is injected through the FileOps seam instead of a real disk:
#   ctest --test-dir build -R "FileSinkTest.DiskFullMarksUnhealthyAppSurvives"
#   ctest --test-dir build -R "FileSinkTest.UnwritablePathFallsBackToStderr"
#   ctest --test-dir build -R "FileSinkTest.RecoveryProbe"
expect "no sink_error from this step - see the note in this script and the FileSinkTest cases"
chmod 500 "${LOGDIR}" 2>/dev/null || echo "chmod not effective here (Windows)" >&2
sleep 2
chmod 700 "${LOGDIR}" 2>/dev/null
sleep 2

step "shutdown"
expect "shutdown_drained, then a summary line on stderr"
kill -INT "${DEMO_PID}" 2>/dev/null
wait "${DEMO_PID}" 2>/dev/null
DEMO_PID=""

step "Scenario E - log pressure (separate run)"
expect "dropped_total rises, WARN and ERROR still appear, the process stays responsive"
"${DEMO}" --config "${LIVE}" --log "${LOG}" --flood 8x50000 --duration 5

step "output checks"
if command -v jq >/dev/null 2>&1; then
  if jq -e . "${LOG}" > /dev/null 2>&1; then
    echo "every line in ${LOG} is valid JSON"
  else
    echo "FAILED: a line in ${LOG} is not valid JSON" >&2
  fi
fi
echo "files kept (active plus rotated):"
ls -l "${LOG}"* 2>/dev/null
echo "no secret material in the log:"
if grep -r -i -E 'password|token|private[_-]key|bearer' "${LOGDIR}" >/dev/null 2>&1; then
  echo "FAILED: secret-looking material found" >&2
else
  echo "none found"
fi
