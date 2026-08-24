"""Prove a built ducklake extension loads into an OBTAINED duckdb and round-trips
an encrypted lake. Compilation is not loadability, so nothing here builds duckdb:
the module under test is whatever `import duckdb` resolves to."""

import glob
import os
import shutil
import sys
import tempfile

import duckdb


def connect(tmp, ext):
  con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
  # httpfs must be loaded alongside ducklake or an encrypted write dies on
  # "DuckDB currently has a read-only crypto module loaded"
  # (opvance/teras-ext-pgwire#84).
  con.execute("INSTALL httpfs; LOAD httpfs;")
  con.execute(f"LOAD '{ext}'")
  return con


def main(ext):
  print("duckdb module:", duckdb.__version__, "from", duckdb.__file__)
  tmp = tempfile.mkdtemp(prefix="loadproof-")
  meta = os.path.join(tmp, "proof.db")
  data = os.path.join(tmp, "lakefiles")
  os.makedirs(data)

  con = connect(tmp, ext)
  loaded = con.execute(
    "SELECT extension_name, loaded FROM duckdb_extensions() "
    "WHERE extension_name IN ('ducklake', 'httpfs') ORDER BY 1"
  ).fetchall()
  print("extensions:", loaded)
  assert ("ducklake", True) in loaded, loaded
  assert ("httpfs", True) in loaded, loaded

  con.execute(
    f"ATTACH 'ducklake:{meta}' AS lake "
    f"(DATA_PATH '{data}/', ENCRYPTED, DATA_INLINING_ROW_LIMIT 0)"
  )
  con.execute("CREATE TABLE lake.t AS SELECT range AS x FROM range(1000)")
  before = con.execute("SELECT count(*), sum(x) FROM lake.t").fetchone()
  print("written:", before)
  con.execute("DETACH lake")
  con.close()

  con = connect(tmp, ext)
  con.execute(f"ATTACH 'ducklake:{meta}' AS lake2")
  after = con.execute("SELECT count(*), sum(x) FROM lake2.t").fetchone()
  print("after re-attach:", after)
  assert after == before == (1000, 499500), (before, after)

  files = glob.glob(os.path.join(data, "**", "*.parquet"), recursive=True)
  print("parquet files:", len(files))
  assert files, "no data files were written, so nothing was encrypted"

  # The per-file keys live in the metadata catalog. A read that never opens it
  # holds no key and MUST fail; succeeding would mean the lake is plaintext.
  try:
    n = con.execute(
      f"SELECT count(*) FROM read_parquet('{data}/**/*.parquet')"
    ).fetchone()
    raise AssertionError(f"keyless read of an encrypted file succeeded: {n}")
  except duckdb.Error as e:
    print("keyless read refused, as it must be:", type(e).__name__, str(e)[:200])

  con.close()
  shutil.rmtree(tmp)
  print("LOAD PROOF PASSED against duckdb", duckdb.__version__)


if __name__ == "__main__":
  main(os.path.abspath(sys.argv[1]))
