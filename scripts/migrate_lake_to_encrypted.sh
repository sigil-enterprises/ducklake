#!/bin/sh
# Migrate an UNENCRYPTED DuckLake into a NEW ENCRYPTED one.
#
# WHY THIS EXISTS, AND WHY THERE IS NO FLAG INSTEAD
#
# `ENCRYPTED` is fixed at lake CREATION. Re-attaching an existing unencrypted
# lake with `ENCRYPTED` does not upgrade it, and does not quietly ignore the
# request - it is refused outright:
#
#   Invalid Input Error: Failed to set encryption - the database is not
#   encrypted but we requested an encrypted database
#
# So encrypting a lake that already holds data means copying that data into a
# lake created encrypted from the start. Any plan that says "enable ENCRYPTED on
# the existing lake" cannot be carried out as written.
#
# WHAT THIS DELIBERATELY DOES NOT DO
#
#   * It does not touch the source. Afterwards the old lake, its Parquet files,
#     its catalog, its WAL, its replicas, its backups and any object-store
#     versions still hold every byte IN PLAINTEXT. Encrypting the copy achieves
#     nothing until all of those are destroyed, and that destruction is a
#     separate deliberate act this script will not perform.
#   * It does not preserve snapshot history. The destination gets ONE snapshot
#     holding the current state; time travel over the source's history does not
#     come across.
#   * It is not a crypto-shred. See teras docs/risk-register.md RISK-001.
#
# Run it against a COPY of the catalog first and compare the printed row counts
# against the source before treating the source as disposable.
set -eu

DUCKDB=${DUCKDB:-duckdb}
EXT=${DUCKLAKE_EXTENSION:-}

usage() {
  echo "usage: $0 --src-meta P --src-data P --dst-meta P --dst-data P" >&2
  exit 2
}

SRC_META=; SRC_DATA=; DST_META=; DST_DATA=
while [ $# -gt 0 ]; do
  case "$1" in
    --src-meta) SRC_META=$2; shift 2 ;;
    --src-data) SRC_DATA=$2; shift 2 ;;
    --dst-meta) DST_META=$2; shift 2 ;;
    --dst-data) DST_DATA=$2; shift 2 ;;
    *) usage ;;
  esac
done
[ -n "$SRC_META" ] && [ -n "$SRC_DATA" ] && [ -n "$DST_META" ] && [ -n "$DST_DATA" ] || usage

load_ext() {
  [ -n "$EXT" ] && echo "LOAD '${EXT}';" || echo "INSTALL ducklake; LOAD ducklake;"
}

# 1. Enumerate the source tables. Ordered so a re-run copies in the same order,
#    which makes a partial failure resumable by inspection.
TABLES=$(
  { load_ext
    echo "ATTACH 'ducklake:${SRC_META}' AS src (DATA_PATH '${SRC_DATA}', READ_ONLY);"
    echo "SELECT schema_name || '.' || table_name FROM duckdb_tables()"
    echo "  WHERE database_name = 'src' AND internal = false"
    echo "  ORDER BY schema_name, table_name;"
  } | "$DUCKDB" -noheader -list -unsigned
)

if [ -z "$TABLES" ]; then
  echo "source lake holds no tables - nothing to migrate" >&2
  exit 1
fi

echo "tables to migrate:"
echo "$TABLES" | sed 's/^/  /'

# 2. Copy. The destination is created ENCRYPTED, so every data file it writes
#    gets its own key.
{
  load_ext
  echo "ATTACH 'ducklake:${SRC_META}' AS src (DATA_PATH '${SRC_DATA}', READ_ONLY);"
  echo "ATTACH 'ducklake:${DST_META}' AS dst (DATA_PATH '${DST_DATA}', ENCRYPTED);"
  echo "$TABLES" | while IFS= read -r t; do
    s=${t%%.*}; n=${t#*.}
    echo "CREATE SCHEMA IF NOT EXISTS dst.\"${s}\";"
    echo "CREATE TABLE dst.\"${s}\".\"${n}\" AS SELECT * FROM src.\"${s}\".\"${n}\";"
  done
} | "$DUCKDB" -unsigned

# 3. Reconcile. The destination is counted by READING ROWS, not by count(*):
#    a count(*) on a DuckLake table is answered from the catalog's record_count
#    and would not prove the encrypted files can actually be decrypted - which is
#    the one thing this migration has to establish.
echo
echo "reconciliation (source counted from catalog, destination by reading rows):"
{
  load_ext
  echo "ATTACH 'ducklake:${SRC_META}' AS src (DATA_PATH '${SRC_DATA}', READ_ONLY);"
  echo "ATTACH 'ducklake:${DST_META}' AS dst (DATA_PATH '${DST_DATA}');"
  echo "$TABLES" | while IFS= read -r t; do
    s=${t%%.*}; n=${t#*.}
    echo "SELECT '${t}' AS tbl,"
    echo "  (SELECT count(*) FROM src.\"${s}\".\"${n}\") AS src_rows,"
    echo "  (SELECT count(*) FROM (SELECT * FROM dst.\"${s}\".\"${n}\") x) AS dst_rows_read;"
  done
} | "$DUCKDB" -unsigned

# 4. Prove the destination is genuinely encrypted - every data file must carry a
#    key. A migration that silently produced plaintext files would be the exact
#    vacuous-green outcome this whole programme exists to prevent.
echo
echo "encryption check (every destination data file must carry a key):"
{
  echo "ATTACH '${DST_META}' AS meta (READ_ONLY);"
  echo "SELECT count(*) AS files,"
  echo "  count(*) FILTER (WHERE encryption_key IS NULL OR encryption_key = '') AS plaintext_files,"
  echo "  min(octet_length(from_base64(encryption_key))) AS min_key_bytes"
  echo "FROM meta.ducklake_data_file;"
} | "$DUCKDB" -unsigned

echo
echo "NOT DONE: the source still holds everything in plaintext - catalog, WAL,"
echo "replicas, backups and object-store versions. Encrypting the copy protects"
echo "nothing until those are destroyed. That is a separate deliberate act."
