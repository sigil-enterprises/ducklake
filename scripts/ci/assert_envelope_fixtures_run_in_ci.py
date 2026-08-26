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

This guard asserts three things, all checkable without a KMS.

0. Every envelope FIXTURE on disk is reached by something CI runs.
1. Every envelope fixture runner is INVOKED by at least one workflow.
2. Every runner actually DISCOVERS the fixtures it names.

(0) IS THE INVERSION, and it is the reason this file was rewritten. The first
version of this guard enumerated RUNNER SCRIPTS in a hand-maintained list, so a
fixture that no runner and no workflow named was INVISIBLE to it - exactly the
shape it exists to catch. `envelope_wrap_idempotence.test` was added by the same
PR that added this guard and was never invoked by anything; the guard was green
throughout. Fixtures and runners are now DISCOVERED FROM DISK, never listed.

A require-env fixture must be named EXPLICITLY, because it cannot run any other
way: the general suite runs it and it SKIPS. A fixture WITHOUT require-env is
allowed to be covered by a workflow that runs the unittest binary over a
`test/sql` glob - which is a real, located invocation this guard finds in a
workflow body, not an assumption.

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
matcher to find it; plants a runner body with `--test-dir` STRIPPED - the exact
historical defect - and requires the discovery check to flag it while sparing
the repaired form; and plants a require-env fixture that NOTHING names and
requires the reachability check to refuse it while sparing the same fixture once
a workflow names it. An absence assertion reports the same zero whether it
is working or broken; without these, a green here would mean nothing.

REFUSAL vs CRASH: a refusal exits non-zero WITH an `::error::` annotation. A
non-zero exit without one is a crash.
"""

import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WORKFLOW_DIR = os.path.join(".github", "workflows")

#: Fixtures are DISCOVERED here, never listed. A list is what let
#: `envelope_wrap_idempotence.test` sit unreferenced while this guard stayed
#: green.
FIXTURE_DIR = os.path.join("test", "sql", "encryption")
FIXTURE_PREFIX = "envelope_"
FIXTURE_SUFFIX = ".test"

#: Runners are discovered the same way: any shell script under test/ that
#: invokes the unittest binary is a runner and must itself be reachable from a
#: workflow.
RUNNER_ROOT = "test"


def discover_fixtures(root):
    directory = os.path.join(root, FIXTURE_DIR)
    if not os.path.isdir(directory):
        return []
    return sorted(
        os.path.join(FIXTURE_DIR, name).replace(os.sep, "/")
        for name in os.listdir(directory)
        if name.startswith(FIXTURE_PREFIX) and name.endswith(FIXTURE_SUFFIX)
    )


def discover_runners(root):
    """Any shell script under test/ that RUNS the unittest binary."""
    found = []
    for directory, _subdirs, names in os.walk(os.path.join(root, RUNNER_ROOT)):
        for name in sorted(names):
            if not name.endswith(".sh"):
                continue
            path = os.path.join(directory, name)
            with open(path, "r") as handle:
                body = handle.read()
            if not unittest_invocations(body):
                continue
            found.append(os.path.relpath(path, root).replace(os.sep, "/"))
    return sorted(found)


#: A workflow line that runs the unittest binary over a `test/sql` GLOB. A
#: fixture without require-env is executed by such a line; a require-env fixture
#: is NOT - it skips there - so only the first may lean on this.
SUITE_GLOB_CALL = re.compile(r"unittest\b.*--test-dir\b.*['\"]test/sql/\*['\"]")


def has_require_env(path):
    with open(path, "r") as handle:
        for line in handle:
            if line.strip().startswith("require-env"):
                return True
    return False


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


def fixture_drivers(fixture, workflows, runner_bodies, reachable_runners):
    """Everything CI actually runs that names this fixture.

    A runner that no workflow invokes is NOT a driver: naming a fixture from a
    script nothing runs is the same nothing as naming it nowhere.
    """
    drivers = [name for name, body in sorted(workflows.items()) if fixture in body]
    drivers += [
        runner
        for runner in sorted(runner_bodies)
        if runner in reachable_runners and fixture in runner_bodies[runner]
    ]
    return drivers


def suite_glob_workflows(workflows):
    return sorted(name for name, body in workflows.items() if SUITE_GLOB_CALL.search(body))


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

    # The inversion's own control. An unreferenced fixture is the defect this
    # guard was rewritten for, and it must be shown to fire on one.
    orphan = "test/sql/encryption/envelope_planted_orphan.test"
    if fixture_drivers(orphan, planted, {}, set()):
        return "an unreferenced fixture was reported as driven by something"
    named_runner = {"test/sql/encryption/run_envelope_e2e.sh": 'FIXTURE="%s"\n' % orphan}
    if fixture_drivers(orphan, planted, named_runner, set()) != []:
        return "a fixture named only by a runner NO workflow invokes was counted as run in CI"
    reached = fixture_drivers(orphan, planted, named_runner, {"test/sql/encryption/run_envelope_e2e.sh"})
    if reached != ["test/sql/encryption/run_envelope_e2e.sh"]:
        return "a fixture named by a runner a workflow DOES invoke was not counted (found %r)" % reached
    workflow_named = {"Named.yml": "      - run: run_envelope_fixture.sh %s\n" % orphan}
    if fixture_drivers(orphan, workflow_named, {}, set()) != ["Named.yml"]:
        return "a fixture named directly by a workflow was not counted"

    if suite_glob_workflows({"Suite.yml": 'build/release/test/unittest --test-dir ./ "test/sql/*"\n'}) != ["Suite.yml"]:
        return "the suite-glob matcher did not find a workflow that plainly runs the unittest binary over test/sql/*"
    if suite_glob_workflows(planted) != []:
        return "the suite-glob matcher reported a glob invocation that is not there"
    return None


def main():
    control = self_test()
    if control:
        print("::error::positive control failed: %s" % control)
        return 1
    print(
        "positive control: the matcher finds a planted invocation and reports no false one; the discovery "
        "check fires on a runner with %s stripped and on one that never greps for assertions, and spares "
        "the repaired form; and the reachability check refuses a planted fixture nothing runs, refuses one "
        "named only by a runner no workflow invokes, and spares one a workflow reaches" % TEST_DIR_FLAG
    )

    bodies = workflow_bodies(REPO_ROOT)
    if not bodies:
        print("::error::no workflows found under %s - the guard has nothing to check" % WORKFLOW_DIR)
        return 1
    print("scanning %d workflow(s)" % len(bodies))

    runners = discover_runners(REPO_ROOT)
    if not runners:
        print(
            "::error::no runner script under %s/ invokes the unittest binary - either they have been deleted "
            "or they spell the binary some other way, and either way this guard has nothing to check"
            % RUNNER_ROOT
        )
        return 1

    findings = 0
    runner_bodies = {}
    reachable_runners = set()
    for runner in runners:
        with open(os.path.join(REPO_ROOT, runner), "r") as handle:
            runner_bodies[runner] = handle.read()
        found = invokers(runner, bodies)
        if not found:
            findings += 1
            print(
                "::error file=%s::no workflow under %s invokes this runner, so the fixtures it drives have "
                "never executed in CI. They carry require-env, so without it they SKIP - and a skip exits zero, "
                "which is why nothing has gone red. This is issue #52." % (runner, WORKFLOW_DIR)
            )
        else:
            reachable_runners.add(runner)
            print("%s: invoked by %s" % (runner, ", ".join(found)))

        for problem in discovery_findings(runner, runner_bodies[runner]):
            findings += 1
            print("::error file=%s::%s" % (runner, problem))

    fixtures = discover_fixtures(REPO_ROOT)
    if not fixtures:
        print(
            "::error::no %s*%s under %s - the fixture-reachability half of this guard has nothing to check"
            % (FIXTURE_PREFIX, FIXTURE_SUFFIX, FIXTURE_DIR)
        )
        return 1
    print("discovered %d envelope fixture(s) on disk" % len(fixtures))

    suite_globs = suite_glob_workflows(bodies)
    for fixture in fixtures:
        drivers = fixture_drivers(fixture, bodies, runner_bodies, reachable_runners)
        if drivers:
            print("%s: run by %s" % (fixture, ", ".join(drivers)))
            continue
        if not has_require_env(os.path.join(REPO_ROOT, fixture)) and suite_globs:
            print(
                "%s: named by nothing, but carries no require-env and the general suite runs it (%s)"
                % (fixture, ", ".join(suite_globs))
            )
            continue
        findings += 1
        print(
            "::error file=%s::nothing CI runs names this fixture - no workflow, and no runner that a workflow "
            "invokes. It carries require-env, so the general suite SKIPS it and a skip exits zero: this file "
            "has never executed. Name it in a workflow, e.g. "
            "`test/sql/encryption/run_envelope_fixture.sh %s`. This is issue #52." % (fixture, fixture)
        )

    if findings:
        print("::error::%d envelope fixture(s) or runner(s) are never executed by CI" % findings)
        return 1
    print("every envelope fixture on disk is reached by something CI runs, and every runner is invoked by at "
          "least one workflow, passes %s on every unittest invocation, and refuses output that reports no "
          "assertions" % TEST_DIR_FLAG)
    return 0


if __name__ == "__main__":
    sys.exit(main())
