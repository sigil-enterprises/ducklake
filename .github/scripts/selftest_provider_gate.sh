#!/usr/bin/env bash
#
# POSITIVE CONTROL for assert_provider_linked.sh (ducklake#46).
#
# A check that asserts a PRESENCE reports the same verdict when the thing is
# there and when the tool is broken or misaimed: both can print a number. Until
# the check has been shown to go RED on a deliberately planted bad fixture, its
# green says nothing. This script plants those fixtures and requires the gate to
# fire on every one of them.
#
# Two tiers, and both matter:
#
#   SYNTHETIC (always) - ELF shared objects built here from generated C. Cheap,
#   runs on every pull request in seconds, and covers the cases a real artifact
#   cannot be coerced into: a healthy build, a provider-less build, a stripped
#   file, and a vacuous internal-linkage pattern.
#
#   REAL (when an artifact path is given) - the artifact that is about to be
#   PUBLISHED, and a copy of it with the provider symbols excised by objcopy.
#   The gate must pass the first and fail the second. A synthetic control proves
#   the script's logic; only this one proves it against the bytes that ship.
#
# Inner `::error::` lines are rewritten before printing. An expected failure must
# not annotate the run - otherwise a green self-test would litter the UI with
# errors and a real one would be indistinguishable from it.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
gate="${here}/assert_provider_linked.sh"
[ -x "$gate" ] || { echo "::error::selftest cannot find $gate"; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

fails=0
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; printf '::error::provider-gate self-test: %s\n' "$*"; fails=$((fails + 1)); }

# Run the gate, capture output+status, and neutralise inner annotations.
# NOT `$(...)` into a status check: command substitution strips trailing
# newlines and, under `set -e`, a non-zero exit inside one aborts the script -
# which is the normal case here, since most of these runs are MEANT to fail.
run_gate() {
  local out_file="$1"; shift
  set +e
  "$gate" "$@" >"$out_file" 2>&1
  local st=$?
  set -e
  sed 's/^::error::/    [expected annotation] /' "$out_file" > "$out_file.neutralised"
  mv "$out_file.neutralised" "$out_file"
  return $st
}

expect_status() {
  local want="$1" label="$2" out="$work/out.txt"; shift 2
  local got=0
  run_gate "$out" "$@" || got=$?
  if [ "$got" -eq "$want" ]; then
    pass "$label (exit $got)"
  else
    fail "$label: expected exit $want, got $got"
    sed 's/^/      | /' "$out"
  fi
}

# A refusal is exit non-zero WITH an annotation. Exit non-zero WITHOUT one is a
# CRASH, and a crash is not a refusal - it is indistinguishable from the gate
# itself falling over. So the annotation is asserted, not assumed.
expect_refusal() {
  local label="$1" out="$work/out.txt"; shift
  local got=0
  run_gate "$out" "$@" || got=$?
  if [ "$got" -eq 0 ]; then
    fail "$label: expected a refusal, the gate PASSED"
    sed 's/^/      | /' "$out"
    return
  fi
  if grep -q '\[expected annotation\]' "$out"; then
    pass "$label (exit $got, ::error:: annotation emitted)"
  else
    fail "$label: exit $got but NO ::error:: annotation - that is a crash, not a refusal"
    sed 's/^/      | /' "$out"
  fi
}

# ---------------------------------------------------------------- synthetic --

gen_so() {
  # $1 = output .so, $2 = "with-provider" | "no-provider"
  local out="$1" flavour="$2" c="$work/gen.c" i
  : > "$c"
  # >1000 external symbols, so the gate's MIN_SYMBOLS floor is cleared by a
  # fixture whose symbol table is genuinely that big rather than by lowering
  # the floor for the test - a floor only the test can clear tests nothing.
  for i in $(seq 1 1100); do
    printf 'int ducklake_filler_%d(void) { return %d; }\n' "$i" "$i" >> "$c"
  done
  # The anchor. Any real DuckLake artifact carries DuckLakeCatalog; the gate
  # refuses a verdict without it, so the fixture must be honest about having it.
  printf 'int DuckLakeCatalog_ctor(void) { return 1; }\n' >> "$c"
  printf 'int DuckLakeCatalog_dtor(void) { return 2; }\n' >> "$c"
  if [ "$flavour" = with-provider ]; then
    printf 'int DuckLakeCryptaProvider_WrapKeys(void) { return 3; }\n' >> "$c"
    printf 'int CryptaClient_Connect(void) { return 4; }\n' >> "$c"
    printf 'void DuckLakeRegisterKmsProvider(void) { }\n' >> "$c"
  fi
  # -fvisibility=hidden keeps these out of .dynsym, so `strip` genuinely leaves
  # the fixture without a readable symbol table. Exported symbols survive a
  # strip in .dynsym, and a "stripped" fixture nm can still read would make the
  # stripped-file case below pass for the wrong reason.
  cc -shared -fPIC -fvisibility=hidden -o "$out" "$c"
}

echo "== synthetic fixtures =="
gen_so "$work/healthy.so" with-provider
gen_so "$work/providerless.so" no-provider
cp "$work/healthy.so" "$work/stripped.so"
strip -s "$work/stripped.so" 2>/dev/null || strip "$work/stripped.so"

# 1. The gate must GREEN on a healthy fixture. Without this the rest is a check
#    that fails on everything, which is as useless as one that passes on
#    everything.
expect_status 0 "healthy fixture, --expect present" --expect present "$work/healthy.so"

# 2. THE control for #46: a provider-less artifact must be REFUSED.
expect_refusal "provider-less fixture, --expect present" --expect present "$work/providerless.so"

# 3. ... and recognised as such when absence is what was asked for, so the two
#    verdicts are shown to be distinguishable rather than uniformly negative.
expect_status 0 "provider-less fixture, --expect absent" --expect absent "$work/providerless.so"

# 4. A stripped file must be a hard refusal, never an "absent" pass. This is the
#    tool-is-broken case: no symbol table, therefore no verdict.
expect_refusal "stripped fixture, --expect absent" --expect absent "$work/stripped.so"
expect_refusal "stripped fixture, --expect present" --expect present "$work/stripped.so"

# 5. ducklake@7f910fe: a symbol pattern that can never match is a vacuous check,
#    so refuse it. Injecting a name no build defines must make the gate refuse
#    even though the two real patterns beside it still score.
PROVIDER_SYMBOL_PATTERNS='DuckLakeCryptaProvider|CryptaProviderRegistrar' \
  expect_refusal "internal-linkage pattern injected, --expect present" \
    --expect present "$work/healthy.so"

# 6. A path that does not exist must refuse, and must NOT be read as "absent".
expect_refusal "nonexistent artifact, --expect present" --expect present "$work/nope/none.so"

# --------------------------------------------------------------------- real --

artifact="${1:-}"
if [ -n "$artifact" ]; then
  echo
  echo "== real artifact: $artifact =="
  [ -f "$artifact" ] || { echo "::error::self-test was given $artifact, which does not exist"; exit 1; }

  # The real artifact must pass. If it does not, publishing it is the defect and
  # the control is moot.
  expect_status 0 "PUBLISHED artifact, --expect present" --expect present "$artifact"

  # Plant the bad fixture: the same bytes with exactly the provider symbols
  # excised. Nothing else changes - same anchor, same symbol count to within the
  # handful removed - so a refusal here can only be about the provider.
  command -v objcopy >/dev/null 2>&1 || { echo "::error::objcopy is not on PATH; cannot plant the bad fixture, so the real-artifact control cannot run"; exit 1; }
  nm -a "$artifact" 2>/dev/null \
    | grep -E 'DuckLakeCryptaProvider|CryptaClient|DuckLakeRegisterKmsProvider' \
    | awk '{print $NF}' | sort -u > "$work/provider_syms.txt"
  planted="$(grep -c . "$work/provider_syms.txt" || true)"
  echo "   planting: excising $planted provider symbol name(s)"
  [ "$planted" -gt 0 ] || { echo "::error::found no provider symbols to excise, so no bad fixture can be planted - the artifact under test is already provider-less"; exit 1; }
  cp "$artifact" "$work/planted.ext"
  objcopy --strip-symbols="$work/provider_syms.txt" "$work/planted.ext"

  # The plant is VERIFIED before it is trusted. objcopy leaving a provider
  # symbol behind (a .dynsym entry, say) would make the refusal below fail for
  # the wrong reason, and a control nobody validated is another vacuous check.
  expect_status 0 "planted fixture is genuinely provider-less, --expect absent" \
    --expect absent "$work/planted.ext"

  # THE measurement: same artifact, provider removed, gate goes RED.
  expect_refusal "planted provider-less artifact, --expect present" \
    --expect present "$work/planted.ext"
fi

echo
if [ "$fails" -ne 0 ]; then
  echo "provider-gate self-test: $fails case(s) failed"
  exit 1
fi
echo "provider-gate self-test: all cases passed"
