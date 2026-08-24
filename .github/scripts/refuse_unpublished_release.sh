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
    # stderr goes to its own file, never into the list. `2>&1` here would let
    # any banner gh writes - a deprecation notice, an update nag - become an
    # entry in `actual` and be printed as an attached asset, and in principle
    # satisfy an expected match.
    local raw errf; errf="$(mktemp)"
    if ! raw="$(gh api "repos/${repo}/releases/tags/${tag}" --jq '.assets[].name' 2>"$errf")"; then
      annotate "cannot read the assets of ${repo}@${tag}: $(tr '\n' ' ' < "$errf"). Refusing to report a publish that cannot be confirmed."
      rm -f "$errf"
      return 1
    fi
    rm -f "$errf"
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
  # eight others." v0.2.0-rc.2 shipped `ducklake.duckdb_extension`, which read
  # as "the extension" and was in fact a Windows mingw DLL, AND
  # `ducklake.duckdb_extension.wasm`, which is just as platform-less. So this is
  # a RULE over the class, not a list of the two names that happen to exist
  # today: any asset that is a duckdb extension and names none of the platforms
  # duckdb builds for is refused.
  local forbidden=0 a
  if [ "${REQUIRE_PLATFORM_IN_NAME:-0}" = "1" ]; then
    local platforms="linux_amd64 linux_arm64 linux_amd64_musl osx_amd64 osx_arm64 windows_amd64 windows_amd64_mingw windows_arm64 wasm_eh wasm_mvp wasm_threads"
    for a in "${actual[@]}"; do
      case "$a" in
        *.duckdb_extension*) ;;
        *) continue ;;
      esac
      local named=0 plat
      for plat in $platforms; do
        case "$a" in *"$plat"*) named=1; break ;; esac
      done
      if [ "$named" -eq 0 ]; then
        echo "  PLATFORM-LESS  ${a}"
        forbidden=1
      fi
    done
  fi
  if [ "$forbidden" -ne 0 ]; then
    annotate "REFUSING: ${repo}@${tag} carries a .duckdb_extension asset whose name says none of the nine platforms duckdb builds for. Such an asset cannot be told apart from the other eight and reads as 'the extension' (ducklake#33)."
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

  # want_status label assets [annotation_substring]
  #
  # The annotation substring is not decorative. This script has four distinct
  # refusal paths and "it refused" is satisfied by all four, so a case could
  # pass while refusing for a reason that has nothing to do with what the case
  # is about - which is the same defect class as a check that cannot tell a
  # healthy system from a dead one.
  _case() {
    local want="$1" label="$2" assets="$3" reason="${4:-}"
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
    if [ -n "$reason" ] && ! grep -q -- "$reason" "$tmp/o"; then
      printf 'FAIL  %s: refused, but not for the reason under test - no annotation naming %s\n' "$label" "$reason"
      sed 's/^/      | /' "$tmp/o"; fails=$((fails + 1)); return
    fi
    printf 'PASS  %s (exit %s%s)\n' "$label" "$st" \
      "$([ "$want" -ne 0 ] && echo ", ::error:: names '$reason'")"
  }

  # The case from the issue: the deploy ran, published nothing, and used to go
  # green. It must now refuse.
  _case 1 "release with NO assets" "" "has NO assets"
  # Published something, but not the platform binary #33 is about.
  _case 1 "release missing the expected asset" "ducklake.duckdb_extension.wasm" "missing at least one asset"
  # The un-suffixed name #33 calls out: it is not the expected asset and must
  # not be accepted as a stand-in for it.
  _case 1 "un-suffixed asset only" "ducklake.duckdb_extension" "missing at least one asset"
  # And the green must be reachable, or this is a check that fails on
  # everything, which carries no more signal than one that passes on everything.
  _case 0 "release carrying the expected asset" \
    "ducklake-vX-linux_amd64.duckdb_extension"$'\n'"ducklake-vX-linux_amd64.duckdb_extension.wasm"
  # The expected asset is present AND a platform-less one is too. #33 says that
  # second one must not exist, so a present-only check would pass this and be
  # wrong.
  # The expected asset is present AND a platform-less one is beside it. #33 says
  # that second one must not exist, so a presence-only check would pass this and
  # be wrong.
  REQUIRE_PLATFORM_IN_NAME=1 \
    _case 1 "expected asset present but a platform-less one beside it" \
      "ducklake-vX-linux_amd64.duckdb_extension"$'\n'"ducklake.duckdb_extension" \
      "says none of the nine platforms"
  # The .wasm variant on v0.2.0-rc.2 is equally platform-less; a rule that only
  # knew the one literal name would miss it, which is why this is a rule.
  REQUIRE_PLATFORM_IN_NAME=1 \
    _case 1 "platform-less .wasm asset beside the expected one" \
      "ducklake-vX-linux_amd64.duckdb_extension"$'\n'"ducklake.duckdb_extension.wasm" \
      "says none of the nine platforms"
  # ... and it must NOT fire on a properly named set, or it is a check that reds
  # every release regardless.
  REQUIRE_PLATFORM_IN_NAME=1 \
    _case 0 "every asset names its platform" \
      "ducklake-vX-linux_amd64.duckdb_extension"$'\n'"ducklake-vX-wasm_eh.duckdb_extension.wasm"$'\n'"README.md"

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
