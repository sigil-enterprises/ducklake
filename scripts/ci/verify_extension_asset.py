#!/usr/bin/env python3
"""Verify a built .duckdb_extension is the artifact a consumer can actually load.

A green build job proves nothing about the file it produced: the fork has
shipped a Windows DLL and a dev-SHA build under names that looked correct.
Every assertion here is on the BYTES, and --load runs a real LOAD.
"""

import argparse
import pathlib
import subprocess
import sys

# The extension footer is 512 trailing bytes of 32-byte null-padded slots.
FOOTER_LEN = 512
SLOT_LEN = 32
SLOT_ABI_TYPE = 3
SLOT_EXTENSION_VERSION = 4
SLOT_DUCKDB_VERSION = 5
SLOT_PLATFORM = 6

ELF_X86_64 = b"\x7fELF\x02\x01"


def fail(msg):
  print(f"FAIL: {msg}", file=sys.stderr)
  sys.exit(1)


def read_footer(path):
  data = path.read_bytes()
  if len(data) < FOOTER_LEN:
    fail(f"{path} is {len(data)} bytes, too short to carry an extension footer")
  footer = data[-FOOTER_LEN:]
  return [
    footer[i * SLOT_LEN:(i + 1) * SLOT_LEN].rstrip(b"\x00").decode("utf-8", "replace")
    for i in range(FOOTER_LEN // SLOT_LEN)
  ]


def main():
  ap = argparse.ArgumentParser()
  ap.add_argument("path", type=pathlib.Path)
  ap.add_argument("--platform", required=True)
  ap.add_argument("--duckdb-version", required=True, help="footer version string, e.g. v1.5.3")
  ap.add_argument("--load", action="store_true", help="LOAD it in the installed duckdb")
  args = ap.parse_args()

  if not args.path.is_file():
    fail(f"{args.path} does not exist -- a missing artifact is a failure, never a skip")
  if args.path.stat().st_size == 0:
    fail(f"{args.path} is empty")

  slots = read_footer(args.path)
  abi, ext_version, duckdb_version, platform = (
    slots[SLOT_ABI_TYPE], slots[SLOT_EXTENSION_VERSION],
    slots[SLOT_DUCKDB_VERSION], slots[SLOT_PLATFORM],
  )
  print(f"footer: abi={abi!r} extension={ext_version!r} duckdb={duckdb_version!r} platform={platform!r}")

  if abi not in ("CPP", "C_STRUCT"):
    fail(f"footer ABI slot is {abi!r}; this file is not a duckdb extension")
  if platform != args.platform:
    fail(f"platform is {platform!r}, expected {args.platform!r}")
  if duckdb_version != args.duckdb_version:
    fail(f"built against duckdb {duckdb_version!r}, expected {args.duckdb_version!r}; "
         "an ABI-mismatched extension cannot be loaded by the consumer")

  if args.platform == "linux_amd64":
    head = args.path.read_bytes()[:6]
    if head != ELF_X86_64:
      fail(f"leading bytes {head!r} are not a 64-bit little-endian ELF; "
           "linux_amd64 asset is the wrong object format")
    which = subprocess.run(["file", "-b", str(args.path)], capture_output=True, text=True)
    print(f"file: {which.stdout.strip() or which.stderr.strip()}")

  if args.load:
    import duckdb
    installed = f"v{duckdb.__version__}"
    if installed != args.duckdb_version:
      fail(f"installed duckdb is {installed}, expected {args.duckdb_version}; "
           "loading against the wrong runtime proves nothing")
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{args.path.resolve()}'")
    loaded = con.execute(
      "SELECT loaded FROM duckdb_extensions() WHERE extension_name = 'ducklake'"
    ).fetchone()
    if not loaded or not loaded[0]:
      fail("LOAD returned without error but ducklake is not reported loaded")
    print(f"LOAD OK in duckdb {installed}")

  print(f"OK: {args.path}")


if __name__ == "__main__":
  main()
