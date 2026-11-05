#!/usr/bin/env bash
# Spec section 21.5 steps 2-4: pause one signal, watch only that one go stale,
# resume it and watch it recover exactly once.
#
# This is the demo behind quickstart scenarios 6.4 and 6.5, and it exercises the
# runtime control plane rather than a launch flag.
set -euo pipefail

BUILD="${BUILD:-build}"
IFACE="${IFACE:-vcan0}"
CONFIG="${CONFIG:-config/can_lab.json}"
SPEED_ID="${SPEED_ID:-0x101}"

if ! ip link show "$IFACE" >/dev/null 2>&1; then
  echo "$IFACE does not exist. Run scripts/setup_vcan.sh first." >&2
  exit 1
fi

SERVICE="$BUILD/apps/tcu_can_service/tcu_can_service"
SIM="$BUILD/apps/vehicle_simulator/vehicle_simulator"
CLI="$BUILD/apps/can_control_cli/can_control_cli"
for binary in "$SERVICE" "$SIM" "$CLI"; do
  if [[ ! -x "$binary" ]]; then
    echo "missing $binary - build first: cmake --build $BUILD" >&2
    exit 1
  fi
done

pids=()
cleanup() {
  echo
  echo "--- stopping ---"
  for pid in "${pids[@]:-}"; do
    kill -TERM "$pid" 2>/dev/null || true
  done
  for pid in "${pids[@]:-}"; do
    wait "$pid" 2>/dev/null || true
  done
}
trap cleanup EXIT

"$SERVICE" --config "$CONFIG" --interface "$IFACE" &
pids+=($!)
"$SIM" --config "$CONFIG" --interface "$IFACE" &
pids+=($!)
sleep 2

echo "--- step 1: everything fresh ---"
"$CLI" health

echo
echo "--- step 2: pause $SPEED_ID only ---"
"$CLI" pause-id "$SPEED_ID"

# speed_kmh has a 500 ms deadline; everything else has 3000 ms. Waiting 1.5 s
# puts us past the first and well inside the others (B-18).
sleep 1.5

echo
echo "--- step 3: speed is STALE, its last good value is still there, and"
echo "            heartbeat and ignition are still FRESH ---"
"$CLI" health

echo
echo "--- step 4: resume it; exactly one recovery ---"
"$CLI" resume-id "$SPEED_ID"
sleep 1
"$CLI" health

echo
echo "--- metrics: one stale transition, one recovery ---"
"$CLI" metrics | grep -E "stale_transitions|recovered"
