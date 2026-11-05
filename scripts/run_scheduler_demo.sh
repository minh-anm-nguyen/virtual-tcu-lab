#!/usr/bin/env bash
# Spec section 20: the whole demo workflow across both project 03 binaries.
#
# Usage: ./scripts/run_scheduler_demo.sh [build-dir]
# Default build dir: build-wsl, matching specs/003-timer-task-scheduler/quickstart.md.

set -euo pipefail

BUILD_DIR=${1:-build-wsl}
TELEMETRY="${BUILD_DIR}/apps/telemetry_timer_demo/telemetry_timer_demo"
MODEM="${BUILD_DIR}/apps/modem_timeout_demo/modem_timeout_demo"

for binary in "${TELEMETRY}" "${MODEM}"; do
  if [[ ! -x "${binary}" ]]; then
    echo "missing: ${binary}" >&2
    echo "build first: cmake --build ${BUILD_DIR}" >&2
    exit 1
  fi
done

step() {
  echo
  echo "===== $* ====="
}

step "step 2: periodic telemetry, 1 s cadence, 3 samples"
"${TELEMETRY}" --interval-ms 1000 --samples 3

step "step 3: reply at 2 s beats the 3 s timeout"
"${MODEM}" --timeout-ms 3000 --reply-ms 2000

step "step 4: reply at 4 s loses; the late reply is ignored"
"${MODEM}" --timeout-ms 3000 --reply-ms 4000

step "step 5: slow callback, fixed-rate: ticks are skipped, never burst"
"${TELEMETRY}" --interval-ms 100 --callback-ms 250 --samples 3 --mode fixed-rate

step "step 5b: the same load as fixed-delay: it drifts and skips nothing"
"${TELEMETRY}" --interval-ms 100 --callback-ms 250 --samples 3 --mode fixed-delay

step "step 6: stop(Drain) finishes due work"
"${TELEMETRY}" --interval-ms 100 --samples 3 --stop drain

step "step 7: stop(Discard) drops what has not started"
"${TELEMETRY}" --interval-ms 100 --samples 3 --stop discard

step "race check: 300 rounds with the deadline equal to the reply latency"
"${MODEM}" --timeout-ms 50 --reply-ms 50 --repeat 300

echo
echo "===== demo complete: every step exited 0 ====="
