#!/usr/bin/env bash
#
# PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.
#
# Measure what the crypta refusal suite actually reaches, with BOTH ARMS.
#
# The claim being measured
# ------------------------
# `src/crypta/ducklake_crypta.cpp:135` - `return entry->second;`, the DEK cache
# HIT - had never executed in the EXTENSION build, i.e. through a real ATTACH.
# (The standalone C++ cache suite does cover it; that is a different binary and
# a different arm below. Stated precisely because the looser claim - "never
# executed in this tree" - is false once that suite exists.) Every unwrap in
# `scripts/mvp_crypta_proof.sh` is a MISS, so the (identity, blob) keying from
# 7df67912 was carried by nothing. The refusal suite is supposed to change that.
#
# Why one arm is not evidence
# ---------------------------
# gcov's `.gcda` counters ACCUMULATE across processes into the same object
# directory. That is the mechanism that lets the proof script merge its coverage
# onto the suite's - and it is a trap for exactly this measurement: a STALE
# `.gcda` from an earlier run makes a line read as covered by THIS run when in
# fact an earlier process covered it. A non-zero would then prove nothing, on the
# line that matters most.
#
# So every arm below deletes the counters first, and the NEGATIVE arm is run as
# well as the positive one:
#
#   negative   the same instrumented binary, the same file, WITHOUT the cases
#              that exercise the cache -> line 58 must read ZERO
#   positive   counters cleared again, cases included -> line 58 must read
#              NON-ZERO
#
# A number that only ever goes up is not a measurement; a pair that moves 0 ->
# non-zero when and only when the test runs is.
#
# THIS IS A MANUAL TOOL. IT GATES NOTHING.
# ----------------------------------------
# `.github/workflows/CryptaRefusals.yml` runs the two SUITES; it does not run
# this. So the arms below, and the "nothing newly dark" check at the end, are
# enforced by whoever remembers to run them and by nobody else - which is the
# same criticism this repo levels at an unrun mutant suite, and it is fair.
#
# The reason it is not wired is cost, stated rather than hidden: it needs a
# second, -O0 build of DuckDB on top of the release build the job already pays
# for, roughly doubling a 30-70 minute job, to gate a number that the mutants do
# not already gate. If that trade stops being worth it, wire it - the script is
# already fail-closed and needs no changes to run in CI.
#
# Usage, inside the devcontainer, after the instrumented build described in
# `.claude/README.md` ("Coverage - opt-in") exists at build/coverage:
#
#   scripts/measure_crypta_refusal_coverage.sh

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COVERAGE_BUILD="${ROOT}/build/coverage"
UNITTEST="${COVERAGE_BUILD}/test/unittest"
TARGET_FILE="src/crypta/ducklake_crypta.cpp"
# `return entry->second;` - the cache HIT. A LINE NUMBER, so it moves whenever
# anything above it in that file grows, and it is a SEPARATE pin from the
# UNREACHABLE dict further down: repairing one and not the other leaves a stale
# pin behind. The #18 cache-key fix moved this 58 -> 135. Re-derive it, do not
# transcribe it:
#   grep -n 'return entry->second;' src/crypta/ducklake_crypta.cpp
CACHE_HIT_LINE=135

if [ ! -x "${UNITTEST}" ]; then
  echo "no instrumented unittest at ${UNITTEST}." >&2
  echo "Build build/coverage first - see .claude/README.md, 'Coverage - opt-in'." >&2
  exit 1
fi

clear_counters() {
  # THE step this whole script exists around. Never skip it between arms.
  find "${COVERAGE_BUILD}" -name '*.gcda' -delete
}

#! Print the execution count gcovr reports for one line of one file, or the
#! word "unreachable" when gcovr does not report that line at all.
line_count() {
  local report="$1" file="$2" line="$3"
  python3 - "${report}" "${file}" "${line}" <<'PY'
import json, sys
report, wanted_file, wanted_line = sys.argv[1], sys.argv[2], int(sys.argv[3])
data = json.load(open(report))
for entry in data.get("files", []):
    if not entry["file"].endswith(wanted_file):
        continue
    for line in entry.get("lines", []):
        if line["line_number"] == wanted_line:
            print(line["count"])
            sys.exit(0)
    # The file IS in the report and the line is not: gcov emits no record for a
    # line with no code, so this is "not a statement", not "not executed".
    print("no-such-line")
    sys.exit(0)
# The file is absent from the report entirely - a different fault, and the one
# that means the measurement never happened.
print("file-not-instrumented")
sys.exit(0)
PY
}

#! Report ONLY the counters written under `objdir`.
#!
#! The trailing positional is load-bearing and `--gcov-object-directory` is NOT a
#! substitute for it. Measured: with both builds populated, the same query reads
#! 12 through `--gcov-object-directory` and 4 through the positional path.
#! gcovr's own help says the search paths "default to --root AND
#! --gcov-object-directory" - so naming the object directory ADDS a search path,
#! it does not narrow one, and the whole tree keeps being scanned.
#!
#! That is the accumulation hazard this script is about, one level up: not a
#! stale `.gcda` inside one build, but a SECOND build's counters merged into the
#! first one's number. It was caught here, by the negative arm reading 8 where it
#! had read 0 - which is the only reason both arms exist.
report_for() {
  local out="$1" objdir="$2"
  gcovr --root "${ROOT}" --filter "${ROOT}/src/crypta/" \
        --gcov-executable "gcov-14" --json "${out}" "${objdir}" > /dev/null
}

echo "=============================================================="
echo " NEGATIVE ARM - counters cleared, the cache cases NOT run"
echo "=============================================================="
clear_counters
# crypta_attach_refusals.test needs no key service and never unwraps anything,
# so it exercises this FILE (the provider is constructed and refuses) without
# ever reaching the cache. That is what makes this arm discriminating rather
# than vacuous: the instrument is demonstrably live on the file, and
# ${CACHE_HIT_LINE} is still zero.
"${UNITTEST}" "test/sql/crypta/crypta_attach_refusals.test" > /dev/null
report_for /tmp/crypta_cov_negative.json "${COVERAGE_BUILD}"
negative="$(line_count /tmp/crypta_cov_negative.json "${TARGET_FILE}" "${CACHE_HIT_LINE}")"
echo "  ${TARGET_FILE}:${CACHE_HIT_LINE} = ${negative}"

echo
echo "=============================================================="
echo " POSITIVE ARM - counters cleared, the full crypta group run"
echo "=============================================================="
clear_counters
# DUCKLAKE_UNITTEST is what points the runner at the INSTRUMENTED binary. Without
# it the runner uses build/release, writes no counters, and this arm reads a
# perfectly plausible zero while measuring nothing.
DUCKLAKE_UNITTEST="${UNITTEST}" "${ROOT}/test/sql/crypta/run_sql_crypta_tests.sh" > /dev/null
report_for /tmp/crypta_cov_positive.json "${COVERAGE_BUILD}"
positive="$(line_count /tmp/crypta_cov_positive.json "${TARGET_FILE}" "${CACHE_HIT_LINE}")"
echo "  ${TARGET_FILE}:${CACHE_HIT_LINE} = ${positive}"

echo
echo "=============================================================="
echo " THE REFUSAL LINES - measured on the C++ suite, which is the"
echo " only thing that runs them"
echo "=============================================================="
# The two builds are separate and the split is not arbitrary. The extension
# coverage build instruments src/crypta/ as compiled into `unittest`; nothing
# `unittest` runs can reach a truncated frame or a socket read error, because
# those need a service that MISBEHAVES. The standalone suite is what supplies
# one, so it is the only build that can measure those branches.
CPP_BUILD="${ROOT}/build/crypta-test-coverage"
cmake -S "${ROOT}/test/cpp/crypta" -B "${CPP_BUILD}" -GNinja \
  -DDUCKLAKE_ROOT="${ROOT}" \
  -DDUCKDB_BUILD_DIR="${ROOT}/build/release" \
  -DCRYPTA_TEST_COVERAGE=ON > /dev/null
cmake --build "${CPP_BUILD}" > /dev/null

# Its OWN object directory, and its own clear. Sharing build/coverage's would let
# the two suites' counters merge, and neither number would then say which suite
# reached what - the exact confusion the arms above exist to remove.
find "${CPP_BUILD}" -name '*.gcda' -delete
"${CPP_BUILD}/crypta_test" > /dev/null
report_for /tmp/crypta_cov_cpp.json "${CPP_BUILD}"

# The two lines below are asserted UNREACHABLE, not merely uncovered, and the
# check is a subset test: they may stay dark, anything NEW going dark fails.
#
# Both are LINE NUMBERS, so they drift whenever the file above them grows, and
# nothing here detects that. Be precise about what a stale entry actually does,
# because the two halves are not equally likely and the check is a SUBSET test
# (`unexpected = dark - UNREACHABLE`):
#
#   - it ALWAYS false-flags the real line. Once the genuinely-unreachable line
#     moves off this list it lands in `unexpected` and the run FAILS - loudly,
#     which is the good case.
#   - it excuses the line now sitting at the stale number ONLY IF that line is
#     itself dark. A stale number naming a covered or unreported line subtracts
#     nothing and is inert.
#
# So the usual outcome is a visible red, not a silent pass; silent excusal needs
# the coincidence of the stale number landing on a dark line. Worth guarding
# against, not the default. The #18 cache-key fix moved both (79 -> 98, 66 -> 143)
# and they are updated here in the same change. Re-derive them, do not assume them
# - and note CACHE_HIT_LINE above is a THIRD pin that drifts independently.
#
# Moved AGAIN by the #19/#20/#21 merge: that branch inserted 20 lines into
# LooksWrapped, which sits ABOVE JsonEscape in this file, so 98 -> 118. Left
# unrepaired, 98 lands on `out += "\\\\";` - an ordinary executable line inside
# the switch - and the check both excuses a line that is not dark and flags the
# real one. Re-derived by content: `grep -n 'return out;' crypta_client.cpp`.
#
#   crypta_client.cpp:118    the closing brace of JsonEscape. gcov counts a
#                            function's closing brace on BOTH the return path and
#                            the exception-unwind path - measured, not assumed:
#                            ExtractBase64Field's brace reads 4143 against 4131
#                            returns, and the excess is its throws unwinding
#                            through. Nothing inside JsonEscape can throw except
#                            an allocation, so its unwind block has no reachable
#                            caller. (This is why a dark brace is worth reading
#                            rather than dismissing: the same signal on Health's
#                            brace was a REAL missing case - a health probe
#                            answered with an error frame - and is now covered.)
#   ducklake_crypta.cpp:143  `crypta returned N keys for one file`. Dead by
#                            construction: ExtractBase64Field already refuses any
#                            count other than the requested one, and UnwrapKey
#                            always requests exactly one. No response can reach
#                            it. Contriving a test to colour it would be testing
#                            the test.
# `|| cpp_arm=$?` and not a bare call: under `set -e` a non-zero exit here would
# abort before the verdict block, and the reason would never be printed.
cpp_arm=0
python3 - /tmp/crypta_cov_cpp.json <<'PY' || cpp_arm=$?
import json, sys

UNREACHABLE = {"crypta_client.cpp": {118}, "ducklake_crypta.cpp": {143}}

data = json.load(open(sys.argv[1]))
files = data.get("files", [])
bad = 0

# The loop below judges what it is GIVEN. Handed nothing, it would iterate zero
# times, leave `bad` at 0, and print "every line is covered or documented
# unreachable" having measured nothing at all - a vacuous green inside the script
# written to prevent vacuous greens. Demonstrated, not feared: gcovr pointed at
# an empty directory emits a well-formed report with no files and exits 0, and
# `.devcontainer/Dockerfile` already names `--gcov-ignore-errors` as a way to
# produce exactly that. A `--filter` that stops matching (ROOT resolved through a
# symlink, a moved CPP_BUILD) does the same.
#
# So the file set is asserted BEFORE anything is judged. Note this does NOT catch
# the missing-.gcda case - gcovr synthesises all-zero lines from the .gcno notes
# there, which the dark-line check below already reds on.
EXPECTED_FILES = set(UNREACHABLE)
seen = {entry["file"].split("/")[-1] for entry in files}
if seen != EXPECTED_FILES:
    print("    FAIL  expected coverage for %s, got %s - the report is not"
          % (sorted(EXPECTED_FILES), sorted(seen) or "nothing"))
    print("          measuring what this check claims to measure.")
    sys.exit(1)
if not any(line["count"] > 0 for entry in files for line in entry["lines"]):
    print("    FAIL  every line reads zero - the binary did not run, or its")
    print("          counters were never written.")
    sys.exit(1)

for entry in sorted(files, key=lambda e: e["file"]):
    name = entry["file"].split("/")[-1]
    lines = entry["lines"]
    hit = sum(1 for line in lines if line["count"] > 0)
    dark = {line["line_number"] for line in lines if line["count"] == 0}
    print("  %-24s %d/%d" % (name, hit, len(lines)))
    unexpected = sorted(dark - UNREACHABLE.get(name, set()))
    if unexpected:
        print("    FAIL  newly dark, and not on the unreachable list: %s" % unexpected)
        bad = 1
    elif dark:
        print("    dark, and documented unreachable: %s" % sorted(dark))
sys.exit(bad)
PY

echo
echo "=============================================================="
echo " VERDICT"
echo "=============================================================="
failed=0
if [ "${negative}" != "0" ]; then
  echo "  FAIL  negative arm read '${negative}', expected 0."
  echo "        CHECK THIS FIRST: has CACHE_HIT_LINE (${CACHE_HIT_LINE}) drifted off"
  echo "        \`return entry->second;\`? It is a hardcoded line number, so any change"
  echo "        above it in ${TARGET_FILE} moves it, and nothing here"
  echo "        detects that. Re-derive:"
  echo "          grep -n 'return entry->second;' ${TARGET_FILE}"
  echo "        A pin landing on a COMMENT reads '${negative}' as the literal string"
  echo "        'unreachable' (gcovr never reports comment lines); one landing on"
  echo "        another executable line reads that line's count instead."
  echo "        Only then: a stale .gcda survived the clear, or something other than"
  echo "        the cache test is reaching that line - and the positive arm below"
  echo "        proves nothing until that is explained."
  failed=1
else
  echo "  ok    negative arm is 0 - nothing but the cache cases reaches line ${CACHE_HIT_LINE}"
fi
# A whole number greater than zero, and nothing else. Matching on a list of known
# bad strings would let a NEW one ("file-not-instrumented", say) read as success.
if ! [ "${positive}" -gt 0 ] 2>/dev/null; then
  echo "  FAIL  positive arm read '${positive}', expected a count above zero."
  failed=1
else
  echo "  ok    positive arm is ${positive} - the cache HIT path executes"
fi
if [ "${cpp_arm}" != "0" ]; then
  echo "  FAIL  a line in src/crypta/ is dark and is not on the documented"
  echo "        unreachable list. Either cover it or justify it there - a line"
  echo "        left dark with no stated reason is the gap this suite exists to"
  echo "        close."
  failed=1
else
  echo "  ok    every line in src/crypta/ is either covered by the C++ suite or"
  echo "        documented unreachable, with the reason recorded above"
fi
exit "${failed}"
