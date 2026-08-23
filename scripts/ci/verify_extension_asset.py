#!/usr/bin/env python3
"""Verify a built .duckdb_extension is the artifact a consumer can actually load.

A green build job proves nothing about the file it produced: this fork has
shipped a Windows DLL and a dev-SHA build under names that looked correct.
Every assertion here is on the BYTES, and --load runs a real LOAD.
"""

import argparse
import pathlib
import struct
import subprocess
import sys

# The extension footer is 512 trailing bytes of 32-byte null-padded slots.
FOOTER_LEN = 512
SLOT_LEN = 32
SLOT_ABI_TYPE = 3
SLOT_EXTENSION_VERSION = 4
SLOT_DUCKDB_VERSION = 5
SLOT_PLATFORM = 6

ELF64_LE = b"\x7fELF\x02\x01"
ELF_MACHINE_X86_64 = 0x3E
ELF_E_MACHINE_OFFSET = 18


def fail(msg):
  print(f"FAIL: {msg}", file=sys.stderr)
  sys.exit(1)


def read_footer(data, path):
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
  ap.add_argument("--extension-name", default="ducklake")
  args = ap.parse_args()

  # duckdb derives the init symbol from the FILE NAME, so a published asset
  # renamed to carry its platform is refused at LOAD with an entrypoint error.
  expected_name = f"{args.extension_name}.duckdb_extension"
  if args.path.name != expected_name:
    fail(f"asset is named {args.path.name!r}; duckdb resolves the entrypoint from the "
         f"file name, so it must be {expected_name!r}")

  if not args.path.is_file():
    fail(f"{args.path} does not exist -- a missing artifact is a failure, never a skip")
  data = args.path.read_bytes()
  if not data:
    fail(f"{args.path} is empty")

  slots = read_footer(data, args.path)
  abi, ext_version, duckdb_version, platform = (
    slots[SLOT_ABI_TYPE], slots[SLOT_EXTENSION_VERSION],
    slots[SLOT_DUCKDB_VERSION], slots[SLOT_PLATFORM],
  )
  print(f"footer: abi={abi!r} extension={ext_version!r} duckdb={duckdb_version!r} platform={platform!r}")

  # Only a CPP extension puts the duckdb version in slot 5; a C_STRUCT one puts
  # its capi version there, so the comparison below would be against the wrong field.
  if abi != "CPP":
    fail(f"footer ABI slot is {abi!r}, expected 'CPP'; this is not a ducklake fork build")
  if platform != args.platform:
    fail(f"platform is {platform!r}, expected {args.platform!r}")
  if duckdb_version != args.duckdb_version:
    fail(f"built against duckdb {duckdb_version!r}, expected {args.duckdb_version!r}; "
         "an ABI-mismatched extension cannot be loaded by the consumer")

  if args.platform == "linux_amd64":
    if data[:6] != ELF64_LE:
      fail(f"leading bytes {data[:6]!r} are not a 64-bit little-endian ELF; "
           "linux_amd64 asset is the wrong object format")
    machine = struct.unpack_from("<H", data, ELF_E_MACHINE_OFFSET)[0]
    if machine != ELF_MACHINE_X86_64:
      fail(f"ELF e_machine is {machine:#x}, expected {ELF_MACHINE_X86_64:#x} (x86-64)")
    described = subprocess.run(["file", "-b", str(args.path)], capture_output=True, text=True).stdout.strip()
    print(f"file: {described}")
    if "x86-64" not in described:
      fail(f"file(1) reports {described!r}, which is not an x86-64 object")

  if args.load:
    import duckdb
    installed = f"v{duckdb.__version__}"
    if installed != args.duckdb_version:
      fail(f"installed duckdb is {installed}, expected {args.duckdb_version}; "
           "loading against the wrong runtime proves nothing")
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    try:
      con.execute(f"LOAD '{args.path.resolve()}'")
    except Exception as exc:
      fail(f"LOAD refused the artifact in duckdb {installed}: {type(exc).__name__}: {exc}")
    loaded = con.execute(
      "SELECT loaded FROM duckdb_extensions() WHERE extension_name = 'ducklake'"
    ).fetchone()
    if not loaded or not loaded[0]:
      fail("LOAD returned without error but ducklake is not reported loaded")
    print(f"LOAD OK in duckdb {installed}")

  print(f"OK: {args.path}")


if __name__ == "__main__":
  main()
