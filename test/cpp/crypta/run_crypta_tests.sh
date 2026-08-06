#!/usr/bin/env bash
#
# PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.
#
# Build and run the crypta refusal suite, and - with --mutants - the red-first
# evidence for it.
#
# Run inside the devcontainer, after a normal build has produced libduckdb:
#
#   docker compose run --rm --entrypoint bash app -lc \
#     'BUILD_EXTENSION_TEST_DEPS=full make release'
#   docker compose run --rm --entrypoint bash app -lc \
#     'test/cpp/crypta/run_crypta_tests.sh --mutants'
#
# What --mutants does, and why it is the point rather than an extra:
#
#   Every case in this suite asserts a REFUSAL. A refusal test that never runs
#   and one that passes are indistinguishable on the console - both are green.
#   So for each guard in src/crypta/, this rebuilds the client with THAT GUARD
#   DELETED and requires the cases naming it to go RED. A case that cannot be
#   made to fail has not been tested, and this script says so by name instead of
#   counting it.
#
# It never touches src/crypta/: each mutant is applied to a throwaway copy and
# handed to the standalone test project through -DCRYPTA_SRC_DIR.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../../.." && pwd)"
BUILD_DIR="${ROOT}/build/crypta-test"
MUTANT_ROOT="${ROOT}/build/crypta-mutants"
DUCKDB_BUILD_DIR="${DUCKDB_BUILD_DIR:-${ROOT}/build/release}"
RUN_MUTANTS=0

for argument in "$@"; do
  case "${argument}" in
    --mutants) RUN_MUTANTS=1 ;;
    *) echo "usage: $0 [--mutants]" >&2; exit 2 ;;
  esac
done

if [ ! -f "${DUCKDB_BUILD_DIR}/src/libduckdb.so" ]; then
  echo "no libduckdb in ${DUCKDB_BUILD_DIR}/src - run 'make release' first" >&2
  exit 1
fi

# TWO source directories since #33, both of them knobs for the same reason: the
# mutant copy has to be able to replace EITHER, or the guard living in the one
# that stayed hardcoded could never be removed. `mutants.py apply` lays a copy of
# both out under these two names (SOURCE_DIRS there), so a mutant build is never
# half copy and half live tree.
configure_and_build() {
  local build_dir="$1"
  local crypta_src="$2"
  local common_src="$3"
  cmake -S "${HERE}" -B "${build_dir}" -GNinja \
    -DDUCKLAKE_ROOT="${ROOT}" \
    -DDUCKDB_BUILD_DIR="${DUCKDB_BUILD_DIR}" \
    -DCRYPTA_SRC_DIR="${crypta_src}" \
    -DDUCKLAKE_COMMON_SRC_DIR="${common_src}" > /dev/null
  cmake --build "${build_dir}" > /dev/null
}

echo "=== building the suite against the real src/crypta and src/common ==="
configure_and_build "${BUILD_DIR}" "${ROOT}/src/crypta" "${ROOT}/src/common"

echo "=== running the suite ==="
"${BUILD_DIR}/crypta_test"

if [ "${RUN_MUTANTS}" -eq 0 ]; then
  echo
  echo "NOTE: the refusal cases above are unproven until --mutants has shown each"
  echo "      of them failing against a client with its guard removed."
  exit 0
fi

echo
echo "=== red-first evidence: one guard removed at a time ==="
mkdir -p "${MUTANT_ROOT}"
failures=0
ran=0
survivors=()

# Read the roster into a variable FIRST, rather than looping over a process
# substitution.
#
# `done < <(python3 ... names)` puts the generator's exit status somewhere
# neither `set -e` nor `pipefail` can see it. If mutants.py failed to import or
# printed nothing, the loop body would never run, `failures` would stay 0, and
# this script would print "the suite's greens are earned" and exit 0 having
# proven exactly nothing - the vacuous green this file exists to prevent, in the
# file that exists to prevent it.
roster="$(python3 "${HERE}/mutants.py" names)"
expected_mutants="$(grep -c . <<< "${roster}" || true)"
if [ "${expected_mutants}" -eq 0 ]; then
  echo "mutants.py produced no mutants - refusing to report anything as proven" >&2
  exit 1
fi

while read -r mutant; do
  [ -n "${mutant}" ] || continue
  ran=$((ran + 1))
  spec="$(python3 "${HERE}/mutants.py" spec "${mutant}")"
  expected_cases="$(python3 "${HERE}/mutants.py" count "${mutant}")"

  # Control 1: the spec must name EXACTLY the cases the mutant claims, and all of
  # them.
  #
  # Counting `> 0` is not enough and the gap is the one that actually happens: in
  # a three-name list, one renamed case leaves two matching, the mutant still
  # reddens off those two, and the third is silently unproven. A spec matching
  # nothing is also invisible on its own - Catch2 runs nothing and exits ZERO.
  matched="$("${BUILD_DIR}/crypta_test" "${spec}" --list-tests | tail -n +2 | grep -c . || true)"
  if [ "${matched}" -ne "${expected_cases}" ]; then
    echo "  ERROR  ${mutant}: its 'reddens' spec matches ${matched} of the ${expected_cases} cases it names"
    failures=$((failures + 1))
    continue
  fi

  # Control 2: those cases must be GREEN before the guard is removed, or the red
  # below proves nothing about this mutant.
  if ! "${BUILD_DIR}/crypta_test" "${spec}" > /dev/null 2>&1; then
    echo "  ERROR  ${mutant}: its cases are already failing unmutated"
    failures=$((failures + 1))
    continue
  fi

  src_dir="${MUTANT_ROOT}/src-${mutant}"
  build_dir="${MUTANT_ROOT}/build-${mutant}"
  python3 "${HERE}/mutants.py" apply "${mutant}" "${src_dir}" > /dev/null
  if ! configure_and_build "${build_dir}" "${src_dir}/crypta" "${src_dir}/common" 2>/dev/null; then
    echo "  ERROR  ${mutant}: the mutated client does not compile"
    failures=$((failures + 1))
    continue
  fi

  # The red. A mutant that exits zero means the guard it removed was not what
  # made the case pass - so the case does not test that guard.
  if "${build_dir}/crypta_test" "${spec}" > "${build_dir}/mutant.log" 2>&1; then
    echo "  SURVIVED ${mutant}: ${matched} case(s) still pass without the guard"
    survivors+=("${mutant}")
    failures=$((failures + 1))
  else
    echo "  RED      ${mutant}: ${matched} case(s) fail without the guard"
  fi
done <<< "${roster}"

echo
# The loop is only evidence if it actually walked the whole roster.
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
echo "All ${ran} guards were removed and every case naming one went red. The"
echo "suite's greens are earned."
