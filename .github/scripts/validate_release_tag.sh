#!/usr/bin/env bash
# REFUSE a Nexus publish for anything that is not a release tag whose peeled
# commit is the commit the build job actually built. The tag becomes part of a
# write-once Nexus path, so `main`, a branch, a bare SHA or a moved tag would
# mint a permanent path for bytes that no tag names.
#
# usage: validate_release_tag.sh TAG BUILT_SHA [REMOTE]
set -euo pipefail
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
