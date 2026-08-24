#!/usr/bin/env python3
"""
The positive control for `mutants.py resolve`.

PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.

`resolve` asserts an ABSENCE - that no mutant in the roster has come unstuck
from the tree. An absence assertion that is silently broken prints exactly what
a healthy one prints: "all N mutants resolve", exit 0. So its clean run is worth
nothing until it has been SEEN to report a planted defect.

Every case below calls the REAL `resolve_roster`, never a copy of its logic, and
against the REAL tree - only the roster is planted. A control that reimplements
what it controls proves the reimplementation.
"""

import copy
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, HERE)

import mutants  # noqa: E402


def problems(roster):
    return mutants.resolve_roster(REPO, roster)


def one(name):
    """A one-mutant roster built from the real one, so only the planted change differs."""
    return [copy.deepcopy(mutants.EXTENSION_MUTANTS[0])]


def main():
    failures = []

    def require(case, roster, want_substring):
        found = problems(roster)
        if want_substring is None:
            if found:
                failures.append("control %s: expected the roster to resolve, got:\n    %s" % (case, "\n    ".join(found)))
            else:
                print("  control %s: resolves, as required" % case)
            return
        if not found:
            failures.append(
                "control %s: resolve_roster reported NOTHING on a roster planted "
                "to break. It cannot tell a stale roster from a live one." % case
            )
            return
        if not any(want_substring in problem for problem in found):
            failures.append(
                "control %s: reported a problem, but not the planted one.\n    wanted substring: %s\n    got:\n    %s"
                % (case, want_substring, "\n    ".join(found))
            )
            return
        print("  control %s: reported, as required" % case)

    # The real roster against the real tree. If this is not clean, every red
    # below is unattributable.
    require("real-roster", mutants.EXTENSION_MUTANTS, None)

    # 1. `old` that cannot match - the #20 shape, a rename the roster missed.
    stale = one("stale")
    stale[0]["old"] = "\tif (CryptaClient::ThisWasRenamedYearsAgo()) {"
    require("old-cannot-match", stale, "occurs 0 times")

    # 2. `old` that matches MORE than once. `substitute_exactly_once` would refuse
    #    it at apply time, hours into a mutation run; this refuses it in
    #    milliseconds.
    ambiguous = one("ambiguous")
    ambiguous[0]["old"] = "#include"
    require("old-matches-many", ambiguous, "expected 1")

    # 3. A source file that is not in this tree - the shape `no_wrapped_key_
    #    refusal_body` was actually in when the guard bodies moved file.
    moved = one("moved")
    moved[0]["file"] = "src/storage/no_such_file.cpp"
    require("file-absent", moved, "not a file in this tree")

    # 4. A `reddens` naming a test that is not here - the #28 shape.
    orphaned = one("orphaned")
    orphaned[0]["reddens"] = ["test/sql/crypta/this_test_does_not_exist.test"]
    require("reddens-absent", orphaned, "not in this tree")

    if failures:
        for failure in failures:
            print("::error::%s" % failure)
        print("::error::%d positive-control case(s) failed - `mutants.py resolve` is not trustworthy" % len(failures))
        return 1
    print("all positive-control cases behaved as required")
    return 0


if __name__ == "__main__":
    sys.exit(main())
