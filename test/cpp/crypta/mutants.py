#!/usr/bin/env python3
"""
Mutants of the crypta envelope provider, one per refusal guard.

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

Each mutant edits a COPY of `src/crypta/`, never the tree. Nothing here can
change what ships; the copy is handed to the standalone test project through
`-DCRYPTA_SRC_DIR`.

Usage
-----
  mutants.py list
  mutants.py names
  mutants.py apply <name> <destination-dir>
  mutants.py spec  <name>     # the Catch test-spec for the cases it must redden
"""

import os
import shutil
import sys

SOURCES = ["crypta_client.cpp", "ducklake_crypta.cpp"]

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
        "reddens": ["crypta: quotes and control characters in an identity are escaped on the wire"],
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
        "name": "no_truncation_check",
        "file": "crypta_client.cpp",
        "why": "the refusal of a value with no closing quote",
        "old": '\t\tif (end == string::npos) {\n'
               '\t\t\tthrow IOException("crypta response is truncated inside a %s value", field);\n'
               '\t\t}',
        "new": '\t\tif (false) {\n'
               '\t\t\tthrow IOException("crypta response is truncated inside a %s value", field);\n'
               '\t\t}',
        "reddens": ["crypta: a response truncated inside a value is refused"],
    },
    {
        "name": "no_count_check",
        "file": "crypta_client.cpp",
        "why": "the requirement that the value count match the request exactly",
        "old": '\tif (out.size() != expected) {',
        "new": '\tif (false) {',
        "reddens": ["crypta: a value count that does not match the request is refused"],
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
        "old": '\tauto response = Request(body);\n'
               '\tThrowIfError(response);\n'
               '\treturn ExtractBase64Field(response, "wrapped", identities.size());',
        "new": '\tRequest(body);\n'
               '\tauto response = Request(body);\n'
               '\tThrowIfError(response);\n'
               '\treturn ExtractBase64Field(response, "wrapped", identities.size());',
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
        "reddens": ["crypta provider: a plaintext key row is refused, never used"],
    },
    {
        "name": "no_cache_lookup",
        "file": "ducklake_crypta.cpp",
        "why": "the cache lookup itself",
        "old": '\t\tif (entry != unwrap_cache.end()) {',
        "new": '\t\tif (false) {',
        "reddens": ["crypta provider: a repeated unwrap hits the cache and does not re-ask crypta"],
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
        "old": '\tauto cache_key = StringUtil::Format("%s|%lld|%s|%s", identity.lake_id, '
               'static_cast<long long>(identity.table_id),\n'
               '\t                                    identity.is_delete_file ? "delete" : "data", '
               'identity.stored_path) +\n'
               '\t                 "|" + base64_value;',
        "new": '\tauto cache_key = base64_value;',
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
        "old": '\tauto cache_key = StringUtil::Format("%s|%lld|%s|%s", identity.lake_id, '
               'static_cast<long long>(identity.table_id),\n'
               '\t                                    identity.is_delete_file ? "delete" : "data", '
               'identity.stored_path) +\n'
               '\t                 "|" + base64_value;',
        "new": '\tauto cache_key = StringUtil::Format("%s|%lld|%s|%s", identity.lake_id, '
               'static_cast<long long>(identity.table_id),\n'
               '\t                                    identity.is_delete_file ? "delete" : "data", '
               'identity.stored_path);',
        "reddens": ["crypta provider: one identity with two blobs does not collide in the cache"],
    },
]

BY_NAME = {mutant["name"]: mutant for mutant in MUTANTS}


def catch_spec(test_names):
    """
    Build a Catch2 test spec. A comma separates specs, so a comma inside a test
    name has to be escaped or the name silently becomes two names that match
    nothing - and a spec matching nothing exits ZERO, which would read as "the
    mutant did not redden" when in fact nothing ran.
    """
    return ",".join(name.replace("\\", "\\\\").replace(",", "\\,") for name in test_names)


def apply_mutant(name, destination):
    mutant = BY_NAME.get(name)
    if mutant is None:
        raise SystemExit("unknown mutant: %s" % name)

    source_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "src", "crypta"))
    if os.path.isdir(destination):
        shutil.rmtree(destination)
    os.makedirs(destination)
    for filename in SOURCES:
        shutil.copyfile(os.path.join(source_dir, filename), os.path.join(destination, filename))

    target = os.path.join(destination, mutant["file"])
    with open(target, "r") as handle:
        content = handle.read()
    occurrences = content.count(mutant["old"])
    if occurrences != 1:
        raise SystemExit(
            "mutant %s: its pattern occurs %d times in %s, expected exactly 1. "
            "The source moved under the mutant; fix the pattern rather than "
            "letting it apply nothing." % (name, occurrences, mutant["file"])
        )
    with open(target, "w") as handle:
        handle.write(content.replace(mutant["old"], mutant["new"]))
    return destination


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    command = argv[1]
    if command == "names":
        for mutant in MUTANTS:
            print(mutant["name"])
    elif command == "list":
        for mutant in MUTANTS:
            print("%-26s %-22s removes %s" % (mutant["name"], mutant["file"], mutant["why"]))
    elif command == "spec":
        print(catch_spec(BY_NAME[argv[2]]["reddens"]))
    elif command == "count":
        # The runner requires the spec to match exactly this many cases. Counting
        # "more than zero" would let one renamed case in a multi-name list go
        # unproven while the mutant still reddened off the others.
        print(len(BY_NAME[argv[2]]["reddens"]))
    elif command == "apply":
        print(apply_mutant(argv[2], argv[3]))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
