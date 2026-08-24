#!/usr/bin/env python3
"""
REFUSE a tree in which the KMS envelope fixtures exist but nothing in CI runs
them.

PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.

WRITTEN TO RED main (87068373). See issue #52.

THE DEFECT
----------
The fork exists to put a KMS envelope over DuckLake's per-file DEKs. The
fixtures that would exercise it need a reachable key service, so they carry
`require-env` and are driven by a runner script. A require-env fixture with no
runner SKIPS, and a skip exits ZERO - so a tree in which the envelope has never
once been executed is indistinguishable, from CI's colour, from one in which it
passes. On main no workflow under `.github/workflows/` mentions any of these
runners at all.

This guard asserts two things, both checkable without a KMS.

1. Every envelope fixture runner is INVOKED by at least one workflow.
2. Every runner actually DISCOVERS the fixtures it names.

(2) exists because (1) alone overclaims, and provably so. Every runner in this
tree invoked the unittest binary WITHOUT `--test-dir`, and that binary walks a
test directory at startup whose default is duckdb's own tree, not ducklake's.
The result was "No test cases matched": zero envelope fixtures executed, exit
zero, and a workflow-reference scan green throughout. So each runner must (a)
pass `--test-dir` to the unittest binary on EVERY invocation, and (b) refuse an
invocation whose output does not contain the word "assertions", which is what
separates a skip from a pass. Neither is a substitute for actually running the
fixtures - `run_envelope_e2e.sh` does that, and it needs a build carrying a
concrete KMS provider - but a runner failing either could not have run them.

POSITIVE CONTROL
----------------
`self_test` plants a workflow body that DOES invoke a runner and requires the
matcher to find it, and plants a runner body with `--test-dir` STRIPPED - the
exact historical defect - and requires the discovery check to flag it while
sparing the repaired form. An absence assertion reports the same zero whether it
is working or broken; without these, a green here would mean nothing.

REFUSAL vs CRASH: a refusal exits non-zero WITH an `::error::` annotation. A
non-zero exit without one is a crash.
"""

import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WORKFLOW_DIR = os.path.join(".github", "workflows")

#: Every runner that drives a fixture the envelope depends on. Each must EXIST
#: (a runner that has been deleted or renamed makes this guard vacuous) and each
#: must be invoked by at least one workflow.
RUNNERS = [
    "test/sql/encryption/run_envelope_e2e.sh",
    "test/sql/crypta/run_sql_crypta_tests.sh",
    "test/sql/encryption/run_envelope_fixture.sh",
]


def workflow_bodies(root):
    directory = os.path.join(root, WORKFLOW_DIR)
    bodies = {}
    if not os.path.isdir(directory):
        return bodies
    for name in sorted(os.listdir(directory)):
        # `.disabled` files are deliberately excluded: a disabled workflow runs
        # nothing, and counting one would be the fail-open this guard is for.
        if not (name.endswith(".yml") or name.endswith(".yaml")):
            continue
        with open(os.path.join(directory, name), "r") as handle:
            bodies[name] = handle.read()
    return bodies


#: Every line that invokes the unittest binary must carry this flag, or the
#: fixtures it names are never discovered and the runner exits zero having run
#: nothing.
TEST_DIR_FLAG = "--test-dir"


#: `${UNITTEST}` in COMMAND POSITION - start of line, or after a pipe, `&&`,
#: `;`, `$(` or `!`. Merely MENTIONING the variable (`[ ! -x "${UNITTEST}" ]`,
#: an error message naming the path) is not an invocation, and counting one as
#: such would make this guard cry wolf on every runner.
UNITTEST_CALL = re.compile(r"""(?:^|[;&|(]|\$\(|\bthen\b|\bdo\b|!\s)\s*"?\$\{UNITTEST\}"?\s""")


def unittest_invocations(body):
    """Lines that RUN the unittest binary. A runner that spells the binary some
    other way yields nothing here, which is itself reported as a finding."""
    found = []
    for number, line in enumerate(body.splitlines(), start=1):
        stripped = line.strip()
        if stripped.startswith("#"):
            continue
        if UNITTEST_CALL.search(stripped):
            found.append((number, stripped))
    return found


def discovery_findings(runner, body):
    """Why this runner could not have executed its fixtures, if it could not."""
    problems = []
    invocations = unittest_invocations(body)
    if not invocations:
        problems.append("no ${UNITTEST} invocation at all - this runner runs no fixtures")
    for number, line in invocations:
        if TEST_DIR_FLAG not in line:
            problems.append(
                "line %d invokes the unittest binary without %s, so it discovers duckdb's tree and not "
                "ducklake's and reports \"No test cases matched\" while exiting zero: %s"
                % (number, TEST_DIR_FLAG, line)
            )
    if "assertions" not in body:
        problems.append(
            "never checks its own output for the word \"assertions\", so a SKIPPED fixture - which exits "
            "zero and never prints it - is indistinguishable from a pass"
        )
    return problems


def invokers(runner, bodies):
    return sorted(name for name, body in bodies.items() if runner in body)


def self_test():
    planted = {"Planted.yml": "jobs:\n  x:\n    steps:\n      - run: test/sql/encryption/run_envelope_e2e.sh\n"}
    found = invokers("test/sql/encryption/run_envelope_e2e.sh", planted)
    if found != ["Planted.yml"]:
        return "the matcher did not find a runner in a workflow that plainly invokes it (found %r)" % found
    if invokers("test/sql/crypta/run_sql_crypta_tests.sh", planted) != []:
        return "the matcher reported an invocation that is not there"

    repaired = 'output="$("${UNITTEST}" --test-dir "${ROOT}" "${FIXTURE}" 2>&1)"\ngrep -q "assertions"\n'
    if discovery_findings("planted", repaired):
        return "the discovery check flagged a runner that plainly passes %s and greps for assertions" % TEST_DIR_FLAG
    stripped = repaired.replace(' --test-dir "${ROOT}"', "")
    if not discovery_findings("planted", stripped):
        return "the discovery check did NOT flag a runner with %s removed - the exact historical defect" % TEST_DIR_FLAG
    if not discovery_findings("planted", repaired.replace('grep -q "assertions"', "true")):
        return "the discovery check did NOT flag a runner that never looks for assertions in its output"
    return None


def main():
    control = self_test()
    if control:
        print("::error::positive control failed: %s" % control)
        return 1
    print(
        "positive control: the matcher finds a planted invocation and reports no false one; the discovery "
        "check fires on a runner with %s stripped and on one that never greps for assertions, and spares "
        "the repaired form" % TEST_DIR_FLAG
    )

    bodies = workflow_bodies(REPO_ROOT)
    if not bodies:
        print("::error::no workflows found under %s - the guard has nothing to check" % WORKFLOW_DIR)
        return 1
    print("scanning %d workflow(s)" % len(bodies))

    findings = 0
    for runner in RUNNERS:
        path = os.path.join(REPO_ROOT, runner)
        if not os.path.exists(path):
            print("::error::%s does not exist - a guard over a runner that is gone is vacuous" % runner)
            findings += 1
            continue
        found = invokers(runner, bodies)
        if not found:
            findings += 1
            print(
                "::error file=%s::no workflow under %s invokes this runner, so the fixtures it drives have "
                "never executed in CI. They carry require-env, so without it they SKIP - and a skip exits zero, "
                "which is why nothing has gone red. This is issue #52." % (runner, WORKFLOW_DIR)
            )
        else:
            print("%s: invoked by %s" % (runner, ", ".join(found)))

        with open(path, "r") as handle:
            body = handle.read()
        for problem in discovery_findings(runner, body):
            findings += 1
            print("::error file=%s::%s" % (runner, problem))

    if findings:
        print("::error::%d envelope fixture runner(s) are never executed by CI" % findings)
        return 1
    print("every envelope fixture runner is invoked by at least one workflow, passes %s on every unittest "
          "invocation, and refuses output that reports no assertions" % TEST_DIR_FLAG)
    return 0


if __name__ == "__main__":
    sys.exit(main())
