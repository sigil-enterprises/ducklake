#!/usr/bin/env python3
"""
Mutants of the crypta refusal guards, one per guard.

PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.

Why this exists
---------------
Every case in `crypta_refusal_test.cpp` and `crypta_cache_test.cpp` asserts that
the client REFUSES something. An unrun refusal test and a passing one look
identical - both print a green tick - so a green here is worth nothing until the
test has been SEEN to fail. This file removes exactly one guard at a time and
names the test cases that must go red because of it.

That is the positive control for an absence assertion: prove the check fires on
a known-bad build before believing its clean run.

TWO ROSTERS, ONE VOCABULARY
---------------------------
Every mutant, in either roster, is the same record: `{name, file, why, old, new,
reddens}` - plus, in MUTANTS, an optional `src` naming which source directory
`file` lives in. `old` is matched EXACTLY and required to occur EXACTLY once, and
`reddens` names the test cases that must go red. `spec` and `count` answer for a
mutant in either roster, and both rosters share one substitution routine, so
there is one mechanism here and not two. What differs is the BUILD a mutant
needs, and that is the only reason they are separate lists:

  MUTANTS            everything the STANDALONE test project compiles - the crypta
                     client and provider in `src/crypta/`, and since #33
                     `src/common/ducklake_util.cpp` for the SQL literal that
                     writes crypta's reply into the catalog. Each mutant edits a
                     COPY of those directories, never the tree, and the copies are
                     handed to the project through `-DCRYPTA_SRC_DIR` and
                     `-DDUCKLAKE_COMMON_SRC_DIR` (SOURCE_DIRS below). Nothing here
                     can change what ships, and a mutant costs seconds. Driven by
                     `test/cpp/crypta/run_crypta_tests.sh --mutants`, in the
                     per-PR gate.

  EXTENSION_MUTANTS  guards in `src/storage/` and `src/functions/` - ATTACH- and
                     read-time refusals that live in full-extension files. The
                     standalone project compiles a handful of leaf translation
                     units and cannot reach them; pulling `ducklake_catalog.cpp`
                     into it would drag in the whole catalog layer, which is
                     exactly what `ducklake_util.cpp` does NOT do (it links with
                     no undefined DuckLake symbol at all). So these are applied
                     IN THE TREE and
                     the whole extension is rebuilt, their `reddens` names
                     sqllogictest files rather than Catch cases (the unittest
                     binary is Catch2 too, so the spec and the exactly-N-cases
                     control are literally the same code), and a mutant costs a
                     rebuild. Driven by `test/sql/crypta/run_storage_mutants.sh`,
                     on its own cadence - NOT the per-PR gate.

An in-tree edit is the part that needs care, and `patch`/`unpatch` are built so
it cannot go unnoticed: `unpatch` is the REVERSE substitution under the same
exactly-once guard, and the runner refuses to finish unless `git diff` on every
touched file comes back empty.

Usage
-----
  mutants.py list [--extension]
  mutants.py names [--extension]
  mutants.py files --extension          # the tree files the roster edits
  mutants.py apply <name> <destination-dir>   # MUTANTS: copy-then-edit
  mutants.py patch <name> <repo-root>         # EXTENSION_MUTANTS: edit in tree
  mutants.py unpatch <name> <repo-root>       # EXTENSION_MUTANTS: reverse it
  mutants.py verify-clean <repo-root>         # no extension mutant is applied
  mutants.py spec  <name>     # the Catch test-spec for the cases it must redden
  mutants.py count <name>     # how many cases that spec must match
"""

import os
import shutil
import sys

# The tree directories the STANDALONE project compiles, and the copy-directory
# name each one is handed to it under.
#
# `src/crypta` is the whole of it up to #33. `src/common/ducklake_util.cpp` joined
# because `WrappedEncryptionKeyLiteral` - the function that puts crypta's reply
# into the metadata catalog's INSERT text - has no reachable end-to-end red: the
# reader validates the reply's alphabet first, and nothing that clears the base64
# alphabet can carry a quote, so an end-to-end case would go green off the reader
# and prove nothing about the literal. It is therefore called directly, which
# needs the translation unit compiled into this binary and mutable by the same
# copy-then-edit mechanism as everything else.
#
# A mutant names its directory with `src`, defaulting to "crypta" - which is
# every mutant written before #33, so none of them had to change.
SOURCE_DIRS = {
    "crypta": ("src/crypta", ["crypta_client.cpp", "ducklake_crypta.cpp"]),
    "common": ("src/common", ["ducklake_util.cpp"]),
}
DEFAULT_SRC = "crypta"

# The five length-prefixed appends that BUILD the cache key. Three mutants below
# rewrite exactly this block - one keeping only the blob, one keeping only the
# identity, one restoring the bare-'|' join - so the pattern is written once here
# rather than copied three times and drifting apart.
CACHE_KEY_COMPOSITION = (
    '\tAppendLengthPrefixed(cache_key, identity.lake_id);\n'
    '\tAppendLengthPrefixed(cache_key, table_id_text);\n'
    '\tAppendLengthPrefixed(cache_key, file_kind);\n'
    '\tAppendLengthPrefixed(cache_key, identity.stored_path);\n'
    '\tAppendLengthPrefixed(cache_key, base64_value);'
)

# Every `old` below is matched EXACTLY and must occur EXACTLY once. A mutant
# whose pattern drifts out of the source is an error, never a silent skip - a
# mutant that quietly applied nothing would report the unmutated build as red-
# proof, which is the vacuous green this whole file exists to prevent.
MUTANTS = [
    {
        "name": "no_empty_path_check",
        "file": "crypta_client.cpp",
        "why": "the constructor's refusal of an empty socket path",
        "old": '\tif (socket_path.empty()) {\n'
               '\t\tthrow InvalidInputException("crypta socket path is empty");\n'
               '\t}',
        "new": '\tif (false) {\n'
               '\t\tthrow InvalidInputException("crypta socket path is empty");\n'
               '\t}',
        "reddens": ["crypta: an empty socket path is refused at construction"],
    },
    {
        "name": "no_path_length_check",
        "file": "crypta_client.cpp",
        "why": "the refusal of a socket path too long for sun_path - replaced by "
               "the silent truncation the guard's own comment warns about",
        "old": '\tif (socket_path.size() >= sizeof(addr.sun_path)) {\n'
               '\t\tthrow InvalidInputException("crypta socket path is too long (max %llu bytes)",\n'
               '\t\t                            static_cast<uint64_t>(sizeof(addr.sun_path) - 1));\n'
               '\t}',
        "new": '\tif (socket_path.size() >= sizeof(addr.sun_path)) {\n'
               '\t\tsocket_path.resize(sizeof(addr.sun_path) - 1);\n'
               '\t}',
        "reddens": ["crypta: an over-long socket path is refused instead of silently truncated"],
    },
    {
        "name": "no_json_escape",
        "file": "crypta_client.cpp",
        "why": "JSON escaping of the identity",
        "old": '\tstring out;\n\tout.reserve(input.size() + 8);',
        "new": '\tstring out;\n\treturn input;\n\tout.reserve(input.size() + 8);',
        # Since #24 the blob goes through JsonEscape too, so early-returning from
        # it strips BOTH escapings and this mutant reddens the injection cases as
        # well. Listed, because a roster that understates what a mutant does is a
        # positive control that has drifted from the thing it controls.
        "reddens": [
            "crypta: quotes and control characters in an identity are escaped on the wire",
            "crypta: a quote in a wrapped blob is escaped on the wire, not spliced into the request",
            "crypta: an array element injected by a wrapped blob does not become a second request item",
            "crypta: an injected element does not break the unwrap of the other rows in its batch",
        ],
    },
    {
        "name": "no_backslash_escape",
        "file": "crypta_client.cpp",
        "why": "the BACKSLASH arm of JsonEscape, leaving the quote arm intact - a "
               "SEMANTIC mutant where no_json_escape is presence-only. Without it "
               "nothing in the roster described changing WHICH characters the "
               "escaper handles, and a trailing backslash escapes the format "
               "string's own closing quote",
        "old": "\t\tcase '\\\\':\n\t\t\tout += \"\\\\\\\\\";\n\t\t\tbreak;\n",
        "new": "",
        "reddens": [
            "crypta: quotes and control characters in an identity are escaped on the wire",
            "crypta: a blob ending in a backslash cannot escape its own closing quote",
        ],
    },
    {
        "name": "no_blob_escape",
        "file": "crypta_client.cpp",
        "why": "JSON escaping of the WRAPPED BLOB in an unwrap request - issue #24. "
               "The identity beside it was escaped and the blob was not, so a "
               "catalog value could write protocol into the frame",
        "old": '\t\tbody += StringUtil::Format("{\\"identity\\":%s,\\"wrapped\\":\\"%s\\"}", IdentityJson(identities[i]),\n'
               '\t\t                           JsonEscape(blobs[i]));',
        "new": '\t\tbody += StringUtil::Format("{\\"identity\\":%s,\\"wrapped\\":\\"%s\\"}", IdentityJson(identities[i]), blobs[i]);',
        "reddens": [
            "crypta: a quote in a wrapped blob is escaped on the wire, not spliced into the request",
            "crypta: an array element injected by a wrapped blob does not become a second request item",
            "crypta: an injected element does not break the unwrap of the other rows in its batch",
        ],
    },
    {
        "name": "widened_base64_alphabet",
        "file": "crypta_client.cpp",
        "why": "the EDGES of the base64 alphabet - issue #24 review. Widens the "
               "letter range to 'A'..'z', which silently admits the six bytes "
               "between 'Z' and 'a' including the BACKSLASH, one of the exactly "
               "two characters that can break a JSON string. Distinct from "
               "no_blob_alphabet_check, which deletes the CALL and so proves only "
               "that the provider consults the guard, never what it answers - "
               "before this mutant existed the whole suite stayed green with the "
               "range widened",
        "old": "\t\tbool in_alphabet = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '+' ||\n"
               "\t\t                   u == '/' || u == '=';",
        "new": "\t\tbool in_alphabet = (u >= 'A' && u <= 'z') || (u >= '0' && u <= '9') || u == '+' ||\n"
               "\t\t                   u == '/' || u == '=';",
        "reddens": ["crypta: the base64 alphabet is exactly the base64 alphabet, at its edges"],
    },
    {
        "name": "no_reply_alphabet_check",
        "file": "crypta_client.cpp",
        "why": "the base64-alphabet validation of a value read out of crypta's "
               "REPLY - issue #33. The mirror image of no_blob_alphabet_check, "
               "which validates a value going the other way: this one is the only "
               "guard in front of a `wrapped` value, because that value is decoded "
               "by NOBODY - it goes to the catalog as SQL text. The reader "
               "enforced the reply's COUNT and never its ALPHABET, so a hostile or "
               "squatted socket could answer with `RExLAAAA',NULL),(...` and write "
               "arbitrary rows into the metadata catalog",
        "old": '\t\tif (!IsBase64(value)) {',
        "new": '\t\tif (false) {',
        # The second name is a TRANSFER of diagnosis, recorded rather than left to
        # be rediscovered. `a dek value that is not valid base64 is refused`
        # predates #33 and named `Blob::FromBase64` - on the UNWRAP path the
        # decoder was the first thing to object, so that path was fail-closed by
        # accident. This guard sits upstream of the decode and now answers first,
        # so the case had to be rewritten to pin the new order: the reader owns the
        # ALPHABET, the decoder still owns LENGTH and PADDING, and the case has a
        # section for each. Only the alphabet section reddens here; the padding
        # section passes under this mutant, which is the point of keeping both.
        "reddens": [
            "crypta: a wrap reply carrying a value outside the base64 alphabet is refused",
            "crypta: a dek value that is not valid base64 is refused",
        ],
    },
    {
        "name": "no_wrapped_key_literal_escape",
        "file": "ducklake_util.cpp",
        "src": "common",
        "why": "the SQL escaping of the wrapped key on the metadata INSERT - issue "
               "#33, and the FIRST mutant in this roster outside src/crypta. It "
               "restores exactly the pre-fix line, `\"'\" + value + \"'\"`, which is "
               "the one unescaped value on a row where `path.path` beside it goes "
               "through SQLString -> SQLLiteralToString. Its own mutant rather than "
               "a section of no_reply_alphabet_check because the two are SEPARATE "
               "LAYERS - and here the outer layer is SUFFICIENT, so this one has no "
               "reachable end-to-end red at all and is proven by a direct call. A "
               "guard whose only evidence is another guard's test is not tested",
        "old": '\treturn SQLLiteralToString(wrapped_base64);',
        "new": '\treturn "\'" + wrapped_base64 + "\'";',
        "reddens": [
            "ducklake: a wrapped key literal escapes its quotes instead of splicing them into the SQL",
        ],
    },
    {
        "name": "no_blob_alphabet_check",
        "file": "ducklake_crypta.cpp",
        "why": "the base64-alphabet validation of a catalog key value - issue #24. "
               "Its own mutant rather than a section of no_blob_escape, because "
               "the two are SEPARATE LAYERS: the escaping keeps the frame "
               "well-formed for any caller of the client, this keeps a value that "
               "could never decode off the wire at all. A guard whose only "
               "evidence is another guard's test is not tested",
        "old": '\tif (!CryptaClient::IsBase64(base64_value)) {',
        "new": '\tif (false) {',
        # The second name arrived here when #24 merged into #18's branch, and it
        # is a TRANSFER, not an addition: the "'|' in a path" case was written as
        # #18 evidence and listed under `cache_key_unprefixed_join`, but its blob
        # carries a '|', so with both guards present this guard refuses it first
        # and it stopped depending on the length prefixes entirely. MEASURED both
        # ways: it survives `cache_key_unprefixed_join` (rc 0) and reddens here
        # (rc 1). The case still tests something real - it just tests THIS layer
        # now, so it is proven where it is actually load-bearing.
        "reddens": [
            "crypta provider: a wrapped key that is not base64 is refused before it reaches crypta",
            "crypta provider: a '|' in a path cannot be re-read as the cache-key separator",
            "crypta provider: the plaintext floor is consulted BEFORE the alphabet check",
        ],
    },
    {
        "name": "no_error_status_check",
        "file": "crypta_client.cpp",
        "why": "surfacing an error status instead of parsing the response for keys",
        "old": '\tthrow IOException("crypta refused the request: %s", message);',
        "new": '\t(void)message;\n\treturn;',
        "reddens": ["crypta: an error status is surfaced, never parsed for keys",
                    "crypta: a health probe answered with an error frame fails the self-test"],
    },
    {
        # The guard MOVED in #31 and this mutant moved with it - the name is kept
        # because the property is the same one, and renaming it would lose the
        # link back to the case that has always proven it.
        #
        # It used to remove `end == string::npos` inside the field reader. Once
        # the reader became string-aware there was nothing left for that branch to
        # observe: an unterminated string swallows every brace after it, so the
        # items array it sits in never closes either, and the truncation is
        # detected one layer out. The refusal now lives in `SplitReplyItems` and
        # this removes it there. Same fixture, same case, same fail-closed
        # outcome - a different line.
        "name": "no_truncation_check",
        "file": "crypta_client.cpp",
        "why": "the refusal of a reply whose items array never closes - the "
               "truncation guard, which moved from the field reader to the item "
               "splitter when the reader became structural (#31)",
        "old": '\tif (!closed) {\n'
               '\t\tthrow IOException("crypta response is truncated inside its items array");\n'
               '\t}',
        "new": '\tif (false) {\n'
               '\t\tthrow IOException("crypta response is truncated inside its items array");\n'
               '\t}',
        "reddens": ["crypta: a response truncated inside a value is refused"],
    },
    {
        # The pattern moved with the reader in #31 - the check is item-wise now,
        # so it counts ITEMS rather than values found by a scan. Same guard, same
        # message, same case: one value per requested item, exactly.
        "name": "no_count_check",
        "file": "crypta_client.cpp",
        "why": "the requirement that the value count match the request exactly",
        "old": '\tif (items.size() != identities.size()) {',
        "new": '\tif (false) {',
        "reddens": ["crypta: a value count that does not match the request is refused"],
    },
    {
        "name": "no_identity_echo_check",
        "file": "crypta_client.cpp",
        "why": "THE BINDING of a reply item to the file it was asked for - issue "
               "#31. crypta echoes the identity beside every value it returns, and "
               "without this the reply is zipped back onto the caller's file list "
               "by ARRAY POSITION. The count check beside it is not a substitute "
               "and this mutant is what shows why: a count is a LENGTH check, so a "
               "reply with its items REORDERED has exactly the right number and "
               "sails through, handing a file another file's DEK. That is a "
               "wrong-key defect, not the refused-batch availability defect the "
               "count check bounds",
        "old": '\t\tRequireEchoedIdentity(members, identities[i], what);',
        "new": '\t\t// MUTANT no_identity_echo_check: the reply was bound to the request here.',
        "reddens": [
            "crypta: a reordered unwrap reply is refused, not zipped onto the caller's file list",
            "crypta: a reordered wrap reply is refused",
            "crypta: a reply item echoing a different identity is refused, one field at a time",
            "crypta: a reply item with no echoed identity is refused",
        ],
    },
    {
        # A SEMANTIC mutant, not a presence-only one, and the same discipline as
        # widened_base64_alphabet: deleting the call proves the reader CONSULTS
        # the binding, never what the binding ANSWERS. This one leaves it called
        # and narrows its JUDGEMENT to the path - which is the shortcut this would
        # plausibly regress into, because the path is the field that looks like
        # the file. Three real substitutions survive it: two lakes can hold a
        # table 1 with a file at the same relative path, the table id is half of
        # what the key is bound to, and a delete file's key row and a data file's
        # are explicitly not interchangeable.
        "name": "identity_echo_path_only",
        "file": "crypta_client.cpp",
        "why": "the OTHER THREE FIELDS of the echoed identity, leaving the path "
               "compared - the narrowed binding that still looks like a binding",
        "old": '\tif (catalog_uuid != expected.lake_id || table_id != expected.table_id || file_kind != expected_kind ||\n'
               '\t    file_path != expected.stored_path) {',
        "new": '\tif (file_path != expected.stored_path) {',
        "reddens": [
            "crypta: a reply item echoing a different identity is refused, one field at a time",
        ],
    },
    {
        # The STRUCTURE half of #31, and it needs its own mutant for the reason
        # the roster keeps repeating: a binding is worth exactly what the reader's
        # item boundaries are worth, and that is a separate layer from the
        # comparison above. Neither identity mutant can redden these two cases -
        # the identity in both of them is the caller's own, echoed exactly - so
        # without this the top-level-member lookup would carry no red-first
        # evidence at all.
        #
        # It restores the flat scan the reader used before #31, scoped to one
        # item. That is enough to hand the attacker both shapes back: a `dek`
        # buried INSIDE the echoed identity object is found first, and of two
        # `dek` members the first wins instead of the pair being refused.
        "name": "item_field_by_flat_scan",
        "file": "crypta_client.cpp",
        "why": "the TOP-LEVEL-ONLY member lookup for an item's value - issue #31, "
               "restoring the flat text scan it replaced. A value nested inside "
               "the echoed identity, or a second one beside the first, then "
               "becomes the attacker's to choose",
        "old": '\t\tstring value;\n'
               '\t\tif (!DecodeJsonString(RequiredMember(members, field, what), value)) {\n'
               '\t\t\tthrow IOException("crypta answered %s with a %s value that is not a JSON string", what, field);\n'
               '\t\t}',
        "new": '\t\tstring value;\n'
               '\t\t// MUTANT item_field_by_flat_scan: the value was the item\'s own top-level member.\n'
               '\t\tauto flat_key = "\\"" + field + "\\":\\"";\n'
               '\t\tauto flat_at = items[i].find(flat_key);\n'
               '\t\tif (flat_at == string::npos) {\n'
               '\t\t\tthrow IOException("crypta answered %s with no %s member", what, field);\n'
               '\t\t}\n'
               '\t\tauto flat_start = flat_at + flat_key.size();\n'
               '\t\tauto flat_end = items[i].find(\'"\', flat_start);\n'
               '\t\tif (flat_end == string::npos) {\n'
               '\t\t\tthrow IOException("crypta answered %s with a %s value that is not a JSON string", what, field);\n'
               '\t\t}\n'
               '\t\tvalue = items[i].substr(flat_start, flat_end - flat_start);\n'
               '\t\t(void)members;',
        "reddens": [
            "crypta: a dek buried inside the echoed identity is not read as the item's own",
            "crypta: a reply item carrying two dek members is refused",
        ],
    },
    {
        "name": "no_frame_upper_bound",
        "file": "crypta_client.cpp",
        "why": "the upper bound on an announced response length",
        "old": '\tif (response_length == 0 || response_length > MAX_FRAME) {',
        "new": '\tif (response_length == 0) {',
        "reddens": ["crypta: an oversized response frame is refused"],
    },
    {
        "name": "no_frame_lower_bound",
        "file": "crypta_client.cpp",
        "why": "the refusal of a zero-length response frame",
        "old": '\tif (response_length == 0 || response_length > MAX_FRAME) {',
        "new": '\tif (response_length > MAX_FRAME) {',
        "reddens": ["crypta: a zero-length response frame is refused"],
    },
    {
        "name": "no_request_frame_limit",
        "file": "crypta_client.cpp",
        "why": "the refusal to send a request past the frame limit",
        "old": '\tif (json_body.size() > MAX_FRAME) {',
        "new": '\tif (false) {',
        "reddens": ["crypta: a request larger than the frame limit is refused before it is sent"],
    },
    {
        "name": "no_eof_check",
        "file": "crypta_client.cpp",
        "why": "treating a short read as a protocol violation rather than a partial success",
        "old": '\t\tif (rc == 0) {\n'
               '\t\t\tthrow IOException("crypta closed the connection while reading %s", what);\n'
               '\t\t}',
        "new": '\t\tif (rc == 0) {\n'
               '\t\t\tbreak;\n'
               '\t\t}',
        "reddens": ["crypta: a connection closed mid-read is refused"],
    },
    {
        "name": "no_wrap_item_separator",
        "file": "crypta_client.cpp",
        "why": "the comma between items in a wrap_batch body - without it a "
               "two-file commit sends JSON crypta cannot parse",
        "old": '\t\tif (i > 0) {\n'
               '\t\t\tbody += ",";\n'
               '\t\t}\n'
               '\t\tbody += StringUtil::Format("{\\"identity\\":%s,\\"dek\\":\\"%s\\"}", IdentityJson(identities[i]),',
        "new": '\t\tif (false) {\n'
               '\t\t\tbody += ",";\n'
               '\t\t}\n'
               '\t\tbody += StringUtil::Format("{\\"identity\\":%s,\\"dek\\":\\"%s\\"}", IdentityJson(identities[i]),',
        "reddens": ["crypta: a multi-item wrap is one request with well-formed separators"],
    },
    {
        "name": "no_single_request_per_batch",
        "file": "crypta_client.cpp",
        "why": "the one-call-per-commit property of WrapBatch. Not a refusal - a "
               "DESIGN claim the class's own note turns on (writes batch, reads "
               "do not), and the connection-count assertions were the only thing "
               "carrying it. Sending the body twice leaves every returned blob "
               "correct and only the count wrong, which is exactly how this would "
               "regress in practice",
        # Anchored on the wrap path's RETURN rather than on its `Request` call:
        # since #31 both batch paths open with the same two lines, so the old
        # pattern matched twice and the exactly-once guard refused it. Sending the
        # body a second time after the reply has been read is the same observable
        # - two connections, every returned blob still correct, only the
        # one-call-per-commit claim broken.
        "old": '\treturn ExtractBoundBase64Field(response, "wrapped", identities);',
        "new": '\tRequest(body);\n'
               '\treturn ExtractBoundBase64Field(response, "wrapped", identities);',
        "reddens": ["crypta: a multi-item wrap is one request with well-formed separators",
                    "crypta provider: WrapKeys batches a whole commit into one call"],
    },
    {
        "name": "no_socket_creation_check",
        "file": "crypta_client.cpp",
        "why": "the refusal of a socket() that failed - so an fd of -1 is carried "
               "into connect() and the error blames the service instead",
        "old": '\tif (handle.fd < 0) {\n'
               '\t\tthrow IOException("could not create a socket for crypta: %s", strerror(errno));\n'
               '\t}',
        "new": '\tif (false) {\n'
               '\t\tthrow IOException("could not create a socket for crypta: %s", strerror(errno));\n'
               '\t}',
        "reddens": ["crypta: a socket that cannot be created is refused, not used"],
    },
    {
        "name": "no_self_test_ok_check",
        "file": "ducklake_crypta.cpp",
        "why": "the ATTACH self-test's refusal of a service that answers but is "
               "not ok - the fail-open that installs the provider on a sealed KEK",
        "old": '\tif (health.find("\\"ok\\":true") == string::npos) {\n'
               '\t\tthrow IOException("crypta at %s did not report ok: %s", client.SocketPath(), health);\n'
               '\t}',
        "new": '\tif (false) {\n'
               '\t\tthrow IOException("crypta at %s did not report ok: %s", client.SocketPath(), health);\n'
               '\t}',
        "reddens": ["crypta: a service that answers but does not report ok fails the self-test"],
    },
    {
        "name": "no_read_error_check",
        "file": "crypta_client.cpp",
        "why": "raising a socket read failure",
        "old": '\t\t\tthrow IOException("crypta read failed while reading %s: %s", what, strerror(errno));',
        "new": '\t\t\tbreak;',
        "reddens": ["crypta: a socket read failure is refused"],
    },
    {
        "name": "no_write_error_check",
        "file": "crypta_client.cpp",
        "why": "raising a socket write failure",
        "old": '\t\t\tthrow IOException("crypta write failed: %s", strerror(errno));',
        "new": '\t\t\tbreak;',
        "reddens": ["crypta: a socket write failure is refused"],
    },
    {
        # A SEMANTIC mutant, not a presence-only one. Deleting a call proves only
        # that the caller CONSULTS the guard; it never proves what the guard
        # ANSWERS. This one leaves LooksWrapped called and changes its JUDGEMENT -
        # it strips the length floor so the decision reverts to the 4-character
        # prefix alone, which is exactly the over-refusing behaviour the floor was
        # added to remove.
        "name": "no_plaintext_length_floor",
        "file": "crypta_client.cpp",
        "why": "the length floor that stops a plaintext DEK being misread as a "
               "wrapped blob. Without it a 44-character key beginning 'RExL' is "
               "refused forever on a lake that never had crypta, with advice that "
               "does not apply - an unrecoverable false positive on the upstream "
               "path, which is the worst shape this change could have taken",
        "old": 'static constexpr idx_t MAX_PLAINTEXT_KEY_BASE64 = 44;\n'
               '\tif (base64_value.size() <= MAX_PLAINTEXT_KEY_BASE64) {\n'
               '\t\treturn false;\n'
               '\t}\n'
               '\treturn StringUtil::StartsWith(base64_value, WRAPPED_PREFIX);',
        "new": '\treturn StringUtil::StartsWith(base64_value, WRAPPED_PREFIX);',
        "reddens": [
            "crypta: LooksWrapped does not misread a plaintext DEK that happens to start with the magic",
            "crypta provider: a plaintext key row is refused, never used",
        ],
    },
    {
        "name": "no_sigpipe_suppression",
        "file": "crypta_client.cpp",
        "why": "the LOCAL suppression of SIGPIPE on the write path. Both platform "
               "arms go at once - MSG_NOSIGNAL on the send and SO_NOSIGPIPE on "
               "the socket - so the mutant is the pre-fix behaviour on Linux AND "
               "on macOS, not just on whichever one the runner happens to be",
        "old": '#ifdef SO_NOSIGPIPE\n'
               '\tint enabled = 1;\n'
               '\tsetsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));\n'
               '#else\n'
               '\t(void)fd;\n'
               '#endif\n'
               '#ifdef MSG_NOSIGNAL\n'
               '\treturn MSG_NOSIGNAL;\n'
               '#else\n'
               '\treturn 0;\n'
               '#endif',
        "new": '\t(void)fd;\n'
               '\treturn 0;',
        "reddens": [
            "crypta: a socket write failure does not kill a host that leaves SIGPIPE at its default"
        ],
    },
    {
        "name": "no_eintr_retry_read",
        "file": "crypta_client.cpp",
        "why": "the EINTR retry on the read side",
        "old": '\t\tif (rc < 0) {\n'
               '\t\t\tif (errno == EINTR) {\n'
               '\t\t\t\tcontinue;\n'
               '\t\t\t}\n'
               '\t\t\tthrow IOException("crypta read failed while reading %s: %s", what, strerror(errno));',
        "new": '\t\tif (rc < 0) {\n'
               '\t\t\tif (false) {\n'
               '\t\t\t\tcontinue;\n'
               '\t\t\t}\n'
               '\t\t\tthrow IOException("crypta read failed while reading %s: %s", what, strerror(errno));',
        "reddens": ["crypta: a signal during the response read is retried, not reported as failure"],
    },
    {
        "name": "no_eintr_retry_write",
        "file": "crypta_client.cpp",
        "why": "the EINTR retry on the write side",
        "old": '\t\tif (rc < 0) {\n'
               '\t\t\tif (errno == EINTR) {\n'
               '\t\t\t\tcontinue;\n'
               '\t\t\t}\n'
               '\t\t\tthrow IOException("crypta write failed: %s", strerror(errno));',
        "new": '\t\tif (rc < 0) {\n'
               '\t\t\tif (false) {\n'
               '\t\t\t\tcontinue;\n'
               '\t\t\t}\n'
               '\t\t\tthrow IOException("crypta write failed: %s", strerror(errno));',
        "reddens": ["crypta: a signal during the request write is retried, not reported as failure"],
    },
    {
        "name": "no_empty_shortcut_wrap",
        "file": "crypta_client.cpp",
        "why": "the empty-batch shortcut on the wrap path",
        "old": '\tif (identities.empty()) {\n'
               '\t\treturn {};\n'
               '\t}\n'
               '\tstring body = StringUtil::Format("{\\"schema\\":\\"%s\\",\\"op\\":\\"wrap_batch\\",\\"items\\":[", WIRE_SCHEMA);',
        "new": '\tif (false) {\n'
               '\t\treturn {};\n'
               '\t}\n'
               '\tstring body = StringUtil::Format("{\\"schema\\":\\"%s\\",\\"op\\":\\"wrap_batch\\",\\"items\\":[", WIRE_SCHEMA);',
        "reddens": ["crypta: an empty batch returns empty WITHOUT contacting the service"],
    },
    {
        "name": "no_empty_shortcut_unwrap",
        "file": "crypta_client.cpp",
        "why": "the empty-batch shortcut on the unwrap path",
        "old": '\tif (identities.empty()) {\n'
               '\t\treturn {};\n'
               '\t}\n'
               '\tstring body = StringUtil::Format("{\\"schema\\":\\"%s\\",\\"op\\":\\"unwrap_batch\\",\\"items\\":[", WIRE_SCHEMA);',
        "new": '\tif (false) {\n'
               '\t\treturn {};\n'
               '\t}\n'
               '\tstring body = StringUtil::Format("{\\"schema\\":\\"%s\\",\\"op\\":\\"unwrap_batch\\",\\"items\\":[", WIRE_SCHEMA);',
        "reddens": ["crypta: an empty batch returns empty WITHOUT contacting the service"],
    },
    {
        "name": "no_size_mismatch_wrap",
        "file": "crypta_client.cpp",
        "why": "the identities-vs-keys count check on the wrap path",
        "old": '\tif (identities.size() != deks.size()) {',
        "new": '\tif (false) {',
        "reddens": ["crypta: a batch with more keys than identities is refused"],
    },
    {
        "name": "no_size_mismatch_unwrap",
        "file": "crypta_client.cpp",
        "why": "the identities-vs-blobs count check on the unwrap path",
        "old": '\tif (identities.size() != blobs.size()) {',
        "new": '\tif (false) {',
        "reddens": ["crypta: a batch with more keys than identities is refused"],
    },
    {
        "name": "no_lake_id_check",
        "file": "ducklake_crypta.cpp",
        "why": "the refusal of an empty lake id",
        "old": '\tif (lake_id.empty()) {',
        "new": '\tif (false) {',
        "reddens": ["crypta provider: an empty lake id is refused"],
    },
    {
        "name": "no_plaintext_refusal",
        "file": "ducklake_crypta.cpp",
        "why": "the refusal of a plaintext key row on an enveloped lake",
        "old": '\tif (!CryptaClient::LooksWrapped(base64_value)) {',
        "new": '\tif (false) {',
        # The ordering case is named here as well as under no_blob_alphabet_check,
        # deliberately: removing THIS guard makes the under-floor value fall
        # through to the alphabet check, so the case's first assertion - that a
        # short value is diagnosed as a downgrade - flips. A guard whose order is
        # only documented in a comment is not pinned by anything.
        "reddens": [
            "crypta provider: a plaintext key row is refused, never used",
            "crypta provider: the plaintext floor is consulted BEFORE the alphabet check",
        ],
    },
    {
        "name": "no_cache_lookup",
        "file": "ducklake_crypta.cpp",
        "why": "the cache lookup itself",
        "old": '\t\tif (entry != unwrap_cache.end()) {',
        "new": '\t\tif (false) {',
        # The two key-confusion composition cases belong here too, and listing them
        # is what VERIFIES the claim their comments make. Each ends by re-reading
        # the legitimate row and requiring the connection count not to move, so a
        # cache that never serves must redden them. A refusal-only case would
        # survive this mutant - which is precisely the over-refusal blind spot the
        # second half of each case exists to close, so the roster has to prove the
        # half is load-bearing rather than take the comment's word for it.
        "reddens": [
            "crypta provider: a repeated unwrap hits the cache and does not re-ask crypta",
            "crypta provider: a '|' in a path cannot be re-read as the cache-key separator",
            "crypta provider: a '|' in an identity field cannot shift a cache-key boundary",
        ],
    },
    {
        "name": "no_cache_clear",
        "file": "ducklake_crypta.cpp",
        "why": "the wholesale clear when the cap is reached",
        "old": '\t\tif (unwrap_cache.size() >= MAX_CACHED_KEYS) {',
        "new": '\t\tif (false) {',
        "reddens": ["crypta provider: the cache is cleared wholesale when the cap is hit"],
    },
    {
        "name": "cache_key_blob_only",
        "file": "ducklake_crypta.cpp",
        "why": "THE key-confusion guard - commit 7df67912. Keys the cache on the "
               "wrapped blob alone, which is the hole that commit closed",
        "old": CACHE_KEY_COMPOSITION,
        "new": '\tcache_key = base64_value;',
        "reddens": [
            "crypta provider: two identities sharing one blob do not collide in the cache",
            "crypta provider: every component of the identity is part of the cache key",
            "crypta provider: a substituted key row is refused by crypta, not served from the cache",
        ],
    },
    {
        "name": "cache_key_identity_only",
        "file": "ducklake_crypta.cpp",
        "why": "the BLOB half of the cache key. The mirror of cache_key_blob_only, "
               "and it exists because that mutant cannot redden the two-blobs "
               "case: with the key reduced to the blob alone, two DIFFERENT blobs "
               "still give two different keys, so the case passes unmutated and "
               "would have been left claiming red-first evidence it did not have",
        "old": CACHE_KEY_COMPOSITION,
        "new": '\tAppendLengthPrefixed(cache_key, identity.lake_id);\n'
               '\tAppendLengthPrefixed(cache_key, table_id_text);\n'
               '\tAppendLengthPrefixed(cache_key, file_kind);\n'
               '\tAppendLengthPrefixed(cache_key, identity.stored_path);',
        "reddens": ["crypta provider: one identity with two blobs does not collide in the cache"],
    },
    {
        "name": "cache_key_unprefixed_join",
        "file": "ducklake_crypta.cpp",
        "why": "the LENGTH PREFIXES that make the cache key injective, restoring the "
               "bare-'|' join they replaced. Without a length in front of each "
               "component nothing fixes where one ends: the boundary is found by "
               "scanning for a separator, so a '|' inside a component is re-read as "
               "structure and the components become readable across their own "
               "boundaries. Two DIFFERENT (identity, blob) pairs then produce one "
               "key, and the substituted row is served the cached DEK instead of "
               "reaching crypta - the key-confusion bypass, back through the join",
        "old": CACHE_KEY_COMPOSITION,
        "new": '\tcache_key = StringUtil::Format("%s|%lld|%s|%s", identity.lake_id, '
               'static_cast<long long>(identity.table_id),\n'
               '\t                               identity.is_delete_file ? "delete" : "data", '
               'identity.stored_path) +\n'
               '\t            "|" + base64_value;',
        # ONE case, not two, and the missing one is the interesting part.
        #
        # This list used to name the "'|' in a path" case as well. MEASURED after
        # #24 merged: that case SURVIVES this mutant - rc 0, 6 assertions, green
        # with the length prefixes deleted. Its blob is `RExLZZZZ|RExLAAAA`, and
        # `IsBase64` now refuses a '|' before the cache key is ever built, so the
        # case never reaches the code this mutant edits. It is proven by
        # `no_blob_alphabet_check` instead, where it is now listed.
        #
        # Leaving it here would have been invisible rather than loud: the runner
        # builds ONE Catch spec from this whole list and only checks the combined
        # exit status, so the identity-field case below reddens, the run is
        # non-zero, and the mutant reports RED while a case it claims to prove
        # quietly proves nothing. That is the shared-guard blind spot - a mutant
        # that reddens off one caller hides a second caller that stopped
        # depending on the guard. Verify a multi-name roster PER CASE, never by
        # the combined status.
        "reddens": [
            "crypta provider: a '|' in an identity field cannot shift a cache-key boundary",
        ],
    },
]

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
# A CALL SITE IS ITS OWN MUTANT. `RefuseWrappedKeyWithoutCrypta` has one body and
# two callers, and it gets three mutants, not one. A body mutant reddens off
# EITHER caller, so with only that one a caller whose test had rotted away would
# stay invisible - the shared-guard blind spot the `cache_key_unprefixed_join`
# note in the roster above describes from the other side. So each call site is
# deleted on its own and must redden its own file, and the body mutant is kept as
# well because deleting a call proves only that the caller CONSULTS the guard and
# never what the guard ANSWERS.
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
        "old": '\tif (crypta_provider && Encryption() != DuckLakeEncryption::ENCRYPTED) {',
        "new": '\tif (false) {',
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
        "file": "src/storage/ducklake_catalog.cpp",
        "why": "the JUDGEMENT inside RefuseWrappedKeyWithoutCrypta - it keeps "
               "being called and keeps consulting LooksWrapped, and simply "
               "answers 'not wrapped' every time. The two call-site mutants below "
               "prove each caller CONSULTS the guard; only this one proves what "
               "the guard ANSWERS, and it is the mutant that would survive if the "
               "predicate itself were inverted or stubbed",
        "old": '\tif (!CryptaClient::LooksWrapped(stored_key)) {',
        "new": '\tif (true) {',
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
        "old": '\t\tdata.encryption_key = transaction.GetCatalog().ResolveStoredEncryptionKey(table.GetTableId(), path.path,\n'
               '\t\t                                                                         data.path, is_delete_file, stored_key);',
        "new": '\t\t// MUTANT no_wrapped_key_refusal_on_scan: the refusal was in the resolution.\n'
               '\t\tauto mutant_crypta = transaction.GetCatalog().CryptaProvider();\n'
               '\t\tif (mutant_crypta) {\n'
               '\t\t\tdata.encryption_key = mutant_crypta->UnwrapKey(\n'
               '\t\t\t    transaction.GetCatalog().CryptaIdentity(table.GetTableId(), path.path, is_delete_file),\n'
               '\t\t\t    stored_key);\n'
               '\t\t} else {\n'
               '\t\t\tdata.encryption_key = Blob::FromBase64(string_t(stored_key));\n'
               '\t\t}\n'
               '\t\t(void)data.path;',
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
        "old": '\t\t\t\t\t\tfile_info.existing_delete_encryption_key = catalog.ResolveStoredEncryptionKey(\n'
               '\t\t\t\t\t\t    table_id, file_info.existing_delete_path, resolved_delete_path, true, stored_delete_key);',
        "new": '\t\t\t\t\t\t// MUTANT no_wrapped_key_refusal_on_flush: the refusal was in the resolution.\n'
               '\t\t\t\t\t\tauto mutant_crypta = catalog.CryptaProvider();\n'
               '\t\t\t\t\t\tif (mutant_crypta) {\n'
               '\t\t\t\t\t\t\tfile_info.existing_delete_encryption_key = mutant_crypta->UnwrapKey(\n'
               '\t\t\t\t\t\t\t    catalog.CryptaIdentity(table_id, file_info.existing_delete_path, true),\n'
               '\t\t\t\t\t\t\t    stored_delete_key);\n'
               '\t\t\t\t\t\t} else {\n'
               '\t\t\t\t\t\t\tfile_info.existing_delete_encryption_key = Blob::FromBase64(stored_delete_key);\n'
               '\t\t\t\t\t\t}\n'
               '\t\t\t\t\t\t(void)resolved_delete_path;',
        # ONE file, and that is the finding rather than an omission:
        # `grep -rn RefuseWrappedKeyWithoutCrypta test/` finds this site named in
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
        "old": '\t\t\t\t\t\tfile_info.existing_delete_encryption_key = catalog.ResolveStoredEncryptionKey(\n'
               '\t\t\t\t\t\t    table_id, file_info.existing_delete_path, resolved_delete_path, true, stored_delete_key);',
        "new": '\t\t\t\t\t\t// MUTANT no_unwrap_on_flush: the unwrap was in the resolution.\n'
               '\t\t\t\t\t\tcatalog.RefuseWrappedKeyWithoutCrypta(resolved_delete_path, stored_delete_key);\n'
               '\t\t\t\t\t\tfile_info.existing_delete_encryption_key = Blob::FromBase64(stored_delete_key);\n'
               '\t\t\t\t\t\t(void)table_id;',
        "reddens": ["test/sql/crypta/crypta_flush_configured_unwrap.test"],
        "redden_at": "CALL ducklake_flush_inlined_data('configured')",
    },
]

# A standalone mutant must name a file the standalone project actually compiles.
# Checked at IMPORT rather than when `apply` happens to run: a mutant pointing at
# a file no build contains would otherwise surface as a copy-time IOError deep
# inside a runner loop, and the loop's own report would have to guess what it
# meant.
for _mutant in MUTANTS:
    _where = _mutant.get("src", DEFAULT_SRC)
    if _where not in SOURCE_DIRS:
        raise SystemExit(
            "mutant %s names source directory '%s', which is not one the "
            "standalone project compiles (%s)"
            % (_mutant["name"], _where, ", ".join(sorted(SOURCE_DIRS)))
        )
    if _mutant["file"] not in SOURCE_DIRS[_where][1]:
        raise SystemExit(
            "mutant %s names %s in '%s', which that directory does not carry (%s)"
            % (_mutant["name"], _mutant["file"], _where, ", ".join(SOURCE_DIRS[_where][1]))
        )

BY_NAME = {mutant["name"]: mutant for mutant in MUTANTS}
for _mutant in EXTENSION_MUTANTS:
    # Names are the roster's public handles - `spec`, `count`, and
    # ducklake-bench's `control_located_in` all resolve a bare name against BOTH
    # lists. A duplicate would make which mutant you got depend on list order.
    if _mutant["name"] in BY_NAME:
        raise SystemExit("duplicate mutant name across the two rosters: %s" % _mutant["name"])
    BY_NAME[_mutant["name"]] = _mutant

EXTENSION_BY_NAME = {mutant["name"]: mutant for mutant in EXTENSION_MUTANTS}


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
            "Replace the line with a marker comment rather than deleting it."
            % _mutant["name"]
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


def apply_mutant(name, destination):
    mutant = BY_NAME.get(name)
    if mutant is None:
        raise SystemExit("unknown mutant: %s" % name)
    if name in EXTENSION_BY_NAME:
        raise SystemExit(
            "mutant %s is a FULL-EXTENSION mutant and cannot be copied into the "
            "standalone project - it lives in %s, which that project does not "
            "compile. Use `patch`/`unpatch` and run_storage_mutants.sh."
            % (name, mutant["file"])
        )

    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
    if os.path.isdir(destination):
        shutil.rmtree(destination)
    # EVERY source directory is copied, not just the mutant's own. The standalone
    # project compiles all of them, and pointing one knob at a copy while the
    # other still pointed into the tree would build a binary that is half mutant
    # and half live source - which is also the shape that would silently start
    # editing the tree if a copy went missing.
    for key, (relative, filenames) in SOURCE_DIRS.items():
        target_dir = os.path.join(destination, key)
        os.makedirs(target_dir)
        for filename in filenames:
            shutil.copyfile(os.path.join(repo_root, relative, filename), os.path.join(target_dir, filename))

    where = mutant.get("src", DEFAULT_SRC)
    substitute_exactly_once(os.path.join(destination, where, mutant["file"]), name, mutant["old"], mutant["new"])
    return destination


def extension_target(name, repo_root):
    mutant = EXTENSION_BY_NAME.get(name)
    if mutant is None:
        raise SystemExit(
            "unknown full-extension mutant: %s (it is %s)"
            % (name, "a standalone mutant" if name in BY_NAME else "in no roster")
        )
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
    problems = []
    for mutant in EXTENSION_MUTANTS:
        target = os.path.join(os.path.abspath(repo_root), mutant["file"])
        with open(target, "r") as handle:
            occurrences = handle.read().count(mutant["old"])
        if occurrences != 1:
            problems.append(
                "%s: the text it removes occurs %d times in %s, expected 1 - it "
                "is either still applied, or the source moved under it"
                % (mutant["name"], occurrences, mutant["file"])
            )
    return problems


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    command = argv[1]
    arguments = argv[2:]
    extension = "--extension" in arguments
    arguments = [argument for argument in arguments if argument != "--extension"]
    roster = EXTENSION_MUTANTS if extension else MUTANTS

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
    elif command == "apply":
        print(apply_mutant(arguments[0], arguments[1]))
    elif command == "patch":
        print(patch_extension(arguments[0], arguments[1]))
    elif command == "unpatch":
        print(unpatch_extension(arguments[0], arguments[1]))
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
