//===----------------------------------------------------------------------===//
//                         DuckLake (sigil fork)
//
// test/cpp/crypta/crypta_identity_echo_test.cpp
//
// The reply is bound to the request by the IDENTITY crypta echoes, never by
// array position. Issue #31.
//
// PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.
//
// WHAT WAS WRONG, precisely. crypta's reply item is an identity BESIDE the
// value - `PlainKey { identity, dek }` on the unwrap side, `WrappedKeyEntry
// { identity, wrapped }` on the wrap side (sigil-enterprises/crypta,
// src/server.rs). The client read the value and threw the identity away, then
// zipped the values onto its file list by index. The only thing standing between
// that and key confusion was the reply-COUNT check from #24 - and a count is a
// LENGTH check standing in for a BINDING check. It catches a reply with the
// wrong NUMBER of items; a REORDER has exactly the right number, so it catches
// nothing at all. The consequence is a file handed another file's DEK: a
// wrong-key defect, not the refused-batch availability defect #28 measured.
//
// A PRECONDITION, not a follow-up. Neither fake in this tree echoed an identity
// before this change - `fake_crypta_server.cpp`'s `OkUnwrapResponse` emitted
// `{"dek":"..."}` and `test/sql/crypta/fake_crypta.py` appended `{"dek": dek}`.
// A guard that verifies an echoed identity, tested against a fake that never
// echoes one, passes without ever running the guard. So both fakes were
// corrected first, and every case here is written against a fake that echoes.
//
// WHAT MAKES THE BINDING REAL, and why half of this file is about structure
// rather than about identities. A binding is worth exactly what the reader's
// item boundaries are worth. A flat scan for `"dek":"` and an item-wise walk
// disagree about which text is a value, and every disagreement is a way around
// the check: a `"dek":"..."` nested INSIDE the echoed identity object, a second
// `dek` member beside the first, an item carrying none so the next one's value
// slides into its place. The identity and the value it binds are therefore read
// out of the SAME structurally-delimited item, by the same walk - and the cases
// below drive each of those shapes rather than asserting that they are handled.
//
// AND THE CONTROL AGAINST OVER-REFUSAL. The comparison is on the four DECODED
// VALUES, never on bytes: crypta re-serialises the identity it parsed, the SQL
// fixture's python stand-in sorts its members, and a reply is free to spell a
// backspace `\b` where this client writes the numeric escape. A byte comparison
// would refuse a healthy service for spelling the same identity differently -
// over-refusal on the read path, which locks an operator out of their own lake.
// The last case here is the one that must pass in EVERY build, mutated or not.
//===----------------------------------------------------------------------===//

#include "crypta_test_support.hpp"
#include "duckdb/common/exception.hpp"

#include <string>
#include <vector>

using namespace duckdb;               // NOLINT
using namespace ducklake_crypta_test; // NOLINT

namespace {

//! Two files in one batch, which is the smallest batch a reorder is visible in.
vector<CryptaFileIdentity> TwoFiles() {
	vector<CryptaFileIdentity> identities {SampleIdentity("t/alpha.parquet"), SampleIdentity("t/beta.parquet")};
	return identities;
}

vector<string> TwoBlobs() {
	vector<string> blobs {WrappedBlob("alpha"), WrappedBlob("beta")};
	return blobs;
}

//! A reply built from identity slices the caller chose, rather than from the
//! ones the request carried. Everything in this file is one of these.
void ServeReply(FakeCryptaServer &server, const std::string &reply) {
	server.Start([&server, reply](FakeConnection &connection, int) {
		server.Record(connection.ReadFrame());
		connection.WriteFrame(reply);
	});
}

} // namespace

//===----------------------------------------------------------------------===//
// The defect itself
//===----------------------------------------------------------------------===//

// mutant: no_identity_echo_check
TEST_CASE("crypta: a reordered unwrap reply is refused, not zipped onto the caller's file list",
          "[crypta][refusal][identity_echo]") {
	// THE CASE THE COUNT CHECK CANNOT CATCH. Two items requested, two items
	// answered - the count is exactly right and stays right - and the two items
	// have swapped places. Positionally zipped, `t/alpha.parquet` is handed the
	// DEK crypta minted for `t/beta.parquet`.
	auto identities = TwoFiles();
	auto dek_alpha = SampleDek('a');
	auto dek_beta = SampleDek('b');

	// Reversed: item 0 carries beta's identity and beta's key, item 1 alpha's.
	std::vector<std::string> echoes {IdentityEcho(identities[1]), IdentityEcho(identities[0])};
	FakeCryptaServer server;
	ServeReply(server, UnwrapResponse(echoes, {dek_beta, dek_alpha}));
	CryptaClient client(server.Path());

	vector<string> keys;
	auto message = ThrownMessage([&]() { keys = client.UnwrapBatch(identities, TwoBlobs()); });

	// The pre-fix consequence, spelled out so the RED PRINTS it rather than
	// merely failing: the call succeeded and `keys[0]` was beta's DEK.
	INFO("returned " << keys.size() << " key(s); first = " << (keys.empty() ? std::string("<none>") : keys[0])
	                 << " | alpha's own DEK = " << dek_alpha << " | beta's = " << dek_beta);
	REQUIRE(keys.empty());
	REQUIRE_THAT(message, Catch::Contains("a different file's identity"));
	// The refusal names BOTH files: which one was asked for and which one came
	// back. An operator staring at a refused scan cannot act on "identity
	// mismatch".
	REQUIRE_THAT(message, Catch::Contains("t/alpha.parquet"));
	REQUIRE_THAT(message, Catch::Contains("t/beta.parquet"));
	// And it says what the rule IS, because the next reader of this message is
	// deciding whether the reply or the request is at fault.
	REQUIRE_THAT(message, Catch::Contains("never by position"));
}

// mutant: no_identity_echo_check
TEST_CASE("crypta: a reordered wrap reply is refused", "[crypta][refusal][identity_echo]") {
	// The wrap half, and it is not symmetry for its own sake. A mis-zipped wrap
	// has a slower fuse than a mis-zipped unwrap: file A's row stores the blob
	// crypta minted for B, the commit succeeds, and the lake reads fine until the
	// day A is read and its key will not unwrap - at which point the DEK that
	// would decrypt A is nowhere. The two paths therefore share ONE reader rather
	// than each carrying a copy of this decision (#26/#51: a decision copied to
	// two sites is how one site grows a guard and the other does not).
	auto identities = TwoFiles();
	std::vector<std::string> echoes {IdentityEcho(identities[1]), IdentityEcho(identities[0])};
	FakeCryptaServer server;
	ServeReply(server, WrapResponse(echoes, {WrappedBlob("forBeta"), WrappedBlob("forAlpha")}));
	CryptaClient client(server.Path());

	vector<string> deks {SampleDek('a'), SampleDek('b')};
	vector<string> wrapped;
	auto message = ThrownMessage([&]() { wrapped = client.WrapBatch(identities, deks); });
	INFO("returned " << wrapped.size() << " blob(s)");
	REQUIRE(wrapped.empty());
	REQUIRE_THAT(message, Catch::Contains("a different file's identity"));
}

// mutants: no_identity_echo_check, identity_echo_path_only
TEST_CASE("crypta: a reply item echoing a different identity is refused, one field at a time",
          "[crypta][refusal][identity_echo]") {
	// ALL FOUR FIELDS, each driven on its own.
	//
	// A comparison narrowed to the path is the shortcut this would plausibly
	// regress into - the path is the field that looks like the file - and it
	// would leave three real substitutions unbound. Two lakes can hold a table 1
	// with a file at the same relative path (`catalog_uuid` is what separates
	// them), the table id is half of what the key is bound to, and a delete
	// file's key row and a data file's are explicitly NOT interchangeable. So
	// each field gets its own section, and the roster carries a mutant that
	// narrows the comparison to the path alone.
	auto identity = SampleIdentity("t/victim.parquet");
	vector<CryptaFileIdentity> identities {identity};
	vector<string> blobs {WrappedBlob("victim")};

	auto substituted = identity;
	SECTION("a different lake") {
		substituted.lake_id = "some-other-lake";
	}
	SECTION("a different table") {
		substituted.table_id = identity.table_id + 1;
	}
	SECTION("a delete file where a data file was asked for") {
		substituted.is_delete_file = !identity.is_delete_file;
	}
	SECTION("a different path") {
		substituted.stored_path = "t/somebody-elses.parquet";
	}

	FakeCryptaServer server;
	ServeReply(server, UnwrapResponse({IdentityEcho(substituted)}, {SampleDek('x')}));
	CryptaClient client(server.Path());

	vector<string> keys;
	auto message = ThrownMessage([&]() { keys = client.UnwrapBatch(identities, blobs); });
	INFO("returned " << keys.size() << " key(s)");
	REQUIRE(keys.empty());
	REQUIRE_THAT(message, Catch::Contains("a different file's identity"));
}

// mutant: no_identity_echo_check
TEST_CASE("crypta: a reply item with no echoed identity is refused", "[crypta][refusal][identity_echo]") {
	// The shape BOTH fakes in this tree emitted until #31, and the reason
	// correcting them had to come first: a reply with no identity in it is a reply
	// nothing can be bound to, so a guard tested against it is a guard that never
	// ran. Required rather than optional - an absent echo is refused, not waved
	// through - because "verify it when it is there" is a check an attacker turns
	// off by omission.
	FakeCryptaServer server;
	// No identities handed to the builder, so it emits the pre-#31 shape verbatim.
	ServeReply(server, UnwrapResponse({}, {SampleDek()}));
	CryptaClient client(server.Path());

	vector<string> keys;
	auto message = ThrownMessage([&]() { keys = client.UnwrapBatch({SampleIdentity()}, {WrappedBlob("only")}); });
	INFO("returned " << keys.size() << " key(s)");
	REQUIRE(keys.empty());
	REQUIRE_THAT(message, Catch::Contains("no identity member"));
}

//===----------------------------------------------------------------------===//
// The structure the binding rests on
//
// A binding is worth what the reader's item boundaries are worth. These two are
// the shapes where a flat scan and a structural walk disagree, and where the
// attacker - not the reader - would otherwise pick which value wins.
//===----------------------------------------------------------------------===//

// mutant: item_field_by_flat_scan
TEST_CASE("crypta: a dek buried inside the echoed identity is not read as the item's own",
          "[crypta][refusal][identity_echo]") {
	// The bypass a flat scan hands over for free. The item's identity echoes the
	// caller's file EXACTLY, so an identity check alone is satisfied; a second
	// `dek` sits INSIDE that identity object, before the item's real one. A reader
	// that searched the item's text would find the buried value first and return
	// it - a wrong key, past a binding that passed.
	//
	// It is refused by nothing here: the item's members are read at its TOP LEVEL
	// only, so the buried value is not a candidate at all. The assertion is
	// therefore that the RIGHT key comes back, which is the only form this
	// property can be observed in.
	auto identity = SampleIdentity("t/victim.parquet");
	auto real_dek = SampleDek('r');
	auto buried_dek = SampleDek('z');

	auto echo = IdentityEcho(identity);
	// Splice the buried member in just before the identity object's closing brace.
	auto buried = echo.substr(0, echo.size() - 1) + ",\"dek\":\"" + Base64Encode(buried_dek) + "\"}";

	FakeCryptaServer server;
	ServeReply(server, UnwrapResponse({buried}, {real_dek}));
	CryptaClient client(server.Path());

	auto keys = client.UnwrapBatch({identity}, {WrappedBlob("victim")});
	REQUIRE(keys.size() == 1);
	REQUIRE(keys[0] == real_dek);
	REQUIRE(keys[0] != buried_dek);
}

// mutant: item_field_by_flat_scan
TEST_CASE("crypta: a reply item carrying two dek members is refused", "[crypta][refusal][identity_echo]") {
	// The other half of the same hazard, and the one where there is no right
	// answer to pick. With two `dek` members on one item, a first-wins reader and
	// a last-wins reader return different keys and BOTH look correct from the
	// outside - so which one the client happens to implement becomes the
	// attacker's choice. Refuse instead. crypta's own parser refuses a duplicate
	// member on the way in (`duplicate field 'identity'`, measured in #28), so
	// refusing one on the way back costs a healthy service nothing.
	auto identity = SampleIdentity("t/victim.parquet");
	auto reply = UnwrapResponse({IdentityEcho(identity)}, {SampleDek('r')});
	// Add a second `dek` member to the one item.
	auto at = reply.rfind("}]}");
	REQUIRE(at != std::string::npos);
	reply = reply.substr(0, at) + ",\"dek\":\"" + Base64Encode(SampleDek('z')) + "\"}]}";

	FakeCryptaServer server;
	ServeReply(server, reply);
	CryptaClient client(server.Path());

	vector<string> keys;
	auto message = ThrownMessage([&]() { keys = client.UnwrapBatch({identity}, {WrappedBlob("victim")}); });
	INFO("returned " << keys.size() << " key(s)");
	REQUIRE(keys.empty());
	REQUIRE_THAT(message, Catch::Contains("more than one dek member"));
}

//===----------------------------------------------------------------------===//
// The control against over-refusal
//
// Every case above asserts that something is refused. A suite made only of those
// cannot tell a binding from a reader that refuses every reply it is given. This
// one names no mutant on purpose: it must pass in EVERY build, mutated or not.
//===----------------------------------------------------------------------===//

TEST_CASE("crypta: an identity echoed in a different member order and escaping is accepted",
          "[crypta][happy][identity_echo]") {
	// THE SHAPE A REAL SERVICE SENDS. crypta does not echo the bytes it was given
	// - it parses the identity into a `FileIdentity` and re-serialises it, and the
	// SQL fixture's python stand-in re-serialises with `sort_keys=True`. So member
	// order is not a property of the wire, and neither is the spelling of an
	// escape: serde writes a backspace `\b` and a form feed `\f` where this
	// client's own encoder writes the six-character numeric forms.
	//
	// `IdentityEcho` deliberately emits the four members in the REVERSE order and
	// uses the short escapes, so a reader that compared the reply's BYTES to the
	// bytes it sent would fail here while passing every refusal case above. That
	// is the failure this case exists to catch, and it is the expensive kind:
	// over-refusal on the read path locks an operator out of their own lake.
	CryptaFileIdentity identity;
	identity.lake_id = "lake-with-a-\"quote\"";
	identity.table_id = 42;
	identity.is_delete_file = true;
	identity.stored_path = std::string("t/odd\bback\fform\nnew\ttab\x01soh.parquet");

	auto dek = SampleDek('o');
	auto echo = IdentityEcho(identity);
	// The fixture is only evidence if it really differs from what the client
	// wrote, so assert the two properties rather than trusting the helper.
	REQUIRE(echo.find("\"file_path\"") < echo.find("\"catalog_uuid\""));
	REQUIRE(echo.find("\\b") != std::string::npos);
	REQUIRE(echo.find("\\f") != std::string::npos);

	FakeCryptaServer server;
	ServeReply(server, UnwrapResponse({echo}, {dek}));
	CryptaClient client(server.Path());

	auto keys = client.UnwrapBatch({identity}, {WrappedBlob("odd")});
	REQUIRE(keys.size() == 1);
	REQUIRE(keys[0] == dek);

	// And the request really did carry the client's OWN spelling, so the two
	// sides genuinely disagreed about bytes and agreed about values.
	REQUIRE(server.Requests().size() == 1);
	REQUIRE(server.Requests()[0].find("\\u0008") != std::string::npos);
	REQUIRE(server.Requests()[0].find("\\b") == std::string::npos);
}
