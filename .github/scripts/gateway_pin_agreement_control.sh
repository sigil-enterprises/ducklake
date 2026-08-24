#!/usr/bin/env bash
#
# PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.
#
# The positive control for gateway_pin_agreement.sh.
#
# That script asserts an EQUALITY, and an equality check that is silently broken
# reports exactly what a healthy one reports: nothing, exit 0. So before its green
# against the real consumer is worth anything, it has to be SEEN to red on
# deliberately bad fixtures - and to stay green on a deliberately good one, so
# "reds on everything" cannot masquerade as a working gate either.
#
# It invokes the REAL script, never a copy of its logic. A control that
# reimplements what it controls proves the reimplementation.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
subject="${here}/gateway_pin_agreement.sh"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

failures=0

# Each case runs the subject and compares its EXIT and its ::error:: annotation
# against what the case expects. `set -e` would abort on the first expected-red,
# so the invocation is guarded.
expect() {
  local name="$1" want="$2" dockerfile="$3" pinfile="$4"
  local out status
  set +e
  out="$("${subject}" "${dockerfile}" "${pinfile}" 2>&1)"
  status=$?
  set -e
  if [ "${want}" = "green" ] && [ "${status}" -ne 0 ]; then
    echo "::error::control ${name}: expected agreement, got exit ${status}"
    printf '%s\n' "${out}" | sed 's/^/    /'
    failures=$((failures + 1))
    return
  fi
  if [ "${want}" = "red" ]; then
    if [ "${status}" -eq 0 ]; then
      echo "::error::control ${name}: the subject reported AGREEMENT on a fixture built to disagree. It cannot tell a drifted pin from an aligned one."
      failures=$((failures + 1))
      return
    fi
    # A refusal is exit non-zero WITH an annotation; a crash is exit non-zero
    # WITHOUT one. A control that accepted a crash as a red would pass on a
    # script that merely fell over.
    if ! printf '%s\n' "${out}" | grep -q '^::error::'; then
      echo "::error::control ${name}: the subject exited ${status} but printed no ::error:: annotation - that is a crash, not a refusal"
      printf '%s\n' "${out}" | sed 's/^/    /'
      failures=$((failures + 1))
      return
    fi
  fi
  echo "  control ${name}: ${want} as required"
}

printf 'v9.9.9\n' > "${work}/pin"
# No trailing newline: the real .github/duckdb-version carries none, and a
# comparator relying on one would red on the real file.
printf 'v9.9.9' > "${work}/pin-no-newline"
printf '' > "${work}/pin-empty"

printf 'FROM x\nARG DUCKDB_VERSION=9.9.9\nRUN true\n' > "${work}/df-agree"
printf 'FROM x\nARG DUCKDB_VERSION=1.2.3\nRUN true\n' > "${work}/df-drift"
printf 'FROM x\nRUN true\n' > "${work}/df-absent"
printf 'FROM x\nARG DUCKDB_VERSION=9.9.9\nARG DUCKDB_VERSION=1.2.3\n' > "${work}/df-twice"
printf 'FROM x\nARG DUCKDB_VERSION=\n' > "${work}/df-empty-arg"

expect agreeing-pins            green "${work}/df-agree"      "${work}/pin"
expect agreeing-pins-no-newline green "${work}/df-agree"       "${work}/pin-no-newline"
expect drifted-consumer-pin     red   "${work}/df-drift"       "${work}/pin"
expect consumer-arg-absent      red   "${work}/df-absent"      "${work}/pin"
expect consumer-arg-twice       red   "${work}/df-twice"       "${work}/pin"
expect consumer-arg-empty       red   "${work}/df-empty-arg"   "${work}/pin"
expect consumer-unreadable      red   "${work}/df-no-such"     "${work}/pin"
expect repo-pin-empty           red   "${work}/df-agree"       "${work}/pin-empty"

if [ "${failures}" -ne 0 ]; then
  echo "::error::${failures} positive-control case(s) failed - the pin-agreement check is not trustworthy and its verdict against the real consumer means nothing"
  exit 1
fi
echo "all positive-control cases behaved as required"
