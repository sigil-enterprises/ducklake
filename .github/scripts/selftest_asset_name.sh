#!/usr/bin/env bash
#
# POSITIVE CONTROL for the PUBLISHED ASSET'S NAME.
#
# duckdb does not read the entrypoint out of the binary. It DERIVES the symbol
# it dlsyms from the FILE NAME: everything up to the first `.` of the basename,
# plus `_duckdb_cpp_init`. So a perfectly good, provider-linked extension is
# UNLOADABLE if it is published under a name whose first dot-field is not
# `ducklake`. Measured on run 32765364900, job 97559586694, against the asset
# this pipeline had just attached to v0.2.0-rc.4:
#
#   IO Error: Extension 'ducklake-v0.2.0-rc.4-duckdb-v1.5.5-linux_amd64.duckdb_extension'
#             did not contain the expected entrypoint function 'ducklake-v0_duckdb_cpp_init'
#
# The name carried the platform, as ducklake#33 asks, and was still useless to
# the consumer. Symbols are not loadability and neither is a good name: this
# script proves BOTH verdicts with ONE binary, so the green is attributable to
# the naming rule and to nothing else about the file.
#
# Usage: selftest_asset_name.sh DUCKDB_CLI LOADABLE_DUCKLAKE_EXTENSION
set -euo pipefail

cli="$1"
src="$2"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fails=0

try_load() { # <path> -> exit status, output on stdout
  "$cli" -unsigned -noheader -csv -c "LOAD '$1'; SELECT 1;" 2>&1
}

# GOOD: first dot-field is exactly `ducklake`, and the platform still rides in
# the name. This is the shape the pipeline publishes.
good="$tmp/ducklake.linux_amd64.v9.9.9.duckdb_extension"
cp "$src" "$good"
if out="$(try_load "$good")"; then
  printf 'PASS  ducklake.<platform>.<tag>.duckdb_extension LOADs\n'
else
  printf 'FAIL  the naming scheme this pipeline publishes does NOT load: %s\n' "$out"
  printf '::error::asset-name self-test: the published naming scheme does not load\n'
  fails=$((fails + 1))
fi

# BAD: the shape that shipped to v0.2.0-rc.4. It must FAIL, and fail for the
# ENTRYPOINT reason - a bare non-zero would also be produced by a missing file,
# a bad CLI, or a corrupt binary, and would let this control pass while proving
# nothing about the name.
bad="$tmp/ducklake-v9.9.9-linux_amd64.duckdb_extension"
cp "$src" "$bad"
if out="$(try_load "$bad")"; then
  printf 'FAIL  a name whose first dot-field is not `ducklake` LOADed anyway - this control cannot fire\n'
  printf '::error::asset-name self-test: the negative case LOADed, so a green here says nothing\n'
  fails=$((fails + 1))
elif printf '%s' "$out" | grep -q 'did not contain the expected entrypoint'; then
  printf 'PASS  ducklake-<tag>-<platform>.duckdb_extension is REFUSED, for the entrypoint reason\n'
else
  printf 'FAIL  the bad name failed, but not for the entrypoint reason: %s\n' "$out"
  printf '::error::asset-name self-test: the negative case failed for an unrelated reason\n'
  fails=$((fails + 1))
fi

if [ "$fails" -ne 0 ]; then
  printf 'asset-name self-test: %s case(s) failed\n' "$fails"
  exit 1
fi
echo "asset-name self-test: all cases passed"
