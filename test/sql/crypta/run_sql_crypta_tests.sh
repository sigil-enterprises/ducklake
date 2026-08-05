#!/usr/bin/env bash
#
# PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.
#
# Run the test/sql/crypta group with a key service behind it.
#
#   docker compose run --rm --entrypoint bash app -lc \
#     'test/sql/crypta/run_sql_crypta_tests.sh'
#
# crypta_attach_refusals.test needs nothing and runs under a plain
# `./build/release/test/unittest "test/sql/crypta/*"`. crypta_key_row_refusals
# .test does not: the envelope provider is only installed on the catalog once
# ATTACH's self-test reaches a live socket, so without one that file SKIPS -
# and a skip is not a pass. This script supplies the socket.
#
# What it supplies is `fake_crypta.py`, which does no cryptography. It enforces
# the identity binding and nothing else, because that is the only property the
# key-row cases depend on. The real cipher, the real KEK, and KEK recovery
# across a restart are proven by scripts/mvp_crypta_proof.sh against the real
# service; nothing here substitutes for that.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../../.." && pwd)"
# Overridable so the coverage measurement can point this at the INSTRUMENTED
# build. It is not a convenience: hardcoding build/release meant
# `measure_crypta_refusal_coverage.sh` ran an uninstrumented binary and wrote no
# counters at all, and its "positive" arm read LOWER than its negative one. The
# two-arm discipline caught it; one arm would have reported a plausible zero.
UNITTEST="${DUCKLAKE_UNITTEST:-${ROOT}/build/release/test/unittest}"

if [ ! -x "${UNITTEST}" ]; then
  echo "no unittest binary at ${UNITTEST} - run 'BUILD_EXTENSION_TEST_DEPS=full make release' first" >&2
  exit 1
fi

# Short, because sun_path is 108 bytes and the client rightly refuses anything
# longer.
SOCKET_DIR="$(mktemp -d /tmp/dlcrypta.XXXXXX)"
SOCKET_PATH="${SOCKET_DIR}/s"
FAKE_LOG="${SOCKET_DIR}/fake.log"
# One CSV line per request the fake serves. A .test file reads it to assert that
# a call did NOT happen, which is the only way to prove a refusal fired BEFORE
# the socket rather than after it.
OPLOG_PATH="${SOCKET_DIR}/ops.csv"
: > "${OPLOG_PATH}"
export DUCKLAKE_FAKE_CRYPTA_OPLOG="${OPLOG_PATH}"

cleanup() {
  if [ -n "${FAKE_PID:-}" ] && kill -0 "${FAKE_PID}" 2>/dev/null; then
    kill "${FAKE_PID}" 2>/dev/null || true
    wait "${FAKE_PID}" 2>/dev/null || true
  fi
  rm -rf "${SOCKET_DIR}"
}
trap cleanup EXIT

python3 "${HERE}/fake_crypta.py" "${SOCKET_PATH}" > "${FAKE_LOG}" 2>&1 &
FAKE_PID=$!

# Wait for the bind rather than sleeping at it: an ATTACH that races the listen
# fails with "cannot reach the crypta key service", which would look like the
# thing under test rather than a startup race.
for _ in $(seq 1 100); do
  [ -S "${SOCKET_PATH}" ] && break
  sleep 0.1
done
if [ ! -S "${SOCKET_PATH}" ]; then
  echo "fake crypta never bound ${SOCKET_PATH}:" >&2
  cat "${FAKE_LOG}" >&2
  exit 1
fi

export DUCKLAKE_FAKE_CRYPTA_SOCKET="${SOCKET_PATH}"
# `|| status=$?` and not a bare call: under `set -e` a failing suite would exit
# here, and the skip guard below - the thing this script exists for - would
# never run. The status is carried to the end instead.
status=0
"${UNITTEST}" "test/sql/crypta/*" || status=$?

# A guard against the failure mode this script exists to remove: a require-env
# file skipping silently while the run still reads green. A skip is not a pass.
#
# Measured, not assumed: a skipped file prints "All tests were skipped (total
# skipped 1)", never the word "assertions", and EXITS ZERO. So neither the exit
# status nor the absence of a failure can distinguish a skip from a pass - only
# the presence of assertions can.
#
# The list is DERIVED, not hard-coded, so a fourth require-env file added later
# is guarded the day it is written rather than the day someone remembers.
required_files="$(grep -l '^require-env' "${HERE}"/*.test | xargs -n1 basename)"
if [ -z "${required_files}" ]; then
  echo "no require-env files found under ${HERE} - the guard has nothing to check" >&2
  exit 1
fi
for required in ${required_files}; do
  output="$("${UNITTEST}" "test/sql/crypta/${required}" 2>&1 || true)"
  if ! grep -q "assertions" <<< "${output}"; then
    # Deliberately not "it was skipped": a file that RAN and FAILED also lands
    # here, and calling that a skip would send the reader hunting the wrong bug.
    echo "${required} reported no assertions - it either skipped or died. A skip is not a pass:" >&2
    echo "${output}" >&2
    exit 1
  fi
done
exit "${status}"
