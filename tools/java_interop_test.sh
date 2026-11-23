#!/usr/bin/env bash
# Java compliance suite: official gRPC-java client against the walleye
# (photon) gRPC server. Starts the server, waits for its PORT line,
# runs the client, propagates its exit code.
set -euo pipefail

readonly SERVER="${TEST_SRCDIR}/${TEST_WORKSPACE}/walleye/grpc/interop/java_interop_server"
readonly CLIENT="${TEST_SRCDIR}/${TEST_WORKSPACE}/java/interop_client"
readonly OUT="${TEST_TMPDIR}/server.out"
readonly ERR="${TEST_TMPDIR}/server.err"

"${SERVER}" >"${OUT}" 2>"${ERR}" &
readonly SRV=$!

port=""
for _ in $(seq 1 100); do
  port=$(grep -o 'PORT [0-9]*' "${OUT}" 2>/dev/null | head -1 | cut -d' ' -f2 || true)
  if [ -n "${port}" ]; then
    break
  fi
  sleep 0.1
done
if [ -z "${port}" ]; then
  echo "FAIL: server did not report a port" >&2
  cat "${ERR}" >&2
  kill "${SRV}" 2>/dev/null || true
  exit 1
fi

code=0
"${CLIENT}" 127.0.0.1 "${port}" || code=$?

kill "${SRV}" 2>/dev/null || true
wait "${SRV}" 2>/dev/null || true
exit "${code}"
