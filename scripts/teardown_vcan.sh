#!/usr/bin/env bash
# Remove the project 05 lab virtual CAN interface.
#
# Idempotent: a missing interface is success, not an error. Refuses any name
# that is not vcan* (NFR-01, research R-16).
set -euo pipefail

IFACE="${1:-vcan0}"

if [[ ! "$IFACE" =~ ^vcan[0-9]*$ ]]; then
  echo "refusing to touch '$IFACE': only vcan* interfaces are allowed in this lab" >&2
  exit 1
fi

if ! ip link show "$IFACE" >/dev/null 2>&1; then
  echo "$IFACE does not exist, nothing to do"
  exit 0
fi

sudo ip link set dev "$IFACE" down
sudo ip link delete dev "$IFACE" type vcan
echo "$IFACE removed"
