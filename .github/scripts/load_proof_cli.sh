#!/usr/bin/env bash
# Prove a built ducklake extension loads into an OBTAINED duckdb CLI and
# round-trips an encrypted lake. Nothing here builds duckdb.
set -euo pipefail

CLI="$1"
EXT="$2"
TMP="$(mktemp -d)"
ROWS=1000
# Derived, never hand-copied: a changed ROWS that leaves EXPECTED stale would
# red the proof for a reason that has nothing to do with the extension.
EXPECTED="$ROWS,$(( ROWS * (ROWS - 1) / 2 ))"

# httpfs must be loaded alongside ducklake or an encrypted write dies on
# "DuckDB currently has a read-only crypto module loaded"
# (opvance/teras-ext-pgwire#84).
PRELUDE="INSTALL httpfs; LOAD httpfs; LOAD '$EXT';"

run() { "$CLI" -unsigned -noheader -csv -c "$1"; }

# The proof is only worth its name against the duckdb the pin NAMES.
want="$(cat "$(dirname "$0")/../duckdb-version")"
"$CLI" --version | grep -q "^$want " || {
  echo "CLI is $("$CLI" --version), not the pinned $want" >&2; exit 1; }

# Nothing below can be read as a proof unless BOTH extensions are really loaded.
loaded="$(run "$PRELUDE SELECT extension_name FROM duckdb_extensions() WHERE loaded AND extension_name IN ('ducklake','httpfs') ORDER BY 1;")"
test "$loaded" = "$(printf 'ducklake\nhttpfs')"
echo "loaded: $(echo "$loaded" | tr '\n' ' ')"

# Assert the bytes ON DISK, not duckdb's behaviour. Every other check here asks
# duckdb what it thinks of the file, so a defect that sets the encrypted footer
# flag over a plaintext payload passes them all. duckdb v1.5.5
# parquet_writer.cpp:513-519 writes "PARE" at the head of an encrypted file and
# "PAR1" at the head of a plaintext one, repeating the same four bytes at the
# tail (:1379-1383). Called in BOTH directions - PARE on the encrypted lake,
# PAR1 on the plaintext control - because a check that only ever sees PARE
# cannot show it is able to say PAR1.
check_magic() { # <dir> <PARE|PAR1>
  local n=0 f head tail
  # A `find | while` body is a SUBSHELL, so an exit there would not fail this
  # script. Read the list into the CURRENT shell instead.
  while IFS= read -r f; do
    head="$(head -c 4 "$f")"
    tail="$(tail -c 4 "$f")"
    test "$head" = "$2" || { echo "$f: head magic '$head', wanted '$2'" >&2; exit 1; }
    test "$tail" = "$2" || { echo "$f: tail magic '$tail', wanted '$2'" >&2; exit 1; }
    n=$((n + 1))
  done < <(find "$1" -name '*.parquet')
  test "$n" -gt 0 || { echo "check_magic found no parquet under $1" >&2; exit 1; }
  echo "magic bytes: $2 head and tail on $n file(s)"
}

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
check_magic "$TMP/enc" PARE

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
plain_written="$(build_lake "$TMP/plain" "$TMP/plain.db" '')"
test "$plain_written" = "$EXPECTED" || {
  echo "unencrypted write returned '$plain_written', wanted '$EXPECTED'" >&2; exit 1; }
check_magic "$TMP/plain" PAR1
plain="$(run "$PRELUDE SELECT count(*) FROM read_parquet('$TMP/plain/**/*.parquet');")"
test "$plain" = "$ROWS" || { echo "unencrypted keyless read returned '$plain'" >&2; exit 1; }
echo "positive control: the same read on an unencrypted lake returned $plain"

rm -rf "$TMP"
echo "LOAD PROOF PASSED against $("$CLI" --version)"
