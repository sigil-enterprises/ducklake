#!/usr/bin/env bash
# Prove a built ducklake extension loads into an OBTAINED duckdb CLI and
# round-trips an encrypted lake. Nothing here builds duckdb.
set -euo pipefail

CLI="$1"
EXT="$2"
TMP="$(mktemp -d)"
ROWS=1000
EXPECTED="1000,499500"

# httpfs must be loaded alongside ducklake or an encrypted write dies on
# "DuckDB currently has a read-only crypto module loaded"
# (opvance/teras-ext-pgwire#84).
PRELUDE="INSTALL httpfs; LOAD httpfs; LOAD '$EXT';"

run() { "$CLI" -unsigned -noheader -csv -c "$1"; }

"$CLI" --version

# Nothing below can be read as a proof unless BOTH extensions are really loaded.
loaded="$(run "$PRELUDE SELECT extension_name FROM duckdb_extensions() WHERE loaded AND extension_name IN ('ducklake','httpfs') ORDER BY 1;")"
test "$loaded" = "$(printf 'ducklake\nhttpfs')"
echo "loaded: $(echo "$loaded" | tr '\n' ' ')"

build_lake() { # <dir> <db> <ENCRYPTED,|>
  mkdir -p "$1"
  run "$PRELUDE
       ATTACH 'ducklake:$2' AS lake (DATA_PATH '$1/', $3 DATA_INLINING_ROW_LIMIT 0);
       CREATE TABLE lake.t AS SELECT range AS x FROM range($ROWS);
       SELECT count(*), sum(x) FROM lake.t;
       DETACH lake;"
}

# --- the lake under test: encrypted ---
written="$(build_lake "$TMP/enc" "$TMP/enc.db" 'ENCRYPTED,')"
test "$written" = "$EXPECTED" || { echo "write returned '$written', wanted '$EXPECTED'" >&2; exit 1; }
test "$(find "$TMP/enc" -name '*.parquet' | wc -l)" -gt 0

after="$(run "$PRELUDE ATTACH 'ducklake:$TMP/enc.db' AS lake2; SELECT count(*), sum(x) FROM lake2.t;")"
test "$after" = "$EXPECTED" || { echo "re-attach returned '$after', wanted '$EXPECTED'" >&2; exit 1; }
echo "re-attach read back: $after"

# The per-file keys live in the metadata catalog, so a read that never opens it
# holds no key. It MUST fail, and fail BECAUSE the file is encrypted - a bare
# non-zero exit would also be produced by a typo or a missing extension.
if err="$(run "$PRELUDE SELECT count(*) FROM read_parquet('$TMP/enc/**/*.parquet');" 2>&1)"; then
  echo "FAIL: keyless read of the ENCRYPTED lake returned '$err'" >&2
  exit 1
fi
grep -q "is encrypted, but 'encryption_config' was not set" <<<"$err" \
  || { echo "keyless read failed, but not because the file is encrypted: $err" >&2; exit 1; }
echo "keyless read refused, as it must be"

# --- positive control: the SAME read must SUCCEED on an unencrypted lake ---
# Without it, a lake silently written in plaintext still looks green, because
# the refusal above would simply never be reached.
build_lake "$TMP/plain" "$TMP/plain.db" '' > /dev/null
plain="$(run "$PRELUDE SELECT count(*) FROM read_parquet('$TMP/plain/**/*.parquet');")"
test "$plain" = "$ROWS" || { echo "unencrypted keyless read returned '$plain'" >&2; exit 1; }
echo "positive control: the same read on an unencrypted lake returned $plain"

rm -rf "$TMP"
echo "LOAD PROOF PASSED against $("$CLI" --version)"
