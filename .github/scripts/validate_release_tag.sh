#!/usr/bin/env bash
# REFUSE a Nexus publish for anything that is not a release tag whose peeled
# commit is the commit the build job actually built. The tag becomes part of a
# write-once Nexus path, so `main`, a branch, a bare SHA or a moved tag would
# mint a permanent path for bytes that no tag names.
#
# usage: validate_release_tag.sh TAG BUILT_SHA [REMOTE]
#        validate_release_tag.sh --selftest   (positive controls, no network)
set -euo pipefail

if [ "${1-}" = --selftest ]; then
  # Runs THIS script as a subprocess against a local bare repo holding an
  # annotated tag, a lightweight tag, and a BRANCH named like a tag, so a
  # broken check reds here instead of minting a permanent Nexus path.
  self="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
  st="$(mktemp -d)"; trap 'rm -rf "$st"' EXIT
  export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
  g() { git -C "$st/w" -c user.name=selftest -c user.email=selftest@invalid "$@"; }
  git init -q "$st/w"
  g commit -q --allow-empty -m a; a="$(g rev-parse HEAD)"
  g commit -q --allow-empty -m b; b="$(g rev-parse HEAD)"
  g tag -a -m annotated v1.0.0 "$a"
  g tag v1.0.1 "$a"
  g branch v9.9.9 "$a"
  g branch -M main
  git clone -q --bare "$st/w" "$st/r.git"
  fails=0
  expect() { # accept|refuse FRAGMENT TAG SHA
    local want="$1" frag="$2" out rc=0; shift 2
    out="$(bash "$self" "$1" "$2" "$st/r.git" 2>&1)" || rc=$?
    if [ "$want" = accept ] && [ "$rc" = 0 ]; then echo "ok   accept $1"
    elif [ "$want" = refuse ] && [ "$rc" != 0 ] && grep -qF -- "$frag" <<<"$out"; then echo "ok   refuse $1 ($frag)"
    else echo "FAIL want=$want tag=$1 sha=$2 rc=$rc: $out"; fails=$((fails+1)); fi
  }
  expect refuse "is not a release tag"      main    "$a"
  expect refuse "is not a release tag"      "$a"    "$a"
  expect refuse "does not exist on"         v9.9.9  "$a"
  expect refuse "does not exist on"         v2.0.0  "$a"
  expect refuse "does not name them"        v1.0.0  "$b"
  expect refuse "does not name them"        v1.0.1  "$b"
  expect refuse "reported no commit sha"    v1.0.0  ""
  expect accept ""                          v1.0.0  "$a"
  expect accept ""                          v1.0.1  "$a"
  [ "$fails" = 0 ] || { echo "::error::validate_release_tag.sh selftest: ${fails} case(s) failed"; exit 1; }
  echo "validate_release_tag.sh selftest: all cases hold"; exit 0
fi

tag="${1-}"; built="${2-}"; remote="${3:-origin}"
err() { printf '::error::%s\n' "$*"; exit 1; }
[[ "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$ ]] \
  || err "'${tag}' is not a release tag (vX.Y.Z or vX.Y.Z-pre). Refusing to mint a permanent Nexus path for it."
[[ "$built" =~ ^[0-9a-f]{40}$ ]] \
  || err "the build job reported no commit sha ('${built}'), so what was built cannot be tied to ${tag}."
out="$(git ls-remote --exit-code --tags "$remote" "refs/tags/${tag}" "refs/tags/${tag}^{}")" \
  || err "refs/tags/${tag} does not exist on ${remote}. A branch or sha of the same name is not a tag."
peeled="$(awk -v r="refs/tags/${tag}^{}" '$2==r {print $1}' <<<"$out")"
[ -n "$peeled" ] || peeled="$(awk -v r="refs/tags/${tag}" '$2==r {print $1}' <<<"$out")"
[ "$peeled" = "$built" ] \
  || err "refs/tags/${tag} points at ${peeled}, but the build job built ${built}. Refusing to publish bytes under a tag that does not name them."
echo "tag ${tag} -> ${peeled}, the commit the build job built"
