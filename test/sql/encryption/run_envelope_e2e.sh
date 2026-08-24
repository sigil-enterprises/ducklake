#!/usr/bin/env bash
#
# PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.
#
# Run the KMS envelope end-to-end fixture with a reachable fake key service
# behind it.
#
#   docker compose run --rm --entrypoint bash app -lc \
#     'test/sql/encryption/run_envelope_e2e.sh'
#
# WHY A RUNNER AND NOT JUST A .test FILE
# --------------------------------------
# `envelope_e2e_wrapped_catalog.test` carries `require-env`, and a require-env
# file SKIPS when the variable is unset - it does not fail. A skip is not a
# pass, and the whole of issue #52 is that the envelope path has never actually
# run. This script supplies the socket AND refuses a run in which the fixture
# reported no assertions.
#
# REFUSAL vs CRASH: every refusal below exits non-zero WITH an `::error::`
# annotation. A non-zero exit without one is a crash.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../../.." && pwd)"
UNITTEST="${DUCKLAKE_UNITTEST:-${ROOT}/build/release/test/unittest}"
FIXTURE="test/sql/encryption/envelope_e2e_wrapped_catalog.test"

if [ ! -x "${UNITTEST}" ]; then
  echo "::error::no unittest binary at ${UNITTEST} - run 'make release' first"
  exit 1
fi

# Short, because sun_path is 108 bytes and the client refuses anything longer.
SOCKET_DIR="$(mktemp -d /tmp/dlkms.XXXXXX)"
SOCKET_PATH="${SOCKET_DIR}/s"
FAKE_LOG="${SOCKET_DIR}/fake.log"
OPLOG_PATH="${SOCKET_DIR}/ops.csv"
: > "${OPLOG_PATH}"
export DUCKLAKE_FAKE_KMS_OPLOG="${OPLOG_PATH}"

cleanup() {
  if [ -n "${FAKE_PID:-}" ] && kill -0 "${FAKE_PID}" 2>/dev/null; then
    kill "${FAKE_PID}" 2>/dev/null || true
    wait "${FAKE_PID}" 2>/dev/null || true
  fi
  rm -rf "${SOCKET_DIR}"
}
trap cleanup EXIT

python3 "${HERE}/fake_kms.py" "${SOCKET_PATH}" > "${FAKE_LOG}" 2>&1 &
FAKE_PID=$!

# Wait for the bind rather than sleeping at it: an ATTACH that races the listen
# fails with "cannot reach the key service", which would look like the thing
# under test rather than a startup race.
for _ in $(seq 1 100); do
  [ -S "${SOCKET_PATH}" ] && break
  sleep 0.1
done
if [ ! -S "${SOCKET_PATH}" ]; then
  echo "::error::fake kms never bound ${SOCKET_PATH}"
  cat "${FAKE_LOG}" >&2
  exit 1
fi

export DUCKLAKE_FAKE_KMS_SOCKET="${SOCKET_PATH}"

# `|| status=$?` and not a bare call: under `set -e` a failing fixture would
# exit here and the skip guard below - the thing this script exists for - would
# never run.
status=0
# `--test-dir "${ROOT}"`: the unittest binary discovers sqllogictest files by
# WALKING the test directory at startup, and its default is duckdb's own tree,
# not ducklake's. Without it every ducklake fixture reports "No test cases
# matched" - measured, and the reason this runner never ran the fixture.
output="$("${UNITTEST}" --test-dir "${ROOT}" "${FIXTURE}" 2>&1)" || status=$?
echo "${output}"

echo "--- fake kms operation log ---"
cat "${OPLOG_PATH}"
echo "--- end operation log ---"

# Measured, not assumed: a skipped file prints "All tests were skipped", never
# the word "assertions", and EXITS ZERO. So neither the exit status nor the
# absence of a failure can distinguish a skip from a pass - only the presence of
# assertions can.
if ! grep -q "assertions" <<< "${output}"; then
  echo "::error::${FIXTURE} reported no assertions - it either skipped or died. A skip is not a pass."
  exit 1
fi

if [ "${status}" -ne 0 ]; then
  echo "::error::${FIXTURE} FAILED - the KMS envelope did not engage end to end (issue #52)"
fi
exit "${status}"
