#!/usr/bin/env python3
"""
REFUSE a `statement error` expectation that cannot tell the refusal it means to
prove from another refusal on the same path.

PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.

WRITTEN TO RED main (87068373). See issue #50.

THE DEFECT CLASS
----------------
`test/sql/encryption/envelope_attach_refusals.test:31` asserts the substring
`encryption`. A sqllogictest `statement error` passes when the expected string
appears ANYWHERE in the thrown message. Nearly every refusal on the encryption
ATTACH path contains the word `encryption`, so that expectation passes whether
the refusal under test fired or an unrelated one did. It is indistinguishable
from a test that was never written.

That is precisely the class `7f910fe` refused for symbol patterns: a check that
cannot fail on a broken system is not a check.

HOW THIS PROVES IT, RATHER THAN ASSERTING IT
--------------------------------------------
A DECOY CORPUS of messages that ACTUALLY EXIST on this path, each quoted
verbatim from the source and each pinned to the file it lives in. For every
`statement error` expectation in the guarded files, the expectation is matched
against every decoy it is NOT about. An expectation that matches a decoy would
have passed on that decoy - so it does not distinguish, and it is refused.

The decoys are VERIFIED PRESENT in the source before they are used. A decoy that
has been reworded no longer proves anything, and silently dropping it would
shrink the corpus until the guard passed by having nothing to check - the exact
fail-open this file exists to catch. A missing decoy is a REFUSAL, not a skip.

POSITIVE CONTROL
----------------
`--self-test` plants one expectation known to be vacuous and one known to be
distinguishing, and requires the guard to flag exactly the first. Run it before
the real scan; a scan whose guard cannot fire reports zero findings for the same
reason a clean tree does.

REFUSAL vs CRASH: a refusal exits non-zero WITH an `::error::` annotation. Any
non-zero exit without one is a crash and must be read as such.
"""

import argparse
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

#: Real messages on the encryption ATTACH/read path. Each entry is
#: (id, source file, a fragment that must be present in that file verbatim,
#:  the message text a `statement error` would be matched against).
#:
#: The fragment and the message are deliberately the same string: it keeps the
#: corpus honest by construction - a decoy can only be used if the source still
#: says it.
DECOYS = [
    (
        "no-kms-provider-in-build",
        "src/storage/ducklake_catalog.cpp",
        "ENCRYPTION_SOCKET was set but this build of DuckLake has no KMS ",
    ),
    (
        "resolved-to-unencrypted",
        "src/storage/ducklake_catalog.cpp",
        "encryption_socket was set, but this DuckLake resolved to UNENCRYPTED - there are no per-file keys to ",
    ),
    (
        "missing-per-file-key",
        "src/storage/ducklake_envelope_guards.cpp",
        "Database is encrypted, but file %s does not have an encryption key",
    ),
    (
        "wrapped-key-without-provider",
        "src/storage/ducklake_envelope_guards.cpp",
        "file %s carries a wrapped encryption key, but this lake ",
    ),
    (
        "unusable-key-length",
        "src/storage/ducklake_envelope_guards.cpp",
        "file %s carries an encryption key of %llu bytes, which is not a length ",
    ),
    (
        "temp-spill-refusal",
        "src/storage/ducklake_envelope_guards.cpp",
        "refusing %s on an enveloped DuckLake while ",
    ),
]

GUARDED_FILES = [
    "test/sql/encryption/envelope_attach_refusals.test",
]

STATEMENT_ERROR = re.compile(r"^statement error\s*$")
SEPARATOR = re.compile(r"^----\s*$")


def load_decoys(root):
    """
    Return [(id, message)], refusing if any decoy has drifted out of its source.
    """
    corpus = []
    missing = []
    for decoy_id, source, message in DECOYS:
        path = os.path.join(root, source)
        try:
            with open(path, "r") as handle:
                body = handle.read()
        except OSError as exception:
            missing.append("%s: cannot read %s (%s)" % (decoy_id, source, exception))
            continue
        if message not in body:
            missing.append("%s: %s no longer contains its message verbatim" % (decoy_id, source))
            continue
        corpus.append((decoy_id, message))
    return corpus, missing


def parse_expectations(path):
    """
    Return [(line_number, expected_substring)] for every `statement error` block.

    The expected string is the block that follows `----` up to the next blank
    line, which is what sqllogictest matches against the thrown message.
    """
    with open(path, "r") as handle:
        lines = handle.read().split("\n")
    expectations = []
    index = 0
    while index < len(lines):
        if STATEMENT_ERROR.match(lines[index]):
            cursor = index + 1
            while cursor < len(lines) and not SEPARATOR.match(lines[cursor]):
                cursor += 1
            if cursor >= len(lines):
                break
            cursor += 1
            expected = []
            while cursor < len(lines) and lines[cursor].strip() != "" and not lines[cursor].startswith("#"):
                expected.append(lines[cursor])
                cursor += 1
            if expected:
                expectations.append((index + 1, "\n".join(expected).strip()))
            index = cursor
            continue
        index += 1
    return expectations


def matching_decoys(expected, corpus):
    return [decoy_id for decoy_id, message in corpus if expected in message]


def self_test(corpus):
    """
    The positive control. A guard asserting the ABSENCE of something reports the
    same zero whether it is working or broken, so it must be shown to fire.
    """
    vacuous = "encryption"
    distinguishing = "carries a wrapped encryption key, but this lake "
    failures = []
    if len(matching_decoys(vacuous, corpus)) < 2:
        failures.append(
            "the planted VACUOUS expectation %r matched fewer than two decoys - the corpus cannot "
            "detect a non-distinguishing assertion, so a clean scan would mean nothing" % vacuous
        )
    hits = matching_decoys(distinguishing, corpus)
    if hits != ["wrapped-key-without-provider"]:
        failures.append(
            "the planted DISTINGUISHING expectation %r matched %r, expected exactly "
            "['wrapped-key-without-provider'] - the guard would flag a healthy assertion" % (distinguishing, hits)
        )
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=REPO_ROOT)
    args = parser.parse_args()

    corpus, missing = load_decoys(args.root)
    if missing:
        for problem in missing:
            print("::error::decoy corpus has drifted: %s" % problem)
        print(
            "::error::the decoy corpus is how this guard tells a distinguishing assertion from a vacuous one. "
            "A shrunken corpus passes by having nothing to check. Re-quote the message and re-run.",
        )
        return 1

    control_failures = self_test(corpus)
    if control_failures:
        for failure in control_failures:
            print("::error::positive control failed: %s" % failure)
        return 1
    print("positive control: the guard fires on a planted vacuous expectation and spares a distinguishing one")

    findings = 0
    for relative in GUARDED_FILES:
        path = os.path.join(args.root, relative)
        if not os.path.exists(path):
            print("::error::guarded file %s does not exist" % relative)
            return 1
        expectations = parse_expectations(path)
        if not expectations:
            print("::error::%s contains no `statement error` expectation - the guard has nothing to check" % relative)
            return 1
        print("%s: %d expectation(s)" % (relative, len(expectations)))
        for line_number, expected in expectations:
            hits = matching_decoys(expected, corpus)
            if len(hits) > 1:
                findings += 1
                print(
                    "::error file=%s,line=%d::the expected substring %r matches %d unrelated refusals on this "
                    "path (%s). This assertion passes whether or not the refusal it names fired, so it cannot "
                    "tell a healthy build from a broken one. Assert the distinguishing clause of the message "
                    "instead." % (relative, line_number, expected, len(hits), ", ".join(hits))
                )

    if findings:
        print("::error::%d vacuous refusal assertion(s)" % findings)
        return 1
    print("no vacuous refusal assertions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
