#!/usr/bin/env python3
"""Positive control for verify_extension_asset.py.

A checker that asserts an ABSENCE reports the same "0 findings" whether it is
working or broken, so its clean run is worthless until it has been shown to go
red on a planted bad artifact. Each mutation below is a defect this fork has
actually shipped or nearly shipped, and each asserts WHICH check fired -- an
exit code alone cannot tell a real red from a crash.
"""

import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
VERIFY = HERE / "verify_extension_asset.py"
FOOTER_LEN = 512
SLOT_LEN = 32
# A value the build can never legitimately produce, so a mutation is never a no-op.
SKEW_VERSION = "v0.0.0-planted-skew"


def mutate_slot(data, index, value):
  out = bytearray(data)
  base = len(out) - FOOTER_LEN + index * SLOT_LEN
  out[base:base + SLOT_LEN] = value.encode().ljust(SLOT_LEN, b"\x00")
  return bytes(out)


def run(path, platform, version, load=False):
  cmd = [sys.executable, str(VERIFY), str(path), "--platform", platform, "--duckdb-version", version]
  if load:
    cmd.append("--load")
  return subprocess.run(cmd, capture_output=True, text=True)


def last_line(text):
  return (text.strip().splitlines() or ["<no stderr>"])[-1]


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

  with tempfile.TemporaryDirectory() as tmpdir:
    tmp = pathlib.Path(tmpdir)
    cases = [
      ("misnamed artifact", tmp / "renamed" / "ducklake.linux_amd64.duckdb_extension", data,
       "duckdb resolves the entrypoint from the file name"),
      ("missing artifact", tmp / "absent" / "ducklake.duckdb_extension", None, "does not exist"),
      ("empty artifact", tmp / "empty" / "ducklake.duckdb_extension", b"", "is empty"),
      ("not an extension", tmp / "garbage" / "ducklake.duckdb_extension", b"\x00" * 4096, "footer ABI slot"),
      ("wrong platform in footer", tmp / "windows" / "ducklake.duckdb_extension",
       mutate_slot(data, 6, "windows_amd64_mingw"), "platform is"),
      ("duckdb version skew in footer", tmp / "skewed" / "ducklake.duckdb_extension",
       mutate_slot(data, 5, SKEW_VERSION), "built against duckdb"),
      ("object format is not ELF", tmp / "notelf" / "ducklake.duckdb_extension",
       b"MZ" + data[2:], "not a 64-bit little-endian ELF"),
      ("ELF built for the wrong machine", tmp / "arm" / "ducklake.duckdb_extension",
       data[:18] + b"\xb7\x00" + data[20:], "e_machine"),
    ]
    for name, path, content, expected in cases:
      path.parent.mkdir(parents=True, exist_ok=True)
      if content is not None:
        path.write_bytes(content)
      res = run(path, platform, version)
      if res.returncode == 0 or expected not in res.stderr:
        failures.append(f"{name}: expected a red mentioning {expected!r}, "
                        f"got rc={res.returncode} {last_line(res.stderr)!r}")
      else:
        print(f"red as required -- {name}: {last_line(res.stderr)}")

    # Footer and expectation agree here, so only the load probe can catch that
    # the installed runtime is not the one the artifact was built against.
    runtime = tmp / "runtime-mismatch" / "ducklake.duckdb_extension"
    runtime.parent.mkdir(parents=True, exist_ok=True)
    runtime.write_bytes(mutate_slot(data, 5, SKEW_VERSION))
    res = run(runtime, platform, SKEW_VERSION, load=True)
    if res.returncode == 0 or "installed duckdb is" not in res.stderr:
      failures.append(f"runtime mismatch: expected a red from the load probe, "
                      f"got rc={res.returncode} {last_line(res.stderr)!r}")
    else:
      print(f"red as required -- runtime mismatch: {last_line(res.stderr)}")

    # Footer, platform and expectation all agree and the body is corrupt, so
    # LOAD is the only check left that can catch it.
    unloadable = tmp / "unloadable" / "ducklake.duckdb_extension"
    unloadable.parent.mkdir(parents=True, exist_ok=True)
    unloadable.write_bytes(data[:24] + b"\x00" * 1000 + data[1024:])
    res = run(unloadable, platform, version, load=True)
    if res.returncode == 0 or "LOAD refused the artifact" not in res.stderr:
      failures.append(f"unloadable body: expected a red from LOAD itself, "
                      f"got rc={res.returncode} {last_line(res.stderr)!r}")
    else:
      print(f"red as required -- unloadable body: {last_line(res.stderr)}")

  # Control: the unmutated artifact must pass the full path including LOAD, or
  # every red above is vacuous.
  res = run(good, platform, version, load=True)
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
