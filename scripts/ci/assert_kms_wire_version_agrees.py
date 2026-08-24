#!/usr/bin/env python3
"""
REFUSE a tree in which the fake key services and the KMS client do not speak the
SAME wire version.

PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.

THE DEFECT THIS EXISTS FOR
--------------------------
`test/sql/crypta/fake_crypta.py` declared `CryptaWireManifest@v2` while every
client in the tree spoke `@v3`. A client that is handed a reply carrying the
wrong schema refuses it - `crypta refused the request: unsupported schema` - so
NO fixture driven by that fake could ATTACH. It went unnoticed for the same
reason as issue #52: those fixtures carry `require-env`, a require-env skip
EXITS ZERO, and nothing in CI ran their runner.

Bumping the string fixes today. This guard is what stops it recurring: the
version is now asserted to be one value across every file that names it, and a
future bump that touches one file reds here instead of silently muting a test
group.

POSITIVE CONTROL
----------------
`self_test` plants two sources that disagree and requires the comparator to
report exactly that, then plants two that agree and requires silence. An
absence assertion reports the same zero whether it is working or broken.

REFUSAL vs CRASH: a refusal exits non-zero WITH an `::error::` annotation. A
non-zero exit without one is a crash.
"""

import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

#: Every file that pins the wire version, with the pattern that reads it out.
#: A file listed here MUST exist and MUST match - a source that has been renamed
#: away makes this guard vacuous, so its absence is a refusal, not a skip.
SOURCES = [
    ("test/sql/encryption/fake_kms.py", r'^WIRE_SCHEMA = "([^"]+)"'),
    ("test/sql/crypta/fake_crypta.py", r'^WIRE_SCHEMA = "([^"]+)"'),
    ("test/kms_provider/test_kms_provider.cpp", r'^constexpr const char \*WIRE_SCHEMA = "([^"]+)";'),
]


def read_version(text, pattern):
    match = re.search(pattern, text, re.MULTILINE)
    return match.group(1) if match else None


def disagreements(found):
    """
    found: [(label, version)]. Returns the sorted set of distinct versions when
    they disagree, else [].
    """
    versions = sorted({version for _, version in found})
    return versions if len(versions) > 1 else []


def self_test():
    agree = [("a", "CryptaWireManifest@v3"), ("b", "CryptaWireManifest@v3")]
    if disagreements(agree) != []:
        return "the comparator reported a disagreement between two identical versions"
    disagree = [("a", "CryptaWireManifest@v2"), ("b", "CryptaWireManifest@v3")]
    if disagreements(disagree) != ["CryptaWireManifest@v2", "CryptaWireManifest@v3"]:
        return "the comparator did not report a plain disagreement"
    # And the READER, not just the comparator: a guard whose regex had rotted
    # would read None from every file and then agree with itself.
    if read_version('WIRE_SCHEMA = "CryptaWireManifest@v9"\n', r'^WIRE_SCHEMA = "([^"]+)"') != "CryptaWireManifest@v9":
        return "the reader did not extract a version from a line that plainly carries one"
    if read_version("nothing here\n", r'^WIRE_SCHEMA = "([^"]+)"') is not None:
        return "the reader invented a version from a source that names none"
    return None


def main():
    control = self_test()
    if control:
        print("::error::positive control failed: %s" % control)
        return 1
    print("positive control: the comparator separates agreement from disagreement, and the reader reads")

    found = []
    findings = 0
    for relative, pattern in SOURCES:
        path = os.path.join(REPO_ROOT, relative)
        if not os.path.exists(path):
            print("::error::%s does not exist - a guard over a source that is gone is vacuous" % relative)
            findings += 1
            continue
        with open(path, "r") as handle:
            version = read_version(handle.read(), pattern)
        if version is None:
            print("::error file=%s::this file no longer declares a wire version the guard can read" % relative)
            findings += 1
            continue
        print("%s: %s" % (relative, version))
        found.append((relative, version))

    conflict = disagreements(found)
    if conflict:
        findings += 1
        print(
            "::error::the KMS wire version is not one value across the tree: %s. A client refuses a reply "
            "carrying a schema it does not speak, so the fixtures driven by the odd one out cannot ATTACH - "
            "and because they carry require-env, they SKIP and exit ZERO instead of going red." % ", ".join(conflict)
        )

    if findings:
        return 1
    print("every source agrees on the KMS wire version")
    return 0


if __name__ == "__main__":
    sys.exit(main())
