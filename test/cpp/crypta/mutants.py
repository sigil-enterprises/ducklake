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
