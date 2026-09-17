#!/usr/bin/env bash
#
# ASSERT that the promotion gate is CALLED, and called UPSTREAM of the upload.
#
# The gate script's own self-test proves every check fires. It says nothing
# about whether anything runs it before the irreversible step, and that is the
# whole difference between a gate that BLOCKS a bad promotion and one that only
# ANNOUNCES it after the release is public. Deleting one `- name:` block from
# Release.yml would turn the first into the second silently, with every other
# check in this repository still green.
#
# This is an ordering assertion over a file, so it is absence-shaped and would
# read as a pass if it were broken. `--selftest` plants the two failures it
# exists to catch - the call removed, and the call moved downstream of the
# upload - and requires a refusal on each.
#
# A REFUSAL is exit non-zero WITH an `::error::` annotation.
set -uo pipefail

annotate() { printf '::error::%s\n' "$*"; }

GATE_STEP='REFUSE to attach an asset to a release that was never proven'
UPLOAD_STEP='Attach the asset to the release'

check() {
  local wf="$1"
  if [ ! -r "$wf" ]; then
    annotate "cannot read ${wf}, so whether the promotion gate is wired in at all is UNKNOWN. Refusing rather than reporting an unread file as correctly wired."
    return 1
  fi
  local gate up
  gate="$(grep -n -- "- name: ${GATE_STEP}" "$wf" | head -1 | cut -d: -f1)"
  up="$(grep -n -- "- name: ${UPLOAD_STEP}" "$wf" | head -1 | cut -d: -f1)"
  if [ -z "$up" ]; then
    annotate "${wf} has no '${UPLOAD_STEP}' step, so this check has nothing to order the gate against. Refusing: the step names moved and this assertion has silently stopped testing anything."
    return 1
  fi
  if [ -z "$gate" ]; then
    annotate "REFUSING: ${wf} never calls the promotion gate. A gate nothing calls is a file, not a control - and a promotion gate that runs only on 'release: published' announces a bad promotion after the release is public rather than preventing the asset that makes it usable."
    return 1
  fi
  if [ "$gate" -gt "$up" ]; then
    annotate "REFUSING: the promotion gate (line ${gate}) runs AFTER the upload (line ${up}). An upload is irreversible and a gate downstream of an irreversible step is not a gate: the asset is already on the release when the job goes red, and a red job does not take it off."
    return 1
  fi
  echo "promotion gate called at ${wf}:${gate}, upload at ${wf}:${up} - gate is upstream."
  return 0
}

selftest() {
  local fails=0 tmp; tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' RETURN
  local real=".github/workflows/Release.yml"

  _case() {
    local want="$1" label="$2" reason="$3" file="$4"
    local out st
    out="$(check "$file" 2>&1)"; st=$?
    if [ "$st" -ne "$want" ]; then
      printf 'FAIL  %s: expected exit %s, got %s\n' "$label" "$want" "$st"
      printf '%s\n' "$out" | sed 's/^/      | /'; fails=$((fails + 1)); return
    fi
    if [ "$want" -ne 0 ] && ! printf '%s\n' "$out" | grep -q -- "::error::.*$reason"; then
      printf 'FAIL  %s: refused, but not for the condition under test (%s)\n' "$label" "$reason"
      printf '%s\n' "$out" | sed 's/^/      | /'; fails=$((fails + 1)); return
    fi
    printf 'PASS  %s (exit %s)\n' "$label" "$st"
  }

  # Green must be reachable, and it is reachable only against the real file.
  _case 0 "the real Release.yml calls the gate before the upload" "" "$real"

  # DELETE THE LIMB. The gate step removed - the exact edit that silently turns
  # this repository's promotion gate back into an announcement.
  awk -v n="$GATE_STEP" '
    $0 ~ ("- name: " n) { skip=1; next }
    skip && /^      - /                  { skip=0 }
    !skip                                { print }
  ' "$real" > "$tmp/nogate.yml"
  if grep -q -- "- name: ${GATE_STEP}" "$tmp/nogate.yml"; then
    printf 'FAIL  the limb-deletion fixture did not actually delete the step\n'; fails=$((fails + 1))
  fi
  _case 1 "the gate call removed" "never calls the promotion gate" "$tmp/nogate.yml"

  # Moved DOWNSTREAM of the upload: present, callable, and useless.
  { grep -v -- "- name: ${GATE_STEP}" "$real"; printf '      - name: %s\n' "$GATE_STEP"; } > "$tmp/after.yml"
  _case 1 "the gate call moved after the upload" "runs AFTER the upload" "$tmp/after.yml"

  # A renamed upload step must refuse rather than silently stop ordering.
  grep -v -- "- name: ${UPLOAD_STEP}" "$real" > "$tmp/noup.yml"
  _case 1 "the upload step was renamed" "has no '${UPLOAD_STEP}' step" "$tmp/noup.yml"

  _case 1 "the workflow cannot be read" "cannot read" "$tmp/does-not-exist.yml"

  echo
  if [ "$fails" -ne 0 ]; then
    printf '::error::promotion call-site self-test: %s case(s) failed\n' "$fails"; return 1
  fi
  echo "promotion call-site self-test: all cases passed"
  return 0
}

if [ "${1:-}" = "--selftest" ]; then selftest; exit $?; fi
check "${1:-.github/workflows/Release.yml}"
