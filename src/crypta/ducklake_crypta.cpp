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

string DuckLakeCryptaProvider::UnwrapKey(const CryptaFileIdentity &identity, const string &base64_value) {
	if (!CryptaClient::LooksWrapped(base64_value)) {
		throw IOException(
		    "file %s carries a plaintext encryption key, but this lake is configured for crypta envelope "
		    "encryption. Refusing to use it: on an enveloped lake a plaintext key row is either a leftover from "
		    "before the envelope was enabled or an attempt to downgrade the file, and using it would defeat the "
		    "envelope entirely",
		    identity.stored_path);
	}
	// The cache key MUST include the identity, not just the blob.
	//
	// Keying on the blob alone is a hole, and a subtle one: read file A, which
	// caches blob-A -> DEK-A; then an attacker pastes blob A onto file B's row;
	// the next read of file B hits the cache and gets DEK-A back WITHOUT crypta
	// ever seeing the mismatched identity. The binding would be bypassed for the
	// life of the process. Keying on (identity, blob) means a substituted row is
	// always a miss and always goes to crypta, which rejects it.
	auto cache_key = StringUtil::Format("%s|%lld|%s|%s", identity.lake_id, static_cast<long long>(identity.table_id),
	                                    identity.is_delete_file ? "delete" : "data", identity.stored_path) +
	                 "|" + base64_value;
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
