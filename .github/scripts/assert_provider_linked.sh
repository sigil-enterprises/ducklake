#!/usr/bin/env bash
#
# Assert the crypta KMS provider is actually LINKED INTO a built artifact.
#
# Vendored into this repo deliberately (ducklake#46). It previously existed only
# in opvance/ducklake-migration-bench, so this repo's release path could not call
# it, and a release could ship a provider-less extension and stay green. A gate
# naming a script that is not in the tree is itself a vacuous gate.
#
# Three traps this script is written to avoid:
#
#   1. `crypta_socket` / `crypta_lake_id` / `crypta_cache_ttl_seconds` are ATTACH
#      OPTION NAMES declared in src/storage/ducklake_storage.cpp. They are present
#      in builds with NO provider at all, so `strings | grep crypta` is a false
#      positive machine. We match provider TYPE and ENTRY symbols instead, which
#      cannot exist unless the provider was compiled.
#
#   2. An assertion reports the same zero whether the thing is absent or the tool
#      is misaimed. So this refuses to return a verdict unless the symbol table it
#      read is demonstrably real: a minimum symbol count AND a sighting of an
#      anchor symbol that MUST be in any DuckLake artifact. A stripped or
#      unreadable file is a hard error, never an "absent" pass.
#
#   3. A pattern naming a symbol with INTERNAL linkage can never match anything,
#      so a check built on one reports a confident zero on a healthy build and on
#      a dead one alike. crypta's copy of this check used to match
#      `CryptaProviderRegistrar`, a struct in an unnamed namespace; it was
#      structurally incapable of returning anything but zero. See ducklake@7f910fe:
#      a symbol pattern that can never match is a vacuous check, so refuse it.
#      Every pattern MUST name a symbol with EXTERNAL linkage, and that is
#      ENFORCED below, not trusted - under --expect present every pattern must
#      score above zero ON ITS OWN.
#
# Failures print a `::error::` annotation as well as exiting non-zero, so a
# refusal is legible in the GitHub Actions UI and not merely in a log tail.
#
# Usage: assert_provider_linked.sh --expect present|absent FILE [FILE...]
set -euo pipefail

# Provider type + entry-point symbols. None can appear in a build that did not
# compile the provider. Deliberately NOT the ATTACH option names, and - per trap
# 3 - deliberately nothing with internal linkage.
# `DuckLakeRegisterKmsProvider` is safe for the opposite reason to the registrar
# it replaced: src/ducklake_extension.cpp calls it from LoadInternal to force the
# archive member in, so it MUST be external or the link itself fails.
# Overridable so the positive control can inject a known-bad pattern and watch
# this refuse it.
PATTERNS="${PROVIDER_SYMBOL_PATTERNS:-DuckLakeCryptaProvider|CryptaClient|DuckLakeRegisterKmsProvider}"

# Any DuckLake artifact carries this; if it does not, we did not read a symbol
# table and every verdict below would be meaningless.
ANCHOR="${PROVIDER_ANCHOR_SYMBOL:-DuckLakeCatalog}"
MIN_SYMBOLS="${PROVIDER_MIN_SYMBOLS:-1000}"

annotate() { printf '::error::%s\n' "$*"; }

expect=""
files=()
while [ $# -gt 0 ]; do
  case "$1" in
    --expect) expect="${2:-}"; shift 2 ;;
    -*) echo "unknown flag: $1" >&2; exit 2 ;;
    *) files+=("$1"); shift ;;
  esac
done
case "$expect" in
  present|absent) ;;
  *) echo "usage: $0 --expect present|absent FILE [FILE...]" >&2; exit 2 ;;
esac
[ "${#files[@]}" -gt 0 ] || { echo "usage: $0 --expect present|absent FILE [FILE...]" >&2; exit 2; }

command -v nm >/dev/null 2>&1 || {
  annotate "nm is not on PATH; cannot read symbol tables, refusing to return a verdict"
  exit 3
}

rc=0
for f in "${files[@]}"; do
  if [ ! -f "$f" ]; then
    # A missing artifact has two completely different causes and they used to
    # print the same line. Distinguishing them is the whole point of this block;
    # neither softens the verdict.
    d="$(dirname "$f")"
    echo "FATAL: $f does not exist"
    case "$d" in
      /*) echo "       looked in $d" ;;
      *)  echo "       looked in $(pwd)/$d" ;;
    esac
    if [ ! -d "$d" ]; then
      echo "       The containing directory does not exist either, so nothing has ever"
      echo "       been written to this path. This check is either aimed at a path the"
      echo "       build never produces, or it is running BEFORE the artifact was staged."
      echo "       It says NOTHING about whether the provider linked."
      annotate "provider check aimed at $f, whose directory does not exist - the check is misaimed, not the build"
    else
      echo "       The containing directory exists but this artifact is not in it."
      echo "       Present in $d:"
      ls -1 "$d" 2>/dev/null | sed 's/^/         /' || echo "         (unreadable)"
      annotate "provider check: $f is missing from an existing directory - failed link or missing staging step"
    fi
    rc=1
    continue
  fi

  syms="$(nm -a "$f" 2>/dev/null || true)"
  total="$(printf '%s\n' "$syms" | grep -c . || true)"
  anchor_hits="$(printf '%s\n' "$syms" | grep -c "$ANCHOR" || true)"

  # DEFINED + EXTERNAL only, and this is the whole of trap 3 rather than a
  # comment about it.
  #
  # `nm -a` prints the entire .symtab, LOCAL symbols included. Grepping those
  # raw lines counts a lowercase type letter - `t`, `d`, `b`, an unnamed-
  # namespace or file-scope-static definition - as a hit, which is exactly the
  # symbol class ducklake@7f910fe and ducklake#46 say must never satisfy this
  # check: an internal-linkage symbol cannot be the one the extension's
  # LoadInternal resolves against, so its presence proves nothing about whether
  # the provider is reachable. Measured:
  #
  #   0000000000000524 t _ZN12_GLOBAL__N_126CryptaProviderRegistrar_fnEi
  #
  # A raw grep counts that. This does not.
  #
  # `U` is excluded for the opposite reason: an UNDEFINED reference to
  # DuckLakeRegisterKmsProvider is what a provider-LESS build has - it is the
  # unresolved symbol the link fails on - so counting it would turn the exact
  # broken state into a pass.
  #
  # nm prints `ADDR TYPE NAME` for a defined symbol and `TYPE NAME` for an
  # undefined one, so the type column is field 2 or field 1 by field count.
  defined_ext="$(printf '%s\n' "$syms" | awk '
    NF == 3 { t = $2; n = $3 }
    NF == 2 { t = $1; n = $2 }
    (NF == 2 || NF == 3) && t ~ /^[ABCDGRSTVW]$/ { print n }
  ')"
  hits="$(printf '%s\n' "$defined_ext" | grep -Ec "$PATTERNS" || true)"

  printf '%s\n' "-- $f"
  printf '    sha256      %s\n' "$( { sha256sum "$f" 2>/dev/null || shasum -a 256 "$f"; } | cut -d' ' -f1)"
  printf '    symbols     %s\n' "$total"
  printf '    anchor(%s) %s\n' "$ANCHOR" "$anchor_hits"
  printf '    provider    %s (defined + EXTERNAL only)\n' "$hits"

  if [ "$total" -lt "$MIN_SYMBOLS" ] || [ "$anchor_hits" -eq 0 ]; then
    echo "    FATAL: symbol table not readable (stripped? wrong file?). Refusing to return a verdict."
    annotate "provider check on $f read $total symbols and $anchor_hits sightings of $ANCHOR; that is not a symbol table, so no verdict is possible"
    rc=1
    continue
  fi

  if [ "$expect" = present ] && [ "$hits" -gt 0 ]; then
    bad_pattern=0
    old_ifs="$IFS"
    IFS='|'
    for pat in $PATTERNS; do
      [ -n "$pat" ] || continue
      pat_hits="$(printf '%s\n' "$defined_ext" | grep -c "$pat" || true)"
      printf '    pattern     %-32s %s\n' "$pat" "$pat_hits"
      [ "$pat_hits" -eq 0 ] && bad_pattern=1
    done
    IFS="$old_ifs"
    if [ "$bad_pattern" -eq 1 ]; then
      echo "    FAIL: a pattern scored zero on an artifact asserted to contain the provider."
      echo "          Either this build is broken, or that pattern names a symbol with"
      echo "          INTERNAL linkage (unnamed namespace / file-scope static) and can"
      echo "          never match anything. Both are defects; neither is a pass."
      annotate "provider check on $f: a pattern scored zero on a supposedly provider-linked artifact - broken build, or a vacuous pattern naming an internal-linkage symbol (see ducklake@7f910fe)"
      rc=1
      continue
    fi
  fi

  if [ "$expect" = present ] && [ "$hits" -eq 0 ]; then
    echo "    FAIL: expected the crypta provider to be linked in; found 0 provider symbols"
    echo "          with DEFINED, EXTERNAL linkage."
    echo "          This build is provider-less. Every ATTACH ... ENCRYPTION_SOCKET will be"
    echo "          refused with 'no KMS encryption provider'. Check the overlay step."
    annotate "provider check on $f: 0 provider symbols - this artifact has NO KMS provider and would refuse every enveloped ATTACH"
    rc=1
  elif [ "$expect" = absent ] && [ "$hits" -ne 0 ]; then
    echo "    FAIL: expected NO provider symbols, found $hits."
    annotate "provider check on $f: expected no provider symbols, found $hits"
    rc=1
  else
    echo "    OK ($expect)"
  fi
done
exit $rc
