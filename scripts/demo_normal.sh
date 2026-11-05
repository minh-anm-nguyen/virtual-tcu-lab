#!/usr/bin/env bash
# Spec section 21.5 steps 1 and 6: the normal-drive demo.
#
# Starts the TCU service, the vehicle ECUs and the BCM on vcan0, shows the
# decoded signals going fresh, then stops everything without a forced kill.
set -euo pipefail

BUILD="${BUILD:-build}"
IFACE="${IFACE:-vcan0}"
CONFIG="${CONFIG:-config/can_lab.json}"
SCENARIO="${SCENARIO:-config/scenarios/normal_drive.json}"

if ! ip link show "$IFACE" >/dev/null 2>&1; then
  echo "$IFACE does not exist. Run scripts/setup_vcan.sh first." >&2
  echo "On a stock WSL2 kernel there is no vcan module at all; use a Linux VM." >&2
  exit 1
fi

SERVICE="$BUILD/apps/tcu_can_service/tcu_can_service"
SIM="$BUILD/apps/vehicle_simulator/vehicle_simulator"
BCM="$BUILD/apps/bcm_simulator/bcm_simulator"
CLI="$BUILD/apps/can_control_cli/can_control_cli"
for binary in "$SERVICE" "$SIM" "$BCM" "$CLI"; do
  if [[ ! -x "$binary" ]]; then
    echo "missing $binary - build first: cmake --build $BUILD" >&2
    exit 1
  fi
done

pids=()
cleanup() {
  echo
  echo "--- stopping (SIGTERM, no forced kill: section 21.5 step 6) ---"
  for pid in "${pids[@]:-}"; do
    kill -TERM "$pid" 2>/dev/null || true
  done
  for pid in "${pids[@]:-}"; do
    wait "$pid" 2>/dev/null || true
  done
}
trap cleanup EXIT

echo "--- starting tcu_can_service ---"
"$SERVICE" --config "$CONFIG" --interface "$IFACE" &
pids+=($!)

echo "--- starting bcm_simulator ---"
"$BCM" --interface "$IFACE" &
pids+=($!)

echo "--- starting vehicle_simulator (scenario: $(basename "$SCENARIO")) ---"
"$SIM" --config "$CONFIG" --interface "$IFACE" --scenario "$SCENARIO" &
pids+=($!)

# Long enough for the 1 s frames to arrive at least twice.
sleep 3

echo
echo "--- health (section 16.2 surface): expect FRESH signals ---"
"$CLI" health

echo
echo "--- a synthetic LOCK command, correlated by sequence ---"
"$CLI" lock
sleep 1
"$CLI" health

echo
echo "--- metrics ---"
"$CLI" metrics
