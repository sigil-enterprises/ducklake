#!/usr/bin/env bash
#
# PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.
#
# Run ANY envelope fixture with a reachable fake key service behind it.
#
#   test/sql/encryption/run_envelope_fixture.sh test/sql/encryption/some.test [...]
#
# run_envelope_e2e.sh drives one named fixture and additionally dumps the
# operation log, which is part of what that fixture asserts. This one takes the
# fixture as an argument so a new envelope .test does not need a new runner -
# the thing that made issue #52 possible was a fixture with nowhere to be run
# from.
#
# THE SKIP GUARD IS THE POINT. Every fixture here carries `require-env`, and a
# require-env skip EXITS ZERO. Measured: a skipped file prints "All tests were
# skipped", never the word "assertions". So neither the exit status nor the
# absence of a failure can tell a skip from a pass - only the presence of
# assertions can.
#
# REFUSAL vs CRASH: every refusal below exits non-zero WITH an `::error::`
# annotation. A non-zero exit without one is a crash.

set -uo pipefail

if [ "$#" -lt 1 ]; then
  echo "::error::no fixture given - usage: run_envelope_fixture.sh <fixture> [<fixture> ...]"
  exit 1
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../../.." && pwd)"
UNITTEST="${DUCKLAKE_UNITTEST:-${ROOT}/build/release/test/unittest}"

if [ ! -x "${UNITTEST}" ]; then
  echo "::error::no unittest binary at ${UNITTEST} - run 'make release' first"
  exit 1
fi

for fixture in "$@"; do
  if [ ! -f "${ROOT}/${fixture}" ]; then
    echo "::error::${fixture} does not exist - a runner over a fixture that is gone proves nothing"
    exit 1
  fi
done

# Short, because sun_path is 108 bytes and a longer path is silently TRUNCATED.
SOCKET_DIR="$(mktemp -d /tmp/dlkmsf.XXXXXX)"
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
# fails with "cannot reach the key service", which reads like the thing under
# test rather than a startup race.
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

overall=0
for fixture in "$@"; do
  echo "== ${fixture}"
  status=0
  output="$("${UNITTEST}" "${fixture}" 2>&1)" || status=$?
  echo "${output}"
  if ! grep -q "assertions" <<< "${output}"; then
    echo "::error file=${fixture}::reported no assertions - it either skipped or died. A skip is not a pass."
    overall=1
    continue
  fi
  if [ "${status}" -ne 0 ]; then
    echo "::error file=${fixture}::FAILED with a live key service behind it"
    overall=1
  fi
done

echo "--- fake kms operation log ---"
cat "${OPLOG_PATH}"
echo "--- end operation log ---"
exit "${overall}"
