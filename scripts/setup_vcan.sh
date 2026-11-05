#!/usr/bin/env bash
# Create a virtual CAN interface for the project 05 lab.
#
# Idempotent: safe to run when the interface already exists (spec section 21.2
# and the Definition of Done). Refuses any name that is not vcan* so the lab
# can never reconfigure a physical interface (NFR-01, research R-16).
set -euo pipefail

IFACE="${1:-vcan0}"

if [[ ! "$IFACE" =~ ^vcan[0-9]*$ ]]; then
  echo "refusing to touch '$IFACE': only vcan* interfaces are allowed in this lab" >&2
  exit 1
fi

if ! modprobe -n vcan >/dev/null 2>&1; then
  echo "kernel module 'vcan' is not available on $(uname -r)." >&2
  echo "Use a native Linux host or a Linux VM; a stock WSL2 kernel has no vcan." >&2
  exit 1
fi

sudo modprobe vcan

if ip link show "$IFACE" >/dev/null 2>&1; then
  echo "$IFACE already exists"
else
  sudo ip link add dev "$IFACE" type vcan
  echo "$IFACE created"
fi

sudo ip link set dev "$IFACE" up
ip -details link show "$IFACE"
