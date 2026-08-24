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

This guard asserts the one thing that is checkable without a KMS: that every
envelope fixture runner is INVOKED by at least one workflow. It does not, and
cannot, assert that the envelope works - `run_envelope_e2e.sh` is what does
that, and it needs a build carrying a concrete KMS provider.

POSITIVE CONTROL
----------------
`self_test` plants a workflow body that DOES invoke a runner and requires the
matcher to find it. An absence assertion reports the same zero whether it is
working or broken; without this, a green here would mean nothing.

REFUSAL vs CRASH: a refusal exits non-zero WITH an `::error::` annotation. A
non-zero exit without one is a crash.
"""

import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WORKFLOW_DIR = os.path.join(".github", "workflows")

#: Every runner that drives a fixture the envelope depends on. Each must EXIST
#: (a runner that has been deleted or renamed makes this guard vacuous) and each
#: must be invoked by at least one workflow.
RUNNERS = [
    "test/sql/encryption/run_envelope_e2e.sh",
    "test/sql/crypta/run_sql_crypta_tests.sh",
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


def invokers(runner, bodies):
    return sorted(name for name, body in bodies.items() if runner in body)


def self_test():
    planted = {"Planted.yml": "jobs:\n  x:\n    steps:\n      - run: test/sql/encryption/run_envelope_e2e.sh\n"}
    found = invokers("test/sql/encryption/run_envelope_e2e.sh", planted)
    if found != ["Planted.yml"]:
        return "the matcher did not find a runner in a workflow that plainly invokes it (found %r)" % found
    if invokers("test/sql/crypta/run_sql_crypta_tests.sh", planted) != []:
        return "the matcher reported an invocation that is not there"
    return None


def main():
    control = self_test()
    if control:
        print("::error::positive control failed: %s" % control)
        return 1
    print("positive control: the matcher finds a planted invocation and reports no false one")

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

    if findings:
        print("::error::%d envelope fixture runner(s) are never executed by CI" % findings)
        return 1
    print("every envelope fixture runner is invoked by at least one workflow")
    return 0


if __name__ == "__main__":
    sys.exit(main())
