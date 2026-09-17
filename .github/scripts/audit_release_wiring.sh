#!/usr/bin/env bash
#
# REFUSE a FINAL release whose tag was cut from a tree that cannot run the
# promotion gate.
#
# The hole this names: on a `release` event GitHub sources the workflow FILE
# from the release's own commit, not from the default branch. Measured on this
# repository - run 32771943481 for v0.2.0-rc.5 has head_sha 2af24b9f, the
# candidate's tree, not main's. So a release cut from a commit that predates the
# gate carries neither the pre-upload step in Release.yml nor PromotionGate.yml:
# the gate does not refuse, it does not run, and `ref: default_branch` cannot
# help because the job never starts.
#
# Nothing in a repository can close that from inside the release event itself.
# What CAN be done is detect it afterwards, from a workflow that runs on the
# DEFAULT BRANCH, where the file is sourced from main. This is that check. It is
# an audit, not a block: by the time it fires the release exists. It is recorded
# as such, and the residual hole is stated on the PR rather than papered over.
#
# A REFUSAL is exit non-zero WITH an `::error::` annotation.
#
# Overrides, for --selftest only:
#   RELEASES_OVERRIDE   newline-separated `tag<TAB>true|false<TAB>published_at`
#   WIRED_OVERRIDE      newline-separated `tag<TAB>0|1` (0 = tree carries the gate)
#   GATE_INTRO_OVERRIDE the commit date the gate step entered the workflow
set -uo pipefail

GATE_CALL='refuse_unproven_promotion.sh'
WORKFLOW='.github/workflows/Release.yml'

annotate() { printf '::error::%s\n' "$*"; }

# ISO-8601 -> epoch seconds. `git log --format=%cI` emits the committer's LOCAL
# offset and the releases API emits `Z`; comparing those two spellings as TEXT
# is wrong by exactly that offset, and wrong in the fail-OPEN direction. Both
# sides go through this, or neither.
iso_epoch() {
  python3 -B -c 'import datetime,sys
try:
    d = datetime.datetime.fromisoformat(sys.argv[1].replace("Z","+00:00"))
    # A NAIVE timestamp parses fine and then .timestamp() reads it as LOCAL
    # time - a silently wrong answer where a refusal is wanted. Print nothing
    # and let the caller fail closed.
    if d.utcoffset() is not None: print(int(d.timestamp()))
except Exception: pass' "$1" 2>/dev/null
}

main() {
  local repo="$1"
  local fails=0

  if [ ! -r .github/gate-exempt-releases ]; then
    annotate "cannot read .github/gate-exempt-releases, so which releases predate the gate is UNKNOWN. Refusing rather than auditing against an unread list - an unread exemption list is indistinguishable from an empty one, and would red every historical release."
    return 1
  fi
  local exempt=() line
  while IFS= read -r line; do
    case "$line" in ''|'#'*) continue ;; esac
    exempt+=("$line")
    printf 'EXEMPT  %s\n' "$line"
  done < .github/gate-exempt-releases

  local rel_raw errf; errf="$(mktemp)"
  if [ "${RELEASES_OVERRIDE+set}" = set ]; then
    [ "${RELEASES_OVERRIDE}" = "__ERR__" ] && {
      annotate "cannot list the releases of ${repo}. Refusing: a read that did not answer returns EMPTY, which is indistinguishable from a repository with no unwired release in it."; rm -f "$errf"; return 1; }
    rel_raw="${RELEASES_OVERRIDE}"
  else
    if ! rel_raw="$(gh api --paginate "repos/${repo}/releases?per_page=100" \
          --jq '.[] | "\(.tag_name)\t\(.prerelease)\t\(.published_at // "")"' 2>"$errf")"; then
      annotate "cannot list the releases of ${repo}: $(tr '\n' ' ' < "$errf"). Refusing: a read that did not answer returns EMPTY, which is indistinguishable from a repository with no unwired release in it."
      rm -f "$errf"; return 1
    fi
  fi
  rm -f "$errf"
  if [ -z "$rel_raw" ]; then
    annotate "the release list for ${repo} answered EMPTY. This repository has releases, so an empty list is a read that did not answer. Refusing rather than auditing nothing and reporting a clean result."
    return 1
  fi

  local tag pre pub e checked=0 unwired=0 st intro pub_e intro_e
  while IFS= read -r line; do
    [ -n "$line" ] || continue
    tag="${line%%$'\t'*}"; pub="${line##*$'\t'}"
    pre="${line#*$'\t'}"; pre="${pre%%$'\t'*}"
    # A prerelease is a CANDIDATE, not a promotion. The pre-upload gate still
    # runs on one - deliberately, see Release.yml - but an rc cut before the
    # gate existed is not the hole this audit is about.
    if [ "$pre" != "false" ]; then
      printf 'skip    %s (prerelease)\n' "$tag"; continue
    fi
    local is_exempt=0
    for e in ${exempt[@]+"${exempt[@]}"}; do
      [ "${e%%[[:space:]]*}" = "$tag" ] && is_exempt=1 && break
    done
    if [ "$is_exempt" -eq 1 ]; then
      # An exemption asserts "this release predates the gate". Check the claim
      # rather than take it: one appended line to a tracked file otherwise
      # clears any red. A release published AFTER the gate step entered
      # Release.yml did not predate anything.
      if [ "${GATE_INTRO_OVERRIDE+set}" = set ]; then
        intro="${GATE_INTRO_OVERRIDE}"
      else
        intro="$(git log --reverse --format=%cI -S"$GATE_CALL" -- "$WORKFLOW" 2>/dev/null | head -1)"
      fi
      if [ -z "$intro" ]; then
        annotate "cannot determine when ${GATE_CALL} entered ${WORKFLOW}, so the claim that ${tag} predates the gate is UNCHECKABLE. Refusing rather than honouring an exemption on an unverified date. Check out with fetch-depth: 0."
        return 1
      fi
      if [ -z "$pub" ]; then
        annotate "the release list gave no published_at for the exempt tag ${tag}, so whether it predates the gate is UNKNOWN. Refusing rather than honouring an exemption on a missing date."
        return 1
      fi
      pub_e="$(iso_epoch "$pub")"
      intro_e="$(iso_epoch "$intro")"
      if [ -z "$pub_e" ] || [ -z "$intro_e" ]; then
        annotate "cannot convert the dates for ${tag} to epochs (published '${pub}', gate '${intro}'), so whether it predates the gate is UNKNOWN. Refusing rather than comparing two timestamps whose zones may differ."
        return 1
      fi
      if [ "$pub_e" -gt "$intro_e" ]; then
        annotate "REFUSING: ${tag} is listed in .github/gate-exempt-releases but was published at ${pub}, AFTER ${GATE_CALL} entered ${WORKFLOW} at ${intro}. The exemption file is for releases that predate the gate; it is not a way to excuse one that could have been gated."
        return 1
      fi
      printf 'skip    %s (exempt, published %s before the gate at %s)\n' "$tag" "$pub" "$intro"; continue
    fi

    checked=$((checked + 1))
    if [ "${WIRED_OVERRIDE+set}" = set ]; then
      st="$(printf '%s\n' "${WIRED_OVERRIDE}" | awk -F'\t' -v t="$tag" '$1==t{print $2; f=1} END{if(!f) print "X"}')"
      if [ "$st" = "X" ]; then
        annotate "no wiring answer was produced for ${tag}. Refusing rather than reading a missing answer as wired."
        return 1
      fi
    else
      # `git show` on a missing path and on a missing OBJECT exit the same way,
      # so establish the tag resolves FIRST - otherwise a shallow clone reads as
      # an unwired release and the diagnosis points at the wrong thing.
      if ! git rev-parse -q --verify "refs/tags/${tag}^{commit}" >/dev/null; then
        annotate "cannot resolve refs/tags/${tag} in this checkout, so whether its tree carries the promotion gate is UNKNOWN. Refusing: 'git could not see it' and 'the tag predates the gate' are not the same answer. Check out with fetch-depth: 0 and fetch tags."
        return 1
      fi
      if git show "refs/tags/${tag}:${WORKFLOW}" 2>/dev/null | grep -q -- "$GATE_CALL"; then st=0; else st=1; fi
    fi
    if [ "$st" -eq 0 ]; then
      printf 'WIRED   %s\n' "$tag"
    else
      printf 'UNWIRED %s\n' "$tag"
      unwired=$((unwired + 1))
    fi
  done <<< "$rel_raw"

  echo "  audited ${checked} final release(s)"
  if [ "$checked" -eq 0 ]; then
    echo "  (no non-exempt final release exists yet; this audit has nothing to say until one is cut)"
  fi
  if [ "$unwired" -ne 0 ]; then
    annotate "REFUSING: ${unwired} final release(s) were cut from a tree that does not call ${GATE_CALL} in ${WORKFLOW}. On a release event GitHub sources the workflow from the RELEASE'S OWN COMMIT, so for those releases the pre-upload gate did not run, could not run, and nothing on the default branch could have made it run. The asset was attached ungated."
    fails=1
  fi
  [ "$fails" -ne 0 ] && return 1
  echo "release-wiring audit: every non-exempt final release was cut from a gate-carrying tree."
  return 0
}

selftest() {
  local here; here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  local fails=0 tmp; tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' RETURN
  _case() {
    local want="$1" label="$2" reason="$3"; shift 3
    # shellcheck disable=SC2163  # the NAME=VALUE pairs ARE the fixture;
    # exporting a computed assignment is exactly what is wanted here.
    ( for kv in "$@"; do export "$kv"; done
      main fake/repo ) > "$tmp/o" 2>&1
    local st=$?
    if grep -qE "unbound variable|command not found|: line [0-9]+:" "$tmp/o"; then
      printf 'FAIL  %s: the script errored, so this verdict is a crash\n' "$label"
      sed 's/^/      | /' "$tmp/o"; fails=$((fails + 1)); return
    fi
    if [ "$st" -ne "$want" ]; then
      printf 'FAIL  %s: expected exit %s, got %s\n' "$label" "$want" "$st"
      sed 's/^/      | /' "$tmp/o"; fails=$((fails + 1)); return
    fi
    if [ "$want" -ne 0 ]; then
      grep -q '^::error::' "$tmp/o" || {
        printf 'FAIL  %s: exit %s with NO annotation - a crash, not a refusal\n' "$label" "$st"
        sed 's/^/      | /' "$tmp/o"; fails=$((fails + 1)); return; }
      grep -q -- "^::error::.*$reason" "$tmp/o" || {
        printf 'FAIL  %s: refused, but not for the condition under test (%s)\n' "$label" "$reason"
        sed 's/^/      | /' "$tmp/o"; fails=$((fails + 1)); return; }
    fi
    printf 'PASS  %s (exit %s)\n' "$label" "$st"
  }

  # Green must be reachable: a final release cut from a wired tree passes.
  _case 0 "a final release cut from a gate-carrying tree" "" \
    "RELEASES_OVERRIDE=v9.9.9"$'\t'"false"$'\t'"2026-09-01T00:00:00Z" "WIRED_OVERRIDE=v9.9.9"$'\t'"0"
  # The hole itself.
  _case 1 "a final release cut from a tree without the gate" "cut from a tree that does not call" \
    "RELEASES_OVERRIDE=v9.9.9"$'\t'"false"$'\t'"2026-09-01T00:00:00Z" "WIRED_OVERRIDE=v9.9.9"$'\t'"1"
  # A prerelease is out of scope and must not red.
  _case 0 "an unwired PRERELEASE is out of scope" "" \
    "RELEASES_OVERRIDE=v9.9.9-rc.1"$'\t'"true"$'\t'"2026-09-01T00:00:00Z" "WIRED_OVERRIDE=v9.9.9-rc.1"$'\t'"1"
  # The exemption path, on the real file's own entry, with a date that predates
  # the gate.
  _case 0 "an exempt release published before the gate is skipped" "" \
    "RELEASES_OVERRIDE=v0.1.0"$'\t'"false"$'\t'"2026-08-01T00:00:00Z" "WIRED_OVERRIDE=v0.1.0"$'\t'"1" \
    "GATE_INTRO_OVERRIDE=2026-09-01T00:00:00Z"
  # R3: the exemption file is one appended line away from clearing any red, so
  # the "predates the gate" claim is CHECKED, not taken.
  _case 1 "an exempt release published AFTER the gate is still refused" "AFTER refuse_unproven_promotion.sh entered" \
    "RELEASES_OVERRIDE=v0.1.0"$'\t'"false"$'\t'"2026-09-30T00:00:00Z" "WIRED_OVERRIDE=v0.1.0"$'\t'"1" \
    "GATE_INTRO_OVERRIDE=2026-09-01T00:00:00Z"
  _case 1 "an exempt release with no published_at" "no published_at for the exempt tag" \
    "RELEASES_OVERRIDE=v0.1.0"$'\t'"false"$'\t' "WIRED_OVERRIDE=v0.1.0"$'\t'"1" \
    "GATE_INTRO_OVERRIDE=2026-09-01T00:00:00Z"
  _case 1 "the gate's introduction date cannot be determined" "UNCHECKABLE" \
    "RELEASES_OVERRIDE=v0.1.0"$'\t'"false"$'\t'"2026-08-01T00:00:00Z" "WIRED_OVERRIDE=v0.1.0"$'\t'"1" \
    "GATE_INTRO_OVERRIDE="
  # The two dates arrive in DIFFERENT spellings - `%cI` carries the committer's
  # local offset, the API carries `Z` - so a text compare is wrong by exactly
  # that offset. Both straddle cases below are decided by the zone, not the
  # digits, and a lexicographic compare gets each one backwards.
  _case 1 "an exempt release published after a gate dated in +01:00 is refused" "AFTER refuse_unproven_promotion.sh entered" \
    "RELEASES_OVERRIDE=v0.1.0"$'\t'"false"$'\t'"2026-09-17T18:30:00Z" "WIRED_OVERRIDE=v0.1.0"$'\t'"1" \
    "GATE_INTRO_OVERRIDE=2026-09-17T19:10:17+01:00"
  _case 0 "an exempt release published before a gate dated in -05:00 is skipped" "" \
    "RELEASES_OVERRIDE=v0.1.0"$'\t'"false"$'\t'"2026-09-17T18:00:00Z" "WIRED_OVERRIDE=v0.1.0"$'\t'"1" \
    "GATE_INTRO_OVERRIDE=2026-09-17T14:10:17-05:00"
  # ... and a date neither side can convert is a REFUSAL, never a pass. Without
  # a fixture here the whole fail-closed block could be deleted at 16 PASS.
  _case 1 "an exempt release whose published_at cannot be converted" "cannot convert the dates" \
    "RELEASES_OVERRIDE=v0.1.0"$'\t'"false"$'\t'"not-a-date" "WIRED_OVERRIDE=v0.1.0"$'\t'"1" \
    "GATE_INTRO_OVERRIDE=2026-09-01T00:00:00Z"
  _case 1 "an exempt release whose published_at carries no zone" "cannot convert the dates" \
    "RELEASES_OVERRIDE=v0.1.0"$'\t'"false"$'\t'"2026-09-17T18:30:00" "WIRED_OVERRIDE=v0.1.0"$'\t'"1" \
    "GATE_INTRO_OVERRIDE=2026-09-17T19:10:17+01:00"
  # ... and the exemption is NAME-scoped, not a blanket.
  _case 1 "the exemption does not cover a different tag" "cut from a tree that does not call" \
    "RELEASES_OVERRIDE=v0.1.0"$'\t'"false"$'\t'"2026-08-01T00:00:00Z"$'\n'"v9.9.9"$'\t'"false"$'\t'"2026-09-01T00:00:00Z" \
    "WIRED_OVERRIDE=v0.1.0"$'\t'"1"$'\n'"v9.9.9"$'\t'"1" \
    "GATE_INTRO_OVERRIDE=2026-09-01T00:00:00Z"
  # Fail-closed reads.
  _case 1 "the release list did not answer" "did not answer returns EMPTY" \
    "RELEASES_OVERRIDE=__ERR__"
  _case 1 "the release list answered EMPTY" "answered EMPTY" \
    "RELEASES_OVERRIDE="
  _case 1 "no wiring answer for a release" "no wiring answer was produced" \
    "RELEASES_OVERRIDE=v9.9.9"$'\t'"false"$'\t'"2026-09-01T00:00:00Z" "WIRED_OVERRIDE=v0.0.0"$'\t'"0"

  # ------------------------------------------------------------------
  # R1: every case above sets WIRED_OVERRIDE, so the line that actually
  # decides the verdict - `git show TAG:Release.yml | grep GATE_CALL` - is
  # never executed. Inverting it, or deleting it and forcing st=0, survives
  # all of them. These cases run it for real: a scratch repo, two tags, the
  # override UNSET, and both directions required.
  _realgit_case() {
    local want="$1" label="$2" reason="$3" tag="$4"
    local d="$tmp/rg"; rm -rf "$d"; mkdir -p "$d/.github/workflows" "$d/.github/scripts"
    ( cd "$d" || exit 1
      git init -q . && git config user.email a@b && git config user.name a
      cp "$here/../gate-exempt-releases" .github/gate-exempt-releases 2>/dev/null \
        || echo "# none" > .github/gate-exempt-releases
      printf 'jobs:\n  publish:\n    steps:\n      - run: echo nothing here\n' \
        > .github/workflows/Release.yml
      git add -A && git commit -qm unwired && git tag v-unwired
      printf 'jobs:\n  publish:\n    steps:\n      - run: bash .github/scripts/%s x y\n' \
        "$GATE_CALL" > .github/workflows/Release.yml
      git add -A && git commit -qm wired && git tag v-wired ) >/dev/null 2>&1
    ( cd "$d" || exit 1
      unset WIRED_OVERRIDE
      export RELEASES_OVERRIDE="${tag}"$'\t'"false"$'\t'"2026-09-01T00:00:00Z"
      main fake/repo ) > "$tmp/o" 2>&1
    local st=$?
    if [ "$st" -ne "$want" ]; then
      printf 'FAIL  %s: expected exit %s, got %s\n' "$label" "$want" "$st"
      sed 's/^/      | /' "$tmp/o"; fails=$((fails + 1)); return
    fi
    if [ "$want" -ne 0 ]; then
      grep -q -- "^::error::.*$reason" "$tmp/o" || {
        printf 'FAIL  %s: refused, but not for the condition under test (%s)\n' "$label" "$reason"
        sed 's/^/      | /' "$tmp/o"; fails=$((fails + 1)); return; }
    fi
    printf 'PASS  %s (exit %s)\n' "$label" "$st"
  }
  _realgit_case 0 "REAL git: a tag whose tree calls the gate is WIRED" "" v-wired
  _realgit_case 1 "REAL git: a tag whose tree does not call the gate is UNWIRED" \
    "cut from a tree that does not call" v-unwired
  _realgit_case 1 "REAL git: a tag absent from the checkout is UNKNOWN, not wired" \
    "cannot resolve refs/tags/v-absent" v-absent

  echo
  if [ "$fails" -ne 0 ]; then
    printf '::error::release-wiring audit self-test: %s case(s) failed\n' "$fails"; return 1
  fi
  echo "release-wiring audit self-test: all cases passed"
  return 0
}

if [ "${1:-}" = "--selftest" ]; then selftest; exit $?; fi
if [ "$#" -ne 1 ]; then
  annotate "usage: audit_release_wiring.sh REPO  |  --selftest"
  exit 2
fi
main "$1"
