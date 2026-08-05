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
	const std::string blob = "RExLQUFBQQ";

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
	auto blob_for = [](int i) { return "RExL" + std::to_string(i); };

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
	const std::string shared_blob = "RExLc2hhcmVk";

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
	const std::string shared_blob = "RExLc2hhcmVk";
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

	REQUIRE(provider.UnwrapKey(identity, "RExLb25l") == dek_one);
	REQUIRE(server.Connections() == 1);
	REQUIRE(provider.UnwrapKey(identity, "RExLdHdv") == dek_two);
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
	const std::string blob_a = "RExLYQ";
	const std::string blob_b = "RExLYg";
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
// A defect this suite found. Reported, not fixed here.
//===----------------------------------------------------------------------===//

TEST_CASE("crypta provider: the cache key delimiter is ambiguous - REPORTED, not fixed",
          "[crypta][cache][key_confusion][known_defect]") {
	// The cache key is built by joining the identity fields and the blob with a
	// literal '|':
	//
	//   Format("%s|%lld|%s|%s", lake_id, table_id, kind, stored_path) + "|" + blob
	//
	// Nothing escapes the fields, so two DIFFERENT (identity, blob) pairs can
	// produce the SAME key whenever a '|' inside a path can be re-read as the
	// delimiter. Below, A's path ends with "|RExLZZZZ" and B's blob begins with
	// "RExLZZZZ|"; both join to
	//
	//   test-lake|7|data|t/p|RExLZZZZ|RExLAAAA
	//
	// so B's read hits A's cache entry and gets DEK-A back without crypta ever
	// seeing the mismatched identity. That is the exact bypass the (identity,
	// blob) keying exists to prevent, reintroduced through the delimiter.
	//
	// The threat model is unchanged from the one the envelope already assumes:
	// an attacker with catalog write access, who controls both a file's path and
	// another file's encryption_key column, and who cannot mint a valid blob.
	//
	// On reachability, precisely: a path with a '|' in it does NOT come from a
	// table name. `stored_path` is the generated basename, and the table name
	// lands in the directory part, which is stripped - so the comment in
	// crypta_client.cpp:48 that justifies escaping by "a table name reaches them"
	// is wrong about this field. The route that does reach it is
	// `ducklake_add_data_files`, which stores an operator-supplied path verbatim.
	// Both are reported on the issue.
	//
	// This asserts the collision POSITIVELY rather than carrying the refusal
	// under Catch's [!shouldfail]. Under that tag ANY failure reads as "the
	// defect is still there" - a server that never started would look identical.
	// Asserted this way the case says exactly what happens, and it still turns
	// RED the day the key is made unambiguous, which is the signal wanted.
	BindingCryptaFake crypta;

	auto identity_a = SampleIdentity("t/p|RExLZZZZ");
	const std::string blob_a = "RExLAAAA";
	auto identity_b = SampleIdentity("t/p");
	const std::string blob_b = "RExLZZZZ|RExLAAAA";

	auto dek_a = DekFor("A");
	crypta.Issue(identity_a, blob_a, dek_a);
	// Deliberately NOT issued for B: crypta would refuse this pair.

	FakeCryptaServer server;
	server.Start([&](FakeConnection &connection, int) { crypta.Serve(connection); });
	DuckLakeCryptaProvider provider(server.Path(), "test-lake");

	REQUIRE(provider.UnwrapKey(identity_a, blob_a) == dek_a);
	REQUIRE(server.Connections() == 1);

	// What SHOULD happen: B is a different file carrying a blob crypta never
	// issued for it, so the read reaches crypta and is refused.
	// What DOES happen, asserted here: B's key collides with A's cache entry, B
	// is handed DEK-A, and crypta is never consulted - the connection count does
	// not move.
	auto served = provider.UnwrapKey(identity_b, blob_b);
	REQUIRE(served == dek_a);
	REQUIRE(server.Connections() == 1);
}
