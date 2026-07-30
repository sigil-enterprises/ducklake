//===----------------------------------------------------------------------===//
//                         DuckLake (sigil fork)
//
// crypta/ducklake_crypta.hpp
//
// Envelope key provider: turns the plaintext key in
// ducklake_data_file.encryption_key into an identity-bound wrapped blob.
//
// PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "crypta/crypta_client.hpp"
#include "duckdb/common/mutex.hpp"

namespace duckdb {

//! Per-lake envelope provider. Owned by the catalog; one per ATTACH.
//!
//! ## Why this needs no schema change
//!
//! `encryption_key` is an opaque VARCHAR holding base64. A wrapped blob is also
//! base64. So the column, every query that selects it, and every DuckLake
//! version that reads it are untouched - the bytes simply stop being a key.
//!
//! ## Round-trip cost, stated honestly
//!
//! Writes batch: the write sites already hold a `vector` of files, so one call
//! wraps a whole commit. Reads are currently ONE call per file, because the
//! unwrap choke point (`ReadDataFile`) is invoked per row from eight-plus
//! queries and batching there means restructuring each caller.
//!
//! That is a performance cost, not the architectural problem the design was
//! built around. The blocker was per-file *HSM* calls - one operator YubiHSM
//! with a 16-session cap against thousands of files - and those do not happen
//! at any batch size: crypta unwraps DEKs in-process under a cached KEK and
//! touches the device only at startup. What remains is a local Unix-socket round
//! trip, and the cache below removes the repeated ones.
class DuckLakeCryptaProvider {
public:
	DuckLakeCryptaProvider(string socket_path, string lake_id);

	//! Wrap one commit's worth of keys in a single call. `deks` are raw bytes;
	//! the returned strings are base64 blobs ready for the catalog column.
	vector<string> WrapKeys(const vector<CryptaFileIdentity> &identities, const vector<string> &deks);

	//! Unwrap a single key read from the catalog. Returns raw DEK bytes.
	//!
	//! Refuses a value that is not a crypta blob: on a lake whose keys are
	//! wrapped, a plaintext row is either a pre-envelope leftover or a downgrade
	//! attempt, and reading it would defeat the envelope. Fail closed, loudly.
	string UnwrapKey(const CryptaFileIdentity &identity, const string &base64_value);

	//! Assert the service is reachable and report what it is rooted in. Called
	//! at ATTACH so a misconfiguration surfaces then, not mid-scan.
	string SelfTest();

	const string &LakeId() const {
		return lake_id;
	}

private:
	CryptaClient client;
	string lake_id;

	//! Unwrapped DEKs, keyed by the wrapped blob.
	//!
	//! Bounded, and keyed by the CIPHERTEXT rather than by the file: two rows
	//! with the same blob are the same key, and a row whose blob changes must
	//! miss. Entries are dropped wholesale when the cap is hit - a scan re-reads
	//! the same handful of files, so a crude cap beats an LRU's bookkeeping here.
	//!
	//! This is a plaintext-DEK cache inside the reader process, which is the
	//! residual this design already accepts and documents: a compromised live
	//! reader holds the keys for what it is reading. It does NOT widen that -
	//! the process holds those keys anyway while the Parquet reader uses them.
	//! It is capped and process-local, and it is the reason no crypto-shred
	//! claim is made anywhere.
	mutex cache_lock;
	unordered_map<string, string> unwrap_cache;
	static constexpr idx_t MAX_CACHED_KEYS = 4096;
};

} // namespace duckdb
