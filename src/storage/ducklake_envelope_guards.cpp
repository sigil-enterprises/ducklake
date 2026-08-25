//===----------------------------------------------------------------------===//
//                         DuckLake
//
// storage/ducklake_envelope_guards.cpp
//
// KMS-agnostic envelope encryption guards.
//
// This file implements the five-question key-resolution choke point and its
// constituent refusals. Nothing here hard-codes a particular KMS — it works
// with any DuckLakeEncryptionProvider implementation.
//
//===----------------------------------------------------------------------===//

#include "storage/ducklake_catalog.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"

#include <cstdlib>

namespace duckdb {

DuckLakeFileIdentity DuckLakeCatalog::BuildEncryptionIdentity(DuckLakeTableIndex table_id, const string &stored_path,
                                                              bool is_delete_file) const {
	DuckLakeFileIdentity identity;
	identity.lake_id = options.encryption_lake_id;
	identity.table_id = static_cast<int64_t>(table_id.index);
	identity.is_delete_file = is_delete_file;
	identity.stored_path = stored_path;
	return identity;
}

void DuckLakeCatalog::RefuseWrappedKeyWithoutProvider(const string &file_path, const string &stored_key) const {
	if (encryption_provider) {
		// A configured lake unwraps rather than refuses; that is the caller's job.
		return;
	}
	if (!DuckLakeEncryptionProvider::LooksWrapped(stored_key)) {
		// A plaintext key on a lake with no envelope provider is the ordinary
		// upstream case.
		return;
	}
	throw IOException("file %s carries a wrapped encryption key, but this lake "
	                  "was attached without "
	                  "ENCRYPTION_SOCKET / ENCRYPTION_LAKE_ID. Re-attach with "
	                  "the envelope options",
	                  file_path);
}

void DuckLakeCatalog::RefuseMissingEncryptionKey(const string &file_path) const {
	if (!IsEncrypted()) {
		// An unencrypted lake stores no per-file key, so a NULL column is what a
		// correct row looks like.
		return;
	}
	// Upstream's message, character for character, because it is what the
	// existing tests and the operator runbooks match on.
	throw InvalidInputException("Database is encrypted, but file %s does not have an encryption key", file_path);
}

void DuckLakeCatalog::RefuseUnusableEncryptionKey(const string &file_path, const string &decoded_key) const {
	// 16, 24 and 32 bytes: AES-128, AES-192, AES-256. READ OFF the two places
	// that consume the key rather than chosen here:
	//
	//   duckdb/extension/parquet/parquet_crypto.cpp  `ParquetCrypto::ValidKey`
	//     switches on 16/24/32 and rejects everything else.
	//   duckdb/third_party/mbedtls/mbedtls_wrapper.cpp  `GetCipher()` switches on
	//     16/24/32 for GCM and raises "Invalid AES key length for GCM" in its
	//     default arm - the INTERNAL error this guard exists to get in front of.
	if (decoded_key.size() == 16 || decoded_key.size() == 24 || decoded_key.size() == 32) {
		return;
	}
	throw InvalidInputException("file %s carries an encryption key of %llu bytes, which is not a length "
	                            "AES accepts (16, 24 or 32). The "
	                            "stored encryption_key for this row is corrupt or was tampered with",
	                            file_path, static_cast<uint64_t>(decoded_key.size()));
}

void DuckLakeCatalog::RequireEncryptedTempSpill(ClientContext &context) {
	if (!encryption_provider) {
		// No envelope, no secret to protect, and upstream's spill behaviour is
		// left exactly as it was.
		return;
	}
	if (Settings::Get<TempFileEncryptionSetting>(context)) {
		// Already on - whether an operator set it, or an earlier enveloped ATTACH
		// in this process did.
		return;
	}
	auto option = DBConfig::GetOptionByName("temp_file_encryption");
	if (!option) {
		throw InvalidInputException("this DuckDB build has no `temp_file_encryption` setting, so an "
		                            "enveloped DuckLake cannot be "
		                            "attached: an out-of-core query would spill decrypted rows to "
		                            "temp_directory in the clear");
	}
	auto &config = DBConfig::GetConfig(context);
	try {
		config.SetOption(context.db.get(), *option, Value::BOOLEAN(true));
	} catch (std::exception &ex) {
		throw InvalidInputException("this DuckLake carries a KMS envelope, so DuckDB's "
		                            "`temp_file_encryption` must be on before it is "
		                            "attached - otherwise an out-of-core query spills decrypted rows to "
		                            "temp_directory in the clear. Turning "
		                            "it on was refused: %s. This process has already written unencrypted "
		                            "temporary files, and DuckDB will "
		                            "not encrypt a temp directory that is half plaintext. Attach the "
		                            "enveloped lake before running work that "
		                            "spills, or start a fresh process",
		                            string(ex.what()));
	}
}

void DuckLakeCatalog::RefuseUnencryptedTempSpill(const string &what) const {
	if (!encryption_provider) {
		return;
	}
	if (Settings::Get<TempFileEncryptionSetting>(db.GetDatabase())) {
		return;
	}
	throw IOException("refusing %s on an enveloped DuckLake while "
	                  "`temp_file_encryption` is off - DuckDB would write any "
	                  "buffer it cannot hold in memory to temp_directory in the "
	                  "clear, which puts decrypted rows on local disk "
	                  "outside the envelope entirely. It was turned on for this "
	                  "process at ATTACH, so something has since turned "
	                  "it back off. Run `SET temp_file_encryption = true`",
	                  what);
}

string DuckLakeCatalog::ResolveStoredEncryptionKey(DuckLakeTableIndex table_id, const string &stored_path,
                                                   const string &resolved_path, bool is_delete_file,
                                                   const Value &stored_key_value) const {
	if (stored_key_value.IsNull()) {
		// QUESTION 1 OF FIVE. Must be asked HERE, not by the caller: while it
		// lived at the call sites, a caller's own `if (!...IsNull())` skipped this
		// whole resolution on a NULL instead of refusing, so an ENCRYPTED lake's
		// missing key reached the Parquet reader as an unreadable error.
		RefuseMissingEncryptionKey(resolved_path);
		// Not encrypted: no key, exactly as upstream leaves the field.
		return string();
	}
	auto stored_key = stored_key_value.GetValue<string>();
	string decoded_key;
	if (encryption_provider) {
		// THE READ CALL SITE of the temp-spill guard. Placed BEFORE the unwrap:
		// once this function returns, the rows this key opens are on their way
		// into the buffer manager, and any one of them can be evicted to
		// temp_directory. Refusing after the unwrap would have already minted
		// the plaintext key it is trying to keep off disk.
		RefuseUnencryptedTempSpill("a read");
		decoded_key =
		    encryption_provider->UnwrapKey(BuildEncryptionIdentity(table_id, stored_path, is_delete_file), stored_key);
	} else {
		RefuseWrappedKeyWithoutProvider(resolved_path, stored_key);
		if (stored_key.empty()) {
			// QUESTION 4 OF FIVE. `VARCHAR` has more than two states and
			// question 1 above tests exactly one of them. `''` says what NULL
			// says - this row carries no usable key - so it gets the SAME
			// refusal in the SAME words.
			RefuseMissingEncryptionKey(resolved_path);
			return string();
		}
		decoded_key = Blob::FromBase64(string_t(stored_key));
	}
	// QUESTION 5 OF FIVE. An unusable key is unusable however it was obtained.
	RefuseUnusableEncryptionKey(resolved_path, decoded_key);
	return decoded_key;
}

void DuckLakeCatalog::PrepareFileKeysForCommit(const vector<DuckLakeFileIdentity> &identities,
                                               vector<string> &keys) const {
	D_ASSERT(identities.size() == keys.size());
	if (encryption_provider) {
		vector<DuckLakeFileIdentity> wrap_identities;
		vector<string> deks;
		vector<idx_t> positions;
		for (idx_t i = 0; i < keys.size(); i++) {
			if (!keys[i].empty()) {
				wrap_identities.push_back(identities[i]);
				deks.push_back(keys[i]);
				positions.push_back(i);
			}
		}
		if (deks.empty()) {
			return;
		}
		auto wrapped = encryption_provider->WrapKeys(wrap_identities, deks);
		if (wrapped.size() != deks.size()) {
			throw IOException("KMS wrapped %llu keys for %llu files", static_cast<uint64_t>(wrapped.size()),
			                  static_cast<uint64_t>(deks.size()));
		}
		for (idx_t i = 0; i < positions.size(); i++) {
			keys[positions[i]] = wrapped[i];
		}
		return;
	}
	// No envelope: store the base64 of the raw DEK exactly as upstream does.
	for (auto &key : keys) {
		if (!key.empty()) {
			key = Blob::ToBase64(string_t(key));
		}
	}
}

} // namespace duckdb
