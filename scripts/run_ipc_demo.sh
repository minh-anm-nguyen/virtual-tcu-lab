#!/usr/bin/env bash
# Project 02 demo, spec section 16: request/response, subscribe, kill the
# service, watch the client reconnect and keep receiving notifications.
#
# Run from the repository root inside WSL2/Linux. The socket must live on the
# Linux filesystem: an AF_UNIX bind on /mnt fails EOPNOTSUPP (research R-00).

set -u

BUILD_DIR="${BUILD_DIR:-build-wsl}"
RUN_DIR="${RUN_DIR:-/tmp/virtual-tcu-demo}"
SOCKET="${RUN_DIR}/vehicle.sock"
SERVER="${BUILD_DIR}/apps/vehicle_service/vehicle_service"
CLIENT="${BUILD_DIR}/apps/tcu_client/tcu_client"

if [ ! -x "${SERVER}" ] || [ ! -x "${CLIENT}" ]; then
  echo "demo: binaries missing. Build first:" >&2
  echo "  cmake -S . -B ${BUILD_DIR} -G Ninja -DCMAKE_BUILD_TYPE=Debug" >&2
  echo "  cmake --build ${BUILD_DIR}" >&2
  exit 1
fi

case "${SOCKET}" in
  /mnt/*)
    echo "demo: refusing a socket on a DrvFs path (bind fails EOPNOTSUPP)" >&2
    exit 1
    ;;
esac

SERVER_PID=""
CLIENT_PID=""

cleanup() {
  if [ -n "${CLIENT_PID}" ]; then kill "${CLIENT_PID}" 2>/dev/null; fi
  if [ -n "${SERVER_PID}" ]; then kill "${SERVER_PID}" 2>/dev/null; fi
  wait 2>/dev/null
  rm -rf "${RUN_DIR}"
}
trap cleanup EXIT INT TERM

# Idempotent: a previous run leaves nothing behind that this one trips over.
rm -rf "${RUN_DIR}"

wait_for_socket() {
  local i=0
  while [ "${i}" -lt 50 ]; do
    if [ -S "${SOCKET}" ]; then
      return 0
    fi
    sleep 0.1
    i=$((i + 1))
  done
  return 1
}

start_server() {
  "${SERVER}" --socket "${SOCKET}" --ignition-period-ms 1000 &
  SERVER_PID=$!
  if ! wait_for_socket; then
    echo "demo: the service never started listening" >&2
    exit 1
  fi
}

echo "=== Virtual TCU Lab / project 02 - IPC Service Framework demo ==="
echo
echo "--- step 1: start vehicle_service ---"
start_server
echo "service pid=${SERVER_PID} socket=${SOCKET}"

echo
echo "--- step 2: client requests status and subscribes ---"
"${CLIENT}" --socket "${SOCKET}" --scenario status-and-subscribe --seconds 9 &
CLIENT_PID=$!
sleep 3

echo
echo "--- step 3: kill the service (SIGKILL), the client must not crash ---"
kill -9 "${SERVER_PID}" 2>/dev/null
wait "${SERVER_PID}" 2>/dev/null
SERVER_PID=""
sleep 2

echo
echo "--- step 4: restart the service, the client must reconnect and re-subscribe ---"
start_server
echo "service pid=${SERVER_PID} restarted"

wait "${CLIENT_PID}"
CLIENT_RC=$?
CLIENT_PID=""

echo
echo "--- result ---"
if [ "${CLIENT_RC}" -eq 0 ]; then
  echo "demo finished: the client survived the kill, reconnected and kept running."
else
  echo "demo finished with client exit code ${CLIENT_RC}" >&2
fi
exit "${CLIENT_RC}"
