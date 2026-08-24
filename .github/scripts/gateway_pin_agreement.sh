#!/usr/bin/env bash
#
# PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.
#
# Does the duckdb this fork BUILDS against equal the duckdb the CONSUMER RUNS?
#
# The previous shape of this question (#44, #45) was `.github/gateway-duckdb-version`:
# a hand-maintained MIRROR of the gateway's pin, compared against
# `.github/duckdb-version` in this same repo. Two files here agreeing with each
# other is not evidence about the gateway. A gateway bump left both files
# consistent, CI green, and the published asset unloadable in production - the
# failure moved one hop out rather than being closed.
#
# So this reads the CONSUMER'S OWN FILE. `opvance/teras-ext-pgwire`
# `image/Dockerfile` carries `ARG DUCKDB_VERSION=<x.y.z>`; that ARG is what the
# gateway image installs from PyPI and what the published ducklake extension must
# load into. Nothing in this repo can make that line agree with itself.
#
# FAIL-CLOSED, every branch. There is no "consumer unreachable, skipping": a skip
# is a green that measured nothing, which is the exact defect this file exists to
# remove. Unreachable is RED.
#
# Usage:
#   gateway_pin_agreement.sh <consumer-dockerfile> <this-repos-duckdb-version-file>
#
# Exit 0 iff the two pins agree. Any refusal is exit non-zero WITH an ::error::
# annotation, so a refusal is distinguishable from a crash.

set -euo pipefail

die() {
  echo "::error::$*"
  exit 1
}

[ "$#" -eq 2 ] || die "usage: $0 <consumer-dockerfile> <duckdb-version-file>"

consumer_file="$1"
pin_file="$2"

[ -r "${consumer_file}" ] || die "the consumer's Dockerfile is not readable at ${consumer_file} - the gateway pin could not be read, so nothing was compared"
[ -r "${pin_file}" ] || die "${pin_file} is not readable"

# `grep -c` counts LINES, and a line count is not an occurrence count. Extract
# first, count the extracted values, so two ARGs on one line cannot pass as one.
consumer_raw="$(grep -oE '^[[:space:]]*ARG[[:space:]]+DUCKDB_VERSION=[^[:space:]]+' "${consumer_file}" || true)"
matches="$(printf '%s' "${consumer_raw}" | grep -c . || true)"

if [ "${matches}" -eq 0 ]; then
  die "no 'ARG DUCKDB_VERSION=' line in ${consumer_file}. The consumer either stopped pinning duckdb there or renamed the ARG; either way this check no longer knows what the gateway runs and must not report agreement"
fi
if [ "${matches}" -ne 1 ]; then
  die "'ARG DUCKDB_VERSION=' occurs ${matches} times in ${consumer_file}. Which one the image build wins with is not something this check can decide, so it refuses rather than guessing"
fi

# Normalise both sides to a bare x.y.z. This repo writes the RELEASE TAG (`v1.5.5`,
# which is what `git describe --exact-match` yields and what the CLI zip URL
# needs); the Dockerfile writes the PyPI version (`1.5.5`). They are the same pin
# in two spellings, and comparing the spellings rather than the pin would red on
# a correct pair.
consumer_pin="${consumer_raw##*=}"
consumer_pin="${consumer_pin#v}"

# `$(...)` strips trailing newlines, which is wanted here, but the file may also
# carry none at all - so strip ALL whitespace rather than relying on either.
repo_pin="$(tr -d '[:space:]' < "${pin_file}")"
repo_pin="${repo_pin#v}"

[ -n "${consumer_pin}" ] || die "the consumer's ARG DUCKDB_VERSION is empty in ${consumer_file}"
[ -n "${repo_pin}" ] || die "${pin_file} is empty - this fork declares no duckdb pin at all"

if [ "${consumer_pin}" != "${repo_pin}" ]; then
  die "duckdb pin DRIFT: this fork builds against ${repo_pin} (${pin_file}), the consumer runs ${consumer_pin} (${consumer_file}, ARG DUCKDB_VERSION). A duckdb extension loads only under the exact patch it was built against, so the asset this repo publishes cannot be loaded by the gateway. Move one of the two."
fi

echo "duckdb pin agreement: this fork ${repo_pin}, consumer ${consumer_pin} - equal"
