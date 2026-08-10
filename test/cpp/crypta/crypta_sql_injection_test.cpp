//===----------------------------------------------------------------------===//
//                         DuckLake (sigil fork)
//
// test/cpp/crypta/crypta_sql_injection_test.cpp
//
// Issue #33: crypta's REPLY is spliced into the metadata catalog's INSERT text
// while every value beside it on the same row is escaped. #24's defect, word
// for word, on the WRITE path.
//
// PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.
//
// `crypta_injection_test.cpp` is #24 and covers the REQUEST frame: a hostile
// CATALOG value reaching the JSON the client emits. This file is the other
// direction and the other grammar - a hostile SOCKET value reaching the SQL the
// metadata manager emits.
//
// THE PATH, END TO END
//
//   crypta's reply  ->  CryptaClient::ExtractBase64Field   (reads the value)
//                   ->  DuckLakeCryptaProvider::WrapKeys
//                   ->  crypta_wrapped_keys[i]
//                   ->  DuckLakeUtil::WrappedEncryptionKeyLiteral  (quotes it)
//                   ->  "INSERT INTO ducklake_data_file VALUES (...)"
//
// The reader enforced the reply's COUNT and never its ALPHABET, and the literal
// wrapped the value in quotes without doubling the ones inside it. So a reply
// shaped
//
//   {"status":"wrapped","items":[{"wrapped":"RExLAAAA',NULL),(9999,1,1,'x"}]}
//
// produced `'RExLAAAA',NULL),(9999,1,1,'x'` inside a VALUES list: arbitrary rows
// into the metadata catalog.
//
// TRUST MODEL - weaker than #24's, and said plainly rather than dressed up.
// This needs a HOSTILE OR SQUATTED crypta socket. `crypta_socket` is a
// per-ATTACH option, so the attacker here is not #24's catalog writer. It is
// still worth closing: the client demonstrably does NOT treat the reply as
// trusted - MAX_FRAME, the 1..MAX_FRAME response-length check, `ThrowIfError`
// and the reply-count check all exist because it does not - and escaping was
// the one place that posture was dropped.
//
// TWO LAYERS, TWO GUARDS, EACH REDDENED ON ITS OWN
//
//   * `CryptaClient::ExtractBase64Field` refuses a reply value outside the
//     base64 alphabet, so the value never becomes a catalog key at all. That is
//     the outer layer, and in production it fires first.
//   * `DuckLakeUtil::WrappedEncryptionKeyLiteral` escapes whatever it is handed,
//     so the row it writes is well-formed for any input. That is the inner
//     layer.
//
// WHY THE INNER LAYER IS PROVEN BY A DIRECT CALL AND NOT END TO END. This is
// the one structural difference from #24 and it is not laziness. #24's inner
// layer could be reddened through the client, because the provider's alphabet
// check and the client's escaping are different objects and the test can call
// the client directly with the provider out of the way. Here the two guards sit
// on ONE straight line through ONE object graph, and the outer one is
// SUFFICIENT: SQL text is broken by `'`, `'` is not in the base64 alphabet, so
// once `ExtractBase64Field` validates, NO value that reaches the literal can
// carry the character the literal escapes. An end-to-end case for the literal is
// therefore not merely awkward, it is impossible - it would go green off the
// reader and prove nothing about the literal, which is exactly the "a guard
// whose only evidence is another guard's test is not tested" trap. So the
// literal is called directly, which is also honest about what it is: a public
// static utility whose contract is "well-formed SQL for any input", with no
// promise that its callers have pre-validated anything.
//
// ISSUE #55 IS THE SAME PATH AND A DIFFERENT DEFECT, which is why it is in this
// file rather than a new one. #33 asked whether the value can BREAK the SQL;
// #55 asks whether the value can be WRITTEN AT ALL. The alphabet check closed
// the first for every input except the empty string - a zero-length value never
// enters the loop that validates the alphabet, so `IsBase64("")` was vacuously
// true - and the empty string is precisely the one value that reached
// `WrappedEncryptionKeyLiteral`'s `return "NULL"` branch. The outcome is worse
// than an injection: an injection is loud, and this was a SUCCESSFUL commit
// against a data file written encrypted with a real DEK whose wrapped form had
// been thrown away. The two guards for it mirror the two above, one per layer -
// the reader refuses an empty value where it enters, the literal refuses to
// write NULL for a file that has a key - and each has its own case and its own
// mutant for the same reason: a guard whose only evidence is another guard's
// test is not tested.
//===----------------------------------------------------------------------===//

#include "common/ducklake_util.hpp"
#include "crypta_test_support.hpp"
#include "duckdb/common/exception.hpp"

#include <string>
#include <vector>

using namespace duckdb;               // NOLINT
using namespace ducklake_crypta_test; // NOLINT

namespace {

//! The payload from the issue: it closes the SQL literal, closes the row, and
//! opens a whole new one. Nothing in it is a `"`, so the reader's narrow scan
//! reads it WHOLE and the reply-count check is satisfied - which is the point.
//! The count check is not a validator, and this is the value that shows it.
const char *const SPLICES_A_ROW = "RExLAAAA',NULL),(9999,1,1,'evil.parquet";

//! A value whose out-of-alphabet byte is a BACKSLASH rather than a quote,
//! spelled as the JSON ESCAPE a reply has to use to carry one.
//!
//! Kept because the alphabet is the guard, not the quote: a reader that special
//! -cased `'` would pass the case above and fail this one. `\` is also the byte
//! `widened_base64_alphabet` proves `IsBase64` must exclude, so the two rosters
//! agree on what the alphabet is.
//!
//! It used to be spliced in RAW, and #31 is what stopped that reaching the
//! guard. A raw trailing backslash escapes the value's OWN closing quote, so
//! with a string-aware reader the frame never terminates its items array and the
//! refusal comes from the STRUCTURE rather than from the alphabet. Fail-closed
//! either way - but this case exists to prove the ALPHABET check, and a case
//! proven by a different guard than the one it names is not proven. Escaped, the
//! DECODED value ends in a real backslash and `IsBase64` is what refuses it,
//! which is the diagnosis this case is named for.
const char *const ENDS_IN_A_BACKSLASH = "RExLAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\\\\";

//! A wrap reply carrying exactly these `wrapped` values, verbatim, each beside
//! the echoed identity of the file it belongs to.
//!
//! `OkWrapResponse` from the fake server echoes the identity out of the REQUEST;
//! this is here for the cases that need the VALUE spliced in with no encoding of
//! any kind, because encoding the fixture would remove the very bytes under
//! test. The identity beside it is not optional since #31: a reply item with no
//! identity is refused before the alphabet is ever consulted, and these cases
//! would then be proven by the wrong guard.
std::string WrapReply(const std::vector<std::string> &identities, const std::vector<std::string> &values) {
	std::string out = "{\"schema\":\"CryptaWireManifest@v2\",\"status\":\"ok\",\"items\":[";
	for (size_t i = 0; i < values.size(); i++) {
		if (i > 0) {
			out += ",";
		}
		out += "{\"identity\":" + identities[i % identities.size()] + ",\"wrapped\":\"" + values[i] + "\"}";
	}
	out += "]}";
	return out;
}

//! A server that answers every wrap request with one canned reply.
void ServeCannedReply(FakeCryptaServer &server, FakeConnection &connection, const std::string &reply) {
	server.Record(connection.ReadFrame());
	connection.WriteFrame(reply);
}

} // namespace

//===----------------------------------------------------------------------===//
// The outer layer: a reply value that could never be a wrapped key does not
// become a catalog key
//===----------------------------------------------------------------------===//

// mutant: no_reply_alphabet_check
TEST_CASE("crypta: a wrap reply carrying a value outside the base64 alphabet is refused",
          "[crypta][refusal][base64][sql_injection]") {
	// The wrap path specifically, and that is not an arbitrary choice of the two
	// fields `ExtractBase64Field` reads. An `unwrap` reply's `dek` is handed to
	// `Blob::FromBase64` immediately afterwards, which already throws on a byte
	// outside the alphabet - so the unwrap path was fail-closed by accident, with
	// a decoder's diagnosis rather than a protocol one. A `wrap` reply's
	// `wrapped` value is decoded by NOBODY: it goes to the catalog as text. This
	// guard is the only thing in front of it, which is why the case that proves
	// the guard has to be this one.
	SECTION("a value that closes the SQL literal and opens a new row") {
		FakeCryptaServer server;
		auto reply = WrapReply({IdentityEcho(SampleIdentity("t/victim.parquet"))}, {SPLICES_A_ROW});
		server.Start([&](FakeConnection &connection, int) { ServeCannedReply(server, connection, reply); });
		CryptaClient client(server.Path());

		// The message names the FIELD, so a reply that went wrong on the unwrap
		// path cannot be mistaken for this one.
		auto message = ThrownMessage([&]() { client.WrapBatch({SampleIdentity("t/victim.parquet")}, {SampleDek()}); });
		REQUIRE_THAT(message, Catch::Contains("crypta returned a wrapped value that is not base64"));
	}

	SECTION("a value ending in a backslash") {
		FakeCryptaServer server;
		auto reply = WrapReply({IdentityEcho(SampleIdentity("t/victim.parquet"))}, {ENDS_IN_A_BACKSLASH});
		server.Start([&](FakeConnection &connection, int) { ServeCannedReply(server, connection, reply); });
		CryptaClient client(server.Path());

		REQUIRE_THAT(ThrownMessage([&]() { client.WrapBatch({SampleIdentity("t/victim.parquet")}, {SampleDek()}); }),
		             Catch::Contains("crypta returned a wrapped value that is not base64"));
	}

	// ONE bad value spoils the WHOLE batch, rather than the good rows being
	// written and the bad one dropped. A partial commit would put some files in
	// the catalog with keys and leave the rest unwritten, which is a worse
	// failure than refusing the transaction.
	SECTION("one bad value in a batch refuses the batch") {
		FakeCryptaServer server;
		auto reply = WrapReply({IdentityEcho(SampleIdentity("t/innocent.parquet")),
		                        IdentityEcho(SampleIdentity("t/victim.parquet"))},
		                       {WrappedBlob("good"), SPLICES_A_ROW});
		server.Start([&](FakeConnection &connection, int) { ServeCannedReply(server, connection, reply); });
		CryptaClient client(server.Path());

		vector<CryptaFileIdentity> identities {SampleIdentity("t/innocent.parquet"), SampleIdentity("t/victim.parquet")};
		vector<string> deks {SampleDek('a'), SampleDek('b')};
		REQUIRE_THAT(ThrownMessage([&]() { client.WrapBatch(identities, deks); }),
		             Catch::Contains("crypta returned a wrapped value that is not base64"));
	}
}

//===----------------------------------------------------------------------===//
// The outer layer, continued: the ONE value the alphabet check admitted
//
// Issue #55. Every other value outside the alphabet is refused by the loop in
// `IsBase64`; the EMPTY string never enters that loop, so it used to fall out
// the bottom as `true`.
//
// It is worse than the rest of the class rather than a corner of it, and the
// reason is what happens NEXT. A refused value is a failed commit - loud, and
// recoverable. An ACCEPTED empty value was a SUCCESSFUL commit:
// `WrappedEncryptionKeyLiteral("")` returned the SQL literal `NULL`, so the
// catalog row was written with no key at all while the data file on disk had
// been encrypted with a real DEK. The wrapped form of that DEK was discarded,
// the transaction reported success, and the file is unreadable forever by
// anyone - including the operator who wrote it. There is no error and no
// warning; the only way to find out is to read the file later and fail.
//
// The refusal is at the ENTRY, not at the write, and that is the file's stated
// posture rather than a preference: the safety of crypta's reply is an
// assertion about the VALUE, not an assumption about the PEER. The inner-layer
// case further down is the second, independent guard on the same value.
//===----------------------------------------------------------------------===//

// mutant: no_empty_reply_value_refusal
TEST_CASE("crypta: a wrap reply carrying an EMPTY value is refused, batch and all",
          "[crypta][refusal][base64][sql_injection]") {
	// The wrap path for the same reason the alphabet case above gives: a `wrapped`
	// value is decoded by NOBODY, it goes to the catalog as text, so the reader is
	// the only thing in front of it.
	SECTION("a single empty wrapped value") {
		FakeCryptaServer server;
		auto reply = WrapReply({IdentityEcho(SampleIdentity("t/victim.parquet"))}, {""});
		server.Start([&](FakeConnection &connection, int) { ServeCannedReply(server, connection, reply); });
		CryptaClient client(server.Path());

		// The same diagnosis the rest of the class gets, and deliberately so: a
		// zero-length value IS a value that is not base64, and a separate message
		// would suggest a separate guard where there is one.
		auto message = ThrownMessage([&]() { client.WrapBatch({SampleIdentity("t/victim.parquet")}, {SampleDek()}); });
		REQUIRE_THAT(message, Catch::Contains("crypta returned a wrapped value that is not base64"));
	}

	// The WHOLE batch, matching the existing `IsBase64` failure rather than
	// dropping the empty value and keeping the rest. A partial answer is the worst
	// outcome available here: it would leave some files in the commit keyed and
	// the others written encrypted with their keys thrown away, which is exactly
	// the silent loss this guard exists to stop, applied to a subset.
	SECTION("one empty value in a batch refuses the batch") {
		FakeCryptaServer server;
		auto reply = WrapReply({IdentityEcho(SampleIdentity("t/innocent.parquet")),
		                        IdentityEcho(SampleIdentity("t/victim.parquet"))},
		                       {WrappedBlob("good"), ""});
		server.Start([&](FakeConnection &connection, int) { ServeCannedReply(server, connection, reply); });
		CryptaClient client(server.Path());

		vector<CryptaFileIdentity> identities {SampleIdentity("t/innocent.parquet"), SampleIdentity("t/victim.parquet")};
		vector<string> deks {SampleDek('a'), SampleDek('b')};
		REQUIRE_THAT(ThrownMessage([&]() { client.WrapBatch(identities, deks); }),
		             Catch::Contains("crypta returned a wrapped value that is not base64"));
	}

	// And the EMPTY REPLY is still the count check's, not this guard's. Asserted
	// so the two are not conflated: "crypta answered with nothing" and "crypta
	// answered with an empty value for a file" are different failures with
	// different diagnoses, and a guard that swallowed the first would hide it.
	SECTION("an empty reply is still refused by the count check") {
		FakeCryptaServer server;
		auto reply = WrapReply({}, {});
		server.Start([&](FakeConnection &connection, int) { ServeCannedReply(server, connection, reply); });
		CryptaClient client(server.Path());

		REQUIRE_THAT(ThrownMessage([&]() { client.WrapBatch({SampleIdentity("t/victim.parquet")}, {SampleDek()}); }),
		             Catch::Contains("crypta returned 0 wrapped values for 1 requested items"));
	}
}

//===----------------------------------------------------------------------===//
// The inner layer: whatever the literal is handed, the SQL it emits is
// well-formed
//===----------------------------------------------------------------------===//

// mutant: no_wrapped_key_literal_escape
TEST_CASE("ducklake: a wrapped key literal escapes its quotes instead of splicing them into the SQL",
          "[crypta][refusal][sql_injection]") {
	// The strongest form, and the mirror of what the identity case asserts in
	// #24: the literal is exactly what `SQLLiteralToString` - the function every
	// sibling value on the same INSERT goes through - would have produced. An
	// expectation spelled out by hand would drift from the sibling it is supposed
	// to match; this cannot.
	const string hostile = SPLICES_A_ROW;
	REQUIRE(DuckLakeUtil::WrappedEncryptionKeyLiteral(hostile, true) == DuckLakeUtil::SQLLiteralToString(hostile));

	// And spelled out once anyway, because "same as the sibling" is only worth
	// anything if the sibling doubles the quote - so the doubling is asserted
	// here directly rather than by reference.
	REQUIRE(DuckLakeUtil::WrappedEncryptionKeyLiteral(hostile, true) == "'RExLAAAA'',NULL),(9999,1,1,''evil.parquet'");

	// The consequence, stated as the property rather than as a string: outside
	// the opening and closing quote, every `'` in the output is part of a
	// doubled pair. That is what "cannot end its own literal early" means, and it
	// holds for any input, not just this fixture.
	auto literal = DuckLakeUtil::WrappedEncryptionKeyLiteral(hostile, true);
	REQUIRE(literal.size() >= 2);
	REQUIRE(literal.front() == '\'');
	REQUIRE(literal.back() == '\'');
	idx_t interior_quotes = 0;
	for (idx_t i = 1; i + 1 < literal.size(); i++) {
		if (literal[i] != '\'') {
			continue;
		}
		// A quote inside the body must be immediately followed by its pair, and
		// the pair is then skipped whole.
		REQUIRE(i + 2 < literal.size());
		REQUIRE(literal[i + 1] == '\'');
		i++;
		interior_quotes++;
	}
	// Two in the fixture, one per `'` in the input. Asserted so a "fix" that
	// DELETED the quotes rather than doubling them cannot pass the parity check
	// above - with none left there is nothing for that loop to disagree with.
	REQUIRE(interior_quotes == 2);
}

// mutant: no_empty_wrapped_key_refusal
TEST_CASE("ducklake: a wrapped key literal refuses to write NULL for a file that HAS a key",
          "[crypta][refusal][sql_injection]") {
	// The SECOND, independent guard on the same value as the empty-reply case
	// above (#55), and the one that decides what the catalog row actually says.
	//
	// It used to open with `if (wrapped_base64.empty()) { return "NULL"; }` for
	// every caller alike, which is what turned an accepted empty reply into
	// SILENT loss rather than a loud failure: the file was written encrypted, the
	// row was written with no key, and the commit succeeded. On an ENCRYPTED lake
	// there is no caller for which that `NULL` is the right answer - the key
	// existed, so a row saying it did not is a lie the reader can never recover
	// from.
	//
	// Kept as its own guard even though the reader upstream now refuses the same
	// value, for the reason this file already gives about the escaping: the two
	// are SEPARATE LAYERS, this one is a public static utility with no promise
	// that its caller pre-validated anything, and a guard whose only evidence is
	// another guard's test is not tested.
	auto message = ThrownMessage([&]() { DuckLakeUtil::WrappedEncryptionKeyLiteral("", true); });
	REQUIRE_THAT(message, Catch::Contains("empty wrapped encryption key"));
	// The diagnosis has to say WHAT WOULD HAVE HAPPENED, not just that something
	// was refused. This throw fires on a path whose whole defect was being
	// invisible, so an operator reading it needs to know the file is fine and the
	// commit is not - rather than being told a constraint failed.
	REQUIRE_THAT(message, Catch::Contains("unreadable forever"));

	// THE OTHER ARM, and it must keep working - this is the half that stops the
	// fix becoming an over-refusal. A file with NO key at all is a real row on a
	// crypta lake: `ducklake_add_data_files` never sets one, and both wrap sites
	// skip empty-key files, so an empty entry arrives here legitimately. `NULL` is
	// the right column value for it, and `''` would not be - that is a key of
	// length zero, a different row entirely.
	REQUIRE(DuckLakeUtil::WrappedEncryptionKeyLiteral("", false) == "NULL");

	// And a real blob is untouched by the discrimination in either direction. The
	// flag decides only what an EMPTY value means; it is not a second escaping
	// switch, and asserting both ways is what pins that.
	const string blob = WrappedBlob("legit");
	REQUIRE(DuckLakeUtil::WrappedEncryptionKeyLiteral(blob, true) == "'" + blob + "'");
	REQUIRE(DuckLakeUtil::WrappedEncryptionKeyLiteral(blob, false) == "'" + blob + "'");
}

//===----------------------------------------------------------------------===//
// Controls against over-refusal and over-encoding
//
// Every case above asserts that something is refused or rewritten. They name no
// mutant on purpose - these must pass in EVERY build, mutated or not, and they
// are what stops "refuse everything" and "escape everything" from counting as
// fixes.
//===----------------------------------------------------------------------===//

TEST_CASE("crypta: a wrap reply of legitimate base64 punctuation is served, not refused",
          "[crypta][happy][sql_injection]") {
	// `+`, `/` and `=` look dangerous and are perfectly legal base64. A reader
	// that refused them would break every real blob crypta ever issued - and it
	// would do so on the WRITE path, where the failure is a lake that cannot be
	// written to at all.
	const std::string first = "RExL+/9ab+/cdEF==" + std::string(48, 'A');
	const std::string second = WrappedBlob("plain");

	FakeCryptaServer server;
	auto reply = WrapReply({IdentityEcho(SampleIdentity("t/one.parquet")),
	                        IdentityEcho(SampleIdentity("t/two.parquet"))},
	                       {first, second});
	server.Start([&](FakeConnection &connection, int) { ServeCannedReply(server, connection, reply); });
	CryptaClient client(server.Path());

	vector<CryptaFileIdentity> identities {SampleIdentity("t/one.parquet"), SampleIdentity("t/two.parquet")};
	vector<string> deks {SampleDek('a'), SampleDek('b')};
	auto wrapped = client.WrapBatch(identities, deks);
	REQUIRE(wrapped.size() == 2);
	// Byte for byte: the reader validates, it does not rewrite.
	REQUIRE(wrapped[0] == first);
	REQUIRE(wrapped[1] == second);
}

TEST_CASE("ducklake: a wrapped key literal quotes the blob without RE-ENCODING it",
          "[crypta][happy][sql_injection]") {
	// The discriminating half, and the one that a careless "escape it like every
	// sibling" would break. `EncryptionKeyLiteral` beside it takes a RAW key and
	// base64-encodes it; this one takes a value that is ALREADY base64 and must
	// not encode it again, or the row it writes decodes to nothing any reader can
	// use. Escaping is not re-encoding, and this is the case that keeps the two
	// apart.
	const string blob = WrappedBlob("legit+/9==");
	REQUIRE(DuckLakeUtil::WrappedEncryptionKeyLiteral(blob, true) == "'" + blob + "'");
	// Explicitly NOT what the raw-key sibling does with the same bytes.
	REQUIRE(DuckLakeUtil::WrappedEncryptionKeyLiteral(blob, true) != DuckLakeUtil::EncryptionKeyLiteral(blob));

	// An empty blob for a file with NO key is the "this file has no key" row and
	// stays a SQL NULL, not an empty string - `''` would be a key of length zero,
	// which is a different row entirely. Since #55 the caller has to SAY that is
	// what it means; the same empty value for a file that HAS a key throws, and
	// the refusal case above is where that half is asserted.
	REQUIRE(DuckLakeUtil::WrappedEncryptionKeyLiteral("", false) == "NULL");
}
