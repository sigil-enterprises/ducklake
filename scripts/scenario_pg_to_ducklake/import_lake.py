"""
Import a NATIVE POSTGRES database into an ENCRYPTED DuckLake, then verify it.

This is the sigil-side feature. The deployment that runs it - and the key dance
around the KEK - is declared in opvance.

Two properties this refuses to take on trust:

  * that the rows arrived. Counted per table, and the DESTINATION is counted by
    READING ROWS. `count(*)` on a DuckLake table is answered from the catalog's
    `record_count` and never opens a Parquet file, so it would report success on
    a lake whose data cannot be decrypted.
  * that the lake is actually encrypted. Every data file must carry a key, and
    the key must be 32 bytes. A lake that quietly wrote plaintext files while
    reporting success is the exact failure this programme exists to prevent.
"""

import os
import sys

import duckdb


def main() -> None:
    ext = os.environ["DUCKLAKE_EXTENSION"]
    src_dsn = os.environ["SOURCE_PG_DSN"]
    catalog = os.environ["LAKE_CATALOG_DSN"]
    data_path = os.environ["LAKE_DATA_PATH"]
    tables = [t for t in os.environ["IMPORT_TABLES"].split(",") if t]

    con = duckdb.connect(config={"allow_unsigned_extensions": True})
    con.execute(f"LOAD '{ext}'")
    con.execute("INSTALL postgres; LOAD postgres")

    # The native Postgres holding the source data.
    con.execute(f"ATTACH '{src_dsn}' AS src (TYPE POSTGRES, READ_ONLY)")

    # The lake. ENCRYPTED is given at CREATION and cannot be added later - an
    # existing unencrypted lake re-attached with ENCRYPTED is refused outright
    # ("Failed to set encryption - the database is not encrypted but we
    # requested an encrypted database"). So a lake that must be encrypted has to
    # be created that way from the first attach.
    con.execute(
        f"ATTACH 'ducklake:postgres:{catalog}' AS lake "
        f"(DATA_PATH '{data_path}', ENCRYPTED)"
    )

    print("== importing native Postgres -> encrypted DuckLake ==")
    for t in tables:
        con.execute(f'CREATE OR REPLACE TABLE lake.main."{t}" AS SELECT * FROM src.public."{t}"')
        print(f"  imported {t}")

    print("\n== row reconciliation (destination counted by READING rows) ==")
    failed = False
    for t in tables:
        src_n = con.execute(f'SELECT count(*) FROM src.public."{t}"').fetchone()[0]
        dst_n = con.execute(
            f'SELECT count(*) FROM (SELECT * FROM lake.main."{t}") x'
        ).fetchone()[0]
        status = "ok" if src_n == dst_n else "MISMATCH"
        if src_n != dst_n:
            failed = True
        print(f"  {t:<20} source={src_n:<8} lake_rows_read={dst_n:<8} {status}")

    print("\n== encryption check (every data file must carry a 32-byte key) ==")
    # The catalog tables are NOT under the lake alias. DuckLake exposes them in a
    # side schema named `__ducklake_metadata_<alias>` - with a Postgres catalog
    # that is where `ducklake_data_file` actually lives, and querying
    # `lake.ducklake_data_file` fails outright.
    files, plaintext, min_key = con.execute(
        "SELECT count(*), "
        "count(*) FILTER (WHERE encryption_key IS NULL OR encryption_key = ''), "
        "min(octet_length(from_base64(encryption_key))) "
        "FROM __ducklake_metadata_lake.ducklake_data_file"
    ).fetchone()
    print(f"  files={files}  plaintext_files={plaintext}  min_key_bytes={min_key}")

    if files == 0 or plaintext != 0 or min_key != 32:
        print("  ENCRYPTION CHECK FAILED", file=sys.stderr)
        failed = True

    if failed:
        raise SystemExit(1)
    print("\nimport verified: rows reconcile and every data file is 256-bit encrypted")


if __name__ == "__main__":
    main()
