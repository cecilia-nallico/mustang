#!/usr/bin/env bash
# End-to-end smoke test: run the fully static walleye binary and check its
# flags, logging and exit codes.
set -euo pipefail

readonly WALLEYE="${TEST_SRCDIR}/${TEST_WORKSPACE}/walleye/walleye"

# Happy path: greeting is logged on stdout by photon's logger. Capture
# first: grepping a live pipe makes the binary die of SIGPIPE once grep
# exits on first match.
out="$("${WALLEYE}" --name=e2e --log_level=1)"
grep -q "Hello, e2e!" <<<"${out}"

# Invalid log level must fail cleanly (absl flags parsed, runtime refuses).
if "${WALLEYE}" --log_level=9 >/dev/null 2>&1; then
  echo "FAIL: --log_level=9 should be rejected" >&2
  exit 1
fi

# Unknown event engine must fail cleanly.
if "${WALLEYE}" --event_engine=devpoll >/dev/null 2>&1; then
  echo "FAIL: --event_engine=devpoll should be rejected" >&2
  exit 1
fi

echo "PASS"
