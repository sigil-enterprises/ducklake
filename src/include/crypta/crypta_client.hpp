//===----------------------------------------------------------------------===//
//                         DuckLake (sigil fork)
//
// crypta/crypta_client.hpp
//
// Client for the crypta envelope key service. PRIVATE-FORK ONLY - this file is
// not upstream-eligible and must never be cherry-picked to the public fork.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

//! What a per-file key is cryptographically bound to.
//!
//! Mirrors crypta's FileIdentity exactly. The path is the path AS STORED in
//! ducklake_data_file / ducklake_delete_file - before FromRelativePath resolves
//! it against the table's data path. Resolving first would bind every key in a
//! table to a mutable table property, so changing the data path would orphan
//! the lot.
struct CryptaFileIdentity {
	//! Operator-configured lake id. Names the compartment, which is what a
	//! per-compartment KEK binds to. There is no usable value inside DuckLake to
	//! derive this from: ducklake_metadata holds only version / created_by /
	//! data_path / encrypted, and DuckLakeCatalog::instance_id is regenerated on
	//! every ATTACH.
	string lake_id;
	int64_t table_id = 0;
	//! Data files and delete files have independent id spaces and separate
	//! catalog tables, so the kind is part of the binding.
	bool is_delete_file = false;
	string stored_path;
};

//! Speaks CryptaWireManifest@v2 over a Unix domain socket.
//!
//! One instance per attached lake. Batch calls are the point: a DuckLake scan
//! materialises its whole file-and-key list before reading any file, so one
//! round trip serves a scan. There is no per-file call anywhere in this class.
class CryptaClient {
public:
	explicit CryptaClient(string socket_path);

	//! Wrap plaintext DEKs. Returns base64 blobs in request order.
	vector<string> WrapBatch(const vector<CryptaFileIdentity> &identities, const vector<string> &deks);
	//! Unwrap base64 blobs. Returns raw DEK bytes in request order.
	vector<string> UnwrapBatch(const vector<CryptaFileIdentity> &identities, const vector<string> &blobs);
	//! Liveness. Never returns key material - safe to log.
	string Health();

	const string &SocketPath() const {
		return socket_path;
	}

	//! True when `value` carries the crypta wrapped-key magic, so a wrapped row
	//! can be told apart from a pre-envelope plaintext key without a service
	//! call. Used to fail closed in both directions: a wrapped lake read by an
	//! unconfigured reader, and a plaintext row served to a configured one.
	static bool LooksWrapped(const string &base64_value);

	//! True when every character of `value` is in the base64 alphabet. The
	//! validation `LooksWrapped` is not: a value outside the alphabet can never
	//! decode to a wrapped key, so it has no business reaching the wire.
	//! Alphabet only - length and padding are crypta's to judge.
	static bool IsBase64(const string &value);

private:
	string socket_path;

	//! Send one framed request, return the response body.
	string Request(const string &json_body);
	//! Extract the base64 values of `field` from an items array, in order.
	//! Strict: throws unless exactly `expected` values are present.
	static vector<string> ExtractBase64Field(const string &response, const string &field, idx_t expected);
	static void ThrowIfError(const string &response);
	static string IdentityJson(const CryptaFileIdentity &identity);
};

} // namespace duckdb
