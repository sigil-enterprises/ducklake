//===----------------------------------------------------------------------===//
//                         DuckLake (sigil fork)
//
// crypta/ducklake_crypta.cpp
//
// PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.
//
//===----------------------------------------------------------------------===//

#include "crypta/ducklake_crypta.hpp"

#include "duckdb/common/exception.hpp"
// DO NOT drop as unused. Since the cache key stopped using StringUtil::Format,
// nothing in this file's production code needs this header - but the
// `cache_key_unprefixed_join` mutant reintroduces `StringUtil::Format` into a copy
// of this source to restore the bare-`|` join it exists to prove is broken. An
// unused-include cleanup here turns that mutant into a compile failure, which the
// runner reports as `ERROR ... the mutated client does not compile` rather than
// silently, so it fails loud - but it is still a trap worth naming here.
#include "duckdb/common/string_util.hpp"

namespace duckdb {

DuckLakeCryptaProvider::DuckLakeCryptaProvider(string socket_path, string lake_id_p)
    : client(std::move(socket_path)), lake_id(std::move(lake_id_p)) {
	if (lake_id.empty()) {
		// Without a lake id every lake on a shared crypta would produce
		// interchangeable bindings, so a key from lake A would unwrap for the same
		// table and path in lake B. Refusing here is the only safe default; there
		// is nothing inside DuckLake to fall back to.
		throw InvalidInputException("crypta_lake_id must be set when crypta_socket is set - it scopes every key to "
		                            "this lake, and without it keys are interchangeable between lakes");
	}
}

vector<string> DuckLakeCryptaProvider::WrapKeys(const vector<CryptaFileIdentity> &identities,
                                                const vector<string> &deks) {
	return client.WrapBatch(identities, deks);
}

//! Append one cache-key component as `<decimal-byte-length>:<raw-bytes>`.
//!
//! Private to this translation unit on purpose: it is an encoding detail of the
//! cache key below, not something a caller has any business reproducing.
//!
//! EMBEDDED NULs are handled, and saying so is not pedantry - an injectivity claim
//! that ignores them is how a length-prefixing scheme fails in practice. `size()`
//! counts a NUL like any other byte and `+=` copies it, so a component containing
//! one is length-prefixed and compared by its full extent. Nothing here goes
//! through `c_str()`, which is what would truncate at the first NUL and silently
//! merge two distinct components into one key.
static void AppendLengthPrefixed(string &key, const string &component) {
	key += std::to_string(component.size());
	key += ':';
	key += component;
}

string DuckLakeCryptaProvider::UnwrapKey(const CryptaFileIdentity &identity, const string &base64_value) {
	if (!CryptaClient::LooksWrapped(base64_value)) {
		throw IOException(
		    "file %s carries a plaintext encryption key, but this lake is configured for crypta envelope "
		    "encryption. Refusing to use it: on an enveloped lake a plaintext key row is either a leftover from "
		    "before the envelope was enabled or an attempt to downgrade the file, and using it would defeat the "
		    "envelope entirely",
		    identity.stored_path);
	}
	// A blob that is not base64 can never unwrap, so refusing it here is strictly
	// better than forwarding it. `LooksWrapped` above tells a wrapped row from a
	// plaintext one - four characters - and that is all it does; leaning on it as
	// validation is what let a catalog value carrying JSON punctuation reach the
	// request builder (issue #24). The builder escapes it now, so this is not the
	// only thing standing between the column and the wire - it is the layer that
	// stops a value that could never be a key from being sent at all.
	if (!CryptaClient::IsBase64(base64_value)) {
		throw IOException("file %s carries an encryption key that is not base64 - a wrapped key that cannot even be "
		                  "decoded is either corruption or an attempt to write something other than a key into the "
		                  "catalog, and neither is worth sending to crypta",
		                  identity.stored_path);
	}
	// The cache key MUST include the identity, not just the blob - and the
	// COMPOSITION of the components must itself be unambiguous.
	//
	// Keying on the blob alone is a hole, and a subtle one: read file A, which
	// caches blob-A -> DEK-A; then an attacker pastes blob A onto file B's row;
	// the next read of file B hits the cache and gets DEK-A back WITHOUT crypta
	// ever seeing the mismatched identity. The binding would be bypassed for the
	// life of the process. Keying on (identity, blob) is NECESSARY for a
	// substituted row to miss the cache, but it is not SUFFICIENT: while the
	// components were joined with a bare `|` the key was not injective, so two
	// DIFFERENT (identity, blob) pairs could still land on one entry and the
	// bypass came straight back through the separator. No byte is safe to use as
	// that separator either: `stored_path` is written by whoever can write the
	// catalog row, and `base64_value` is only prefix-checked (`LooksWrapped`),
	// never base64-validated, so neither can be assumed free of any particular
	// character.
	//
	// On reachability, stated carefully because the obvious candidate is a dead end:
	// `ducklake_add_data_files` does store an operator-supplied path verbatim, but
	// the row it writes carries NO encryption key, and a keyless row throws in
	// `ReadDataFile` before an identity is ever built - so it never reaches here.
	// The real route is the threat model the envelope already assumes: an attacker
	// with catalog write access sets both `path` and a wrapped `encryption_key` on
	// the same `ducklake_data_file` row, which is full control of `stored_path`.
	//
	// So each component is LENGTH-PREFIXED as `<decimal-byte-length>:<raw-bytes>`
	// and the five are concatenated with NO separator between them.
	//
	// That is injective by construction, and the argument is worth stating because
	// it is the whole point of the encoding. A decoder starts where a component
	// starts. It reads the run of decimal digits up to the FIRST `:` - unambiguous
	// because the run is written by this function and contains digits only - and
	// then consumes EXACTLY that many bytes BY COUNT, never scanning for a
	// delimiter. Each component's boundary is therefore fixed before any of its
	// bytes is looked at, so no content can be re-read as structure: a component
	// whose bytes begin with digits or with `:` is already inside the counted run,
	// and a length-0 component consumes nothing and leaves the decoder positioned
	// exactly on the next length. Since the decoding is unique, distinct component
	// tuples cannot share an encoding - which is what makes a substituted row
	// always a miss, and always a trip to crypta, which rejects it.
	//
	// The COMPONENT SET is the other half of the property, and it is the one that
	// would hide a fresh instance of this same bug: a field crypta BINDS to but the
	// key OMITS means two files differing only in that field share an entry, and
	// every collision test above still passes. The invariant is therefore
	// wire-set == key-set. `CryptaFileIdentity` has exactly four fields and
	// `CryptaClient::IdentityJson` puts exactly those four on the wire
	// (catalog_uuid, table_id, file_kind, file_path); the five components below are
	// those four plus the blob. Adding a field to the identity means adding it
	// here, or the binding silently stops being covered by the cache key.
	const string file_kind = identity.is_delete_file ? "delete" : "data";
	// `table_id` is `int64_t`, so this cast is width-neutral rather than narrowing,
	// and `std::to_string` is injective over it - including negatives, whose `-` is
	// an ordinary byte inside the length prefix's count.
	const string table_id_text = std::to_string(static_cast<long long>(identity.table_id));
	string cache_key;
	// A hint, not a bound: the three big components verbatim plus slack for the
	// table id, the file kind and the five `<length>:` prefixes.
	cache_key.reserve(identity.lake_id.size() + identity.stored_path.size() + base64_value.size() + 128);
	AppendLengthPrefixed(cache_key, identity.lake_id);
	AppendLengthPrefixed(cache_key, table_id_text);
	AppendLengthPrefixed(cache_key, file_kind);
	AppendLengthPrefixed(cache_key, identity.stored_path);
	AppendLengthPrefixed(cache_key, base64_value);
	{
		lock_guard<mutex> guard(cache_lock);
		auto entry = unwrap_cache.find(cache_key);
		if (entry != unwrap_cache.end()) {
			return entry->second;
		}
	}

	vector<CryptaFileIdentity> identities {identity};
	vector<string> blobs {base64_value};
	auto keys = client.UnwrapBatch(identities, blobs);
	if (keys.size() != 1) {
		throw IOException("crypta returned %llu keys for one file", static_cast<uint64_t>(keys.size()));
	}

	{
		lock_guard<mutex> guard(cache_lock);
		if (unwrap_cache.size() >= MAX_CACHED_KEYS) {
			unwrap_cache.clear();
		}
		unwrap_cache[cache_key] = keys[0];
	}
	return keys[0];
}

string DuckLakeCryptaProvider::SelfTest() {
	// Reachability only. It deliberately does NOT wrap a probe key: that would
	// write a key nobody asked for, and reachability is the thing that fails.
	auto health = client.Health();
	if (health.find("\"ok\":true") == string::npos) {
		throw IOException("crypta at %s did not report ok: %s", client.SocketPath(), health);
	}
	return health;
}

} // namespace duckdb
