#!/usr/bin/env python3
"""Positive control for verify_extension_asset.py.

A checker that asserts an ABSENCE reports the same "0 findings" whether it is
working or broken, so its clean run is worthless until it has been shown to go
red on a planted bad artifact. Each mutation below is a defect this fork has
actually shipped or nearly shipped.
"""

import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
VERIFY = HERE / "verify_extension_asset.py"
FOOTER_LEN = 512
SLOT_LEN = 32
# Any value the build can never legitimately produce, so a mutation is never a no-op.
SKEW_VERSION = "v0.0.0-planted-skew"


def mutate_slot(data, index, value):
  out = bytearray(data)
  base = len(out) - FOOTER_LEN + index * SLOT_LEN
  out[base:base + SLOT_LEN] = value.encode().ljust(SLOT_LEN, b"\x00")
  return bytes(out)


def run(path, platform, version, load):
  cmd = [sys.executable, str(VERIFY), str(path), "--platform", platform, "--duckdb-version", version]
  if load:
    cmd.append("--load")
  return subprocess.run(cmd, capture_output=True, text=True)


def main():
  if len(sys.argv) != 4:
    print("usage: selftest.py <good-artifact> <platform> <duckdb-version>", file=sys.stderr)
    return 2
  good, platform, version = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
  if version == SKEW_VERSION:
    print(f"expected version must differ from the planted skew {SKEW_VERSION}", file=sys.stderr)
    return 2
  data = good.read_bytes()
  failures = []

  with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    cases = {
      "missing artifact": (tmp / "absent.duckdb_extension", None),
      "empty artifact": (tmp / "empty.duckdb_extension", b""),
      "not an extension": (tmp / "garbage.duckdb_extension", b"\x00" * 4096),
      "wrong platform in footer": (
        tmp / "windows.duckdb_extension", mutate_slot(data, 6, "windows_amd64_mingw")),
      "duckdb version skew in footer": (
        tmp / "skewed.duckdb_extension", mutate_slot(data, 5, SKEW_VERSION)),
      "object format is not ELF": (
        tmp / "notelf.duckdb_extension", b"MZ" + data[2:]),
    }
    for name, (path, content) in cases.items():
      if content is not None:
        path.write_bytes(content)
      res = run(path, platform, version, load=False)
      if res.returncode == 0:
        failures.append(f"{name}: verifier PASSED a bad artifact")
      else:
        print(f"red as required -- {name}: {res.stderr.strip().splitlines()[-1]}")

    # Footer and expectation agree here, so only the load probe can catch that
    # the installed runtime is not the one the artifact was built against.
    runtime = tmp / "runtime-mismatch.duckdb_extension"
    runtime.write_bytes(mutate_slot(data, 5, SKEW_VERSION))
    res = run(runtime, platform, SKEW_VERSION, load=True)
    if res.returncode == 0:
      failures.append("runtime mismatch: verifier PASSED a load against the wrong duckdb")
    else:
      print(f"red as required -- runtime mismatch: {res.stderr.strip().splitlines()[-1]}")

  # Control: the unmutated artifact must still pass, or every red above is vacuous.
  res = run(good, platform, version, load=False)
  if res.returncode != 0:
    failures.append(f"control: verifier REJECTED the good artifact: {res.stderr.strip()}")

  for f in failures:
    print(f"SELFTEST FAIL: {f}", file=sys.stderr)
  if failures:
    return 1
  print("selftest OK: verifier reds on every planted defect and passes the good artifact")
  return 0


if __name__ == "__main__":
  sys.exit(main())
