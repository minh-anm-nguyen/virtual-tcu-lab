#!/usr/bin/env bash
# Project 06 demo: build, then run the spec section 20 sequence.
#
# Prefers a real vcan0 so cansend and candump work against the same bus. Falls
# back to project 05's FakeCanTransport when the interface is missing and we
# cannot create it, and says which one it used - a demo must never imply that
# frames went on the wire when they did not (project 05's section 18.1 rule).

set -euo pipefail

cd "$(dirname "$0")/.."
BUILD_DIR="${BUILD_DIR:-build-wsl}"
IFACE="${IFACE:-vcan0}"

echo "== configuring and building =="
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build "$BUILD_DIR" --target virtual_dbc_demo dbc_validate dbc_decode -j"$(nproc)"

echo
echo "== validating the lab DBC =="
"$BUILD_DIR/apps/dbc_validate/dbc_validate" configs/virtual_tcu.dbc

echo
echo "== decoding the spec section 7.3 vectors =="
"$BUILD_DIR/apps/dbc_decode/dbc_decode" --dbc configs/virtual_tcu.dbc \
  --hex 100:1027010050000000 \
  --hex 101:ABC0000000000000 \
  --hex 102:017FD20400000000 \
  --hex 102:027F9CFF00000000

CREATED_VCAN=0
MODE="--fake"
if ip link show "$IFACE" >/dev/null 2>&1; then
  MODE="--iface $IFACE"
elif [ "$(id -u)" = "0" ] && scripts/setup_vcan.sh >/dev/null 2>&1; then
  MODE="--iface $IFACE"
  CREATED_VCAN=1
else
  echo
  echo "NOTE: $IFACE is not present and this shell cannot create it."
  echo "      Running over FakeCanTransport instead. For the on-the-wire path:"
  echo "        sudo scripts/setup_vcan.sh && IFACE=$IFACE $0"
fi

echo
echo "== running the demo ($MODE) =="
# shellcheck disable=SC2086
"$BUILD_DIR/apps/virtual_dbc_demo/virtual_dbc_demo" --config configs/decoder.json $MODE

if [ "$CREATED_VCAN" = "1" ]; then
  echo
  echo "== tearing down the interface this script created =="
  scripts/teardown_vcan.sh || true
fi

cat <<'NOTES'

What to look for, per quickstart section 3:
  B   the section 7.3 values: 100 km/h, ignition On, -40 degC, -674 Nm,
      12.34 V on page 1 and -10 degC on page 2
  C   a wrong-length frame reports decode_error while last_good stays 100 km/h
  D   an unknown id is counted, and an extended 0x100 is NOT aliased onto the
      standard 0x100
  E   both axes at once: observed=valid with last_good marked STALE
  F   one valid frame recovers it, with a new epoch
  G   an overlapping DBC is rejected: the version does not move and the service
      stays ready
  G2  a valid edit commits as version 2, and signals whose meaning did not
      change keep their last-good value
  H   a burst leaves the queue depth bounded and never deadlocks
  I   shutdown is ordered and reports what it drained
NOTES
