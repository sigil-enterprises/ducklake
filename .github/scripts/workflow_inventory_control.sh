#!/usr/bin/env bash
#
# PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.
#
# The positive control for workflow_inventory.py.
#
# That script asserts an ABSENCE - that no advertised workflow is a ghost. An
# absence assertion that is silently broken prints what a healthy one prints:
# "all N active workflows have a file", exit 0. So it has to be SEEN to report a
# planted ghost before its clean run counts for anything, and seen to stay GREEN
# on a clean fixture so "reds on everything" cannot pass as a working gate.
#
# It invokes the REAL script, never a copy of its logic.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
subject="${here}/workflow_inventory.py"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

failures=0

expect() {
  local name="$1" want="$2" json="$3" paths="$4"
  local out status
  set +e
  out="$(python3 "${subject}" "${json}" "${paths}" 2>&1)"
  status=$?
  set -e
  if [ "${want}" = "green" ]; then
    if [ "${status}" -ne 0 ]; then
      echo "::error::control ${name}: expected a clean inventory, got exit ${status}"
      printf '%s\n' "${out}" | sed 's/^/    /'
      failures=$((failures + 1))
      return
    fi
  else
    if [ "${status}" -eq 0 ]; then
      echo "::error::control ${name}: the subject reported a CLEAN inventory on a fixture built to contain a ghost. It cannot tell an advertised-but-absent workflow from a present one."
      failures=$((failures + 1))
      return
    fi
    # A refusal is exit non-zero WITH an annotation; a crash is exit non-zero
    # without one.
    if ! printf '%s\n' "${out}" | grep -q '^::error::'; then
      echo "::error::control ${name}: exit ${status} with no ::error:: annotation - that is a crash, not a refusal"
      printf '%s\n' "${out}" | sed 's/^/    /'
      failures=$((failures + 1))
      return
    fi
  fi
  echo "  control ${name}: ${want} as required"
}

printf '%s\n' '.github/workflows/Real.yml' > "${work}/paths"
: > "${work}/paths-empty"

cat > "${work}/clean.json" <<'JSON'
{"workflows": [
  {"name": "Real", "path": ".github/workflows/Real.yml", "state": "active"},
  {"name": "Retired", "path": ".github/workflows/Gone.yml", "state": "disabled_manually"}
]}
JSON

cat > "${work}/ghost.json" <<'JSON'
{"workflows": [
  {"name": "Real", "path": ".github/workflows/Real.yml", "state": "active"},
  {"name": "Crypta Refusal Tests", "path": ".github/workflows/Ghost.yml", "state": "active"}
]}
JSON

cat > "${work}/none-active.json" <<'JSON'
{"workflows": [
  {"name": "Retired", "path": ".github/workflows/Gone.yml", "state": "disabled_manually"}
]}
JSON

printf '{"workflows": []}\n' > "${work}/empty.json"

expect clean-inventory     green "${work}/clean.json"       "${work}/paths"
# The load-bearing one: an ACTIVE workflow whose file is on no ref.
expect planted-ghost       red   "${work}/ghost.json"        "${work}/paths"
# A disabled record must NOT be reported - it is exactly the disposition this
# check asks for, so reporting it would make the fix impossible to apply.
expect disabled-not-a-ghost green "${work}/clean.json"       "${work}/paths"
# Vacuity: an empty API response, or one with nothing active, would let this
# pass by having nothing to ask.
expect empty-api           red   "${work}/empty.json"        "${work}/paths"
expect nothing-active      red   "${work}/none-active.json"  "${work}/paths"
# And a failed git side, which would otherwise report every workflow a ghost.
expect empty-path-list     red   "${work}/clean.json"        "${work}/paths-empty"

if [ "${failures}" -ne 0 ]; then
  echo "::error::${failures} positive-control case(s) failed - the workflow inventory check is not trustworthy"
  exit 1
fi
echo "all positive-control cases behaved as required"
