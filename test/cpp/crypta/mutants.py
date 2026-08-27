#!/usr/bin/env python3
"""
Mutants of the encryption-envelope refusal guards, one per guard.

This is a public fork of `duckdb/ducklake` (`sigil-enterprises/ducklake`), and
this file has always been committed to its public `main`. An earlier revision
opened with "PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork" -
a stale instruction pointed at `duckdb/ducklake`, not a confidentiality marker
for this repo, but confusing either way on a file that has been public since it
landed. Removed rather than carried forward (bench#182).

Why this exists
----------------
An unrun refusal test and a passing one look identical - both print a green
tick - so a green here is worth nothing until the test has been SEEN to fail.
This file removes exactly one guard at a time and names the test cases that
must go red because of it.

That is the positive control for an absence assertion: prove the check fires on
a known-bad build before believing its clean run.

ONE ROSTER
----------
`EXTENSION_MUTANTS` covers guards in `src/storage/` and `src/functions/` -
ATTACH- and read-time refusals reachable only by rebuilding the whole
extension: `reddens` names sqllogictest FILES rather than Catch case titles
(the unittest binary is Catch2 too, so the spec and the exactly-N-cases
control are literally the same code, at a coarser grain - see `redden_at`
below), and a mutant is applied and reversed IN THE TREE via `patch`/
`unpatch`, never on a copy. `unpatch` is the REVERSE substitution under the
same exactly-once guard, and callers are expected to require `git diff` on
every touched file to come back empty before treating a run as clean.

A prior revision of this file also carried a second, STANDALONE roster
(`MUTANTS`) of 42 mutants against `src/crypta/crypta_client.cpp` and
`src/crypta/ducklake_crypta.cpp` - a wire-protocol crypta client that predates
the KMS-agnostic `EncryptionProvider` abstraction. `9e73bf28` removed that
source ("remove leaked crypta files") but left the roster quoting it, and its
companion test double (`test/cpp/crypta/fake_crypta_server.{cpp,hpp}`) is gone
too; every one of those 42 mutants named a file that no longer exists in this
tree, and `mutants.py resolve .` (without `--extension`) reported all 42 as
unappliable. Removed rather than rewritten (bench#182): the wire-protocol
client it exercised has no successor to point at line-for-line, and the
refusal semantics it was proving - a resolved-encryption check, a wrapped-key
refusal on read and on flush, a null-key refusal on flush - are exactly what
`EXTENSION_MUTANTS` below already proves against the CURRENT provider
architecture, gated on every push and PR by `.github/workflows/MutantRoster.yml`.

Usage
-----
  mutants.py list [--extension]
  mutants.py names [--extension]
  mutants.py files --extension          # the tree files the roster edits
  mutants.py resolve <repo-root> [--extension] # every `old` matches once, every
                                              # `reddens` file exists
  mutants.py patch <name> <repo-root>         # apply a mutant in the tree
  mutants.py unpatch <name> <repo-root>       # reverse it
  mutants.py verify-clean <repo-root>         # no mutant is applied
  mutants.py spec  <name>     # the Catch test-spec for the cases it must redden
  mutants.py count <name>     # how many cases that spec must match

`--extension` is accepted everywhere for backward compatibility with callers
that still pass it (this file's own CI workflow among them); there is now only
the one roster, so it changes nothing.
"""

import os
import sys

# Every `old` below is matched EXACTLY and must occur EXACTLY once. A mutant
# whose pattern drifts out of the source is an error, never a silent skip - a
# mutant that quietly applied nothing would report the unmutated build as red-
# proof, which is the vacuous green this whole file exists to prevent.

# --------------------------------------------------------------------------
# The FULL-EXTENSION roster - guards the standalone project structurally cannot
# reach (#45).
#
# Same record, same exactly-once guard, same `reddens` contract. Three things
# about it are worth stating rather than discovering:
#
#   1. `file` is a path from the REPO ROOT, not a basename inside src/crypta.
#      These files are not copied anywhere - there is nowhere to copy them to
#      that still builds - so `patch` edits the tree and `unpatch` reverses it.
#   2. `reddens` names sqllogictest FILES. DuckDB's `test/unittest` is a Catch2
#      binary that registers each .test file as a test case named by its path, so
#      `catch_spec` and the "spec must match exactly this many cases" control are
#      the same code doing the same job at a coarser grain. Coarser is the honest
#      word for it: a sqllogictest file is bigger than a Catch case, so "the file
#      went red" is weaker evidence than "this case went red". The runner closes
#      that gap with `redden_at` below.
#   3. `redden_at` is the one field this roster adds, and it exists because of
#      that grain. It is a fragment of the FAILING STATEMENT the mutant is
#      supposed to flip, and the runner requires it in the failure output. Without
#      it a file that died on its fixture, its ATTACH, or an unrelated assertion
#      would report RED and be counted as evidence for a guard it never reached.
#      Matched on the STATEMENT's own text, never on a line number: a line-number
#      assertion silently retargets the moment anything above it moves.
#
# A CALL SITE IS ITS OWN MUTANT. `RefuseWrappedKeyWithoutProvider` has one body and
# two callers, and it gets three mutants, not one. A body mutant reddens off
# EITHER caller, so with only that one a caller whose test had rotted away would
# stay invisible - the shared-guard blind spot the `cache_key_unprefixed_join`
# note in the roster above describes from the other side. So each call site is
# deleted on its own and must redden its own file, and the body mutant is kept as
# well because deleting a call proves only that the caller CONSULTS the guard and
# never what the guard ANSWERS.
#
# THE RESOLUTION ASKS THREE QUESTIONS, NOT TWO (#53). `ResolveStoredEncryptionKey`
# answers: is the column NULL on an ENCRYPTED lake, is crypta configured, is an
# unconfigured lake's blob wrapped. Every mutant below that hand-restores the
# branches therefore restores ALL THREE minus the one it is about - otherwise it
# would redden a case it does not name, and be evidence for two guards while
# claiming to be evidence for one.
#
# HONEST GAP, stated rather than left to be discovered: the NULL refusal
# (`RefuseMissingEncryptionKey`) has ONE mutant here, on the FLUSH call site,
# because that is the site the defect was on. By this roster's own shared-guard
# rule it wants two more - a BODY mutant (what the guard ANSWERS, as opposed to
# whether a caller consults it) and a SCAN call-site mutant. Both have a case
# waiting for them already: `adversary_flush_null_key.test` carries the scan-path
# refusal as its positive control, in the same file and the same run. They are
# not written yet. Until they are, the scan site's null refusal is asserted by a
# test that has never been shown to fail without it. Tracked at #56.
EXTENSION_MUTANTS = [
    {
        "name": "no_resolved_encryption_check",
        "file": "src/storage/ducklake_catalog.cpp",
        "why": "FinalizeLoad's refusal of an ATTACH where crypta was configured "
        "but the lake RESOLVED to unencrypted (#19). The constructor "
        "cannot make this call - the DEFAULT is AUTOMATIC, and only the "
        "initializer resolves it - so without this the ATTACH succeeded, "
        "the self-test passed, and NOT ONE key was written: the operator "
        "asked for envelope encryption and got neither an envelope nor "
        "encryption, silently",
        "old": "\tif (encryption_provider && Encryption() != DuckLakeEncryption::ENCRYPTED) {",
        "new": "\tif (false) {",
        "reddens": ["test/sql/crypta/crypta_config_refusals.test"],
        # The ATTACH that must stop being refused. It is the file's FIRST
        # statement after the requires, so a fixture failure cannot counterfeit
        # it - there is no fixture ahead of it to fail.
        #
        # Note what this fragment SKIPS. The .test file writes the path as
        # `ducklake:__TEST_DIR__/crypta_automatic.db`, but sqllogictest expands
        # __TEST_DIR__ before echoing the failing statement, so the output reads
        # `ducklake:duckdb_unittest_tempdir/476/crypta_automatic.db` - with a run
        # number in it. A marker copied verbatim from the .test file therefore
        # never matches. Measured: the first run of this roster reported
        # WRONG-RED on exactly that, which is the redden_at check doing its job on
        # its own author. `verify_markers` below now refuses the substitution
        # token outright.
        "redden_at": "crypta_automatic.db' AS automatic",
    },
    {
        "name": "no_wrapped_key_refusal_body",
        # The guard BODIES moved out of ducklake_catalog.cpp into their own file
        # when the provider abstraction landed. The roster kept naming the old
        # file, so this mutant could not be applied at ALL - and an unappliable
        # mutant is a positive control that has never fired.
        "file": "src/storage/ducklake_envelope_guards.cpp",
        "why": "the JUDGEMENT inside RefuseWrappedKeyWithoutProvider - it keeps "
        "being called and keeps consulting LooksWrapped, and simply "
        "answers 'not wrapped' every time. The two call-site mutants below "
        "prove each caller CONSULTS the guard; only this one proves what "
        "the guard ANSWERS, and it is the mutant that would survive if the "
        "predicate itself were inverted or stubbed",
        "old": "\tif (!DuckLakeEncryptionProvider::LooksWrapped(stored_key)) {",
        "new": "\tif (true) {",
        # Both files, because both call sites go through this body. The runner
        # verifies the roster PER CASE - it requires each named file to fail, and
        # each `redden_at` to appear - so a mutant that reddened only one of them
        # is reported, never absorbed into a combined non-zero status.
        "reddens": [
            "test/sql/crypta/crypta_unconfigured_reader_refusal.test",
            "test/sql/crypta/crypta_flush_unconfigured_refusal.test",
        ],
        "redden_at": [
            "SELECT sum(id) FROM unconfigured.alpha",
            "CALL ducklake_flush_inlined_data('unconfigured')",
        ],
    },
    {
        "name": "no_wrapped_key_refusal_on_scan",
        "file": "src/storage/ducklake_metadata_manager.cpp",
        "why": "the REFUSAL half of the resolution at the scan site - "
        "ReadDataFile's null-provider branch, reached exactly when the "
        "crypta options were absent from the ATTACH. Without it the "
        "wrapped blob is base64-decoded and handed to the Parquet reader "
        "AS IF IT WERE A KEY, and mbedtls asserts on the length (#20) - "
        "fail-closed by accident, and a blob whose length happened to be "
        "valid would have been TRIED. Since #26 the site calls "
        "ResolveStoredEncryptionKey rather than the refusal directly, so "
        "the mutant restores the two branches BY HAND with only the "
        "refusal missing. That is deliberate and it keeps this mutant's "
        "meaning exactly what it always was: it removes the refusal at "
        "THIS site and nothing else - a configured lake still unwraps, so "
        "no case reddens for the wrong reason",
        # clang-format rewrapped this call when the provider abstraction landed;
        # the roster pinned the pre-format aligned-continuation spelling, so the
        # exactly-once match found zero.
        "old": "\t\tdata.encryption_key = transaction.GetCatalog().ResolveStoredEncryptionKey(\n"
        "\t\t    table.GetTableId(), path.path, data.path, is_delete_file, stored_key);",
        # The hand-restored branches carry the NULL refusal too. Since #53 the
        # resolution answers THREE questions, not two, and a mutant that dropped
        # the null one as well would redden `adversary_flush_null_key.test`'s scan
        # control - a case it does not name - which is a mutant measuring two
        # guards and attributing both to one.
        "new": "\t\t// MUTANT no_wrapped_key_refusal_on_scan: the refusal was in the resolution.\n"
        "\t\tif (stored_key.IsNull()) {\n"
        "\t\t\ttransaction.GetCatalog().RefuseMissingEncryptionKey(data.path);\n"
        "\t\t} else {\n"
        # `.template GetValue<string>()`, not `.GetValue<string>()`.
        # ReadDataFile is a TEMPLATE over the row type, so `stored_key` -
        # deduced from `row.GetBaseValue(...)` - is a dependent type and
        # g++-14 parses the bare `<` as less-than. Measured: the first run
        # of this mutant reported `ERROR ... the mutated extension does not
        # compile`, which is the runner refusing to count a mutant it could
        # not build rather than absorbing it into a red.
        "\t\t\tauto mutant_key = stored_key.template GetValue<string>();\n"
        "\t\t\tauto mutant_provider = transaction.GetCatalog().EncryptionProvider();\n"
        "\t\t\tif (mutant_provider) {\n"
        "\t\t\t\tdata.encryption_key = mutant_provider->UnwrapKey(\n"
        "\t\t\t\t    transaction.GetCatalog().BuildEncryptionIdentity(table.GetTableId(), path.path, is_delete_file),\n"
        "\t\t\t\t    mutant_key);\n"
        "\t\t\t} else {\n"
        "\t\t\t\tdata.encryption_key = Blob::FromBase64(string_t(mutant_key));\n"
        "\t\t\t}\n"
        "\t\t}",
        "reddens": ["test/sql/crypta/crypta_unconfigured_reader_refusal.test"],
        "redden_at": "SELECT sum(id) FROM unconfigured.alpha",
    },
    {
        "name": "no_wrapped_key_refusal_on_flush",
        "file": "src/functions/ducklake_flush_inlined_data.cpp",
        "why": "the REFUSAL half of the resolution at the SECOND decode site - "
        "the inlined-deletion flush path, which reads a delete-file key "
        "with its own hand-written query and never passes through "
        "ReadDataFile. The original fix called ReadDataFile 'THE unwrap "
        "choke point'; that was false, and this is the site that proves "
        "it. Since #26 the site calls ResolveStoredEncryptionKey, so the "
        "mutant restores the two branches by hand with only the refusal "
        "missing - the CONFIGURED direction is left intact so this stays "
        "a mutant of the unconfigured guard alone, and the "
        "no_unwrap_on_flush mutant below covers the other direction on "
        "its own",
        "old": "\t\t\t\t\tfile_info.existing_delete_encryption_key = catalog.ResolveStoredEncryptionKey(\n"
        "\t\t\t\t\t    table_id, file_info.existing_delete_path, resolved_delete_path, true, stored_delete_key);",
        # The NULL refusal is kept, for the same reason as on the scan site: this
        # mutant removes ONE of the three questions, and a mutant that removed two
        # would redden `adversary_flush_null_key.test` as well and be evidence for
        # neither guard on its own.
        "new": "\t\t\t\t\t// MUTANT no_wrapped_key_refusal_on_flush: the refusal was in the resolution.\n"
        "\t\t\t\t\tif (stored_delete_key.IsNull()) {\n"
        "\t\t\t\t\t\tcatalog.RefuseMissingEncryptionKey(resolved_delete_path);\n"
        "\t\t\t\t\t} else {\n"
        "\t\t\t\t\t\tauto mutant_key = stored_delete_key.GetValue<string>();\n"
        "\t\t\t\t\t\tauto mutant_provider = catalog.EncryptionProvider();\n"
        "\t\t\t\t\t\tif (mutant_provider) {\n"
        "\t\t\t\t\t\t\tfile_info.existing_delete_encryption_key = mutant_provider->UnwrapKey(\n"
        "\t\t\t\t\t\t\t    catalog.BuildEncryptionIdentity(table_id, file_info.existing_delete_path, true), mutant_key);\n"
        "\t\t\t\t\t\t} else {\n"
        "\t\t\t\t\t\t\tfile_info.existing_delete_encryption_key = Blob::FromBase64(mutant_key);\n"
        "\t\t\t\t\t\t}\n"
        "\t\t\t\t\t}",
        # ONE file, and that is the finding rather than an omission:
        # `grep -rn RefuseWrappedKeyWithoutProvider test/` finds this site named in
        # exactly one .test file. It is also a file whose state is NOT
        # constructible from public operations - #25 forbids inlining on a crypta
        # lake - so the key is PLANTED there on purpose, which the file says in
        # its own header. That is honest, and it is the whole coverage this call
        # site has.
        "reddens": ["test/sql/crypta/crypta_flush_unconfigured_refusal.test"],
        "redden_at": "CALL ducklake_flush_inlined_data('unconfigured')",
    },
    {
        "name": "no_unwrap_on_flush",
        "file": "src/functions/ducklake_flush_inlined_data.cpp",
        "why": "#26 ITSELF, restored exactly - the UNWRAP half of the resolution "
        "at the flush site, with the refusal left fully in place. This is "
        "the code that shipped before the fix: a CONFIGURED crypta lake "
        "base64-decoding a wrapped delete-file key and handing the bytes "
        "to the Parquet reader, dying on 'INTERNAL Error: Invalid AES key "
        "length for GCM'. It is the mirror of no_wrapped_key_refusal_on_"
        "flush above and the reason the two halves are one call: with the "
        "refusal alone, this site was HALF guarded and read as guarded. "
        "The mutant proves the configured direction is load-bearing rather "
        "than decorative, which deleting the whole call could not - that "
        "would redden off the unconfigured arm and tell you nothing about "
        "the unwrap",
        "old": "\t\t\t\t\tfile_info.existing_delete_encryption_key = catalog.ResolveStoredEncryptionKey(\n"
        "\t\t\t\t\t    table_id, file_info.existing_delete_path, resolved_delete_path, true, stored_delete_key);",
        "new": "\t\t\t\t\t// MUTANT no_unwrap_on_flush: the unwrap was in the resolution.\n"
        "\t\t\t\t\tif (stored_delete_key.IsNull()) {\n"
        "\t\t\t\t\t\tcatalog.RefuseMissingEncryptionKey(resolved_delete_path);\n"
        "\t\t\t\t\t} else {\n"
        "\t\t\t\t\t\tauto mutant_key = stored_delete_key.GetValue<string>();\n"
        "\t\t\t\t\t\tcatalog.RefuseWrappedKeyWithoutProvider(resolved_delete_path, mutant_key);\n"
        "\t\t\t\t\t\tfile_info.existing_delete_encryption_key = Blob::FromBase64(mutant_key);\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t\t(void)table_id;",
        "reddens": ["test/sql/crypta/crypta_flush_configured_unwrap.test"],
        "redden_at": "CALL ducklake_flush_inlined_data('configured')",
    },
    {
        "name": "no_null_key_refusal_on_flush",
        "file": "src/functions/ducklake_flush_inlined_data.cpp",
        "why": "#53 ITSELF, restored exactly - the shape v0.1.0-rc.1 shipped. "
        "#26's fix moved TWO of the three questions the resolution asks "
        "onto the catalog and left the third, 'is the column NULL on an "
        "ENCRYPTED lake', inline in ReadDataFile. This site's own answer to "
        "it was an `if (!chunk->GetValue(9, row_idx).IsNull())` wrapped "
        "around the whole call, which SKIPS the resolution rather than "
        "refusing - so an ENCRYPTED lake's delete file with no key was "
        "refused by name on the scan path and silently accepted here, "
        'dying inside the Parquet reader on "is encrypted, but '
        "'encryption_config' was not set\" (#20's shape, again). The mutant "
        "restores exactly that `if` and nothing else: the unwrap and the "
        "unconfigured refusal stay reachable for a non-NULL key, so this "
        "reddens on the NULL and on nothing else. It is what makes the "
        "UNCONDITIONAL call load-bearing rather than a tidier way of "
        "writing the same thing",
        "old": "\t\t\t\t\tfile_info.existing_delete_encryption_key = catalog.ResolveStoredEncryptionKey(\n"
        "\t\t\t\t\t    table_id, file_info.existing_delete_path, resolved_delete_path, true, stored_delete_key);",
        "new": "\t\t\t\t\t// MUTANT no_null_key_refusal_on_flush: v0.1.0-rc.1 skipped the resolve on NULL.\n"
        "\t\t\t\t\tif (!stored_delete_key.IsNull()) {\n"
        "\t\t\t\t\t\tfile_info.existing_delete_encryption_key = catalog.ResolveStoredEncryptionKey(\n"
        "\t\t\t\t\t\t    table_id, file_info.existing_delete_path, resolved_delete_path, true, stored_delete_key);\n"
        "\t\t\t\t\t}",
        # ONE file, and it is the adversary's, written against the released tag
        # before the fix existed. Its positive control is what makes the red mean
        # something: the same lake, the same run, the SCAN path refusing the same
        # NULL by name - so "the flush did not refuse" cannot be read as "no such
        # refusal exists anywhere".
        "reddens": ["test/sql/crypta/adversary_flush_null_key.test"],
        "redden_at": "CALL ducklake_flush_inlined_data('flusher')",
    },
    {
        "name": "no_partition_alter_refusal",
        "file": "src/storage/ducklake_table_entry.cpp",
        "why": "issue #100's ALTER-time guard - ALTER TABLE ... SET PARTITIONED BY "
        "with a non-empty key list is refused on an enveloped lake because "
        "ducklake_file_partition_value is load-bearing (a partitioned scan "
        "reads it back to resolve which files satisfy a predicate), unlike "
        "column stats, which can just be dropped. Without this guard, "
        "requesting partitioning on a crypta lake silently succeeds and the "
        "very next INSERT writes a cleartext cohort-identifying value into "
        "the metadata catalog. This is the guard AND its only call site at "
        "once - there is no other statement in this codebase that enables "
        "partitioning",
        "old": "\tif (!partition_data->fields.empty()) {\n"
        "\t\tauto &duck_catalog = ParentCatalog().Cast<DuckLakeCatalog>();\n"
        "\t\tif (duck_catalog.EncryptionProvider()) {",
        "new": "\tif (false) {\n"
        "\t\tauto &duck_catalog = ParentCatalog().Cast<DuckLakeCatalog>();\n"
        "\t\tif (duck_catalog.EncryptionProvider()) {",
        "reddens": ["test/sql/crypta/crypta_partition_refusals.test"],
        "redden_at": "ALTER TABLE naive.person SET PARTITIONED BY (part_key)",
    },
    {
        "name": "no_partition_write_refusal_body",
        "file": "src/storage/ducklake_transaction.cpp",
        "why": "the JUDGEMENT inside RefusePartitionValuesOnEnvelopedLake - the "
        "write-side half of #100, for a table partitioned BEFORE the lake's "
        "envelope was configured (LoadExistingDuckLake rebuilds "
        "DuckLakePartition straight from persisted metadata, never through "
        "the ALTER statement the guard above sits on, so that guard cannot "
        "see this case). The two call-site mutants below prove each "
        "producer CONSULTS this function; only this one proves what it "
        "ANSWERS, and it is the mutant that would survive if the predicate "
        "itself were inverted or stubbed",
        "old": "\tif (!ducklake_catalog.EncryptionProvider()) {\n"
        "\t\treturn;\n"
        "\t}\n"
        "\tif (file.partition_values.empty()) {\n"
        "\t\treturn;\n"
        "\t}",
        "new": "\tif (true) {\n"
        "\t\treturn;\n"
        "\t}\n"
        "\tif (file.partition_values.empty()) {\n"
        "\t\treturn;\n"
        "\t}",
        # Both files: the persisted-before-crypta test drives both the
        # AppendFiles and AddCompaction call sites, so a mutant of the shared
        # body reddens both of that file's assertions at once. Naming both
        # marker fragments keeps the runner honest about which statement
        # actually failed, rather than one non-zero status standing for two.
        "reddens": [
            "test/sql/crypta/crypta_partition_refusals.test",
            "test/sql/crypta/crypta_partition_refusals.test",
        ],
        "redden_at": [
            "INSERT INTO reattached.person VALUES (1, 'PT-000046-SENTINEL')",
            "CALL ducklake_merge_adjacent_files('reattached', 'person')",
        ],
    },
    {
        "name": "no_partition_write_refusal_on_append",
        "file": "src/storage/ducklake_transaction.cpp",
        "why": "the AppendFiles call site of RefusePartitionValuesOnEnvelopedLake "
        "- proves an ordinary INSERT/UPDATE/MERGE/CTAS actually CONSULTS the "
        "guard rather than the guard merely existing unreachable. Only this "
        "call is removed; RedactStatsOnEnvelopedLake and the AddCompaction "
        "call site are untouched, so this reddens on the INSERT path alone",
        "old": "\t\tRedactStatsOnEnvelopedLake(file);\n"
        "\t\tRefusePartitionValuesOnEnvelopedLake(table_id, file);",
        "new": "\t\tRedactStatsOnEnvelopedLake(file);",
        "reddens": ["test/sql/crypta/crypta_partition_refusals.test"],
        "redden_at": "INSERT INTO reattached.person VALUES (1, 'PT-000046-SENTINEL')",
    },
    {
        "name": "no_partition_write_refusal_on_compaction",
        "file": "src/storage/ducklake_transaction.cpp",
        "why": "the AddCompaction call site of RefusePartitionValuesOnEnvelopedLake "
        "- ducklake_merge_adjacent_files never calls AppendFiles, so the "
        "INSERT-path mutant above cannot see this producer. Only this call "
        "is removed; the AppendFiles call site is untouched, so this reddens "
        "on the compaction path alone",
        "old": "\tRedactStatsOnEnvelopedLake(entry.written_file);\n"
        "\tRefusePartitionValuesOnEnvelopedLake(table_id, entry.written_file);",
        "new": "\tRedactStatsOnEnvelopedLake(entry.written_file);",
        "reddens": ["test/sql/crypta/crypta_partition_refusals.test"],
        "redden_at": "CALL ducklake_merge_adjacent_files('reattached', 'person')",
    },
]

BY_NAME = {mutant["name"]: mutant for mutant in EXTENSION_MUTANTS}
EXTENSION_BY_NAME = BY_NAME


def redden_markers(mutant):
    """
    The failing-statement fragments a mutant must produce, one per named case.

    Normalised to a list the same length as `reddens` so the runner can pair them
    positionally. A single string is the common case and is written as one.
    """
    markers = mutant.get("redden_at")
    if markers is None:
        return [None] * len(mutant["reddens"])
    if isinstance(markers, str):
        markers = [markers]
    if len(markers) != len(mutant["reddens"]):
        raise SystemExit(
            "mutant %s: %d redden_at marker(s) for %d case(s). One marker per "
            "case, or none at all - a roster that pairs them by luck is not a "
            "roster." % (mutant["name"], len(markers), len(mutant["reddens"]))
        )
    return markers


# sqllogictest's own substitution tokens. A marker is matched against the
# statement as the RUNNER ECHOES IT, which is after substitution, so a marker
# carrying one of these can never match - and "can never match" is reported as
# WRONG-RED, i.e. as a defect in the guard rather than in the roster. Refusing
# them here turns a confusing red into an obvious one.
SUBSTITUTED_IN_OUTPUT = ["__TEST_DIR__", "__WORKING_DIRECTORY__", "${"]


def verify_markers(mutant):
    for marker in redden_markers(mutant):
        if marker is None:
            continue
        for token in SUBSTITUTED_IN_OUTPUT:
            if token in marker:
                raise SystemExit(
                    "mutant %s: its redden_at marker contains %s, which "
                    "sqllogictest EXPANDS before it echoes the failing "
                    "statement. Copied verbatim from a .test file it can never "
                    "match, and a marker that can never match reports the guard "
                    "as broken. Use a fragment of the statement that survives "
                    "substitution.\n  marker: %s" % (mutant["name"], token, marker)
                )


# Checked at IMPORT, not when `cases` happens to be called. A marker list that
# had drifted out of step with its `reddens` would otherwise sit there until the
# one subcommand that reads it ran - and the subcommand that reads it is the one
# deciding whether a mutant counts as proven.
for _mutant in EXTENSION_MUTANTS:
    redden_markers(_mutant)
    verify_markers(_mutant)
    # An extension mutant is reversed by substituting `new` back to `old`, so an
    # EMPTY `new` cannot be reversed: `"".count("")` is one-per-character, the
    # exactly-once guard reports a five-figure occurrence count, and the mutant
    # is stuck in the tree. MEASURED, on the first deletion mutant written here -
    # "occurs 223478 times" - and it is the reason a call-site mutant replaces the
    # call with a marker comment instead of deleting the line. A standalone mutant
    # MAY have an empty `new` (`no_backslash_escape` does) because it works on a
    # throwaway copy and is never reversed; an extension mutant edits the tree, so
    # it may not.
    if not _mutant["new"]:
        raise SystemExit(
            "extension mutant %s has an empty 'new'. An in-tree mutant is reversed "
            "by substituting 'new' back to 'old', and the empty string cannot be "
            "matched exactly once - it would leave the guard deleted in the tree. "
            "Replace the line with a marker comment rather than deleting it." % _mutant["name"]
        )


def catch_spec(test_names):
    """
    Build a Catch2 test spec. A comma separates specs, so a comma inside a test
    name has to be escaped or the name silently becomes two names that match
    nothing - and a spec matching nothing exits ZERO, which would read as "the
    mutant did not redden" when in fact nothing ran.
    """
    return ",".join(name.replace("\\", "\\\\").replace(",", "\\,") for name in test_names)


def substitute_exactly_once(target, name, old, new):
    """
    Rewrite `old` to `new` in `target`, and refuse unless `old` is there exactly
    once.

    The exactly-once demand is the whole point and it is shared by both rosters
    deliberately. A pattern that has drifted out of the source would otherwise
    apply NOTHING, the build would come back unmutated, its tests would pass, and
    the runner would report that as "the guard was removed and nothing broke" -
    a survivor that is really a typo. Raising here turns that into a loud error
    with the file and the count in it.
    """
    with open(target, "r") as handle:
        content = handle.read()
    occurrences = content.count(old)
    if occurrences != 1:
        raise SystemExit(
            "mutant %s: its pattern occurs %d times in %s, expected exactly 1. "
            "The source moved under the mutant; fix the pattern rather than "
            "letting it apply nothing." % (name, occurrences, target)
        )
    with open(target, "w") as handle:
        handle.write(content.replace(old, new))


def extension_target(name, repo_root):
    mutant = EXTENSION_BY_NAME.get(name)
    if mutant is None:
        raise SystemExit("unknown mutant: %s" % name)
    return mutant, os.path.join(os.path.abspath(repo_root), mutant["file"])


def patch_extension(name, repo_root):
    """
    Apply a full-extension mutant IN THE TREE.

    There is no copy-and-build path here: the guard lives in a file the whole
    extension is compiled from, so the only build that exercises it is the tree's
    own. `unpatch` reverses it, the runner traps so an interrupted run still
    reverses it, and the runner then requires `git diff` to be empty before it
    will report anything as proven. The tree is the safety net, not this function.
    """
    mutant, target = extension_target(name, repo_root)
    substitute_exactly_once(target, name, mutant["old"], mutant["new"])
    return target


def unpatch_extension(name, repo_root):
    """
    Reverse a full-extension mutant - the same substitution, the other way round.

    A reverse substitution rather than a saved backup on purpose: a backup file
    can be stale, absent, or from a different mutant, and restoring the wrong one
    is silent. The reverse runs under the same exactly-once guard, so restoring a
    file that is not in exactly the mutated state is an error rather than a
    plausible-looking overwrite.
    """
    mutant, target = extension_target(name, repo_root)
    substitute_exactly_once(target, name, mutant["new"], mutant["old"])
    return target


def verify_clean(repo_root):
    """
    Assert that NO full-extension mutant is currently applied to the tree.

    Content-based, not `git diff`, and that is forced rather than preferred: this
    runs inside the devcontainer, and a git WORKTREE checkout has a `.git` FILE
    pointing at an absolute host path that does not exist in the container, so
    git there answers "not a git repository" - a check that errors is a check
    that proves nothing. Reading the source is also the more direct question. Each
    mutant's `old` is exactly the text it removes, so `old` present exactly once,
    for every mutant, IS "nothing is applied". Anything else names the mutant that
    is still in the tree.
    """
    return resolve_roster(repo_root, EXTENSION_MUTANTS)


def resolve_roster(repo_root, roster):
    """
    Every mutant in `roster` must RESOLVE against the tree at `repo_root`.

    Resolving means two things, and both have gone wrong here before:

      1. `old` names a file that exists and occurs in it EXACTLY ONCE. A mutant
         whose `old` cannot match is a positive control that can never fire, and
         a positive control that can never fire is a vacuous check - the repo
         refused one of those before, at 7f910fe, for the same reason. The
         provider abstraction renamed `CryptaClient` to
         `DuckLakeEncryptionProvider`, moved the guard bodies into
         `ducklake_envelope_guards.cpp`, and let clang-format rewrap a call site;
         three mutants stopped matching and NOTHING went red, because nothing
         asked (#20).

      2. Every file in `reddens` exists. A mutant that reddens a test which is
         not in the tree cannot be judged either - the runner would look for a
         failure in a file it cannot run. Seven of these named files that had not
         survived a repo migration (#28).

    Both questions are answerable in milliseconds with no build, which is the
    point: the expensive mutation run should never be the first thing to
    discover that its own roster no longer refers to this repository.

    Returns a list of problem strings; empty means the roster resolves.
    """
    problems = []
    root = os.path.abspath(repo_root)
    for mutant in roster:
        target = os.path.join(root, mutant["file"])
        try:
            with open(target, "r") as handle:
                occurrences = handle.read().count(mutant["old"])
        except IOError:
            problems.append(
                "%s: names %s, which is not a file in this tree - the mutant "
                "cannot be applied at all" % (mutant["name"], mutant["file"])
            )
            continue
        if occurrences != 1:
            problems.append(
                "%s: the text it removes occurs %d times in %s, expected 1 - it "
                "is either still applied, or the source moved under it" % (mutant["name"], occurrences, mutant["file"])
            )
        for case in mutant["reddens"]:
            if not os.path.exists(os.path.join(root, case)):
                problems.append(
                    "%s: reddens %s, which is not in this tree - the mutant's "
                    "red could never be observed" % (mutant["name"], case)
                )
    return problems


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    command = argv[1]
    arguments = argv[2:]
    # Accepted and discarded: kept for callers (this file's own CI workflow
    # among them) that still pass it from when there were two rosters. There is
    # now only the one.
    arguments = [argument for argument in arguments if argument != "--extension"]
    roster = EXTENSION_MUTANTS

    if command == "names":
        for mutant in roster:
            print(mutant["name"])
    elif command == "list":
        for mutant in roster:
            print("%-32s %-46s removes %s" % (mutant["name"], mutant["file"], mutant["why"]))
    elif command == "files":
        # De-duplicated but order-preserving: the runner asserts `git diff` is
        # empty on each of them before it starts and after it finishes, and two
        # mutants in the same file must not make it check twice.
        seen = []
        for mutant in roster:
            if mutant["file"] not in seen:
                seen.append(mutant["file"])
        for path in seen:
            print(path)
    elif command == "spec":
        print(catch_spec(BY_NAME[arguments[0]]["reddens"]))
    elif command == "count":
        # The runner requires the spec to match exactly this many cases. Counting
        # "more than zero" would let one renamed case in a multi-name list go
        # unproven while the mutant still reddened off the others.
        print(len(BY_NAME[arguments[0]]["reddens"]))
    elif command == "cases":
        # One case per line, paired with the failing-statement fragment it must
        # produce. TAB-separated because a test name is a path and a marker is
        # SQL - both can carry spaces, neither carries a tab.
        mutant = BY_NAME[arguments[0]]
        for case, marker in zip(mutant["reddens"], redden_markers(mutant)):
            print("%s\t%s" % (case, marker if marker is not None else ""))
    elif command == "patch":
        print(patch_extension(arguments[0], arguments[1]))
    elif command == "unpatch":
        print(unpatch_extension(arguments[0], arguments[1]))
    elif command == "resolve":
        problems = resolve_roster(arguments[0] if arguments else ".", roster)
        for problem in problems:
            print("::error::%s" % problem)
        # `raise SystemExit`, never `return`. `main` is called as `main(sys.argv)`
        # with its value DISCARDED, so a `return 1` here exits 0 - the subcommand
        # would print six ::error:: lines about a roster that resolves nowhere and
        # the CI step would go green on them. Measured, on the first draft of this
        # very subcommand, by running it against a directory with no `src/`. That
        # is the exact defect class this check exists to remove, one level up, so
        # `resolve_control.py` now runs the CLI and asserts the EXIT CODE too.
        if not roster:
            raise SystemExit("::error::the roster is EMPTY - this check would pass vacuously")
        if problems:
            raise SystemExit(1)
        print("all %d mutants resolve against the tree" % len(roster))
    elif command == "verify-clean":
        problems = verify_clean(arguments[0])
        for problem in problems:
            print(problem)
        if problems:
            raise SystemExit(1)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
