//===----------------------------------------------------------------------===//
//                         DuckLake
//
// storage/ducklake_encryption_provider.hpp
//
// KMS-agnostic envelope encryption provider interface.
//
// DuckLake encrypts Parquet files with per-file DEKs. When a KMS envelope is
// configured, the DEK stored in the catalog is a wrapped blob rather than a
// plaintext key. This abstract interface is what the catalog calls to
// wrap/unwrap/rewrap those blobs; a concrete provider (crypta, a cloud KMS,
// a local HSM) implements it and is wired at ATTACH.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/vector.hpp"

#include <functional>

namespace duckdb {

//! What a per-file encryption key is bound to.
//!
//! Mirrors the KMS identity concept exactly: the four-tuple (lake_id,
//! table_id, file_kind, stored_path) is what the plaintext DEK is
//! cryptographically tied to, so a blob from one file cannot be used as a
//! key for another.
//!
//! The path is the path AS STORED in ducklake_data_file /
//! ducklake_delete_file - before FromRelativePath resolves it against the
//! table's data path. Resolving first would bind every key in a table to a
//! mutable table property, so changing the data path would orphan the lot.
struct DuckLakeFileIdentity {
	string lake_id;
	int64_t table_id = 0;
	//! Data files and delete files have independent id spaces and separate
	//! catalog tables, so the kind is part of the binding.
	bool is_delete_file = false;
	string stored_path;
};

//! One item of a rewrap batch reply.
//!
//! The pair is the whole point, and neither half is worth much alone.
//! `wrapped` without `rewrapped` cannot tell a moved row from an untouched
//! one, so a sweep would rewrite every row on every pass and could never
//! report convergence; `rewrapped` without `wrapped` says something happened
//! and hands back nothing to write.
struct DuckLakeRewrapResult {
	//! Base64 blob the row should now carry. When `rewrapped` is false this
	//! is the blob that was SENT - the row was already under the active key
	//! - so a converged sweep writes nothing.
	string wrapped;
	//! Whether the provider actually moved this row onto the active key.
	bool rewrapped = false;
};

//! KMS-agnostic envelope encryption provider.
//!
//! One instance per attached lake. Owned by DuckLakeCatalog; created at
//! ATTACH when the encryption envelope options are set.
//!
//! All three operations (wrap / unwrap / rewrap) operate on BATCHES because
//! a DuckLake scan materialises its whole file-and-key list before reading
//! any file, so one round trip serves a scan. A concrete provider may
//! implement them with any wire protocol it chooses.
//!
//! ## Why this needs no schema change
//!
//! `encryption_key` is an opaque VARCHAR holding base64. A wrapped blob is
//! also base64. So the column, every query that selects it, and every
//! DuckLake version that reads it are untouched - the bytes simply stop
//! being a plaintext key.
class DuckLakeEncryptionProvider {
public:
	//! How long an unwrapped DEK may sit in the cache, when ATTACH does not
	//! say. Five minutes — a cache entry's entire value is not re-asking the
	//! KMS for the same file inside one scan, and a scan is seconds to a
	//! couple of minutes.
	static constexpr int64_t DEFAULT_CACHE_TTL_SECONDS = 300;
	//! The ceiling an operator may configure — one hour. A configurable TTL
	//! with no ceiling would let an operator restore the unbounded
	//! process-lifetime residual via configuration.
	static constexpr int64_t MAX_CACHE_TTL_SECONDS = 3600;

	virtual ~DuckLakeEncryptionProvider() = default;

	//! Wrap one commit's worth of keys in a single call. `deks` are raw key
	//! bytes; the returned strings are base64 blobs ready for the catalog
	//! column.
	virtual vector<string> WrapKeys(const vector<DuckLakeFileIdentity> &identities,
	                                const vector<string> &deks) = 0;

	//! Unwrap a single key read from the catalog. Returns raw DEK bytes.
	//!
	//! MUST refuse a value that is not an envelope blob: on an enveloped
	//! lake, a plaintext row is either a pre-envelope leftover or a downgrade
	//! attempt, and reading it would defeat the envelope.
	virtual string UnwrapKey(const DuckLakeFileIdentity &identity, const string &base64_value) = 0;

	//! Move one batch of stored blobs onto the KMS's ACTIVE key — the
	//! consumer half of a key rotation's sweep step (mint → serve both →
	//! SWEEP → retire). Returns blobs ready to go back into the catalog
	//! column, each flagged with whether the provider actually moved it.
	virtual vector<DuckLakeRewrapResult> RewrapKeys(const vector<DuckLakeFileIdentity> &identities,
	                                                 const vector<string> &blobs) = 0;

	//! Assert the KMS is reachable and report what it is rooted in. Called
	//! at ATTACH so a misconfiguration surfaces then, not mid-scan.
	virtual string SelfTest() = 0;

	//! The operator-configured lake id that scopes every key.
	virtual const string &LakeId() const = 0;

	//! True when `value` carries the wrapped-key BLOB HEADER, so a wrapped
	//! row can be told apart from a pre-envelope plaintext key without a
	//! service call.
	//!
	//! The default implementation checks for the "DLK1" magic prefix after
	//! base64 decode — the crypta blob header. A provider that uses a
	//! different wire format MUST override this; the catalog depends on it to
	//! detect wrapped blobs when no provider is configured.
	//!
	//! Used to fail closed in both directions: a wrapped lake read by an
	//! unconfigured reader is refused, and a plaintext row served to a
	//! configured reader is refused.
	static bool LooksWrapped(const string &base64_value);

	//! True when every character of `value` is in the base64 alphabet.
	//! Alphabet only — length and padding are the KMS's to judge.
	static bool IsBase64(const string &value);

	//! A factory that creates a concrete encryption provider from its ATTACH
	//! parameters. Registered at extension init time by the bench build that
	//! supplies the concrete KMS implementation; when no factory is registered
	//! (the upstream build), the catalog refuses ENCRYPTION_SOCKET at ATTACH.
	using Factory = std::function<unique_ptr<DuckLakeEncryptionProvider>(string socket, string lake_id, int64_t ttl)>;
	static void RegisterFactory(Factory factory);
	static const Factory &GetFactory();

private:
	static Factory factory_;
};

} // namespace duckdb
