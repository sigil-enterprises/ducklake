#!/usr/bin/env bash
# Write-once upload of FILE and FILE.sha256 to a Nexus raw URL, then read both
# back and verify. Each file is probed and written ON ITS OWN, so a run that
# died between the two PUTs is completed by a re-run instead of skipping the
# missing checksum forever.
#
# The write-once guarantee here is CLIENT-SIDE (probe, then PUT): two
# concurrent writers can both see 404. The repository's own write policy is
# what would close that race; sigil-enterprises-raw is ALLOW today. This job
# runs with cancel-in-progress off, and a 400/409 from a repo that refuses
# redeploys is treated as "already there" and compared, never as success.
#
# usage: nexus_publish.sh FILE URL [KEEP_DIR]   (env: NEXUS_USERNAME, NEXUS_PASSWORD)
set -euo pipefail
file="$1"; url="$2"; keep="${3:-}"
err() { printf '::error::%s\n' "$*"; exit 1; }
: "${NEXUS_USERNAME:?}" "${NEXUS_PASSWORD:?}"
[ -n "$NEXUS_USERNAME" ] && [ -n "$NEXUS_PASSWORD" ] || err "NEXUS_USERNAME/NEXUS_PASSWORD are empty."

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

# The ONE place credentials are rendered: a curl config on stdin, never argv.
# `\` and `"` are escaped because curl parses the quoted value.
creds() {
  local u="${NEXUS_USERNAME//\\/\\\\}" p="${NEXUS_PASSWORD//\\/\\\\}"
  u="${u//\"/\\\"}"; p="${p//\"/\\\"}"
  printf 'user = "%s:%s"\n' "$u" "$p"
}
ncurl() { creds | curl -sS --config - "$@"; }

name="$(basename "$file")"
sha="$(sha256sum "$file" | cut -d' ' -f1)"
printf '%s  %s\n' "$sha" "$name" > "$tmp/$name.sha256"

same_as_published() { # local remote
  local code
  code="$(ncurl -o "$tmp/existing" -w '%{http_code}' "$2")" || err "GET $2 failed at the transport level."
  [ "$code" = 200 ] || err "GET $2 answered HTTP ${code} after the repository said it exists."
  cmp -s "$1" "$tmp/existing"
}

put_once() { # local remote
  local code
  code="$(ncurl -o /dev/null -w '%{http_code}' "$2")" || err "probing $2 failed at the transport level."
  case "$code" in
    200) same_as_published "$1" "$2" || err "$2 already holds DIFFERENT bytes. Refusing to overwrite a published path."
         echo "$2 already holds these exact bytes; not re-uploading."; return 0 ;;
    404) ;;
    *)   err "probing $2 answered HTTP ${code}; neither present nor absent, refusing to guess." ;;
  esac
  code="$(ncurl -o "$tmp/put.out" -w '%{http_code}' --upload-file "$1" "$2")" || err "PUT $2 failed at the transport level."
  case "$code" in
    200|201|204) echo "uploaded $2 (HTTP ${code})" ;;
    400|409) same_as_published "$1" "$2" || err "PUT $2 was refused (HTTP ${code}) and the path holds DIFFERENT bytes."
             echo "$2 was written concurrently with these exact bytes (HTTP ${code})." ;;
    *)   err "PUT $2 answered HTTP ${code}; not a success. $(head -c 300 "$tmp/put.out" 2>/dev/null)" ;;
  esac
}

put_once "$file" "$url"
put_once "$tmp/$name.sha256" "$url.sha256"

# Read BOTH back; the upload's own word is not evidence.
mkdir "$tmp/rb"
code="$(ncurl -o "$tmp/rb/$name" -w '%{http_code}' "$url")"; [ "$code" = 200 ] || err "read-back of $url answered HTTP ${code}."
code="$(ncurl -o "$tmp/rb/$name.sha256" -w '%{http_code}' "$url.sha256")"; [ "$code" = 200 ] || err "read-back of $url.sha256 answered HTTP ${code}."
(cd "$tmp/rb" && sha256sum -c "$name.sha256") || err "the published bytes do not match the published checksum."
cmp -s "$file" "$tmp/rb/$name" || err "the published bytes differ from the candidate."
if [ -n "$keep" ]; then mkdir -p "$keep" && cp "$tmp/rb/$name" "$keep/$name"; fi
echo "sha256=${sha}"
