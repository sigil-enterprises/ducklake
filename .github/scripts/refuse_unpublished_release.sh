#!/usr/bin/env bash
#
# REFUSE a publish that published nothing (ducklake#37).
#
# The defect this replaces: three `Deploy extension binaries / Deploy to nightly`
# jobs concluded `success` on every run while uploading nothing, because the S3
# credentials they need are not set on this repository. The last line the deploy
# script printed was "Missing AWS credentials: AWS_ACCESS_KEY_ID and
# AWS_SECRET_ACCESS_KEY are required", and the check went green anyway. Three
# green checks per run asserted a deployment that did not happen, which is worse
# than three red ones, because red gets investigated.
#
# The rule: A CHECK THAT CANNOT PERFORM ITS FUNCTION MUST NOT REPORT SUCCESS.
#
# A REFUSAL is exit non-zero WITH an `::error::` annotation. Exit non-zero
# WITHOUT one is a CRASH, and a crash is indistinguishable from this script
# falling over - it says nothing about the release. So every exit path below
# that is not a success annotates first.
#
# Usage:
#   refuse_unpublished_release.sh REPO TAG EXPECTED_ASSET [EXPECTED_ASSET...]
#
# Asset names are read back from the GitHub API - from the RELEASE, not from the
# upload step's exit code. An upload that reported success and attached nothing
# is exactly the failure mode being closed, so its own word is not evidence.
#
# ASSET_LIST_OVERRIDE (newline-separated, possibly empty) substitutes for the
# API call. It exists for --selftest, which must be able to present a release
# with no assets without creating one.
set -uo pipefail

annotate() { printf '::error::%s\n' "$*"; }

main() {
  local repo="$1" tag="$2"; shift 2
  local expected=("$@")
  local actual=()

  if [ "${ASSET_LIST_OVERRIDE+set}" = set ]; then
    # `while read` over a here-string, not `$(...)` into an array: command
    # substitution strips trailing newlines, and `grep -c .` counts non-empty
    # lines only, so both silently disagree with the API about an empty list.
    local line
    while IFS= read -r line; do
      [ -n "$line" ] && actual+=("$line")
    done <<< "${ASSET_LIST_OVERRIDE}"
  else
    local raw
    if ! raw="$(gh api "repos/${repo}/releases/tags/${tag}" --jq '.assets[].name' 2>&1)"; then
      annotate "cannot read the assets of ${repo}@${tag}: ${raw}. Refusing to report a publish that cannot be confirmed."
      return 1
    fi
    local line
    while IFS= read -r line; do
      [ -n "$line" ] && actual+=("$line")
    done <<< "${raw}"
  fi

  echo "release ${repo}@${tag} carries ${#actual[@]} asset(s):"
  if [ "${#actual[@]}" -eq 0 ]; then
    echo "  (none)"
  else
    printf '  %s\n' "${actual[@]}"
  fi

  if [ "${#actual[@]}" -eq 0 ]; then
    annotate "REFUSING: ${repo}@${tag} has NO assets. The publish step reported success and attached nothing - that is the exact failure ducklake#37 records. Not reporting green for a deploy that did not deploy."
    return 1
  fi

  local missing=0 want found a
  for want in "${expected[@]}"; do
    found=0
    for a in "${actual[@]}"; do
      [ "$a" = "$want" ] && found=1 && break
    done
    if [ "$found" -eq 1 ]; then
      echo "  OK       ${want}"
    else
      echo "  MISSING  ${want}"
      missing=1
    fi
  done

  if [ "$missing" -ne 0 ]; then
    annotate "REFUSING: ${repo}@${tag} is missing at least one asset this publish claimed to attach. A release that does not carry the platform binary is a release no consumer can install (ducklake#33)."
    return 1
  fi

  # ducklake#33: "An un-suffixed asset should not exist at all - a DuckDB
  # extension without its platform in the name is indistinguishable from the
  # eight others." v0.2.0-rc.2 shipped `ducklake.duckdb_extension`, which read as
  # "the extension" and was in fact a Windows mingw DLL. So the absence is
  # asserted, not merely the presence.
  local forbidden=0 bad
  if [ -n "${FORBIDDEN_ASSET_NAMES:-}" ]; then
    while IFS= read -r bad; do
      [ -n "$bad" ] || continue
      for a in "${actual[@]}"; do
        if [ "$a" = "$bad" ]; then
          echo "  FORBIDDEN ${bad}"
          forbidden=1
        fi
      done
    done <<< "${FORBIDDEN_ASSET_NAMES}"
  fi
  if [ "$forbidden" -ne 0 ]; then
    annotate "REFUSING: ${repo}@${tag} carries a platform-less asset name. A .duckdb_extension whose name does not say which of the nine platforms it is cannot be told apart from the other eight (ducklake#33)."
    return 1
  fi

  echo "every expected asset is attached to the release."
  return 0
}

# --------------------------------------------------------------- self-test --
#
# POSITIVE CONTROL. This script's whole job is to say no; a script that says no
# has never been shown to work until it has been shown to say no. Each case
# asserts BOTH halves of a refusal - the non-zero exit AND the annotation -
# because the exit code alone cannot tell a refusal from a crash.
selftest() {
  local fails=0 out st
  local tmp; tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' RETURN

  _case() { # want_status label assets...
    local want="$1" label="$2" assets="$3"
    ASSET_LIST_OVERRIDE="$assets" main fake/repo vX ducklake-vX-linux_amd64.duckdb_extension \
      > "$tmp/o" 2>&1
    st=$?
    sed 's/^::error::/    [expected annotation] /' "$tmp/o" > "$tmp/o2"; mv "$tmp/o2" "$tmp/o"
    if [ "$st" -ne "$want" ]; then
      printf 'FAIL  %s: expected exit %s, got %s\n' "$label" "$want" "$st"
      sed 's/^/      | /' "$tmp/o"; fails=$((fails + 1)); return
    fi
    if [ "$want" -ne 0 ] && ! grep -q '\[expected annotation\]' "$tmp/o"; then
      printf 'FAIL  %s: exit %s but NO ::error:: annotation - a crash, not a refusal\n' "$label" "$st"
      sed 's/^/      | /' "$tmp/o"; fails=$((fails + 1)); return
    fi
    printf 'PASS  %s (exit %s%s)\n' "$label" "$st" \
      "$([ "$want" -ne 0 ] && echo ', ::error:: annotation emitted')"
  }

  # The case from the issue: the deploy ran, published nothing, and used to go
  # green. It must now refuse.
  _case 1 "release with NO assets" ""
  # Published something, but not the platform binary #33 is about.
  _case 1 "release missing the expected asset" "ducklake.duckdb_extension.wasm"
  # The un-suffixed name #33 calls out: it is not the expected asset and must
  # not be accepted as a stand-in for it.
  _case 1 "un-suffixed asset only" "ducklake.duckdb_extension"
  # And the green must be reachable, or this is a check that fails on
  # everything, which carries no more signal than one that passes on everything.
  _case 0 "release carrying the expected asset" \
    "ducklake-vX-linux_amd64.duckdb_extension"$'\n'"ducklake-vX-linux_amd64.duckdb_extension.wasm"
  # The expected asset is present AND a platform-less one is too. #33 says that
  # second one must not exist, so a present-only check would pass this and be
  # wrong.
  FORBIDDEN_ASSET_NAMES='ducklake.duckdb_extension' \
    _case 1 "expected asset present but a platform-less one beside it" \
      "ducklake-vX-linux_amd64.duckdb_extension"$'\n'"ducklake.duckdb_extension"
  # ... and the forbidden list must not fire when the name is absent, or it is a
  # check that reds every release regardless.
  FORBIDDEN_ASSET_NAMES='ducklake.duckdb_extension' \
    _case 0 "forbidden name absent" "ducklake-vX-linux_amd64.duckdb_extension"

  echo
  if [ "$fails" -ne 0 ]; then
    printf '::error::refuse-unpublished self-test: %s case(s) failed\n' "$fails"
    return 1
  fi
  echo "refuse-unpublished self-test: all cases passed"
  return 0
}

if [ "${1:-}" = "--selftest" ]; then
  selftest
  exit $?
fi

if [ "$#" -lt 3 ]; then
  annotate "usage: $0 REPO TAG EXPECTED_ASSET [EXPECTED_ASSET...] (or --selftest)"
  exit 2
fi
main "$@"
exit $?
