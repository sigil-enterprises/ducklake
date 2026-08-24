#!/usr/bin/env bash
#
# POSITIVE CONTROL for assert_provider_linked.sh (ducklake#46).
#
# A check that asserts a PRESENCE reports a confident number whether the thing
# is there or the tool is misaimed. Until it has been watched go RED on a
# deliberately bad fixture, its green says nothing. This plants those fixtures
# and requires a refusal on every one.
#
# Every case asserts THREE things, not one:
#   - the exit status;
#   - for a refusal, that an ::error:: annotation was emitted, because exit
#     non-zero WITHOUT one is a CRASH and a crash is indistinguishable from this
#     script falling over;
#   - for a refusal, WHICH annotation. The gate has five distinct refusal paths
#     and a bare "it refused" is satisfied by all five, so a case could pass
#     while the gate refused for a reason that has nothing to do with what the
#     case is about. Each expectation therefore names a substring of the
#     annotation it demands.
#
# Two tiers:
#
#   SYNTHETIC (always) - ELF/Mach-O shared objects built here from generated C
#   and C++. Seconds, on every trigger.
#
#   REAL (--real-providerless FILE, repeatable / trailing FILE) - actual
#   DuckLake artifacts. The provider-less ones are REAL, UNMODIFIED, published
#   builds, not fixtures manufactured by deleting the records the gate reads
#   - a plant made by stripping exactly the symbol table under test would be
#   circular, and `objcopy --strip-symbols` in particular leaves .dynsym intact,
#   so the "provider-less" binary would still export and register the provider.
#
# Inner ::error:: lines are rewritten before printing: an EXPECTED failure must
# not annotate the run, or a green self-test litters the UI and a real failure
# is lost among the decoys.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
gate="${here}/assert_provider_linked.sh"
[ -x "$gate" ] || { echo "::error::selftest cannot find $gate"; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

fails=0
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; printf '::error::provider-gate self-test: %s\n' "$*"; fails=$((fails + 1)); }

# NOT `$(...)`: command substitution strips trailing newlines and, under `set
# -e`, a non-zero exit inside one aborts the caller - which is the NORMAL case
# here, since most of these runs are meant to fail.
run_gate() {
  local out_file="$1"; shift
  local st=0
  "$gate" "$@" >"$out_file" 2>&1 || st=$?
  sed 's/^::error::/    [expected annotation] /' "$out_file" > "$out_file.neutralised"
  mv "$out_file.neutralised" "$out_file"
  return $st
}

expect_pass() {
  local label="$1" out="$work/out.txt"; shift
  local got=0
  run_gate "$out" "$@" || got=$?
  if [ "$got" -eq 0 ]; then
    pass "$label (exit 0)"
  else
    fail "$label: expected the gate to PASS, it exited $got"
    sed 's/^/      | /' "$out"
  fi
}

# $1 label, $2 substring the annotation MUST contain, rest = gate args
expect_refusal() {
  local label="$1" want="$2" out="$work/out.txt"; shift 2
  local got=0
  run_gate "$out" "$@" || got=$?
  if [ "$got" -eq 0 ]; then
    fail "$label: expected a refusal, the gate PASSED"
    sed 's/^/      | /' "$out"
    return
  fi
  if ! grep -q '\[expected annotation\]' "$out"; then
    fail "$label: exit $got but NO ::error:: annotation - that is a crash, not a refusal"
    sed 's/^/      | /' "$out"
    return
  fi
  # Scoped to the ANNOTATION line, not the whole output: the gate prints a
  # human "FAIL:"/"FATAL:" block as well, and matching that would let a case
  # pass on prose while the annotation named a different refusal path - the
  # same defect class this check exists to close, one level up.
  if ! grep -q -- "\[expected annotation\].*$want" "$out"; then
    fail "$label: refused, but not for the reason under test - no annotation containing '$want'"
    sed 's/^/      | /' "$out"
    return
  fi
  pass "$label (exit $got, ::error:: names '$want')"
}

# ---------------------------------------------------------------- synthetic --

# >1000 EXTERNAL filler symbols, so the gate's MIN_SYMBOLS floor is cleared by a
# fixture whose symbol table is genuinely that big rather than by lowering the
# floor for the test. A floor only the test can clear tests nothing.
#
# Deliberately NOT built with -fvisibility=hidden. Hidden visibility makes these
# LOCAL in the Mach-O symbol table, which would red the healthy case on macOS
# for a reason that has nothing to do with the provider - and, worse, the older
# cut of this file DID build them hidden and the gate passed anyway, which is
# the measurement proving the gate used to count internal-linkage symbols.
gen_c_common() {
  local c="$1" i
  : > "$c"
  for i in $(seq 1 1100); do
    printf 'int ducklake_filler_%d(void) { return %d; }\n' "$i" "$i" >> "$c"
  done
  # The anchor. Any real DuckLake artifact carries DuckLakeCatalog; the gate
  # refuses a verdict without it.
  printf 'int DuckLakeCatalog_ctor(void) { return 1; }\n' >> "$c"
  printf 'int DuckLakeCatalog_dtor(void) { return 2; }\n' >> "$c"
}

gen_so() { # $1 out, $2 with-provider|no-provider
  local out="$1" flavour="$2" c="$work/gen.c"
  gen_c_common "$c"
  if [ "$flavour" = with-provider ]; then
    printf 'int DuckLakeCryptaProvider_WrapKeys(void) { return 3; }\n' >> "$c"
    printf 'int CryptaClient_Connect(void) { return 4; }\n' >> "$c"
    printf 'void DuckLakeRegisterKmsProvider(void) { }\n' >> "$c"
  fi
  cc -shared -fPIC -o "$out" "$c"
}

echo "== synthetic fixtures =="
gen_so "$work/healthy.so" with-provider
gen_so "$work/providerless.so" no-provider

# An artifact that is NOT an object file at all. nm cannot read it, so the gate
# must refuse a verdict rather than report "absent" - the tool-is-broken case,
# which an absence assertion otherwise reports identically to a real absence.
head -c 200000 /dev/urandom > "$work/junk.so"

# A real object file with a real symbol table that is simply not a DuckLake one:
# no anchor, few symbols. Distinct from junk.so - one is unreadable, the other
# is readable and aimed at the wrong file.
# It must trip the ANCHOR condition and ONLY the anchor condition. A one-symbol
# .so trips `total < MIN_SYMBOLS` at the same time, so the case named for the
# anchor would be satisfied by the size floor and prove nothing about the
# anchor. Full filler, anchor removed.
sed '/DuckLakeCatalog/d' "$work/gen.c" > "$work/noanchor.c"
cc -shared -fPIC -o "$work/noanchor.so" "$work/noanchor.c"

# THE case ducklake#46 and ducklake@7f910fe are about, and it must be a symbol
# that EXISTS with INTERNAL linkage - not a name absent from the file, which
# tests something else entirely. crypta's old check matched
# CryptaProviderRegistrar, a struct in an unnamed namespace: it was
# structurally incapable of returning anything but zero, and healthy and dead
# builds reported identically.
{
  cat "$work/gen.c"
  echo 'namespace { int CryptaProviderRegistrar_fn(int x) { return x + 1; } }'
  echo 'int DuckLakeCryptaProvider_WrapKeys(void) { return 3; }'
  echo 'int CryptaClient_Connect(void) { return CryptaProviderRegistrar_fn(4); }'
  echo 'void DuckLakeRegisterKmsProvider(void) { }'
} > "$work/internal.cpp"
c++ -O0 -shared -fPIC -o "$work/internal.so" "$work/internal.cpp"

# 1. Healthy fixture must GREEN, or this is a check that fails on everything,
#    which carries no more signal than one that passes on everything.
expect_pass "healthy fixture, --expect present" --expect present "$work/healthy.so"

# 2. THE control for #46: a provider-less artifact must be REFUSED, and refused
#    for having no provider symbols rather than for any of the other four
#    reasons the gate can refuse.
expect_refusal "provider-less fixture, --expect present" \
  "0 provider symbols" --expect present "$work/providerless.so"

# 3. ... and recognised as genuinely absent when absence is what was asked for,
#    so the two verdicts are shown to be distinguishable rather than uniformly
#    negative.
expect_pass "provider-less fixture, --expect absent" --expect absent "$work/providerless.so"

# 4. Unreadable file: a hard refusal in BOTH directions, never an absent pass.
expect_refusal "unreadable file, --expect absent" \
  "not a symbol table" --expect absent "$work/junk.so"
expect_refusal "unreadable file, --expect present" \
  "not a symbol table" --expect present "$work/junk.so"

# 5. Readable, big enough to clear the size floor, but not a DuckLake artifact:
#    no anchor, so no verdict. The demanded substring names the anchor count so
#    the size floor cannot satisfy this case.
expect_refusal "wrong file (no anchor), --expect present" \
  "and 0 sightings of DuckLakeCatalog" --expect present "$work/noanchor.so"

# 6. ducklake@7f910fe. The symbol EXISTS in the fixture, with internal linkage.
#    A gate that greps `nm -a` raw counts it and reports a confident green; this
#    one must refuse, and must say WHY - that the pattern can never name a
#    reachable symbol.
PROVIDER_SYMBOL_PATTERNS='DuckLakeCryptaProvider|CryptaProviderRegistrar' \
  expect_refusal "DEFINED but internal-linkage pattern, --expect present" \
    "vacuous pattern" --expect present "$work/internal.so"

# 7. ... while the same fixture passes on the three real patterns, so case 6's
#    red is attributable to the internal-linkage pattern and to nothing else
#    about that file.
expect_pass "same fixture on the real patterns, --expect present" \
  --expect present "$work/internal.so"

# 8. A path that does not exist must refuse, and must not be read as "absent".
expect_refusal "nonexistent artifact, --expect present" \
  "does not exist" --expect present "$work/nope/none.so"

# --------------------------------------------------------------------- real --

real_providerless=()
real_ok=""
while [ $# -gt 0 ]; do
  case "$1" in
    # `shift 2` with only one argument left shifts NOTHING and returns 1. Under
    # `set -uo pipefail` that made a malformed invocation loop forever: measured
    # rc=124 under `timeout 5`. A hung job burns the whole runner timeout and
    # emits no annotation at all - a crash in its worst form. Guard the arity.
    --real-providerless)
      [ $# -ge 2 ] || { echo "::error::--real-providerless needs a FILE"; exit 1; }
      real_providerless+=("$2"); shift 2 ;;
    # Without this an unknown flag silently became the artifact path.
    -*) echo "::error::unknown flag: $1"; exit 1 ;;
    *) real_ok="$1"; shift ;;
  esac
done

# Repeatable, because the controls must cover MORE THAN ONE OBJECT FORMAT. The
# artifacts this pipeline publishes are ELF; a control that is only a PE would
# never demonstrate the gate returns zero on a provider-less ELF, and the
# synthetic tier would be silently carrying that coverage alone.
for ctl in ${real_providerless+"${real_providerless[@]}"}; do
  echo
  echo "== REAL provider-less DuckLake artifact: $ctl =="
  if [ ! -f "$ctl" ]; then
    echo "::error::the real provider-less control artifact is missing: $ctl"
    exit 1
  fi
  # Unmodified published bytes. Nothing here manufactured them, so the refusal
  # cannot be an artefact of how the fixture was made.
  expect_refusal "REAL provider-less asset $(basename "$ctl"), --expect present" \
    "0 provider symbols" --expect present "$ctl"
done

if [ -n "$real_ok" ]; then
  echo
  echo "== REAL artifact under test: $real_ok =="
  if [ ! -f "$real_ok" ]; then
    echo "::error::the artifact under test is missing: $real_ok"
    exit 1
  fi
  expect_pass "artifact under test, --expect present" --expect present "$real_ok"
fi

echo
if [ "$fails" -ne 0 ]; then
  echo "provider-gate self-test: $fails case(s) failed"
  exit 1
fi
echo "provider-gate self-test: all cases passed"
