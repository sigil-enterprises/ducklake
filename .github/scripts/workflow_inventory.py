#!/usr/bin/env python3
"""
Every workflow the Actions API advertises as `active` must have a file on this ref.

PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.

WHY
---
The Actions tab is what a human reads to decide what is covered. GitHub keeps a
workflow record alive after its file leaves a branch, so a workflow can be listed
`active`, with a green-looking history, and be incapable of running. #36 found
two: `Crypta Refusal Tests` and `Storage-Layer Mutants` - both advertised
`active`, neither present on `main`, neither run since 2026-08-10, and both RED
the last time they did.

The name is the damage. "Crypta Refusal Tests" is, by its name, the suite that
asserts the envelope layer refuses what it must refuse. A reader of the Actions
tab concludes that surface is covered. It is not covered, it is not running, and
a suite that cannot run reports nothing - which reads identically to
nothing-wrong.

WHAT IT IS NOT
--------------
This does not check that a workflow PASSES, or that it ran recently. It checks
the one thing that makes every other reading of the Actions tab meaningless: that
the advertised check EXISTS on the branch it claims to gate.

NOT "absent from main"
---------------------
A workflow file living only on a feature branch is registered `active` the
moment it is pushed, and it RUNS on that branch. Calling that a ghost would red
on every branch that adds a workflow, which is a check nobody would keep. The
question asked here is stricter and is the one that matters: does the file exist
on ANY ref in this repository? If it exists nowhere, no push to any branch can
ever run it, and the Actions tab is advertising a check that does not exist.

Usage:
  workflow_inventory.py <workflows.json> <paths-on-some-branch.txt>

`workflows.json` is the body of `GET /repos/{owner}/{repo}/actions/workflows`.
The second file is one workflow path per line - every path present on any ref -
which the caller produces with git, because only git can answer it.
Exit 0 iff every `active` workflow's `path` appears in that list.
"""

import json
import os
import sys


def ghosts(workflows, paths_on_some_branch):
    """
    The `active` workflows whose file is on no ref at all. Returns [(name, path)].

    `state` is checked for `active` only: `disabled_manually` and
    `disabled_inactivity` are records GitHub keeps for a workflow that is NOT
    being advertised as a live check, which is exactly the disposition #36 asks
    for when the file is gone for good.
    """
    found = []
    for workflow in workflows:
        if workflow.get("state") != "active":
            continue
        path = workflow.get("path") or ""
        if not path:
            found.append((workflow.get("name", "<unnamed>"), "<no path>"))
            continue
        if path not in paths_on_some_branch:
            found.append((workflow.get("name", "<unnamed>"), path))
    return found


def main(argv):
    if len(argv) != 3:
        print("::error::usage: %s <workflows.json> <paths-on-some-branch.txt>" % argv[0])
        return 2
    with open(argv[1]) as handle:
        body = json.load(handle)
    workflows = body["workflows"] if isinstance(body, dict) else body

    # A response that carried no workflows would make every question below pass
    # by having nothing to ask. This repository has workflows; zero means the
    # fetch failed in a way that still returned valid JSON.
    if not workflows:
        print("::error::the Actions API returned NO workflows - this check would pass vacuously")
        return 1

    active = [w for w in workflows if w.get("state") == "active"]
    if not active:
        print("::error::the Actions API lists no ACTIVE workflow at all - nothing would be checked")
        return 1

    with open(argv[2]) as handle:
        paths = {line.strip() for line in handle if line.strip()}
    # The list is produced by git over every ref. Empty means the producer
    # failed, and an empty list would report EVERY workflow as a ghost - loud,
    # but for the wrong reason, and it would train a reader to ignore this check.
    if not paths:
        print("::error::the list of workflow paths present on some ref is EMPTY - the git side of this check failed, and every workflow would be reported a ghost for the wrong reason")
        return 1

    missing = ghosts(workflows, paths)
    for name, path in missing:
        print(
            "::error::workflow '%s' is advertised ACTIVE but %s exists on NO ref in this repository. "
            "It cannot run, and a check that cannot run reports nothing - which reads "
            "as nothing-wrong. Either restore the file or disable the workflow "
            "(PUT /actions/workflows/{id}/disable) so the repository stops claiming "
            "coverage it does not have." % (name, path)
        )
    if missing:
        print("::error::%d advertised workflow(s) exist on no ref at all" % len(missing))
        return 1
    print("all %d active workflow(s) have a file on some ref:" % len(active))
    for workflow in active:
        print("  %s  (%s)" % (workflow["path"], workflow["name"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
