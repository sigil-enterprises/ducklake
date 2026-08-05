//===----------------------------------------------------------------------===//
//                         DuckLake (sigil fork)
//
// test/cpp/crypta/crypta_cache_test.cpp
//
// The DEK cache in `DuckLakeCryptaProvider`, and the key-confusion attack its
// (identity, blob) keying exists to stop.
//
// PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.
//
// Before this file, the cache HIT branch had never executed anywhere: every unwrap in
// `scripts/mvp_crypta_proof.sh` is a MISS, so the keying introduced in 7df67912
// - which IS the fix for the key-confusion hole - was carried by nothing.
//
// The load-bearing assertion throughout is the CONNECTION COUNT. A cache hit is
// precisely "the client did not go to crypta again", and a miss is precisely
// "it did". Comparing returned DEKs alone cannot tell the two apart.
//===----------------------------------------------------------------------===//

#include "crypta/ducklake_crypta.hpp"
#include "crypta_test_support.hpp"
#include "duckdb/common/exception.hpp"

#include <chrono>
#include <map>
#include <thread>

using namespace duckdb;               // NOLINT
using namespace ducklake_crypta_test; // NOLINT

namespace {

//! What a request carried, pulled back off the wire.
struct WireIdentity {
	std::string lake_id;
	long long table_id = 0;
	std::string file_kind;
	std::string file_path;
	std::string wrapped;

	bool operator<(const WireIdentity &other) const {
		if (lake_id != other.lake_id) {
			return lake_id < other.lake_id;
		}
		if (table_id != other.table_id) {
			return table_id < other.table_id;
		}
		if (file_kind != other.file_kind) {
			return file_kind < other.file_kind;
		}
		if (file_path != other.file_path) {
			return file_path < other.file_path;
		}
		return wrapped < other.wrapped;
	}
};

bool ParseWireIdentity(const std::string &body, WireIdentity &out) {
	if (!DecodeJsonStringField(body, "catalog_uuid", out.lake_id)) {
		return false;
	}
	if (!DecodeJsonStringField(body, "file_kind", out.file_kind)) {
		return false;
	}
	if (!DecodeJsonStringField(body, "file_path", out.file_path)) {
		return false;
	}
	if (!DecodeJsonStringField(body, "wrapped", out.wrapped)) {
		return false;
	}
	auto key = std::string("\"table_id\":");
	auto at = body.find(key);
	if (at == std::string::npos) {
		return false;
	}
	out.table_id = std::strtoll(body.c_str() + at + key.size(), nullptr, 10);
	return true;
}

WireIdentity ToWire(const CryptaFileIdentity &identity, const std::string &blob) {
	WireIdentity wire;
	wire.lake_id = identity.lake_id;
	wire.table_id = identity.table_id;
	wire.file_kind = identity.is_delete_file ? "delete" : "data";
	wire.file_path = identity.stored_path;
	wire.wrapped = blob;
	return wire;
}

//! A fake crypta that actually enforces the binding: it hands back a DEK only
//! for an (identity, blob) pair it issued, and refuses everything else the way
//! the real service does. Without this, a test could not tell "the cache
//! bypassed the service" from "the service agreed".
class BindingCryptaFake {
public:
	void Issue(const CryptaFileIdentity &identity, const std::string &blob, const std::string &dek) {
		issued[ToWire(identity, blob)] = dek;
	}
	void Serve(FakeConnection &connection) {
		auto body = connection.ReadFrame();
		WireIdentity wire;
		if (!ParseWireIdentity(body, wire)) {
			connection.WriteFrame(ErrorResponse("malformed request"));
			return;
		}
		auto entry = issued.find(wire);
		if (entry == issued.end()) {
			connection.WriteFrame(ErrorResponse("unwrap failed: not valid for this KEK and file identity"));
			return;
		}
		connection.WriteFrame(OkUnwrapResponse({entry->second}));
	}

private:
	std::map<WireIdentity, std::string> issued;
};

//! A 32-byte DEK that is distinct per label, so a confused key is visibly the
//! wrong one rather than accidentally equal.
std::string DekFor(const std::string &label) {
	std::string dek = "dek:" + label;
	dek.resize(32, '.');
	return dek;
}

} // namespace

//===----------------------------------------------------------------------===//
// Construction
//===----------------------------------------------------------------------===//

// mutant: no_lake_id_check
TEST_CASE("crypta provider: an empty lake id is refused", "[crypta][cache][provider]") {
	// Without a lake id every lake on a shared crypta produces interchangeable
	// bindings: a key from lake A would unwrap for the same table and path in
	// lake B.
	FakeCryptaServer server;
	REQUIRE_THAT(ThrownMessage([&]() { DuckLakeCryptaProvider provider(server.Path(), ""); }),
	             Catch::Contains("crypta_lake_id must be set"));
}

TEST_CASE("crypta provider: WrapKeys batches a whole commit into one call", "[crypta][cache][provider]") {
	// The wrap half of the provider, which every other case here reaches only
	// through the SQL fixture. It is a two-line delegation, and the only thing it
	// can get wrong is silently, so the assertions are on the WIRE rather than on
	// the return value: one connection for the whole commit, both identities
	// present, in the order they were given.
	//
	// The count matters beyond tidiness. The design note on this class turns on
	// writes batching per commit while reads do not; a WrapKeys that opened one
	// connection per file would quietly invalidate that, and nothing else in this
	// suite would notice.
	FakeCryptaServer server;
	server.Start([&](FakeConnection &connection, int) {
		server.Record(connection.ReadFrame());
		connection.WriteFrame(OkWrapResponse({"RExLfirst", "RExLsecond"}));
	});
	DuckLakeCryptaProvider provider(server.Path(), "commit-lake");
	vector<CryptaFileIdentity> identities {SampleIdentity("first.parquet"), SampleIdentity("second.parquet")};
	vector<string> deks {DekFor("first"), DekFor("second")};
	auto blobs = provider.WrapKeys(identities, deks);
	REQUIRE(blobs.size() == 2);
	REQUIRE(blobs[0] == "RExLfirst");
	REQUIRE(blobs[1] == "RExLsecond");
	REQUIRE(server.Connections() == 1);
	auto request = server.Requests().at(0);
	REQUIRE_THAT(request, Catch::Contains("first.parquet"));
	REQUIRE_THAT(request, Catch::Contains("second.parquet"));
	REQUIRE(request.find("first.parquet") < request.find("second.parquet"));
}

//===----------------------------------------------------------------------===//
// A plaintext key row on an enveloped lake
//===----------------------------------------------------------------------===//

// mutant: no_plaintext_refusal
TEST_CASE("crypta provider: a plaintext key row is refused, never used", "[crypta][cache][plaintext]") {
	// Invariant 2 in .claude/README.md, and case 8 of the MVP proof. On a lake
	// whose keys are wrapped, a plaintext row is either a pre-envelope leftover or
	// a downgrade attempt. Using it would defeat the envelope entirely.
	FakeCryptaServer server;
	server.Start([](FakeConnection &connection, int) {
		connection.ReadFrame();
		connection.WriteFrame(OkUnwrapResponse({DekFor("should-never-be-asked-for")}));
	});
	DuckLakeCryptaProvider provider(server.Path(), "test-lake");
	auto identity = SampleIdentity("t/plaintext.parquet");

	SECTION("a bare 24-character key, the pre-envelope shape") {
		auto message = ThrownMessage([&]() { provider.UnwrapKey(identity, "AAAAAAAAAAAAAAAAAAAAAA=="); });
		REQUIRE_THAT(message, Catch::Contains("carries a plaintext encryption key"));
		REQUIRE_THAT(message, Catch::Contains("t/plaintext.parquet"));
	}
	SECTION("an empty key column") {
		REQUIRE_THAT(ThrownMessage([&]() { provider.UnwrapKey(identity, ""); }),
		             Catch::Contains("carries a plaintext encryption key"));
	}
	SECTION("a near-miss magic - one byte off the prefix") {
		REQUIRE_THAT(ThrownMessage([&]() { provider.UnwrapKey(identity, "RExKMQAAAAA"); }),
		             Catch::Contains("carries a plaintext encryption key"));
	}

	// THE SECOND CALLER OF THE LENGTH FLOOR.
	//
	// The floor in LooksWrapped was added for the UNCONFIGURED direction, where a
	// plaintext DEK that happens to start with the magic would be refused forever
	// as wrapped. But LooksWrapped serves BOTH directions, so the floor changes
	// this caller's answer too, and a guard shared by two callers needs a case on
	// each rather than one case plus the assumption that the function is the same
	// function.
	//
	// Here the change is an improvement and this pins it: a 44-character value
	// with a correct RExL prefix is a plaintext DEK wearing a blob's hat. Without
	// the floor it reads as wrapped and is sent to crypta - a socket call on a
	// value that is not a blob. With it, the downgrade is refused BEFORE the
	// socket, which the connection assertion below then proves.
	SECTION("a 44-character plaintext DEK that happens to carry the magic") {
		const string dek_wearing_the_magic = "RExL" + string(40, 'A');
		REQUIRE(dek_wearing_the_magic.size() == 44);
		REQUIRE_THAT(ThrownMessage([&]() { provider.UnwrapKey(identity, dek_wearing_the_magic); }),
		             Catch::Contains("carries a plaintext encryption key"));
	}

	// It refused before the socket, so nothing was asked of crypta.
	std::this_thread::sleep_for(std::chrono::milliseconds(60));
	REQUIRE(server.Connections() == 0);
}

//===----------------------------------------------------------------------===//
// The cache HIT path - executed by nothing before this file
//===----------------------------------------------------------------------===//

// mutant: no_cache_lookup
TEST_CASE("crypta provider: a repeated unwrap hits the cache and does not re-ask crypta",
          "[crypta][cache][hit]") {
	auto dek = DekFor("A");
	FakeCryptaServer server;
	server.Start([&](FakeConnection &connection, int) {
		connection.ReadFrame();
		connection.WriteFrame(OkUnwrapResponse({dek}));
	});
	DuckLakeCryptaProvider provider(server.Path(), "test-lake");
	auto identity = SampleIdentity("t/a.parquet");
	const std::string blob = WrappedBlob("QUFBQQ");

	auto first = provider.UnwrapKey(identity, blob);
	REQUIRE(first == dek);
	REQUIRE(server.Connections() == 1);

	for (int i = 0; i < 5; i++) {
		REQUIRE(provider.UnwrapKey(identity, blob) == dek);
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(60));
	REQUIRE(server.Connections() == 1);
}

// mutant: no_cache_clear
TEST_CASE("crypta provider: the cache is cleared wholesale when the cap is hit", "[crypta][cache][clear][slow]") {
	// MAX_CACHED_KEYS is a private constant; 4096 is its value in
	// src/include/crypta/ducklake_crypta.hpp. If that number changes this test
	// must change with it - which is the point, because the clear is the only
	// eviction there is.
	const int MAX_CACHED_KEYS = 4096;

	FakeCryptaServer server;
	server.Start([&](FakeConnection &connection, int index) {
		connection.ReadFrame();
		connection.WriteFrame(OkUnwrapResponse({DekFor(std::to_string(index))}));
	});
	DuckLakeCryptaProvider provider(server.Path(), "test-lake");

	auto identity_for = [](int i) { return SampleIdentity("t/file_" + std::to_string(i) + ".parquet"); };
	auto blob_for = [](int i) { return WrappedBlob(std::to_string(i)); };

	// Fill the cache exactly to the cap. No clear has happened yet.
	for (int i = 0; i < MAX_CACHED_KEYS; i++) {
		provider.UnwrapKey(identity_for(i), blob_for(i));
	}
	REQUIRE(server.Connections() == MAX_CACHED_KEYS);

	// The first entry is still there.
	provider.UnwrapKey(identity_for(0), blob_for(0));
	REQUIRE(server.Connections() == MAX_CACHED_KEYS);

	// One more distinct key: a miss, a service call, and THEN the cap check fires
	// and drops everything before inserting it.
	provider.UnwrapKey(identity_for(MAX_CACHED_KEYS), blob_for(MAX_CACHED_KEYS));
	REQUIRE(server.Connections() == MAX_CACHED_KEYS + 1);

	// The survivor of the clear is the entry inserted after it - and only that
	// one. Asserting both halves is what distinguishes "cleared" from "never
	// cached anything".
	provider.UnwrapKey(identity_for(MAX_CACHED_KEYS), blob_for(MAX_CACHED_KEYS));
	REQUIRE(server.Connections() == MAX_CACHED_KEYS + 1);

	provider.UnwrapKey(identity_for(0), blob_for(0));
	REQUIRE(server.Connections() == MAX_CACHED_KEYS + 2);
	REQUIRE(server.HandlerError().empty());
}

//===----------------------------------------------------------------------===//
// Key confusion - what the (identity, blob) keying exists to prevent
//===----------------------------------------------------------------------===//

// mutant: cache_key_blob_only
TEST_CASE("crypta provider: two identities sharing one blob do not collide in the cache",
          "[crypta][cache][key_confusion]") {
	// This is the hole 7df67912 closed, stated as a test for the first time.
	//
	// Keying the cache on the blob alone: read file A, caching blob -> DEK-A; then
	// paste blob A onto file B's row; the next read of B hits the cache and gets
	// DEK-A back WITHOUT crypta ever seeing the mismatched identity.
	const std::string shared_blob = WrappedBlob("c2hhcmVk");

	auto dek_a = DekFor("A");
	auto dek_b = DekFor("B");
	FakeCryptaServer server;
	server.Start([&](FakeConnection &connection, int index) {
		connection.ReadFrame();
		connection.WriteFrame(OkUnwrapResponse({index == 0 ? dek_a : dek_b}));
	});
	DuckLakeCryptaProvider provider(server.Path(), "test-lake");

	auto identity_a = SampleIdentity("t/a.parquet");
	auto identity_b = SampleIdentity("t/b.parquet");

	REQUIRE(provider.UnwrapKey(identity_a, shared_blob) == dek_a);
	REQUIRE(server.Connections() == 1);

	// The second identity MUST be a miss even though the blob is byte-identical.
	REQUIRE(provider.UnwrapKey(identity_b, shared_blob) == dek_b);
	REQUIRE(server.Connections() == 2);
	REQUIRE(dek_a != dek_b);
}

// mutant: cache_key_blob_only
TEST_CASE("crypta provider: every component of the identity is part of the cache key",
          "[crypta][cache][key_confusion]") {
	const std::string shared_blob = WrappedBlob("c2hhcmVk");
	auto base = SampleIdentity("t/a.parquet");

	struct Variation {
		const char *what;
		CryptaFileIdentity identity;
	};
	std::vector<Variation> variations;
	{
		auto v = base;
		v.lake_id = "a-different-lake";
		variations.push_back({"lake_id", v});
	}
	{
		auto v = base;
		v.table_id = base.table_id + 1;
		variations.push_back({"table_id", v});
	}
	{
		auto v = base;
		// Invariant 3: a delete file's key row must not be interchangeable with a
		// data file's.
		v.is_delete_file = !base.is_delete_file;
		variations.push_back({"file_kind", v});
	}
	{
		auto v = base;
		v.stored_path = "t/somewhere_else.parquet";
		variations.push_back({"stored_path", v});
	}

	for (auto &variation : variations) {
		DYNAMIC_SECTION("changing " << variation.what << " is a cache miss") {
			auto dek_base = DekFor("base");
			auto dek_variant = DekFor(variation.what);
			FakeCryptaServer server;
			server.Start([&](FakeConnection &connection, int index) {
				connection.ReadFrame();
				connection.WriteFrame(OkUnwrapResponse({index == 0 ? dek_base : dek_variant}));
			});
			DuckLakeCryptaProvider provider(server.Path(), "test-lake");

			REQUIRE(provider.UnwrapKey(base, shared_blob) == dek_base);
			REQUIRE(server.Connections() == 1);
			REQUIRE(provider.UnwrapKey(variation.identity, shared_blob) == dek_variant);
			REQUIRE(server.Connections() == 2);
		}
	}
}

// mutant: cache_key_identity_only
//
// NOT cache_key_blob_only, and the distinction is the whole point of naming a
// mutant per case: reducing the key to the blob alone leaves two DIFFERENT blobs
// with two different keys, so that mutant cannot redden this case. It needs the
// mirror mutant, which drops the blob half.
TEST_CASE("crypta provider: one identity with two blobs does not collide in the cache",
          "[crypta][cache][key_confusion]") {
	auto dek_one = DekFor("one");
	auto dek_two = DekFor("two");
	FakeCryptaServer server;
	server.Start([&](FakeConnection &connection, int index) {
		connection.ReadFrame();
		connection.WriteFrame(OkUnwrapResponse({index == 0 ? dek_one : dek_two}));
	});
	DuckLakeCryptaProvider provider(server.Path(), "test-lake");
	auto identity = SampleIdentity("t/a.parquet");

	REQUIRE(provider.UnwrapKey(identity, WrappedBlob("b25l")) == dek_one);
	REQUIRE(server.Connections() == 1);
	REQUIRE(provider.UnwrapKey(identity, WrappedBlob("dHdv")) == dek_two);
	REQUIRE(server.Connections() == 2);
}

// mutant: cache_key_blob_only
TEST_CASE("crypta provider: a substituted key row is refused by crypta, not served from the cache",
          "[crypta][cache][key_confusion]") {
	// The end-to-end form, against a fake that enforces the binding the way the
	// real service does. This is MVP proof case 7 - key rows swapped between two
	// files - but reached through the cache, which the proof never exercises
	// because every unwrap there is a first unwrap.
	BindingCryptaFake crypta;
	auto identity_a = SampleIdentity("t/a.parquet");
	auto identity_b = SampleIdentity("t/b.parquet");
	const std::string blob_a = WrappedBlob("YQ");
	const std::string blob_b = WrappedBlob("Yg");
	auto dek_a = DekFor("A");
	auto dek_b = DekFor("B");
	crypta.Issue(identity_a, blob_a, dek_a);
	crypta.Issue(identity_b, blob_b, dek_b);

	FakeCryptaServer server;
	server.Start([&](FakeConnection &connection, int) { crypta.Serve(connection); });
	DuckLakeCryptaProvider provider(server.Path(), "test-lake");

	// Read A normally. Its DEK is now in the cache under A's blob.
	REQUIRE(provider.UnwrapKey(identity_a, blob_a) == dek_a);
	REQUIRE(server.Connections() == 1);

	// Now paste A's blob onto B's row. It must go to crypta - and crypta must
	// refuse, because the identity does not match what the blob is bound to.
	REQUIRE_THAT(ThrownMessage([&]() { provider.UnwrapKey(identity_b, blob_a); }),
	             Catch::Contains("not valid for this KEK and file identity"));
	REQUIRE(server.Connections() == 2);

	// And B's own row still reads correctly afterwards.
	REQUIRE(provider.UnwrapKey(identity_b, blob_b) == dek_b);
	REQUIRE(server.Connections() == 3);
}

//===----------------------------------------------------------------------===//
// Key confusion through the COMPOSITION of the cache key
//
// The two cases below assert one property: the composition that builds the cache
// key is INJECTIVE, so no two distinct (identity, blob) pairs can ever land on
// the same entry.
//
// These two carry no `// mutant:` marker while every other case in this file does,
// and that is correct AT THIS COMMIT rather than an omission: the guard they assert
// does not exist in `src/` yet, so they are RED against the tree as it stands and
// no mutant is needed to prove they CAN fail. The roster entry lands with the fix.
//
// Each case pins the property from BOTH sides. A case made only of refusals
// cannot detect OVER-refusal: a key so broken that it NEVER matches satisfies
// every collision assertion here, while silently sending every unwrap to crypta -
// a performance and availability regression. Measured, not supposed: built against
// `no_cache_lookup` (the cache lookup forced to `false`), both cases below PASSED.
// So after the substituted row is refused, each re-reads the LEGITIMATE row and
// requires it to still be served from cache, with the connection count NOT moving.
//
// The honest limit of that, stated because it is easy to overclaim: this adds no
// DETECTION to the suite as a whole. Any build failing these added assertions also
// fails the `[crypta][cache][hit]` case above, which is why they redden under
// `no_cache_lookup` and under no other mutant. What they buy is per-case
// SELF-CONTAINMENT, and that is worth having here for a concrete reason rather
// than a hypothetical one: `run_crypta_tests.sh` drives mutants BY TEST-NAME SPEC,
// so it really does run these two cases in isolation from the hit case - and in
// isolation, without the second half, they would prove strictly less than they
// appear to.
//
// The cost is that `no_cache_lookup` now reddens three cases where one would
// suffice to pin its guard, so the roster is a little less precise about which
// case pins what. That trade is deliberate.
//===----------------------------------------------------------------------===//

TEST_CASE("crypta provider: a '|' in a path cannot be re-read as the cache-key separator",
          "[crypta][cache][key_confusion]") {
	// The cache key is built by joining the identity fields and the blob with a
	// literal '|':
	//
	//   Format("%s|%lld|%s|%s", lake_id, table_id, kind, stored_path) + "|" + blob
	//
	// Nothing escapes a field and nothing prefixes it with its length, so that
	// join is NOT injective: two DIFFERENT (identity, blob) pairs produce the
	// SAME key whenever a '|' inside a field can be re-read as the separator.
	// Below, A's path ends with "|RExLZZZZ" and B's blob is exactly
	// "RExLZZZZ|" followed by A's blob; both therefore join to
	//
	//   test-lake|7|data|t/p|RExLZZZZ|<blob_a>
	//
	// blob_a is built with WrappedBlob so it clears LooksWrapped's length floor
	// - a 44-character-or-shorter fixture would be refused as plaintext before
	// the cache is reached, and this case would silently stop testing the
	// collision. blob_b inherits the "RExL" prefix and is longer still.
	//
	// A is read first, so on a key like that B is handed A's cached DEK and
	// crypta never sees the mismatched identity - the exact bypass the
	// (identity, blob) keying exists to prevent, reintroduced through the
	// separator.
	//
	// What MUST happen, and what this case asserts: B is a different file
	// carrying a blob crypta never issued for it, so B MISSES the cache, reaches
	// crypta, and is REFUSED. The CONNECTION COUNT is the real assertion. A
	// refusal on its own proves nothing about the cache - only the count
	// distinguishes "the cache was bypassed" from "the service was asked and
	// disagreed", so the throw and `Connections() == 2` are one assertion in two
	// halves.
	//
	// The threat model is the one the envelope already assumes: an attacker with
	// catalog write access, who controls both a file's path and another file's
	// encryption_key column, and who cannot mint a valid blob. That write access IS
	// the route - the attacker sets `path` and a wrapped `encryption_key` on the
	// same `ducklake_data_file` row, which is full control of `stored_path`.
	//
	// Three routes that sound plausible and are NOT it, recorded so nobody re-cites
	// them: a table name (`CanGeneratePathFromName` substitutes the table UUID for
	// any name outside alphanumerics/`_`/`-`, so such a name reaches the path
	// nowhere); `ducklake_add_data_files` (it does store a path verbatim, but the
	// row carries no encryption key and a keyless row throws in `ReadDataFile`
	// before an identity is built, so it never reaches the cache); and a hive
	// partition value (`HivePartitioning::Escape` is `StringUtil::URLEncode`).
	//
	// Asserted as a refusal rather than carried under Catch's [!shouldfail]:
	// under that tag ANY failure reads as "the defect is still there", and a
	// server that never started would look identical to a real collision.
	BindingCryptaFake crypta;

	auto identity_a = SampleIdentity("t/p|RExLZZZZ");
	const std::string blob_a = WrappedBlob("AAAA");
	auto identity_b = SampleIdentity("t/p");
	const std::string blob_b = "RExLZZZZ|" + blob_a;

	auto dek_a = DekFor("A");
	crypta.Issue(identity_a, blob_a, dek_a);
	// B's pair is deliberately NOT issued: crypta must refuse it - which it can
	// only do if it is asked at all.

	FakeCryptaServer server;
	server.Start([&](FakeConnection &connection, int) { crypta.Serve(connection); });
	DuckLakeCryptaProvider provider(server.Path(), "test-lake");

	REQUIRE(provider.UnwrapKey(identity_a, blob_a) == dek_a);
	REQUIRE(server.Connections() == 1);

	REQUIRE_THAT(ThrownMessage([&]() { provider.UnwrapKey(identity_b, blob_b); }),
	             Catch::Contains("not valid for this KEK and file identity"));
	REQUIRE(server.Connections() == 2);

	// The other half: A's own row must STILL be served from cache. A key that
	// never matches would satisfy everything above while quietly turning every
	// unwrap into a round trip, so the count staying at 2 is what says the fix
	// separated these two entries rather than simply breaking the cache.
	REQUIRE(provider.UnwrapKey(identity_a, blob_a) == dek_a);
	REQUIRE(server.Connections() == 2);
}

TEST_CASE("crypta provider: a '|' in an identity field cannot shift a cache-key boundary",
          "[crypta][cache][key_confusion]") {
	// The same non-injective join, with the ambiguity moved OFF the path/blob
	// boundary so the property is not pinned to one hand-built string. Both
	// pairs here carry the SAME blob; everything that differs lives inside the
	// identity, and the shifted separator runs across the lake_id, table_id,
	// file_kind and stored_path boundaries at once:
	//
	//   C: lake_id "test-lake",            table_id 7, data,   path "t/p|9|delete|q.parquet"
	//      -> test-lake|7|data|t/p|9|delete|q.parquet|RExLc2hhcmVk
	//   D: lake_id "test-lake|7|data|t/p", table_id 9, delete, path "q.parquet"
	//      -> test-lake|7|data|t/p|9|delete|q.parquet|RExLc2hhcmVk
	//
	// Byte-identical, while C and D disagree on the lake, the table, the file
	// kind AND the path - so one collision confuses every component of the
	// binding at once, and no amount of escaping applied to `stored_path` alone
	// would close it. The fix has to make the COMPOSITION unambiguous.
	//
	// Reachability here is weaker than in the case above and is not claimed to
	// be equal: `lake_id` is operator-configured, not attacker-supplied. This
	// case asserts the composition property the fix must have, not a second live
	// route into it.
	//
	// The CONNECTION COUNT is again the real assertion: D must be a miss that
	// reaches crypta, so the refusal counts only with the count at 2.
	BindingCryptaFake crypta;

	const std::string shared_blob = "RExLc2hhcmVk";
	auto identity_c = SampleIdentity("t/p|9|delete|q.parquet");
	auto identity_d = SampleIdentity("q.parquet");
	identity_d.lake_id = "test-lake|7|data|t/p";
	identity_d.table_id = 9;
	identity_d.is_delete_file = true;

	auto dek_c = DekFor("C");
	crypta.Issue(identity_c, shared_blob, dek_c);
	// D's pair is deliberately NOT issued.

	FakeCryptaServer server;
	server.Start([&](FakeConnection &connection, int) { crypta.Serve(connection); });
	DuckLakeCryptaProvider provider(server.Path(), "test-lake");

	REQUIRE(provider.UnwrapKey(identity_c, shared_blob) == dek_c);
	REQUIRE(server.Connections() == 1);

	REQUIRE_THAT(ThrownMessage([&]() { provider.UnwrapKey(identity_d, shared_blob); }),
	             Catch::Contains("not valid for this KEK and file identity"));
	REQUIRE(server.Connections() == 2);

	// And C's own row is still a HIT, for the same reason as in the case above:
	// this must fail if the fix made the key un-matchable rather than injective.
	REQUIRE(provider.UnwrapKey(identity_c, shared_blob) == dek_c);
	REQUIRE(server.Connections() == 2);
}
