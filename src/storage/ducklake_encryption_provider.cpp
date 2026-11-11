#include "storage/ducklake_encryption_provider.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

#include <map>
#include <mutex>

namespace duckdb {

namespace {

//! Base64 of a 32-byte key: the longest `encryption_key` a lake without an envelope stores. A
//! wrapped value carries a header and an authentication tag on top of the key, so it is longer.
constexpr idx_t LONGEST_PLAINTEXT_KEY_LENGTH = 44;

//! The named provider registry (issue #120). Registered for the life of the process so a provider
//! survives ATTACH/DETACH; never freed, hence a leaked pointer rather than a global with an exit-time
//! destructor. Keyed by the lower-cased name; std::map so RegisteredProviderNames() comes out sorted.
struct ProviderRegistry {
	std::mutex lock;
	std::map<string, DuckLakeEncryptionProvider::ProviderFactory> providers;
};

ProviderRegistry &Registry() {
	static auto *registry = new ProviderRegistry();
	return *registry;
}

string JoinNames(const vector<string> &names) {
	if (names.empty()) {
		return "none";
	}
	return "'" + StringUtil::Join(names, "', '") + "'";
}

} // namespace

// This project builds at C++11 (duckdb's CMakeLists pins CMAKE_CXX_STANDARD 11), where a static
// constexpr member still needs an out-of-line definition the moment it is ODR-used. Passing one
// through a variadic - StringUtil::Format, and so every exception message - is exactly that: a
// vararg takes no lvalue-to-rvalue conversion, so the linker asks for an address.
//
// Measured, not reasoned about. Without these definitions the loadable extension links fine while
// `duckdb`, `test/unittest` and `tools/plan_serializer` - every target that links
// libducklake_extension.a statically - fail with `undefined reference to
// DuckLakeEncryptionProvider::MAX_CACHE_TTL_SECONDS`. That is the same asymmetry that hid the
// dropped provider registration: a shared library and a static archive do not agree about what a
// symbol costs.
constexpr int64_t DuckLakeEncryptionProvider::DEFAULT_CACHE_TTL_SECONDS;
constexpr int64_t DuckLakeEncryptionProvider::MAX_CACHE_TTL_SECONDS;
constexpr const char *DuckLakeEncryptionProvider::DEFAULT_PROVIDER_NAME;

bool DuckLakeEncryptionProvider::LooksWrapped(const string &stored_value) {
	// A wrapped value starts with the fixed header "DLK1", whose first three bytes base64 as the
	// literal prefix "RExL". The length floor is part of the test: without it a plaintext key that
	// happens to start with those three bytes would be misread as wrapped and refused forever.
	if (stored_value.size() <= LONGEST_PLAINTEXT_KEY_LENGTH) {
		return false;
	}
	return stored_value.compare(0, 4, "RExL") == 0;
}

bool DuckLakeEncryptionProvider::IsBase64(const string &value) {
	if (value.empty()) {
		return false;
	}
	for (auto character : value) {
		auto c = static_cast<unsigned char>(character);
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' ||
		    c == '=') {
			continue;
		}
		return false;
	}
	return true;
}

void DuckLakeEncryptionProvider::RegisterFactory(Factory factory) {
	// The legacy signature carries no property map, so a lake handing this provider options would see
	// them silently dropped - refused instead. It also has no way to run without a socket.
	RegisterProvider(DEFAULT_PROVIDER_NAME,
	                 [factory](DatabaseInstance &db, const Config &config) -> unique_ptr<DuckLakeEncryptionProvider> {
		                 if (!config.properties.empty()) {
			                 throw InvalidInputException(
			                     "encryption provider '%s' was registered through the single-factory API and "
			                     "takes no ENCRYPTION_PROVIDER_OPTIONS - drop them, or select a provider that "
			                     "accepts them with ENCRYPTION_PROVIDER",
			                     config.provider_name);
		                 }
		                 if (config.encryption_socket.empty()) {
			                 throw InvalidInputException("encryption provider '%s' requires ENCRYPTION_SOCKET - supply "
			                                             "the address of its key service",
			                                             config.provider_name);
		                 }
		                 return factory(db, config.encryption_socket, config.encryption_lake_id,
		                                config.cache_ttl_seconds);
	                 });
}

bool DuckLakeEncryptionProvider::HasFactory() {
	auto &registry = Registry();
	std::lock_guard<std::mutex> guard(registry.lock);
	return !registry.providers.empty();
}

void DuckLakeEncryptionProvider::RegisterProvider(const string &name, ProviderFactory factory) {
	if (name.empty()) {
		throw InvalidInputException("a DuckLake encryption provider must be registered under a non-empty name");
	}
	if (!factory) {
		throw InvalidInputException("DuckLake encryption provider '%s' was registered with an empty factory", name);
	}
	auto key = StringUtil::Lower(name);
	auto &registry = Registry();
	std::lock_guard<std::mutex> guard(registry.lock);
	if (registry.providers.find(key) != registry.providers.end()) {
		throw InvalidInputException("a DuckLake encryption provider named '%s' is already registered - registering a "
		                            "second one under the same name would make the provider a lake gets depend on "
		                            "extension load order",
		                            name);
	}
	registry.providers.emplace(std::move(key), std::move(factory));
}

bool DuckLakeEncryptionProvider::HasProvider(const string &name) {
	auto &registry = Registry();
	std::lock_guard<std::mutex> guard(registry.lock);
	return registry.providers.find(StringUtil::Lower(name)) != registry.providers.end();
}

vector<string> DuckLakeEncryptionProvider::RegisteredProviderNames() {
	auto &registry = Registry();
	std::lock_guard<std::mutex> guard(registry.lock);
	vector<string> names;
	for (auto &entry : registry.providers) {
		names.push_back(entry.first);
	}
	return names;
}

unique_ptr<DuckLakeEncryptionProvider> DuckLakeEncryptionProvider::Create(DatabaseInstance &db,
                                                                          const string &requested_name, Config config) {
	ProviderFactory factory;
	{
		auto &registry = Registry();
		std::lock_guard<std::mutex> guard(registry.lock);
		vector<string> names;
		for (auto &entry : registry.providers) {
			names.push_back(entry.first);
		}
		if (!requested_name.empty()) {
			auto entry = registry.providers.find(StringUtil::Lower(requested_name));
			if (entry == registry.providers.end()) {
				throw InvalidInputException("no DuckLake encryption provider named '%s' is registered in this "
				                            "process - registered: %s",
				                            requested_name, JoinNames(names));
			}
			config.provider_name = entry->first;
			factory = entry->second;
		} else {
			auto entry = registry.providers.find(DEFAULT_PROVIDER_NAME);
			if (entry == registry.providers.end() && registry.providers.size() == 1) {
				// One provider and none named 'default': there is nothing to choose between, so an
				// ATTACH written before providers had names keeps resolving to it.
				entry = registry.providers.begin();
			}
			if (entry == registry.providers.end()) {
				throw InvalidInputException("this process has %llu DuckLake encryption providers registered (%s) and "
				                            "none is named '%s' - select one with ENCRYPTION_PROVIDER",
				                            static_cast<uint64_t>(names.size()), JoinNames(names),
				                            DEFAULT_PROVIDER_NAME);
			}
			config.provider_name = entry->first;
			factory = entry->second;
		}
	}
	// Called OUTSIDE the lock: a factory may do I/O, and it must not serialise every ATTACH in the
	// process behind it.
	auto provider = factory(db, config);
	if (!provider) {
		throw InternalException("DuckLake encryption provider '%s' returned no provider", config.provider_name);
	}
	return provider;
}

} // namespace duckdb
