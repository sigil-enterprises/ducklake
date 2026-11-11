//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_encryption_provider.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/unordered_map.hpp"

#include <functional>

namespace duckdb {
class DatabaseInstance;

//! What a per-file encryption key is cryptographically bound to, so a wrapped blob minted for one
//! file cannot be used as the key for another.
struct DuckLakeFileIdentity {
	string lake_id;
	int64_t table_id = 0;
	//! Data files and delete files have independent id spaces and separate catalog tables, so the
	//! kind of file is part of the binding.
	bool is_delete_file = false;
	//! The path as stored in ducklake_data_file / ducklake_delete_file, before it is resolved
	//! against the table's data path - which is a mutable table property.
	string stored_path;
};

//! One entry of a rewrap batch reply.
struct DuckLakeRewrapResult {
	//! The value the row should carry from now on. When `rewrapped` is false this is the value that
	//! was sent, so a converged sweep writes nothing.
	string wrapped;
	//! Whether the provider moved this entry onto its active key.
	bool rewrapped = false;
};

//! Envelope encryption provider: wraps the per-file DEKs DuckLake stores in `encryption_key` so the
//! metadata catalog never holds a usable key. One instance per attached lake.
//!
//! Every operation takes a batch: a scan materialises its whole file-and-key list before reading any
//! file, so one round trip serves a scan.
//!
//! `encryption_key` stays an opaque VARCHAR, so no catalog schema change is required.
class DuckLakeEncryptionProvider {
public:
	//! Lifetime of a cached unwrapped DEK when ATTACH does not specify one.
	static constexpr int64_t DEFAULT_CACHE_TTL_SECONDS = 300;
	//! Ceiling for a configured cache TTL, so it cannot be raised to an effectively unbounded one.
	static constexpr int64_t MAX_CACHE_TTL_SECONDS = 3600;

	virtual ~DuckLakeEncryptionProvider() = default;

	//! Wrap one commit's worth of keys. `deks` are raw key bytes; the returned strings are the
	//! values the `encryption_key` column will carry.
	virtual vector<string> WrapKeys(const vector<DuckLakeFileIdentity> &identities, const vector<string> &deks) = 0;

	//! Unwrap a single stored value into raw DEK bytes. Must refuse a value that is not an envelope
	//! blob: on an enveloped lake a plaintext key is a pre-envelope leftover or a downgrade attempt.
	virtual string UnwrapKey(const DuckLakeFileIdentity &identity, const string &stored_value) = 0;

	//! Move a batch of stored values onto the provider's active key - the sweep step of a key
	//! rotation, which never exposes the plaintext DEK to the caller.
	virtual vector<DuckLakeRewrapResult> RewrapKeys(const vector<DuckLakeFileIdentity> &identities,
	                                                const vector<string> &blobs) = 0;

	//! Assert the key service is reachable and report what it is rooted in. Called at ATTACH so a
	//! misconfiguration surfaces there rather than mid-scan.
	virtual string SelfTest() = 0;

	//! The configured lake id that scopes every key in this lake.
	virtual const string &GetLakeId() const = 0;

	//! True when `stored_value` carries the wrapped-value header, which is fixed at this interface
	//! level rather than chosen per provider. Lets a reader that has no provider tell a wrapped row
	//! from a plaintext one without a service call, and refuse it instead of handing ciphertext to
	//! the Parquet reader as a key.
	static bool LooksWrapped(const string &stored_value);

	//! True when `value` is non-empty and every character is in the base64 alphabet. Alphabet only:
	//! length and padding are the provider's to judge.
	static bool IsBase64(const string &value);

	//! THE PROVIDER REGISTRY (issue #120).
	//!
	//! Providers are registered per PROCESS under a NAME, and each attached lake SELECTS one with the
	//! ATTACH option ENCRYPTION_PROVIDER. The registry used to be a single slot, which made the
	//! provider a property of the process rather than of the catalog: one process could not ATTACH two
	//! lakes rooted in different key services - a second provider could not be registered, and a
	//! second lake had no way to ask for anything but the first one's.
	//!
	//! A lake that names no provider gets the one registered as DEFAULT_PROVIDER_NAME, or - when
	//! there is no such entry - the only one registered. With several registered and none of them the
	//! default, an ATTACH that names none is refused rather than resolved by registration order.
	static constexpr const char *DEFAULT_PROVIDER_NAME = "default";

	//! Creates the provider for one attached lake through the LEGACY single-factory API.
	//!
	//! `db` is the attaching database: a provider takes its crypto primitives from it rather than
	//! choosing its own, so it follows whatever policy that database is configured with.
	using Factory = std::function<unique_ptr<DuckLakeEncryptionProvider>(
	    DatabaseInstance &db, const string &encryption_socket, const string &encryption_lake_id,
	    idx_t cache_ttl_seconds)>;
	//! LEGACY: registers `factory` under DEFAULT_PROVIDER_NAME. Throws when that name is already taken:
	//! a second one would make the provider a lake gets depend on extension load order. A provider
	//! registered this way cannot receive ENCRYPTION_PROVIDER_OPTIONS, so supplying them for it is
	//! refused at ATTACH, and it still requires ENCRYPTION_SOCKET.
	static void RegisterFactory(Factory factory);
	//! LEGACY: whether ANY provider is registered in this process.
	static bool HasFactory();

	//! Everything ATTACH hands a provider for one lake.
	struct Config {
		//! The registry name the lake resolved to.
		string provider_name;
		//! ENCRYPTION_SOCKET; empty when the ATTACH supplied none, which only a named provider allows.
		string encryption_socket;
		//! ENCRYPTION_LAKE_ID; never empty - the catalog refuses an envelope without one.
		string encryption_lake_id;
		idx_t cache_ttl_seconds = 0;
		//! ENCRYPTION_PROVIDER_OPTIONS, passed through UNINTERPRETED: what the keys mean is the
		//! provider's business, so DuckLake fixes no option names for any particular key service.
		unordered_map<string, string> properties;
	};
	using ProviderFactory =
	    std::function<unique_ptr<DuckLakeEncryptionProvider>(DatabaseInstance &db, const Config &config)>;
	//! Registers `factory` under `name` (compared case-insensitively). Throws on an empty name and on a
	//! name already taken - two providers answering to one name would make the one a lake gets depend on
	//! extension load order, which is exactly what the single slot refused.
	static void RegisterProvider(const string &name, ProviderFactory factory);
	//! Runs `registration` - DuckLake registering the providers its build carries, at LOAD - with the
	//! duplicate-name refusal relaxed on THIS thread: a name that is already taken is left to whoever
	//! registered it first, and registration carries on as if it had succeeded. Every other refusal
	//! (an empty name, an empty factory) still throws, and a registration made outside this call -
	//! a host registering after LOAD - still collides.
	//!
	//! Why: a host that registered a name before `LOAD ducklake` must not make the load throw, and a
	//! load retried after a partial registration must surface the error that stopped it, not a
	//! collision with a name its own first attempt registered.
	static void RegisterOwnProviders(const std::function<void()> &registration);
	static bool HasProvider(const string &name);
	//! Registered names, sorted, for error messages and diagnostics.
	static vector<string> RegisteredProviderNames();
	//! Resolves the provider a lake asked for and creates it. `requested_name` empty means the ATTACH
	//! named none. Throws, naming what IS registered, when the request resolves to nothing - never
	//! falls back to some other provider.
	static unique_ptr<DuckLakeEncryptionProvider> Create(DatabaseInstance &db, const string &requested_name,
	                                                     Config config);
};

} // namespace duckdb
