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
#        nexus_publish.sh --selftest   (positive controls against a local stub)
set -euo pipefail

if [ "${1-}" = --selftest ]; then
  # Runs THIS script as a subprocess against an in-memory HTTP stub, so each
  # refusal and each completion path is exercised on the real code.
  self="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
  st="$(mktemp -d)"; pid=""
  trap '[ -n "$pid" ] && kill "$pid" 2>/dev/null; rm -rf "$st"' EXIT
  printf 'candidate bytes\n' > "$st/a.bin"
  shaline="$( (cd "$st" && sha256sum a.bin) )"
  cat > "$st/stub.py" <<'STUB'
import http.server, sys
store = {"/diff/a.bin": b"OTHER bytes\n", "/half/a.bin": open(sys.argv[2], "rb").read()}
puts = {}
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, body=b""):
        self.send_response(code); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        if self.path.startswith("/__puts/"): return self.send(200, str(puts.get(self.path[7:], 0)).encode())
        if self.path.startswith("/err5/"): return self.send(503, b"down")
        b = store.get(self.path)
        self.send(200, b) if b is not None else self.send(404)
    def do_PUT(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if self.path.startswith("/redir/"):
            self.send_response(302); self.send_header("Location", "/login"); self.send_header("Content-Length", "0"); self.end_headers(); return
        puts[self.path] = puts.get(self.path, 0) + 1; store[self.path] = body; self.send(201)
s = http.server.HTTPServer(("127.0.0.1", 0), H)
open(sys.argv[1], "w").write(str(s.server_port))
s.serve_forever()
STUB
  python3 "$st/stub.py" "$st/port.tmp" "$st/a.bin" & pid=$!; disown "$pid"
  for _ in $(seq 100); do [ -s "$st/port.tmp" ] && break; sleep 0.1; done
  [ -s "$st/port.tmp" ] || { echo "::error::selftest stub did not start"; exit 1; }
  base="http://127.0.0.1:$(cat "$st/port.tmp")"
  export NEXUS_USERNAME="self\"test\\" NEXUS_PASSWORD=x   # exercises the \\ and " escaping
  fails=0
  run() { bash "$self" "$st/a.bin" "$base/$1/a.bin" 2>&1; }
  fail() { echo "FAIL $*"; fails=$((fails+1)); }
  expect_refuse() { # PREFIX FRAGMENT
    local out rc=0; out="$(run "$1")" || rc=$?
    if [ "$rc" != 0 ] && grep -qF -- "$2" <<<"$out"; then echo "ok   refuse $1 ($2)"; else fail "$1 want refusal '$2', rc=$rc: $out"; fi
  }
  expect_refuse diff  "already holds DIFFERENT bytes"
  expect_refuse redir "answered HTTP 302; not a success"
  expect_refuse err5  "answered HTTP 503; neither present nor absent"
  # A run that died between the two PUTs: the asset exists, the checksum does
  # not. A re-run must complete the checksum, not skip it.
  out="$(run half)" || fail "half rc!=0: $out"
  got="$(curl -sS "$base/half/a.bin.sha256")"
  if [ "$got" = "$shaline" ] && [ "$(curl -sS "$base/__puts/half/a.bin")" = 0 ]; then echo "ok   missing .sha256 completed, asset untouched"
  else fail "half: sha256 file '$got' (want '$shaline'), asset puts=$(curl -sS "$base/__puts/half/a.bin")"; fi
  # First publish writes both; a second identical publish writes nothing.
  out="$(run ok)" || fail "ok first rc!=0: $out"
  out="$(run ok)" || fail "ok second rc!=0: $out"
  p1="$(curl -sS "$base/__puts/ok/a.bin")"; p2="$(curl -sS "$base/__puts/ok/a.bin.sha256")"
  if [ "$p1" = 1 ] && [ "$p2" = 1 ] && grep -q "not re-uploading" <<<"$out"; then echo "ok   second identical publish is a no-op"
  else fail "ok: puts asset=$p1 sha=$p2 after two publishes: $out"; fi
  [ "$fails" = 0 ] || { echo "::error::nexus_publish.sh selftest: ${fails} case(s) failed"; exit 1; }
  echo "nexus_publish.sh selftest: all cases hold"; exit 0
fi

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
