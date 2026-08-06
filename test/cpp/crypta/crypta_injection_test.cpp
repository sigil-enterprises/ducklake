//===----------------------------------------------------------------------===//
//                         DuckLake (sigil fork)
//
// test/cpp/crypta/crypta_injection_test.cpp
//
// Issue #24: the wrapped blob is spliced into the request JSON while every
// identity field beside it is escaped.
//
// PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.
//
// `crypta_refusal_test.cpp` already proves the IDENTITY is escaped. Nothing
// proved the same of the blob, and nothing could: the escaping case there reads
// the frame back for `file_path` and `catalog_uuid` only, and the
// unterminated-value case is about a RESPONSE. This file asserts on the
// REQUEST frame the client emits when the blob itself is hostile.
//
// REACHABILITY - stated carefully, because the first version of this comment
// got it wrong and a wrong reachability claim is worse than none.
//
// The route is CATALOG WRITE ACCESS, and only that. An actor who can write
// `ducklake_data_file` sets `encryption_key` and `stored_path` on the same row,
// so it controls the blob bytes outright. The only thing between that column
// and the format string is a four-character prefix test. This is the threat
// model the envelope already assumes and is unchanged from #18: an attacker
// with catalog write who cannot mint a valid blob.
//
// It is NOT "an operator adds a data file". That claim was in this comment and
// is FALSE - `ducklake_add_data_files` never sets `encryption_key` at all, both
// wrap sites `continue` past a row whose key is empty
// (ducklake_metadata_manager.cpp:3782, :3949), and on an encrypted lake the
// resolution throws "does not have an encryption key" BEFORE the identity is
// built. A row added that way never reaches `UnwrapKey`. That throw used to be
// cited by line number inside `ReadDataFile`; it now lives on the catalog, as
// `DuckLakeCatalog::RefuseMissingEncryptionKey`, reached through
// `ResolveStoredEncryptionKey` from BOTH decode sites (#53). Named rather than
// numbered on purpose - a line-number citation retargets silently, which is
// exactly what this one did.
//
// Two more dead routes, recorded so nobody re-walks them: a table name cannot
// carry the metacharacter (`CanGeneratePathFromName` admits only alphanumerics,
// `_` and `-`, and falls back to the table UUID), and a hive partition value is
// `URLEncode`d.
//
// Two layers, two guards, and each is reddened on its own - a guard whose only
// evidence is another guard's test is not tested:
//
//   * `DuckLakeCryptaProvider::UnwrapKey` refuses a blob that is not base64 at
//     all, so the value never reaches the wire. That is the outer layer, and in
//     production it fires first.
//   * `CryptaClient::UnwrapBatch` escapes the blob, so a frame it emits is
//     well-formed whatever it was handed. That is the inner layer, and the
//     client is a public class - the provider is not its only possible caller.
//
// MEASURED, not assumed - on both sides of the socket.
//
// HERE: before the fix the injection DOES change the frame the client emits.
// `SplitItems` below - a structural scanner that FAILS rather than guess, not a
// substring count - sees TWO items where the caller asked for one.
//
// AT CRYPTA (measured against its real `Request` type and its real
// `Service::handle`, sigil-enterprises/crypta @ c5b87be - every line below is
// an observed answer, not a reading of serde's documentation):
//   * DUPLICATE MEMBER is REFUSED:
//       `protocol: unrecognised request: duplicate field 'identity'`
//     Specifically NOT last-wins, which would have been a live bypass - the
//     attacker's identity never replaces the caller's. Worth measuring rather
//     than assuming, because `Request` is an INTERNALLY TAGGED enum
//     (`#[serde(tag = "op")]`), so serde buffers the map and replays it into
//     the variant's visitor, and that replay is the path where duplicate
//     handling is least obvious. It was measured with a control first: the
//     frame is valid JSON carrying `"identity":` twice, so the error is about
//     the duplicate and not about a broken frame.
//   * EXTRA ARRAY ELEMENT parses cleanly and is SERVED. crypta has no count
//     check of any kind, so a caller that sent one item is answered with two
//     DEKs, in request order.
//   * The two ways to keep the COUNT right while still injecting are both
//     refused at the frame parse, so the count check cannot be sidestepped:
//     closing the array early (`RExL"}]}`) leaves the client's own trailing
//     `,{...}]}` outside the object -> `trailing characters`; ending the blob
//     with a backslash to swallow the next item escapes the format string's
//     closing quote -> `expected ',' or '}'`. An injection can therefore only
//     ever ADD elements, and the client emits all N itself so it cannot be
//     made to under-count.
//
// So what stops key confusion is neither of those. It is the reply-count check
// in `ExtractBase64Field` (crypta_client.cpp), which refuses a reply that does
// not carry exactly one value per requested item - and an injected element
// always makes the reply longer. The pre-fix consequence is therefore a
// REFUSED BATCH, an availability defect, not a file served the wrong key.
// `an injected element does not break the other rows in its batch` is that
// measurement, kept as a test so the bound is re-derived on every run rather
// than remembered from a comment.
//===----------------------------------------------------------------------===//

#include "crypta/ducklake_crypta.hpp"
#include "crypta_test_support.hpp"
#include "duckdb/common/exception.hpp"

#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace duckdb;               // NOLINT
using namespace ducklake_crypta_test; // NOLINT

namespace {

//! Split the top-level objects out of the request's `items` array, and FAIL
//! rather than guess.
//!
//! A structural scanner rather than a count of `{"identity":` occurrences: the
//! question being asked is "how many items would a parser see", and a substring
//! count cannot answer it. It tracks string state and escapes, so an escaped
//! brace or quote inside a value opens and closes nothing - which is exactly
//! what the fix relies on.
//!
//! Returns false on anything it cannot account for: no `items` array, a brace
//! that closes one never opened, a string still open at the end, or an array
//! that never closes.
//!
//! The bool is load-bearing and was added after review. An earlier version
//! returned the vector alone and, on malformed input, returned a PLAUSIBLE
//! COUNT - failing silently in the one direction that hides an injection. The
//! payload that showed it is `RExL"}],"x":"`: unescaped, that closes the item
//! and the array early, the scan stops at the injected `]`, and a bare
//! `SplitItems(body).size() == 1` PASSES against the unfixed client. An oracle
//! that answers confidently on input it did not understand is worse than none.
bool SplitItems(const std::string &body, std::vector<std::string> &items) {
	items.clear();
	const std::string opener = "\"items\":[";
	auto start = body.find(opener);
	if (start == std::string::npos) {
		return false;
	}
	int depth = 0;
	bool in_string = false;
	bool escaped = false;
	bool closed = false;
	size_t item_start = 0;
	size_t i = start + opener.size();
	for (; i < body.size(); i++) {
		char c = body[i];
		if (in_string) {
			if (escaped) {
				escaped = false;
			} else if (c == '\\') {
				escaped = true;
			} else if (c == '"') {
				in_string = false;
			}
			continue;
		}
		if (c == '"') {
			in_string = true;
		} else if (c == '{') {
			if (depth == 0) {
				item_start = i;
			}
			depth++;
		} else if (c == '}') {
			if (depth == 0) {
				return false;
			}
			depth--;
			if (depth == 0) {
				items.push_back(body.substr(item_start, i - item_start + 1));
			}
		} else if (c == ']' && depth == 0) {
			closed = true;
			break;
		}
	}
	return closed && !in_string && depth == 0;
}

//! The count, for assertions that only care how many items a parser would see.
//! Returns -1 when the frame could not be accounted for, so a malformed frame
//! can never be mistaken for a well-formed one of any size.
int ItemCount(const std::string &body) {
	std::vector<std::string> items;
	if (!SplitItems(body, items)) {
		return -1;
	}
	return static_cast<int>(items.size());
}

//! A DEK that names the file it belongs to, so a reply zipped onto the wrong
//! file is visible in the assertion rather than being two indistinguishable
//! blocks of 'k'.
//!
//! The digest goes FIRST because the value is cut to 32 bytes. An earlier
//! version was `"dek-for:" + path` truncated, which silently collapsed to the
//! same DEK for any two paths agreeing in their first 24 characters - and a
//! shifted zip would then compare EQUAL, defeating the one property this helper
//! exists to provide. No fixture was long enough to trip it, so it had to be
//! reasoned about rather than observed; putting the digest in front of the
//! truncation point removes the length dependence entirely.
std::string DekForPath(const std::string &path) {
	std::string dek = "dek:" + std::to_string(std::hash<std::string>()(path)) + ":" + path;
	dek.resize(32, '.');
	return dek;
}

//! A fake crypta that answers the request it was actually SENT.
//!
//! Load-bearing: it derives one DEK per item it can parse out of the frame, in
//! frame order. A server that replied with a fixed list would answer the
//! request the test MEANT to send and could never show an injected element at
//! all. This one replies to the frame on the wire, so an extra item produces an
//! extra reply - which is how the injection becomes observable end to end.
void ServeParsedItems(FakeCryptaServer &server, FakeConnection &connection) {
	auto body = connection.ReadFrame();
	server.Record(body);
	std::vector<std::string> deks;
	std::vector<std::string> parsed;
	// A frame the scanner cannot account for gets an empty reply, so the client
	// reddens on its own count check rather than the fake inventing a plausible
	// answer for a frame nobody understood.
	SplitItems(body, parsed);
	for (auto &item : parsed) {
		std::string path;
		if (!DecodeJsonStringField(item, "file_path", path)) {
			path = "<no file_path>";
		}
		deks.push_back(DekForPath(path));
	}
	connection.WriteFrame(OkUnwrapResponse(deks));
}

//! A blob that closes its own JSON string and opens a whole new array element.
//! Well-formed JSON with no duplicate key - the shape a strict parser accepts.
//!
//! It keeps a plausible FULL blob in front of the injection rather than
//! degenerating to the bare four characters, because that is the shape the
//! attacker actually has: crypta's blob magic is `DLK1`, and base64("DLK") is
//! "RExL", so every legitimate blob already satisfies the prefix gate. The gate
//! costs the attacker nothing and does not force a truncated value.
const char *const INJECTS_AN_ELEMENT = "RExLMQAAAAB2YWxpZC1sb29raW5nLWJsb2I="
                                       "\"},{\"identity\":{\"catalog_uuid\":\"test-lake\",\"table_id\":7,"
                                       "\"file_kind\":\"data\",\"file_path\":\"t/ghost.parquet\"},"
                                       "\"wrapped\":\"RExLghost";

//! A blob that emits a SECOND `identity` member into the SAME object. It
//! deliberately re-opens `wrapped` too, so the object the format string closes
//! is well-formed JSON - a duplicate-key object, not merely a broken frame.
const char *const INJECTS_A_DUPLICATE_MEMBER = "RExL\",\"identity\":{\"catalog_uuid\":\"other-lake\",\"table_id\":99,"
                                               "\"file_kind\":\"delete\",\"file_path\":\"t/somebody-elses.parquet\"},"
                                               "\"wrapped\":\"RExLother";

//! A blob whose LAST byte is a backslash.
//!
//! The other fixtures attack with `"`. This one attacks with `\\`, which is the
//! other half of the pair `JsonEscape` exists for and the one no blob in this
//! suite fed it: unescaped, the trailing backslash escapes the format string's
//! OWN closing quote, so the value runs on past the end of its item instead of
//! terminating. Measured against crypta, that shape answers
//! `expected ',' or '}'` - so it is refused there too, but only because the
//! grammar happens to catch it downstream.
const char *const ENDS_IN_A_BACKSLASH = "RExLAAAA\\";

size_t CountOccurrences(const std::string &haystack, const std::string &needle) {
	size_t count = 0;
	for (size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + 1)) {
		count++;
	}
	return count;
}

} // namespace

//===----------------------------------------------------------------------===//
// The inner layer: whatever the client is handed, the frame it emits is
// well-formed and says exactly what the caller asked
//===----------------------------------------------------------------------===//

// mutant: no_blob_escape
TEST_CASE("crypta: a quote in a wrapped blob is escaped on the wire, not spliced into the request",
          "[crypta][refusal][json_escape][injection]") {
	FakeCryptaServer server;
	server.Start([&](FakeConnection &connection, int) { ServeParsedItems(server, connection); });
	CryptaClient client(server.Path());

	std::string blob = INJECTS_A_DUPLICATE_MEMBER;
	// The reply this earns depends on what the frame says, so the call is allowed
	// to throw. The assertions are on the FRAME, which is recorded either way.
	ThrownMessage([&]() { client.UnwrapBatch({SampleIdentity("t/victim.parquet")}, {blob}); });

	auto requests = server.Requests();
	REQUIRE(requests.size() == 1);
	auto &body = requests[0];

	// 1. The strongest form, and the mirror of what the identity case asserts:
	//    scanning the value to its next UNESCAPED quote gives back exactly the
	//    bytes that went in. Unescaped, the value ends at `RExL` and this fails.
	std::string decoded;
	REQUIRE(DecodeJsonStringField(body, "wrapped", decoded));
	REQUIRE(decoded == blob);

	// 2. The blob's `identity` did not become a member of the request. Post-fix
	//    the injected copy is spelled `{\"identity\":`, so an exact-substring
	//    search finds only the real one.
	REQUIRE(CountOccurrences(body, "\"identity\":") == 1);
	REQUIRE(CountOccurrences(body, "\"wrapped\":\"") == 1);
	REQUIRE(CountOccurrences(body, "\"catalog_uuid\":\"other-lake\"") == 0);

	// The object stays ONE item either way - that is what makes this the
	// duplicate-member shape rather than the extra-element shape below.
	REQUIRE(ItemCount(body) == 1);

	// 3. And the identity a parser would read is still the caller's.
	std::string path;
	REQUIRE(DecodeJsonStringField(body, "file_path", path));
	REQUIRE(path == "t/victim.parquet");
}

// mutant: no_blob_escape
TEST_CASE("crypta: an array element injected by a wrapped blob does not become a second request item",
          "[crypta][refusal][json_escape][injection]") {
	FakeCryptaServer server;
	server.Start([&](FakeConnection &connection, int) { ServeParsedItems(server, connection); });
	CryptaClient client(server.Path());

	std::string blob = INJECTS_AN_ELEMENT;
	// One file in, so one item must go out. The call itself may or may not
	// succeed depending on what the fake replies - the assertion is on the FRAME,
	// which is recorded either way.
	ThrownMessage([&]() { client.UnwrapBatch({SampleIdentity("t/victim.parquet")}, {blob}); });

	auto requests = server.Requests();
	REQUIRE(requests.size() == 1);

	// A parser's count, not a grep's. Unescaped, this is 2.
	std::vector<std::string> items;
	REQUIRE(SplitItems(requests[0], items));
	REQUIRE(items.size() == 1);

	std::string decoded;
	REQUIRE(DecodeJsonStringField(items[0], "wrapped", decoded));
	REQUIRE(decoded == blob);

	// The ghost path is still IN the frame - it is part of the blob's value, and
	// escaping does not delete text, it neuters it. What must not survive is the
	// ghost as a FIELD, so the assertion is on the field, not on the bytes: one
	// item, one identity, one path, and that path is the caller's.
	REQUIRE(CountOccurrences(requests[0], "\"identity\":") == 1);
	REQUIRE(CountOccurrences(requests[0], "\"file_path\":\"") == 1);
	std::string path;
	REQUIRE(DecodeJsonStringField(items[0], "file_path", path));
	REQUIRE(path == "t/victim.parquet");
}

// mutant: no_blob_escape
TEST_CASE("crypta: an injected element does not break the unwrap of the other rows in its batch",
          "[crypta][refusal][json_escape][injection]") {
	// The consequence case, and the honest bound on the severity.
	//
	// A caller zips the reply back onto its file list positionally, so an element
	// injected in the middle is the shape that would hand file N the DEK for file
	// N-1. It does not get that far: an extra request item produces an extra
	// reply value, and the reply-count check refuses the whole batch. The
	// measured pre-fix failure of this case is therefore
	// "crypta returned 3 dek values for 2 requested items" - a refused batch, an
	// availability defect. Not key confusion.
	//
	// Recorded as a test rather than a note so the bound is re-measured on every
	// run: if a future change ever let a mismatched reply through, this stops
	// being green in a way a comment could not.
	FakeCryptaServer server;
	server.Start([&](FakeConnection &connection, int) { ServeParsedItems(server, connection); });
	CryptaClient client(server.Path());

	vector<CryptaFileIdentity> identities {SampleIdentity("t/hostile.parquet"), SampleIdentity("t/innocent.parquet")};
	vector<string> blobs {INJECTS_AN_ELEMENT, "RExLinnocent"};

	auto keys = client.UnwrapBatch(identities, blobs);
	REQUIRE(keys.size() == 2);
	// Each file gets ITS OWN key. Naming the path inside the DEK is what makes a
	// shifted zip a visible failure instead of a passing comparison.
	REQUIRE(keys[0] == DekForPath("t/hostile.parquet"));
	REQUIRE(keys[1] == DekForPath("t/innocent.parquet"));
	REQUIRE(ItemCount(server.Requests()[0]) == 2);
}

//===----------------------------------------------------------------------===//
// The outer layer: a value that could never be a key does not reach the wire
//===----------------------------------------------------------------------===//

// mutant: no_blob_alphabet_check
TEST_CASE("crypta provider: a wrapped key that is not base64 is refused before it reaches crypta",
          "[crypta][refusal][base64][injection]") {
	FakeCryptaServer server;
	server.Start([&](FakeConnection &connection, int) { ServeParsedItems(server, connection); });
	DuckLakeCryptaProvider provider(server.Path(), "test-lake");
	auto identity = SampleIdentity("t/victim.parquet");

	SECTION("a blob carrying JSON punctuation") {
		auto message = ThrownMessage([&]() { provider.UnwrapKey(identity, INJECTS_AN_ELEMENT); });
		REQUIRE_THAT(message, Catch::Contains("is not base64"));
		REQUIRE_THAT(message, Catch::Contains("t/victim.parquet"));
	}
	// EVERY fixture below goes through WrappedBlob, and the reason is not tidiness.
	//
	// `UnwrapKey` consults LooksWrapped BEFORE IsBase64, and #19/#20/#21 narrowed
	// LooksWrapped from `startswith("RExL")` to `startswith("RExL") AND length >
	// 44`. A short literal - the shape these two sections used to carry - is
	// therefore no longer a "wrapped blob" at all: it is refused as a PLAINTEXT
	// DOWNGRADE, before the alphabet check this case exists to test, and the case
	// then reports a message it never wrote. Fail-closed either way, so nothing
	// was unsafe; the evidence was simply gone.
	//
	// Measured rather than reasoned, and the discriminator is length alone: the
	// JSON-punctuation section above kept passing throughout, because
	// INJECTS_AN_ELEMENT is a long concatenated literal. Same case, same guard
	// order, same expectation - only the length differs.
	//
	// So: a fixture that must reach IsBase64 has to clear the floor first.
	// WrappedBlob appends 64 characters, which clears it by construction and
	// cannot silently stop doing so.
	SECTION("a blob carrying a control character") {
		REQUIRE_THAT(ThrownMessage([&]() { provider.UnwrapKey(identity, WrappedBlob(std::string("\x01\x1f"))); }),
		             Catch::Contains("is not base64"));
	}
	SECTION("a blob carrying a character that is merely outside the alphabet") {
		REQUIRE_THAT(ThrownMessage([&]() { provider.UnwrapKey(identity, WrappedBlob("with-a-hyphen")); }),
		             Catch::Contains("is not base64"));
	}

	// The load-bearing assertion: refusing is not enough, it must refuse WITHOUT
	// asking crypta. A value that can never decode has no business on the wire.
	std::this_thread::sleep_for(std::chrono::milliseconds(60));
	REQUIRE(server.Connections() == 0);
}

//===----------------------------------------------------------------------===//
// Controls against over-refusal
//
// Every case above asserts that something is refused or rewritten. A suite made
// only of those cannot tell a correct fix from one that refuses everything, or
// an escaper from a mangler. These two are what make the greens above mean
// something, and they name no mutant on purpose - they must pass in EVERY
// build, mutated or not.
//===----------------------------------------------------------------------===//

TEST_CASE("crypta: base64 punctuation in a blob survives the wire byte for byte", "[crypta][happy][injection]") {
	// `+`, `/` and `=` look dangerous and are perfectly legal base64. An escaper
	// that touched them, or a validator that refused them, would break every real
	// blob crypta ever issued.
	FakeCryptaServer server;
	server.Start([&](FakeConnection &connection, int) { ServeParsedItems(server, connection); });
	CryptaClient client(server.Path());

	vector<CryptaFileIdentity> identities {SampleIdentity("t/one.parquet"), SampleIdentity("t/two.parquet")};
	vector<string> blobs {"RExL+/9ab+/cdEF==", "RExLZm9vYmFyYmF6cXV4"};

	auto keys = client.UnwrapBatch(identities, blobs);
	REQUIRE(keys.size() == 2);
	REQUIRE(keys[0] == DekForPath("t/one.parquet"));
	REQUIRE(keys[1] == DekForPath("t/two.parquet"));

	std::vector<std::string> items;
	REQUIRE(SplitItems(server.Requests()[0], items));
	REQUIRE(items.size() == 2);
	for (size_t i = 0; i < items.size(); i++) {
		std::string decoded;
		REQUIRE(DecodeJsonStringField(items[i], "wrapped", decoded));
		REQUIRE(decoded == blobs[i]);
		// Byte for byte: no escape was introduced into a value that needed none.
		REQUIRE(items[i].find(blobs[i]) != std::string::npos);
	}
}

TEST_CASE("crypta provider: a legitimate blob with base64 punctuation is served, not refused",
          "[crypta][happy][injection]") {
	// The discriminating half of the alphabet check. A guard that always threw
	// would pass every refusal case above; this is the one it cannot pass.
	FakeCryptaServer server;
	server.Start([&](FakeConnection &connection, int) { ServeParsedItems(server, connection); });
	DuckLakeCryptaProvider provider(server.Path(), "test-lake");

	//
	// The punctuation is the point and it is preserved: '+', '/' and '=' are all
	// in IsBase64's alphabet, so this value must be SERVED. It goes through
	// WrappedBlob for the same reason as the refusal fixtures above, and here the
	// consequence of getting it wrong is worse rather than milder: a literal under
	// the 44-character floor is refused as a plaintext downgrade, so this case -
	// the ONLY one asserting the alphabet check does not OVER-refuse - fails while
	// every refusal case around it still passes. That is the shape that tempts a
	// reader to relax the floor, which would reinstate the ~6e-8 false positive the
	// floor exists to prevent. The fixture is what was wrong, never the guard.
	auto identity = SampleIdentity("t/legit.parquet");
	REQUIRE(provider.UnwrapKey(identity, WrappedBlob("+/9ab+/cdEF==")) == DekForPath("t/legit.parquet"));
	REQUIRE(server.Connections() == 1);
}

TEST_CASE("crypta provider: the plaintext floor is consulted BEFORE the alphabet check",
          "[crypta][refusal][base64][injection]") {
	// THE ORDER, WRITTEN DOWN. It was not, anywhere, and that is what let two
	// separate fixtures lose their evidence silently.
	//
	// `DuckLakeCryptaProvider::UnwrapKey` asks LooksWrapped first and IsBase64
	// second, so a value that fails BOTH is reported as a downgrade attempt and
	// never as a malformed blob. This case pins that, so the next guard added to
	// this path changes a RED test rather than quietly stealing another case's
	// coverage.
	//
	// This is the third time the pattern has bitten on this one decode path:
	// `no_blob_alphabet_check`'s own roster note records the "'|' in a path" case
	// transferring from `cache_key_unprefixed_join` when #24 merged, for exactly
	// this reason; #41 records the flush fixture losing its shape to #25; and this
	// case records the third.
	//
	// It is NOT a complaint about the ordering, which is correct on the merits: a
	// short non-base64 value is refused either way and the envelope holds. What is
	// wrong is only the DIAGNOSIS - a malformed or truncated catalog value is
	// named a downgrade attempt, which sends the reader somewhere else entirely.
	FakeCryptaServer server;
	server.Start([&](FakeConnection &connection, int) { ServeParsedItems(server, connection); });
	DuckLakeCryptaProvider provider(server.Path(), "test-lake");
	auto identity = SampleIdentity("t/short.parquet");

	// Carries the magic, fails the alphabet, and is UNDER the floor - so all three
	// predicates are in play and only the first one answers.
	const std::string under_floor_and_not_base64 = "RExLwith-a-hyphen";
	REQUIRE(under_floor_and_not_base64.size() <= 44);
	auto message = ThrownMessage([&]() { provider.UnwrapKey(identity, under_floor_and_not_base64); });
	REQUIRE_THAT(message, Catch::Contains("carries a plaintext encryption key"));
	REQUIRE_THAT(message, Catch::Contains("t/short.parquet"));

	// The same bytes ABOVE the floor reach the alphabet check instead. One
	// difference - length - and a different guard answers. That pair is the whole
	// assertion; either half alone would be consistent with a single guard.
	REQUIRE_THAT(ThrownMessage([&]() { provider.UnwrapKey(identity, WrappedBlob("with-a-hyphen")); }),
	             Catch::Contains("is not base64"));

	// Neither reached the wire, whichever guard answered.
	std::this_thread::sleep_for(std::chrono::milliseconds(60));
	REQUIRE(server.Connections() == 0);
}

//===----------------------------------------------------------------------===//
// The alphabet itself, at its edges
//
// `no_blob_alphabet_check` deletes the CALL to `IsBase64`, so it proves the
// provider consults it. Nothing proved what it ANSWERS, and the gap was not
// academic: every blob literal in this suite uses `-`, `|`, `"`, `{`, `,` or a
// control character as its out-of-alphabet byte, so the six characters between
// 'Z' (0x5A) and 'a' (0x61) were untested. Widening the range check to
// `u >= 'A' && u <= 'z'` left the ENTIRE suite green, --mutants included, while
// `RExL\` sailed through what the header calls "the validation" - and `\` is
// the second of the exactly two bytes that can break a JSON string, which is
// the whole reason this guard names the alphabet it does.
//
// mutant: widened_base64_alphabet
//===----------------------------------------------------------------------===//

TEST_CASE("crypta: the base64 alphabet is exactly the base64 alphabet, at its edges",
          "[crypta][refusal][base64][injection]") {
	// In: the 64 encoding characters plus the padding byte. Nothing else.
	for (unsigned char c = 'A'; c <= 'Z'; c++) {
		REQUIRE(CryptaClient::IsBase64(std::string(1, static_cast<char>(c))));
	}
	for (unsigned char c = 'a'; c <= 'z'; c++) {
		REQUIRE(CryptaClient::IsBase64(std::string(1, static_cast<char>(c))));
	}
	for (unsigned char c = '0'; c <= '9'; c++) {
		REQUIRE(CryptaClient::IsBase64(std::string(1, static_cast<char>(c))));
	}
	REQUIRE(CryptaClient::IsBase64("+"));
	REQUIRE(CryptaClient::IsBase64("/"));
	REQUIRE(CryptaClient::IsBase64("="));

	// OUT: the boundary bytes a range check gets wrong. The first six are the
	// gap between 'Z' and 'a' - the ones a widened `'A'..'z'` would admit - and
	// the backslash among them is the one that matters most.
	const char *const outside[] = {"@", "[", "\\", "]", "^", "_", "`", "{", "|", "}",
	                               "~", "*", ",", "-", ".", ":", "\"", " "};
	for (auto candidate : outside) {
		REQUIRE_FALSE(CryptaClient::IsBase64(candidate));
	}
	// And the two ends of the byte range, which a signed `char` comparison
	// would get wrong in opposite directions.
	REQUIRE_FALSE(CryptaClient::IsBase64(std::string(1, static_cast<char>(0x7f))));
	REQUIRE_FALSE(CryptaClient::IsBase64(std::string(1, static_cast<char>(0x80))));
	REQUIRE_FALSE(CryptaClient::IsBase64(std::string(1, static_cast<char>(0xff))));
	REQUIRE_FALSE(CryptaClient::IsBase64(std::string(1, '\0')));

	// A real blob passes whole, and one bad byte anywhere spoils it - the guard
	// is over the WHOLE value, not a prefix.
	REQUIRE(CryptaClient::IsBase64("RExLMQAAAAB2YWxpZC1sb29raW5nLWJsb2I="));
	REQUIRE_FALSE(CryptaClient::IsBase64("RExLMQAAAAB2YWxpZC1sb29raW5nLWJsb2I=\\"));
	// Empty is vacuously in the alphabet. `LooksWrapped` is what rejects it, and
	// it runs first - asserted here so nobody "fixes" this into a length rule.
	REQUIRE(CryptaClient::IsBase64(""));
	REQUIRE_FALSE(CryptaClient::LooksWrapped(""));
}

// mutant: no_backslash_escape
TEST_CASE("crypta: a blob ending in a backslash cannot escape its own closing quote",
          "[crypta][refusal][json_escape][injection]") {
	// A roster gap rather than a coverage hole, and the distinction is worth
	// keeping straight. Removing `JsonEscape`'s backslash arm WOULD already have
	// reddened the identity escaping case, which feeds a `\` through
	// `stored_path`. So the suite was not blind here - unlike the `IsBase64`
	// alphabet edges, where widening the range left every case green. What was
	// missing is that no mutant DESCRIBED that change, so the roster never
	// claimed it, and no blob in the suite carried the byte on the path where an
	// attacker actually supplies it. Both are fixed: this case, and a semantic
	// mutant that strips only the backslash arm rather than the whole function.
	FakeCryptaServer server;
	server.Start([&](FakeConnection &connection, int) { ServeParsedItems(server, connection); });
	CryptaClient client(server.Path());

	std::string blob = ENDS_IN_A_BACKSLASH;
	REQUIRE(blob[blob.size() - 1] == '\\');
	ThrownMessage([&]() { client.UnwrapBatch({SampleIdentity("t/victim.parquet")}, {blob}); });

	auto requests = server.Requests();
	REQUIRE(requests.size() == 1);

	// The item still terminates where the client intended, so the frame is one
	// well-formed item rather than a string that swallowed the array's tail.
	std::vector<std::string> items;
	REQUIRE(SplitItems(requests[0], items));
	REQUIRE(items.size() == 1);

	std::string decoded;
	REQUIRE(DecodeJsonStringField(items[0], "wrapped", decoded));
	REQUIRE(decoded == blob);

	// And the escape is present AS an escape - a doubled backslash, not a lone
	// one that would consume the quote after it.
	REQUIRE(CountOccurrences(requests[0], "\\\\") == 1);
}
