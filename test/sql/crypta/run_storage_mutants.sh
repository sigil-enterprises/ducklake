#!/usr/bin/env bash
#
# PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.
#
# The red-first evidence for the crypta guards that live in FULL-EXTENSION files
# - the ones test/cpp/crypta/run_crypta_tests.sh structurally cannot reach (#45).
#
# Run inside the devcontainer, after a normal build:
#
#   docker compose run --rm --entrypoint bash app -lc \
#     'BUILD_EXTENSION_TEST_DEPS=full make release'
#   docker compose run --rm --entrypoint bash app -lc \
#     'test/sql/crypta/run_storage_mutants.sh'
#
# WHY THIS IS A SECOND RUNNER AND NOT A FLAG ON THE FIRST
# -------------------------------------------------------
# The roster is one file and one vocabulary (test/cpp/crypta/mutants.py); what
# differs is the BUILD, and it differs by two orders of magnitude.
#
#   run_crypta_tests.sh --mutants   the STANDALONE roster - `python3
#                                   test/cpp/crypta/mutants.py names`. Each is a
#                                   compile of two files against a copy of
#                                   src/crypta. Seconds per mutant. Cheap enough
#                                   for the per-PR gate, which is where it runs.
#
#   this                            the EXTENSION roster - `python3
#                                   test/cpp/crypta/mutants.py names --extension`.
#                                   Each is an in-tree edit to a file the whole
#                                   extension is compiled from, so each one costs
#                                   a full `make release` - and TWO of them,
#                                   because the tree is restored and rebuilt
#                                   afterwards so the next mutant's clean control
#                                   is measured against a clean binary. 2N + 1
#                                   builds per run.
#
# NEITHER COUNT IS WRITTEN HERE, and that is the fix rather than the omission.
# This header said "35 standalone" while the roster held 40, and "4 in-tree"
# while it held 5, and then 6 - a number restated in a comment drifts every time
# the roster grows and nothing reds when it does. The runner PRINTS the derived
# count on every run ("All N full-extension guards were removed"), so the honest
# place to read it is the output or the roster, never prose. `.github/workflows/
# StorageMutants.yml` refuses a literal count in either file for the same reason.
#
# Bolting these onto the per-PR gate would multiply every pull request by 2N + 1
# extension builds to re-prove guards that only change when someone edits them.
# So this has its own runner, its own workflow, and its own cadence: scheduled
# and on demand, plus on any push that TOUCHES one of the files the roster edits.
# That last part is what stops the separation becoming an escape hatch - the run
# fires exactly when its subject moves.
#
# WHAT IT EDITS, AND HOW THAT IS MADE SAFE
# -----------------------------------------
# Unlike the standalone harness, this one CANNOT work on a copy: the guard lives
# in a file the extension is built from, and there is no -DSRC_DIR to point
# somewhere else. So it edits the tree. Three things keep that honest:
#
#   - `unpatch` is the REVERSE substitution under the same exactly-once guard, so
#     restoring a file that is not in exactly the mutated state is a loud error
#     rather than a plausible-looking overwrite.
#   - a trap unpatches on any exit, including an interrupt.
#   - `mutants.py verify-clean` re-reads the SOURCE and requires every mutant's
#     text to be present, exactly once, before the run starts, after each mutant,
#     and again before anything is reported as proven. It reads the source rather
#     than asking `git diff` because this runs inside the devcontainer, where a
#     git-WORKTREE checkout's `.git` file points at an absolute host path that
#     does not exist in the container and git answers "not a git repository" - a
#     check that errors is a check that proves nothing.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../../.." && pwd)"
MUTANTS="${ROOT}/test/cpp/crypta/mutants.py"
UNITTEST="${DUCKLAKE_UNITTEST:-${ROOT}/build/release/test/unittest}"
LOG_DIR="${ROOT}/build/storage-mutants"

if [ ! -x "${UNITTEST}" ]; then
  echo "no unittest binary at ${UNITTEST} - run 'BUILD_EXTENSION_TEST_DEPS=full make release' first" >&2
  exit 1
fi

mkdir -p "${LOG_DIR}"

# ---------------------------------------------------------------------------
# The tree must carry no applied mutant, and must still carry none at the end.
# ---------------------------------------------------------------------------
mutated_files="$(python3 "${MUTANTS}" files --extension)"
if [ -z "${mutated_files}" ]; then
  echo "the extension roster names no files - refusing to report anything as proven" >&2
  exit 1
fi

assert_tree_restored() {
  local when="$1"
  local problems
  if problems="$(python3 "${MUTANTS}" verify-clean "${ROOT}")"; then
    return 0
  fi
  echo "TREE NOT CLEAN ${when}:" >&2
  echo "${problems}" >&2
  echo "  A mutant is still applied, or a local edit is in the way. Restore with" >&2
  echo "  'git checkout --' on the roster's files before trusting anything here:" >&2
  echo "${mutated_files}" | sed 's/^/    /' >&2
  return 1
}

assert_tree_restored "before the run" || exit 1

# ---------------------------------------------------------------------------
# The fake key service. Two of the three files here carry `require-env
# DUCKLAKE_FAKE_CRYPTA_SOCKET`, and a require-env file with no env SKIPS - which
# exits ZERO and prints no assertions. Without a socket the clean control would
# pass vacuously and every mutant would look like a survivor.
#
# Same fake, same reasoning, as run_sql_crypta_tests.sh; see that file for what
# it does and does not do.
# ---------------------------------------------------------------------------
SOCKET_DIR="$(mktemp -d /tmp/dlstoremut.XXXXXX)"
SOCKET_PATH="${SOCKET_DIR}/s"
FAKE_LOG="${SOCKET_DIR}/fake.log"
OPLOG_PATH="${SOCKET_DIR}/ops.csv"
: > "${OPLOG_PATH}"
export DUCKLAKE_FAKE_CRYPTA_OPLOG="${OPLOG_PATH}"

APPLIED_MUTANT=""

cleanup() {
  local status=$?
  # The mutant comes off FIRST and unconditionally. An interrupted run that left
  # a guard deleted in the tree is the one failure mode of an in-tree harness,
  # and it is the one a human would most easily commit by accident.
  if [ -n "${APPLIED_MUTANT}" ]; then
    echo "restoring ${APPLIED_MUTANT} on the way out" >&2
    python3 "${MUTANTS}" unpatch "${APPLIED_MUTANT}" "${ROOT}" > /dev/null || \
      echo "  FAILED to reverse ${APPLIED_MUTANT} - 'git checkout --' the roster files NOW" >&2
    APPLIED_MUTANT=""
  fi
  if [ -n "${FAKE_PID:-}" ] && kill -0 "${FAKE_PID}" 2>/dev/null; then
    kill "${FAKE_PID}" 2>/dev/null || true
    wait "${FAKE_PID}" 2>/dev/null || true
  fi
  rm -rf "${SOCKET_DIR}"
  exit "${status}"
}
trap cleanup EXIT

python3 "${HERE}/fake_crypta.py" "${SOCKET_PATH}" > "${FAKE_LOG}" 2>&1 &
FAKE_PID=$!

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

rebuild() {
  local label="$1"
  local log="${LOG_DIR}/build-${label}.log"
  if ! ( cd "${ROOT}" && BUILD_EXTENSION_TEST_DEPS=full make release ) > "${log}" 2>&1; then
    echo "  build FAILED (${label}) - tail of ${log}:" >&2
    tail -n 30 "${log}" >&2
    return 1
  fi
  return 0
}

# Run ONE case and answer three questions about it, not one: did it run at all,
# did it pass, and what did it say.
#
# "Did it run at all" is separate from "did it pass" on purpose. A skipped
# sqllogictest file prints "All tests were skipped" and EXITS ZERO, so neither
# the status nor the absence of a failure can tell a skip from a pass - only the
# word "assertions" can. This is the same control run_sql_crypta_tests.sh applies
# to the whole group, applied here per case because here a vacuous green would be
# read as evidence about a GUARD.
run_case() {
  local case_name="$1"
  local out_file="$2"
  local status=0
  "${UNITTEST}" "${case_name}" > "${out_file}" 2>&1 || status=$?
  if ! grep -q "assertions" "${out_file}"; then
    echo "__NOASSERTIONS__"
    return 0
  fi
  echo "${status}"
  return 0
}

echo "=== full-extension red-first evidence: one guard removed at a time ==="
echo "    unittest: ${UNITTEST}"
echo

roster="$(python3 "${MUTANTS}" names --extension)"
expected_mutants="$(grep -c . <<< "${roster}" || true)"
if [ "${expected_mutants}" -eq 0 ]; then
  echo "the extension roster is empty - refusing to report anything as proven" >&2
  exit 1
fi

failures=0
ran=0
survivors=()

while read -r mutant; do
  [ -n "${mutant}" ] || continue
  ran=$((ran + 1))
  spec="$(python3 "${MUTANTS}" spec "${mutant}")"
  expected_cases="$(python3 "${MUTANTS}" count "${mutant}")"

  # Control 1: the spec must name EXACTLY the cases the mutant claims, and all of
  # them. A spec matching nothing lists nothing, runs nothing, and exits ZERO -
  # which reads as "the mutant did not redden" when in fact nothing ran.
  matched="$("${UNITTEST}" "${spec}" --list-tests | tail -n +2 | grep -c . || true)"
  if [ "${matched}" -ne "${expected_cases}" ]; then
    echo "  ERROR  ${mutant}: its 'reddens' spec matches ${matched} of the ${expected_cases} cases it names"
    failures=$((failures + 1))
    continue
  fi

  # Read the case list into a VARIABLE, and check its length, before looping.
  #
  # Not `done < <(python3 ... cases ...)`. Process substitution puts the
  # generator's exit status somewhere neither `set -e` nor `pipefail` can see it,
  # so a `mutants.py` that crashed or printed nothing would leave BOTH loops below
  # with an empty body: the clean control would find nothing wrong, the mutated
  # loop would find nothing surviving, and this script would print RED having run
  # NOT ONE case. The standalone runner learned exactly this on its roster loop;
  # the per-case loops here are the same hazard one level down.
  cases="$(python3 "${MUTANTS}" cases "${mutant}")"
  case_lines="$(grep -c . <<< "${cases}" || true)"
  if [ "${case_lines}" -ne "${expected_cases}" ]; then
    echo "  ERROR  ${mutant}: its case list has ${case_lines} line(s) for ${expected_cases} case(s) - the roster generator failed"
    failures=$((failures + 1))
    continue
  fi

  # Control 2, PER CASE: green before the guard is removed. Per case and not on
  # the combined spec, because a combined status hides which half of a two-case
  # roster was actually carrying it - the blind spot the standalone roster
  # documents at cache_key_unprefixed_join, in the runner that could have caught
  # it.
  clean_ok=1
  checked=0
  while IFS=$'\t' read -r case_name marker; do
    [ -n "${case_name}" ] || continue
    checked=$((checked + 1))
    status="$(run_case "${case_name}" "${LOG_DIR}/clean-${mutant}-$(basename "${case_name}").log")"
    if [ "${status}" = "__NOASSERTIONS__" ]; then
      echo "  ERROR  ${mutant}: ${case_name} reported NO assertions unmutated - it skipped or died, and a skip is not a pass"
      clean_ok=0
    elif [ "${status}" != "0" ]; then
      echo "  ERROR  ${mutant}: ${case_name} is already failing unmutated (rc ${status})"
      clean_ok=0
    fi
  done <<< "${cases}"
  if [ "${checked}" -ne "${expected_cases}" ]; then
    echo "  ERROR  ${mutant}: the clean control ran ${checked} of ${expected_cases} case(s)"
    clean_ok=0
  fi
  if [ "${clean_ok}" -ne 1 ]; then
    failures=$((failures + 1))
    continue
  fi

  python3 "${MUTANTS}" patch "${mutant}" "${ROOT}" > /dev/null
  APPLIED_MUTANT="${mutant}"
  if ! rebuild "${mutant}"; then
    echo "  ERROR  ${mutant}: the mutated extension does not compile"
    failures=$((failures + 1))
    python3 "${MUTANTS}" unpatch "${mutant}" "${ROOT}" > /dev/null
    APPLIED_MUTANT=""
    rebuild "restore-${mutant}" || exit 1
    continue
  fi

  # The red, PER CASE, and with the failing statement named.
  #
  # A non-zero exit alone is not enough at this grain. A sqllogictest file is far
  # bigger than a Catch case: it carries fixtures, ATTACHes, controls. If it died
  # on any of those - a fixture that stopped building, a control that broke for an
  # unrelated reason - the file would still exit non-zero and be counted as
  # evidence for a guard it never reached. So the mutant also names the statement
  # it is supposed to flip, and that statement's own text must appear in the
  # failure output. Its TEXT, never its line number: a line-number assertion
  # retargets silently the moment anything above it moves.
  reddened=1
  verdicts=()
  checked=0
  while IFS=$'\t' read -r case_name marker; do
    [ -n "${case_name}" ] || continue
    checked=$((checked + 1))
    log="${LOG_DIR}/mutated-${mutant}-$(basename "${case_name}").log"
    status="$(run_case "${case_name}" "${log}")"
    if [ "${status}" = "__NOASSERTIONS__" ]; then
      echo "  SURVIVED ${mutant}: ${case_name} reported NO assertions with the guard removed"
      reddened=0
    elif [ "${status}" = "0" ]; then
      echo "  SURVIVED ${mutant}: ${case_name} still passes without the guard"
      reddened=0
    elif [ -n "${marker}" ] && ! grep -qF -- "${marker}" "${log}"; then
      echo "  WRONG-RED ${mutant}: ${case_name} failed, but not at the statement it names"
      echo "            expected the failure to quote: ${marker}"
      echo "            see ${log}"
      reddened=0
    else
      # The measurement, printed rather than merely acted on. A reader should be
      # able to take the numbers off this transcript without re-deriving them.
      verdicts+=("clean rc 0 -> mutated rc ${status}  ${case_name}")
    fi
  done <<< "${cases}"

  # The mutated loop must have walked the whole list too. Without this a list that
  # emptied between the clean control and here would leave `reddened` at its
  # initial 1 and report RED for a mutant no case was run against.
  if [ "${checked}" -ne "${expected_cases}" ]; then
    echo "  ERROR  ${mutant}: the mutated run covered ${checked} of ${expected_cases} case(s)"
    reddened=0
  fi

  if [ "${reddened}" -eq 1 ]; then
    echo "  RED      ${mutant}: ${matched} case(s) fail without the guard, each at the statement it names"
    for verdict in "${verdicts[@]}"; do
      echo "             ${verdict}"
    done
  else
    survivors+=("${mutant}")
    failures=$((failures + 1))
  fi

  python3 "${MUTANTS}" unpatch "${mutant}" "${ROOT}" > /dev/null
  APPLIED_MUTANT=""
  assert_tree_restored "after ${mutant}" || exit 1
  rebuild "restore-${mutant}" || exit 1
done <<< "${roster}"

echo
assert_tree_restored "at the end of the run" || exit 1

if [ "${ran}" -ne "${expected_mutants}" ]; then
  echo "only ${ran} of ${expected_mutants} mutants ran - nothing here is proven" >&2
  exit 1
fi
if [ "${failures}" -ne 0 ]; then
  echo "${failures} mutant(s) did not produce the red they must."
  if [ "${#survivors[@]}" -ne 0 ]; then
    echo "Survivors (the cases naming them are NOT proven): ${survivors[*]}"
  fi
  exit 1
fi
echo "All ${ran} full-extension guards were removed and every case naming one went"
echo "red at the statement it names. These greens are earned."
