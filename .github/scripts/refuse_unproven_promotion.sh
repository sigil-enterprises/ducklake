#!/usr/bin/env bash
#
# REFUSE promoting a release candidate that was never proven (ducklake#33, #37).
#
# The defect this closes: promotion from a release-candidate to a final release
# is a human running `gh release create`, and nothing between that command and a
# published final release checks anything at all. Measured on v0.2.0-rc.5, every
# one of these was true at once and nothing reported it:
#
#   - the tag points at aac361e8, which carries ZERO check-runs. It was never
#     built. (a5fde619 carries 29, so a zero here is a real zero, not a query
#     that returns empty on failure.)
#   - the release object and its CI sit on a DIFFERENT sha, 2af24b9f. The tag
#     and the evidence disagree.
#   - the release carries zero assets. There is nothing to promote.
#   - main is 20 commits ahead, including a landed security fix that a promotion
#     of this tag would silently drop.
#
# The rule, the same one .github/scripts/refuse_unpublished_release.sh states:
# A CHECK THAT CANNOT PERFORM ITS FUNCTION MUST NOT REPORT SUCCESS. Here that
# bites hardest on the API reads. `gh` writes error bodies to STDOUT, so a naive
# capture swallows a 403 into what looks like data, and a rate-limited read
# returns EMPTY - indistinguishable from "this sha has no failing checks". Every
# read below captures stderr separately, tests the exit status, and REFUSES on a
# read that did not answer. An absence is only ever reported from an answer.
#
# A REFUSAL is exit non-zero WITH an `::error::` annotation. Exit non-zero
# WITHOUT one is a CRASH and says nothing about the release.
#
# Usage:
#   refuse_unproven_promotion.sh REPO TAG
#   refuse_unproven_promotion.sh --selftest
#
# Overrides, all for --selftest, which must present a bad release without
# creating one. Each substitutes for exactly one read; `__ERR__` makes that read
# report failure, which is how the fail-closed paths are shown to fire.
#   CHECKRUNS_OVERRIDE    newline-separated `conclusion<TAB>name`
#   RELEASE_SHA_OVERRIDE  the sha the release CI actually ran on
#   TAG_SHA_OVERRIDE      the sha the tag points at
#   ASSET_LIST_OVERRIDE   newline-separated asset names
#   ANCESTRY_OVERRIDE     newline-separated `sha<TAB>0|1` (0 = is an ancestor)
set -uo pipefail

annotate() { printf '::error::%s\n' "$*"; }

# Every override goes through here so the `__ERR__` fail-closed path is
# identical to a real read that did not answer.
_read() {
  local var="$1"; shift
  local ovr="${var}_OVERRIDE"
  if [ "${!ovr+set}" = set ]; then
    [ "${!ovr}" = "__ERR__" ] && return 1
    printf '%s' "${!ovr}"; return 0
  fi
  local errf st out; errf="$(mktemp)"
  out="$("$@" 2>"$errf")"; st=$?
  if [ "$st" -ne 0 ]; then
    printf '%s' "$(tr '\n' ' ' < "$errf")" >&2
    rm -f "$errf"; return 1
  fi
  rm -f "$errf"; printf '%s' "$out"; return 0
}

main() {
  local repo="$1" tag="$2"
  local fails=0
  echo "promotion gate: ${repo}@${tag}"

  # ------------------------------------------------- the sha under promotion --
  local tag_sha errf; errf="$(mktemp)"
  if ! tag_sha="$(_read TAG_SHA gh api "repos/${repo}/git/ref/tags/${tag}" \
                    --jq '.object.sha' 2>"$errf")"; then
    annotate "cannot resolve which sha ${repo}@${tag} points at: $(tr '\n' ' ' < "$errf"). Refusing: a promotion gate that does not know what it is gating proves nothing about it."
    rm -f "$errf"; return 1
  fi
  rm -f "$errf"
  if [ -z "$tag_sha" ]; then
    annotate "the tag ref read for ${repo}@${tag} answered EMPTY. An empty answer is not 'no sha'; it is a read that did not answer. Refusing rather than promoting an unidentified commit."
    return 1
  fi
  echo "  tag ${tag} -> ${tag_sha}"

  # ------------------------------ 1. the sha was BUILT AT ALL (check-runs > 0) --
  # Not "no failures": v0.2.0-rc.5 has no failures BECAUSE it has no check-runs.
  # Absence of red is not presence of green, and this is the case that proves it.
  local cr_raw
  errf="$(mktemp)"
  if ! cr_raw="$(_read CHECKRUNS gh api --paginate \
        "repos/${repo}/commits/${tag_sha}/check-runs" \
        --jq '.check_runs[] | "\(.conclusion // "PENDING")\t\(.name)"' 2>"$errf")"; then
    annotate "cannot read the check-runs for ${tag_sha}: $(tr '\n' ' ' < "$errf"). Refusing: a read that did not answer returns EMPTY, which is indistinguishable from a sha with nothing wrong with it."
    rm -f "$errf"; return 1
  fi
  rm -f "$errf"
  # Inline rather than via a helper: `local -n` is bash 4.3+ and this must run
  # under the bash 3.2 a macOS hand tests with. `$(...)` into an array is not
  # used either - command substitution strips trailing newlines and disagrees
  # with the API about an empty list, which is the one case that matters here.
  local runs=() _l
  while IFS= read -r _l; do [ -n "$_l" ] && runs+=("$_l"); done <<< "$cr_raw"
  echo "  ${tag_sha} carries ${#runs[@]} check-run(s)"
  if [ "${#runs[@]}" -eq 0 ]; then
    annotate "REFUSING: ${tag_sha} carries NO check-runs. The commit under promotion was never built or tested. Absence of red is not presence of green: there are no failures here BECAUSE there is no evidence here. (Every v0.2.0 candidate measured so far - rc.3, rc.4 and rc.5 - is in this state.)"
    fails=1
  fi

  # --------------------------------- 2. every check-run CONCLUDED success --
  # `.conclusion`, not a rollup and not `gh pr checks`, which renders CANCELLED
  # as "fail" and would make a green unreachable for an unrelated reason. A null
  # conclusion is a run still going: not a success, so not promotable.
  local bad=0 r concl name
  for r in ${runs[@]+"${runs[@]}"}; do
    concl="${r%%$'\t'*}"; name="${r#*$'\t'}"
    if [ "$concl" != "success" ]; then
      echo "    NOT-SUCCESS  ${concl}  ${name}"
      bad=$((bad + 1))
    fi
  done
  if [ "$bad" -ne 0 ]; then
    annotate "REFUSING: ${bad} of ${#runs[@]} check-run(s) on ${tag_sha} did not conclude success. A release is promoted from a green commit or not at all."
    fails=1
  elif [ "${#runs[@]}" -gt 0 ]; then
    echo "    all ${#runs[@]} check-run(s) concluded success"
  fi

  # --------------- 3. the tag's sha and the sha the release CI ran on AGREE --
  # v0.2.0-rc.5 is precisely the case where they do not: the tag is on aac361e8
  # and every check-run is on 2af24b9f. Either alone reads as fine; only the
  # comparison shows that the evidence is about a different commit.
  local rel_ref
  errf="$(mktemp)"
  if ! rel_ref="$(_read RELEASE_SHA gh api "repos/${repo}/releases/tags/${tag}" \
                    --jq '.target_commitish' 2>"$errf")"; then
    annotate "cannot read the release object for ${repo}@${tag}: $(tr '\n' ' ' < "$errf"). Refusing: without it there is nothing to compare the tag's sha against."
    rm -f "$errf"; return 1
  fi
  rm -f "$errf"
  if [ -z "$rel_ref" ]; then
    annotate "the release object for ${repo}@${tag} named NO commit. Refusing rather than treating an unanswered field as agreement."
    return 1
  fi
  # `.target_commitish` is a COMMITTISH, not a sha: `gh release create --target
  # main` stores the literal string `main`. Comparing that to a sha would refuse
  # every normally-created release - fail-closed, but with green unreachable,
  # which carries no more information than a check that passes on everything.
  # Resolve it to a sha first, and refuse if it cannot be resolved.
  local rel_sha=""
  if [[ "$rel_ref" =~ ^[0-9a-f]{40}$ ]]; then
    rel_sha="$rel_ref"
  else
    errf="$(mktemp)"
    if ! rel_sha="$(_read RESOLVED_SHA gh api "repos/${repo}/commits/${rel_ref}" \
                      --jq '.sha' 2>"$errf")"; then
      annotate "the release names '${rel_ref}', which is not a sha, and it could not be resolved to one: $(tr '\n' ' ' < "$errf"). Refusing: an unresolved committish cannot be compared to the tag's sha, and an unanswered comparison is not agreement."
      rm -f "$errf"; return 1
    fi
    rm -f "$errf"
    if [ -z "$rel_sha" ]; then
      annotate "resolving the release's committish '${rel_ref}' answered EMPTY. Refusing rather than reading an unanswered resolution as agreement."
      return 1
    fi
    echo "  release names '${rel_ref}', which resolves to ${rel_sha}"
  fi
  echo "  release CI ran on ${rel_sha}"
  if [ "$rel_sha" != "$tag_sha" ]; then
    annotate "REFUSING: the tag ${tag} points at ${tag_sha} but the release's own commit is ${rel_sha}. The tag and the evidence are about DIFFERENT commits, so no check-run result on either says anything about what would be promoted."
    fails=1
  else
    echo "    tag sha and release sha AGREE"
  fi

  # ------------------------------- 4. the NAMED assets are actually attached --
  # A named set from .github/promoted-assets, not a count: a release carrying
  # one stray file is not a release carrying the binary a consumer installs.
  local duckdb_version="" line
  if [ ! -r .github/duckdb-version ]; then
    annotate "cannot read .github/duckdb-version, so the asset names this release must carry cannot be built. Refusing: an empty substitution yields a name no release would ever carry, and C4 would then refuse for the WRONG reason."
    return 1
  fi
  duckdb_version="$(cat .github/duckdb-version)"
  if [ ! -r .github/promoted-assets ]; then
    annotate "cannot read .github/promoted-assets, so there is no statement of what this release must carry. Refusing rather than promoting against an empty expectation - an empty expected set is satisfied by a release with no assets at all."
    return 1
  fi
  local expected=()
  while IFS= read -r line; do
    case "$line" in ''|'#'*) continue ;; esac
    line="${line//%TAG%/$tag}"
    line="${line//%DUCKDB_VERSION%/$duckdb_version}"
    expected+=("$line")
  done < .github/promoted-assets
  if [ "${#expected[@]}" -eq 0 ]; then
    annotate "REFUSING: .github/promoted-assets names no assets. A gate with an empty expected set passes every release including one carrying nothing."
    return 1
  fi

  local as_raw
  errf="$(mktemp)"
  if ! as_raw="$(_read ASSET_LIST gh api "repos/${repo}/releases/tags/${tag}" \
                   --jq '.assets[].name' 2>"$errf")"; then
    annotate "cannot read the assets of ${repo}@${tag}: $(tr '\n' ' ' < "$errf"). Refusing to confirm a publish that cannot be read back."
    rm -f "$errf"; return 1
  fi
  rm -f "$errf"
  local actual=()
  while IFS= read -r _l; do [ -n "$_l" ] && actual+=("$_l"); done <<< "$as_raw"
  echo "  release carries ${#actual[@]} asset(s)"
  local missing=0 want a found
  for want in ${expected[@]+"${expected[@]}"}; do
    found=0
    for a in ${actual[@]+"${actual[@]}"}; do [ "$a" = "$want" ] && found=1 && break; done
    if [ "$found" -eq 1 ]; then echo "    OK       ${want}"
    else echo "    MISSING  ${want}"; missing=$((missing + 1)); fi
  done
  if [ "$missing" -ne 0 ]; then
    annotate "REFUSING: ${repo}@${tag} is missing ${missing} of the ${#expected[@]} asset(s) .github/promoted-assets requires. A release that does not carry the binary a consumer installs is a release there is nothing to promote."
    fails=1
  fi

  # ------------------- 5. the promoted sha CONTAINS every required commit --
  # Stated as an ancestry relation so a promotion that drops a landed fix is
  # visible rather than inferred from a commit count. main being ahead is not by
  # itself a defect; main being ahead BY A COMMIT THIS LIST NAMES is.
  if [ ! -r .github/required-commits ]; then
    annotate "cannot read .github/required-commits, so no statement exists of what a promotion must not drop. Refusing rather than promoting against an unread list."
    return 1
  fi
  local required=() rsha rwhy
  while IFS= read -r line; do
    case "$line" in ''|'#'*) continue ;; esac
    required+=("$line")
  done < .github/required-commits
  # Symmetric with the asset guard above, and for the same reason: an empty or
  # fully-commented list yields dropped=0 and passes C5 silently, so a gate
  # reading the file from a tree that predates an entry would report containment
  # it never checked.
  if [ "${#required[@]}" -eq 0 ]; then
    annotate "REFUSING: .github/required-commits names no commits. An empty required set passes C5 on every candidate including one that dropped a landed fix, which is the whole condition this check exists to state."
    return 1
  fi

  local anc_raw="" dropped=0
  if [ "${ANCESTRY_OVERRIDE+set}" = set ]; then
    [ "${ANCESTRY_OVERRIDE}" = "__ERR__" ] && {
      annotate "cannot determine ancestry for ${tag_sha}. Refusing: an ancestry question that was not answered is not an answer of 'contains it'."; return 1; }
    anc_raw="${ANCESTRY_OVERRIDE}"
  fi
  for line in ${required[@]+"${required[@]}"}; do
    rsha="${line%%[[:space:]]*}"; rwhy="${line#*[[:space:]]}"
    local st
    if [ -n "${ANCESTRY_OVERRIDE+set}" ] && [ "${ANCESTRY_OVERRIDE+set}" = set ]; then
      st="$(printf '%s\n' "$anc_raw" | awk -F'\t' -v s="$rsha" '$1==s{print $2; found=1} END{if(!found) print "X"}')"
    else
      # A shallow or partial clone cannot answer this, and `--is-ancestor`
      # exits non-zero for BOTH "no" and "I cannot see that commit". Establish
      # the commit is present FIRST, so a refusal is never really a clone defect.
      if ! git cat-file -e "${rsha}^{commit}" 2>/dev/null; then
        annotate "cannot resolve the required commit ${rsha} in this checkout, so whether ${tag_sha} contains it is UNKNOWN. Refusing: 'git could not see it' and 'the release dropped it' exit the same way, and neither is a pass. Check out with fetch-depth: 0."
        return 1
      fi
      if ! git cat-file -e "${tag_sha}^{commit}" 2>/dev/null; then
        annotate "cannot resolve the promoted commit ${tag_sha} in this checkout, so no ancestry question about it can be answered. Refusing. Check out with fetch-depth: 0."
        return 1
      fi
      git merge-base --is-ancestor "$rsha" "$tag_sha" 2>/dev/null; st=$?
    fi
    if [ "$st" = "X" ]; then
      annotate "no ancestry answer was produced for the required commit ${rsha}. Refusing rather than reading a missing answer as containment."
      return 1
    fi
    if [ "$st" -eq 0 ]; then
      echo "    CONTAINS  ${rsha}  ${rwhy}"
    else
      echo "    DROPS     ${rsha}  ${rwhy}"
      dropped=$((dropped + 1))
    fi
  done
  if [ "$dropped" -ne 0 ]; then
    annotate "REFUSING: ${tag_sha} does not contain ${dropped} commit(s) .github/required-commits names. Promoting it would ship a release without a fix that has already landed."
    fails=1
  fi

  echo
  if [ "$fails" -ne 0 ]; then return 1; fi
  echo "promotion gate: ${repo}@${tag} is proven promotable."
  return 0
}

# --------------------------------------------------------------- self-test --
#
# POSITIVE CONTROL. Every check above asserts an ABSENCE - no missing assets, no
# sha mismatch, no dropped commit - and an absence-shaped check is
# indistinguishable from one that is silently broken: both report zero. Each
# case below plants a fixture violating EXACTLY ONE condition and requires the
# refusal to name THAT condition, because "it refused" is satisfied by all six
# refusal paths and a case could otherwise pass on the wrong one.
selftest() {
  local fails=0 st
  local tmp; tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' RETURN

  local GOOD_SHA="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
  local GOOD_RUNS="success"$'\t'"build"$'\n'"success"$'\t'"test"
  # Derived from the same file main() substitutes from. A hand-written literal
  # here drifts the moment the pin moves, and the drift shows up as C0 failing
  # for a reason that has nothing to do with what C0 tests.
  local _dv; _dv="$(cat .github/duckdb-version)" || {
    printf '::error::self-test cannot read .github/duckdb-version, so the good-asset fixture cannot be built. Refusing rather than testing against a name the gate would never expect.\n'; return 1; }
  local GOOD_ASSETS="ducklake.linux_amd64.vX.duckdb-${_dv}.duckdb_extension"
  local GOOD_ANC="6e3a2e12a2d24a41d8a629ec91f93447a42df4cc"$'\t'"0"

  # _case want label reason [VAR=VAL ...] - unnamed overrides take the good value
  _case() {
    local want="$1" label="$2" reason="$3"; shift 3
    # shellcheck disable=SC2163  # the NAME=VALUE pairs are the fixture; exporting
    # a computed assignment is the point.
    ( for kv in "$@"; do export "$kv"; done
      # `${VAR:=default}` would be wrong here and was: it substitutes on NULL as
      # well as on unset, so every fixture planting an EMPTY answer - the exact
      # shape a rate-limited read returns - was silently replaced by the good
      # value and the case passed on the wrong refusal. Test for SET instead.
      [ "${TAG_SHA_OVERRIDE+s}" = s ]    || TAG_SHA_OVERRIDE="$GOOD_SHA"
      [ "${RELEASE_SHA_OVERRIDE+s}" = s ] || RELEASE_SHA_OVERRIDE="$GOOD_SHA"
      [ "${CHECKRUNS_OVERRIDE+s}" = s ]  || CHECKRUNS_OVERRIDE="$GOOD_RUNS"
      [ "${ASSET_LIST_OVERRIDE+s}" = s ] || ASSET_LIST_OVERRIDE="$GOOD_ASSETS"
      [ "${ANCESTRY_OVERRIDE+s}" = s ]   || ANCESTRY_OVERRIDE="$GOOD_ANC"
      export TAG_SHA_OVERRIDE RELEASE_SHA_OVERRIDE CHECKRUNS_OVERRIDE
      export ASSET_LIST_OVERRIDE ANCESTRY_OVERRIDE
      main fake/repo vX ) > "$tmp/o" 2>&1
    st=$?
    sed 's/^::error::/    [expected annotation] /' "$tmp/o" > "$tmp/o2"; mv "$tmp/o2" "$tmp/o"
    if [ "$st" -ne "$want" ]; then
      printf 'FAIL  %s: expected exit %s, got %s\n' "$label" "$want" "$st"
      sed 's/^/      | /' "$tmp/o"; fails=$((fails + 1)); return
    fi
    # A bash error AFTER the annotation still exits non-zero and still carries an
    # ::error:: line, so the annotation test alone cannot tell a refusal from a
    # refusal-then-crash. C1 passed on exactly that before this assertion existed.
    if grep -qE "$(basename "$0"): line [0-9]+:|unbound variable|command not found" "$tmp/o"; then
      printf 'FAIL  %s: the script errored (interpreter diagnostic in output), so this verdict is a crash\n' "$label"
      sed 's/^/      | /' "$tmp/o"; fails=$((fails + 1)); return
    fi
    if [ "$want" -ne 0 ]; then
      if ! grep -q '\[expected annotation\]' "$tmp/o"; then
        printf 'FAIL  %s: exit %s but NO ::error:: annotation - a crash, not a refusal\n' "$label" "$st"
        sed 's/^/      | /' "$tmp/o"; fails=$((fails + 1)); return
      fi
      if ! grep -q -- "\[expected annotation\].*$reason" "$tmp/o"; then
        printf 'FAIL  %s: refused, but NOT for the condition under test - no annotation naming %s\n' "$label" "$reason"
        sed 's/^/      | /' "$tmp/o"; fails=$((fails + 1)); return
      fi
    fi
    printf 'PASS  %s (exit %s%s)\n' "$label" "$st" \
      "$([ "$want" -ne 0 ] && echo ", ::error:: names '$reason'")"
  }

  # The green must be REACHABLE first. A gate that refuses everything carries no
  # more information than one that passes everything, and every red below would
  # be unattributable.
  _case 0 "C0 fully proven release passes" ""

  # C1 - the v0.2.0-rc.5 case: zero check-runs. Absence of red, not green.
  _case 1 "C1 tag sha carries NO check-runs" "carries NO check-runs" \
    "CHECKRUNS_OVERRIDE="
  # C2 - built, and red. Two shapes, because a null conclusion is a run still
  # going and reads as "not failed" to anything looking only for `failure`.
  _case 1 "C2 a check-run concluded failure" "did not conclude success" \
    "CHECKRUNS_OVERRIDE=success"$'\t'"build"$'\n'"failure"$'\t'"test"
  _case 1 "C2 a check-run is still pending" "did not conclude success" \
    "CHECKRUNS_OVERRIDE=success"$'\t'"build"$'\n'"PENDING"$'\t'"test"
  # C3 - the tag and the evidence on different commits (v0.2.0-rc.5, exactly).
  _case 1 "C3 tag sha and release sha disagree" "DIFFERENT commits" \
    "RELEASE_SHA_OVERRIDE=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
  # C4 - nothing to promote.
  _case 1 "C4 release carries no assets" "missing 1 of the 1 asset" \
    "ASSET_LIST_OVERRIDE="
  # ... and a release carrying SOME asset that is not the named one. A count
  # would be satisfied here; a named set is not.
  _case 1 "C4 release carries an asset, but not the named one" "missing 1 of the 1 asset" \
    "ASSET_LIST_OVERRIDE=README.md"
  # C5 - a landed required fix is not an ancestor of the promoted sha.
  _case 1 "C5 promoted sha drops a required commit" "does not contain 1 commit" \
    "ANCESTRY_OVERRIDE=6e3a2e12a2d24a41d8a629ec91f93447a42df4cc"$'\t'"1"

  # ------------------------------------------------ the REAL git path (C5) --
  #
  # Every case above sets ANCESTRY_OVERRIDE, so every one of them takes the
  # override branch and the real `git cat-file` / `merge-base --is-ancestor`
  # code is NEVER EXECUTED. Those cases are a positive control for the override
  # plumbing, not for the gate: with the arguments to `--is-ancestor` inverted -
  # the one mutation that silently turns "dropped" into "contained" - the whole
  # suite above still printed all cases passed. Delete-the-limb could not see it
  # either, because the limb fires either way.
  #
  # So these cases run with ANCESTRY_OVERRIDE UNSET, against real objects in a
  # scratch repository built here: a known ancestor and a known non-ancestor, in
  # BOTH directions, so an inverted comparison fails one of them whichever way
  # it is inverted. The scratch repo also owns its own .github/, which is how
  # the file-reading refusals below are exercised on real files.
  _realgit_case() {
    local want="$1" label="$2" reason="$3" reqfile="$4" tagsha_var="$5" dropdv="${6:-}"
    local d; d="$(mktemp -d)"
    (
      cd "$d" || exit 99
      git init -q .; git config user.email t@e; git config user.name t
      git commit -q --allow-empty -m A; local A; A="$(git rev-parse HEAD)"
      git commit -q --allow-empty -m B; local B; B="$(git rev-parse HEAD)"
      # C is a real commit that is NOT an ancestor of B - a sibling off A.
      git checkout -q -b side "$A"
      git commit -q --allow-empty -m C; local C; C="$(git rev-parse HEAD)"
      git checkout -q "$B"
      mkdir -p .github
      [ "$dropdv" = "nodv" ] || echo "vTEST" > .github/duckdb-version
      echo 'ducklake.linux_amd64.%TAG%.duckdb-%DUCKDB_VERSION%.duckdb_extension' > .github/promoted-assets
      # The fixture names which real commit the required list holds.
      case "$reqfile" in
        ancestor)     echo "$A  a real ancestor of the promoted sha" ;;
        nonancestor)  echo "$C  a real commit that is NOT an ancestor" ;;
        descendant)   echo "$B  a real DESCENDANT of the promoted sha" ;;
        unresolvable) echo "0123456789012345678901234567890123456789  a commit no clone has" ;;
        empty)        echo "# every line a comment, so the set is empty" ;;
      esac > .github/required-commits
      local T; case "$tagsha_var" in A) T="$A" ;; B) T="$B" ;; C) T="$C" ;; esac
      TAG_SHA_OVERRIDE="$T" \
      RELEASE_SHA_OVERRIDE="$T" \
      CHECKRUNS_OVERRIDE="success"$'\t'"build" \
      ASSET_LIST_OVERRIDE="ducklake.linux_amd64.vX.duckdb-vTEST.duckdb_extension" \
        main fake/repo vX
    ) > "$tmp/o" 2>&1
    st=$?
    rm -rf "$d"
    sed 's/^::error::/    [expected annotation] /' "$tmp/o" > "$tmp/o2"; mv "$tmp/o2" "$tmp/o"
    if grep -qE "unbound variable|command not found|: line [0-9]+:" "$tmp/o"; then
      printf 'FAIL  %s: the script errored, so this verdict is a crash\n' "$label"
      sed 's/^/      | /' "$tmp/o"; fails=$((fails + 1)); return
    fi
    if [ "$st" -ne "$want" ]; then
      printf 'FAIL  %s: expected exit %s, got %s\n' "$label" "$want" "$st"
      sed 's/^/      | /' "$tmp/o"; fails=$((fails + 1)); return
    fi
    if [ "$want" -ne 0 ] && ! grep -q -- "\[expected annotation\].*$reason" "$tmp/o"; then
      printf 'FAIL  %s: refused, but NOT for the condition under test - no annotation naming %s\n' "$label" "$reason"
      sed 's/^/      | /' "$tmp/o"; fails=$((fails + 1)); return
    fi
    printf 'PASS  %s (exit %s%s)\n' "$label" "$st" \
      "$([ "$want" -ne 0 ] && echo ", ::error:: names '$reason'")"
  }

  # REAL git, required commit IS an ancestor of the promoted sha -> green. This
  # is also the only C0-shaped case in which a real read path actually runs.
  _realgit_case 0 "C5-real required commit IS an ancestor (real git)" "" ancestor B
  # REAL git, required commit is NOT an ancestor -> refuse. Inverting the
  # arguments to --is-ancestor flips exactly this pair, and one of the two
  # fails whichever way the inversion goes.
  _realgit_case 1 "C5-real required commit is NOT an ancestor (real git)" \
    "does not contain 1 commit" nonancestor B
  # The promoted sha is the ANCESTOR and the required commit its DESCENDANT.
  # This is the direction an inverted comparison gets wrong in the dangerous
  # way - it would report CONTAINS for a fix the candidate does not carry - and
  # together with the case above it pins the comparison in both directions.
  _realgit_case 1 "C5-real required commit is a DESCENDANT of the promoted sha" \
    "does not contain 1 commit" descendant A
  # `--is-ancestor` exits non-zero for "cannot see that commit" exactly as for
  # "no", so a commit the clone lacks must refuse as UNKNOWN, not as dropped.
  _realgit_case 1 "C5-real required commit not in the checkout (real git)" \
    "cannot resolve the required commit" unresolvable B
  # F3: an empty or fully-commented list yields dropped=0 and would pass.
  _realgit_case 1 "C5-real required-commits names no commits" \
    "names no commits" empty B
  # F6: a missing duckdb-version must refuse for ITS reason, not via C4.
  _realgit_case 1 "a missing .github/duckdb-version refuses explicitly" \
    "cannot read .github/duckdb-version" ancestor B nodv

  # C6 - FAIL CLOSED. Each read, made to report failure the way a 403 or a
  # rate-limited read does. Every one of these returns EMPTY in the shape that
  # would otherwise read as "nothing wrong here", so each must refuse and must
  # name the read that did not answer rather than the absence it implies.
  _case 1 "C6 tag ref read did not answer" "does not know what it is gating" \
    "TAG_SHA_OVERRIDE=__ERR__"
  _case 1 "C6 tag ref read answered EMPTY" "answered EMPTY" \
    "TAG_SHA_OVERRIDE="
  _case 1 "C6 check-runs read did not answer" "indistinguishable from a sha with nothing wrong" \
    "CHECKRUNS_OVERRIDE=__ERR__"
  _case 1 "C6 release object read did not answer" "nothing to compare the tag" \
    "RELEASE_SHA_OVERRIDE=__ERR__"
  _case 1 "C6 release named no commit" "named NO commit" \
    "RELEASE_SHA_OVERRIDE="
  _case 1 "C6 asset read did not answer" "cannot be read back" \
    "ASSET_LIST_OVERRIDE=__ERR__"
  _case 1 "C6 ancestry could not be determined" "was not answered" \
    "ANCESTRY_OVERRIDE=__ERR__"

  # F5 - `.target_commitish` is routinely a BRANCH NAME. It must be resolved to
  # a sha and then compared, not compared as a string: comparing `main` to a sha
  # refuses every normally-created release, which is fail-closed with green
  # unreachable. Both directions, so the resolution is shown to decide the
  # verdict rather than merely to run.
  _case 0 "F5 release names a branch that resolves to the tag's sha" "" \
    "RELEASE_SHA_OVERRIDE=main" "RESOLVED_SHA_OVERRIDE=$GOOD_SHA"
  _case 1 "F5 release names a branch resolving to a DIFFERENT sha" "DIFFERENT commits" \
    "RELEASE_SHA_OVERRIDE=main" "RESOLVED_SHA_OVERRIDE=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
  _case 1 "F5 the branch name could not be resolved at all" "not a sha, and it could not be resolved" \
    "RELEASE_SHA_OVERRIDE=main" "RESOLVED_SHA_OVERRIDE=__ERR__"
  _case 1 "F5 resolving the branch name answered EMPTY" "answered EMPTY" \
    "RELEASE_SHA_OVERRIDE=main" "RESOLVED_SHA_OVERRIDE="

  echo
  if [ "$fails" -ne 0 ]; then
    printf '::error::promotion-gate self-test: %s case(s) failed\n' "$fails"
    return 1
  fi
  echo "promotion-gate self-test: all cases passed"
  return 0
}

if [ "${1:-}" = "--selftest" ]; then
  selftest
  exit $?
fi

if [ "$#" -ne 2 ]; then
  annotate "usage: $0 REPO TAG (or --selftest)"
  exit 2
fi
main "$@"
exit $?
