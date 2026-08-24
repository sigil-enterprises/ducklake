"""Prove a built ducklake extension loads into an OBTAINED duckdb and round-trips
an encrypted lake. Compilation is not loadability, so nothing here builds duckdb:
the module under test is whatever `import duckdb` resolves to."""

import glob
import os
import shutil
import sys
import tempfile

import duckdb

ROWS = 1000
EXPECTED = (ROWS, ROWS * (ROWS - 1) // 2)


def check(cond, what):
  # Not `assert`: PYTHONOPTIMIZE would strip every check and leave this
  # script printing its way to a green exit 0.
  if not cond:
    raise SystemExit(f"LOAD PROOF FAILED: {what}")


def magic(path):
  """The first and last four bytes of a parquet file.

  duckdb v1.5.5 parquet_writer.cpp:513-519 writes "PARE" at the head of an
  encrypted file and "PAR1" at the head of a plaintext one, and repeats the
  same four bytes at the tail (:1379-1383).
  """
  size = os.path.getsize(path)
  with open(path, "rb") as f:
    head = f.read(4)
    f.seek(size - 4)
    tail = f.read(4)
  return head, tail


def check_magic(files, want):
  """Assert the bytes ON DISK, not duckdb's behaviour.

  Every other check here asks duckdb what it thinks of the file, so a defect
  that sets the encrypted footer flag over a plaintext payload passes them all.
  These are the bytes that actually leave the hospital's hardware. Asserted in
  BOTH directions - PARE on the encrypted lake, PAR1 on the plaintext control -
  because a check that only ever sees PARE cannot show it is able to say PAR1.
  """
  check(bool(files), "check_magic called with no files")
  for f in files:
    head, tail = magic(f)
    check(head == want, f"{f}: head magic {head!r}, wanted {want!r}")
    check(tail == want, f"{f}: tail magic {tail!r}, wanted {want!r}")
  print("magic bytes:", want.decode(), "head and tail on", len(files), "file(s)")


def connect(ext):
  con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
  # httpfs must be loaded alongside ducklake or an encrypted write dies on
  # "DuckDB currently has a read-only crypto module loaded"
  # (opvance/teras-ext-pgwire#84). Its own failures must never be mistaken for
  # the keyless-read refusal below, so it is loaded here and checked here.
  con.execute("INSTALL httpfs; LOAD httpfs;")
  con.execute(f"LOAD '{ext}'")
  loaded = dict(con.execute(
    "SELECT extension_name, loaded FROM duckdb_extensions() "
    "WHERE extension_name IN ('ducklake', 'httpfs')"
  ).fetchall())
  check(loaded.get("ducklake") is True, f"ducklake not loaded: {loaded}")
  check(loaded.get("httpfs") is True, f"httpfs not loaded: {loaded}")
  return con


def build_lake(ext, data, meta, encrypted):
  opts = "ENCRYPTED, " if encrypted else ""
  con = connect(ext)
  con.execute(
    f"ATTACH 'ducklake:{meta}' AS lake "
    f"(DATA_PATH '{data}/', {opts}DATA_INLINING_ROW_LIMIT 0)"
  )
  con.execute(f"CREATE TABLE lake.t AS SELECT range AS x FROM range({ROWS})")
  written = con.execute("SELECT count(*), sum(x) FROM lake.t").fetchone()
  con.execute("DETACH lake")
  con.close()
  check(written == EXPECTED, f"write returned {written}, wanted {EXPECTED}")

  files = glob.glob(os.path.join(data, "**", "*.parquet"), recursive=True)
  check(bool(files), f"no data files written under {data}")
  check_magic(files, b"PARE" if encrypted else b"PAR1")
  return files


def keyless_read(ext, data):
  """Read the data files with no metadata catalog open, so with no key."""
  con = connect(ext)
  try:
    return "ok", con.execute(
      f"SELECT count(*) FROM read_parquet('{data}/**/*.parquet')"
    ).fetchone()
  except duckdb.Error as e:
    return "error", str(e)
  finally:
    con.close()


def main(ext):
  print("duckdb module:", duckdb.__version__, "from", duckdb.__file__)
  tmp = tempfile.mkdtemp(prefix="loadproof-")

  # --- the lake under test: encrypted ---
  enc = os.path.join(tmp, "enc")
  os.makedirs(enc)
  files = build_lake(ext, enc, os.path.join(tmp, "enc.db"), encrypted=True)
  print("encrypted lake wrote", len(files), "parquet files")

  con = connect(ext)
  con.execute(f"ATTACH 'ducklake:{os.path.join(tmp, 'enc.db')}' AS lake2")
  after = con.execute("SELECT count(*), sum(x) FROM lake2.t").fetchone()
  con.close()
  check(after == EXPECTED, f"re-attach returned {after}, wanted {EXPECTED}")
  print("re-attach read back:", after)

  kind, detail = keyless_read(ext, enc)
  check(kind == "error", f"keyless read of the ENCRYPTED lake succeeded: {detail}")
  # A bare "some error" would pass on a typo'd path or a missing extension, so
  # the message must actually be duckdb refusing an encrypted file.
  check("encrypted" in detail and "encryption_config" in detail,
        f"keyless read failed, but not because the file is encrypted: {detail}")
  print("keyless read refused, as it must be:", detail.splitlines()[0])

  # --- positive control: the SAME read must SUCCEED on an unencrypted lake ---
  # Without this, a lake silently written in plaintext would still look green,
  # because the refusal above would simply never be reached.
  plain = os.path.join(tmp, "plain")
  os.makedirs(plain)
  build_lake(ext, plain, os.path.join(tmp, "plain.db"), encrypted=False)
  kind, detail = keyless_read(ext, plain)
  check(kind == "ok", f"keyless read of the UNENCRYPTED lake failed: {detail}")
  check(detail == (ROWS,), f"unencrypted keyless read returned {detail}")
  print("positive control: the same read on an unencrypted lake returned", detail)

  shutil.rmtree(tmp)
  print("LOAD PROOF PASSED against duckdb", duckdb.__version__)


if __name__ == "__main__":
  main(os.path.abspath(sys.argv[1]))
