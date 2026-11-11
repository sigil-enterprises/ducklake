//===----------------------------------------------------------------------===//
//                 DuckLake TEST-ONLY encryption provider registry tests
//
// test/kms_provider/test_kms_registry_unittest.cpp
//
// Catch cases for the named provider registry (issue #120), compiled into the
// `unittest` binary ONLY when the test provider is overlaid (see this
// directory's CMakeLists.txt). They need a C++ caller: duplicate and empty-name
// registration, and the legacy double RegisterFactory, cannot be reached from
// SQL at all.
//
// THE REGISTRY IS PROCESS-GLOBAL AND HAS NO RESET HOOK, on purpose: a provider
// must survive ATTACH/DETACH, and a reset entry point would be one more way to
// swap a lake's key service out from under it. So a case that needs the
// registry in a particular state BEFORE DuckLake loads cannot share a process
// with anything else. Those cases carry the hidden tag `[.]` - excluded from a
// default run - and are each run in a FRESH unittest process by the Envelope
// workflow. Each REQUIREs its own precondition (nothing registered yet) and
// FAILS, never skips, when it does not hold.
//
//===----------------------------------------------------------------------===//

#include "catch.hpp"

#include "duckdb.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/database.hpp"

// Relative, so it resolves identically from test/kms_provider/ and from the
// src/crypta-provider/ overlay: both sit two levels below the repository root.
#include "../../src/include/storage/ducklake_encryption_provider.hpp"

#include <algorithm>
#include <functional>

using namespace duckdb;

namespace {

//! A provider that is never asked to do anything but identify itself.
class StubProvider : public DuckLakeEncryptionProvider {
public:
	StubProvider(string root_p, string lake_id_p) : root(std::move(root_p)), lake_id(std::move(lake_id_p)) {
	}
	vector<string> WrapKeys(const vector<DuckLakeFileIdentity> &, const vector<string> &deks) override {
		return deks;
	}
	string UnwrapKey(const DuckLakeFileIdentity &, const string &stored_value) override {
		return stored_value;
	}
	vector<DuckLakeRewrapResult> RewrapKeys(const vector<DuckLakeFileIdentity> &, const vector<string> &) override {
		return {};
	}
	string SelfTest() override {
		return root;
	}
	const string &GetLakeId() const override {
		return lake_id;
	}

private:
	string root;
	string lake_id;
};

DuckLakeEncryptionProvider::ProviderFactory StubFactory(const string &root) {
	return [root](DatabaseInstance &, const DuckLakeEncryptionProvider::Config &config) {
		return unique_ptr<DuckLakeEncryptionProvider>(new StubProvider(root, config.encryption_lake_id));
	};
}

DuckLakeEncryptionProvider::Factory LegacyStubFactory(const string &root) {
	return [root](DatabaseInstance &, const string &, const string &lake_id, idx_t) {
		return unique_ptr<DuckLakeEncryptionProvider>(new StubProvider(root, lake_id));
	};
}

//! The message `action` threw, or "" when it threw nothing. Asserting on the
//! MESSAGE, not just on "threw", is what tells the refusal under test from any
//! other throw on the same path.
string ThrownMessage(const std::function<void()> &action) {
	try {
		action();
	} catch (std::exception &ex) {
		ErrorData error(ex);
		return error.RawMessage();
	}
	return "";
}

DuckLakeEncryptionProvider::Config LakeConfig(const string &lake_id) {
	DuckLakeEncryptionProvider::Config config;
	config.encryption_socket = "/nonexistent/socket";
	config.encryption_lake_id = lake_id;
	config.cache_ttl_seconds = 60;
	return config;
}

} // namespace

// Runs in any process: whatever else ran first, opening a database loads
// DuckLake, which registers the test build's 'default' and 'test_kms_alt'.
TEST_CASE("DuckLake encryption provider registry - refusals and case-insensitive lookup", "[ducklake_kms_registry]") {
	DuckDB db(nullptr);
	auto &instance = *db.instance;
	REQUIRE(DuckLakeEncryptionProvider::HasProvider("default"));
	REQUIRE(DuckLakeEncryptionProvider::HasProvider("test_kms_alt"));

	SECTION("case-insensitive lookup") {
		REQUIRE(DuckLakeEncryptionProvider::HasProvider("TEST_KMS_ALT"));
		REQUIRE(DuckLakeEncryptionProvider::HasProvider("Default"));
		auto config = LakeConfig("lake-upper");
		config.properties["tag"] = "upper";
		auto provider = DuckLakeEncryptionProvider::Create(instance, "TEST_KMS_ALT", config);
		REQUIRE(provider->SelfTest() == "test-kms-alt:upper");
		// Names are reported lower-cased, the key they are stored under.
		auto names = DuckLakeEncryptionProvider::RegisteredProviderNames();
		REQUIRE(std::find(names.begin(), names.end(), "test_kms_alt") != names.end());
		REQUIRE(std::find(names.begin(), names.end(), "TEST_KMS_ALT") == names.end());
	}

	SECTION("a duplicate RegisterProvider is refused, including one differing only in case") {
		auto message = ThrownMessage(
		    [] { DuckLakeEncryptionProvider::RegisterProvider("test_kms_alt", StubFactory("impostor")); });
		REQUIRE_THAT(message, Catch::Contains("named 'test_kms_alt' is already registered"));
		message = ThrownMessage(
		    [] { DuckLakeEncryptionProvider::RegisterProvider("Test_KMS_Alt", StubFactory("impostor")); });
		REQUIRE_THAT(message, Catch::Contains("named 'Test_KMS_Alt' is already registered"));
		// The original survives the refused impostors.
		auto config = LakeConfig("lake-survivor");
		config.properties["tag"] = "survivor";
		REQUIRE(DuckLakeEncryptionProvider::Create(instance, "test_kms_alt", config)->SelfTest() ==
		        "test-kms-alt:survivor");
	}

	SECTION("a second legacy RegisterFactory is refused") {
		auto message =
		    ThrownMessage([] { DuckLakeEncryptionProvider::RegisterFactory(LegacyStubFactory("impostor")); });
		REQUIRE_THAT(message, Catch::Contains("named 'default' is already registered"));
	}

	SECTION("an empty name is refused at registration") {
		auto message = ThrownMessage([] { DuckLakeEncryptionProvider::RegisterProvider("", StubFactory("nameless")); });
		REQUIRE_THAT(message, Catch::Contains("must be registered under a non-empty name"));
		REQUIRE(!DuckLakeEncryptionProvider::HasProvider(""));
	}
}

// FRESH PROCESS ONLY (hidden). Two providers, neither named 'default': an
// ATTACH that names none must be refused, never resolved by registration
// order. load_extensions=false keeps DuckLake - and so the test build's own
// 'default' - out of the registry, which is the state a host embedding two
// providers of its own would be in.
TEST_CASE("DuckLake encryption provider registry - several providers, none named default",
          "[.][ducklake_kms_registry_no_default]") {
	REQUIRE(!DuckLakeEncryptionProvider::HasFactory());
	DBConfig config;
	config.options.load_extensions = false;
	DuckDB db(nullptr, &config);
	REQUIRE(!DuckLakeEncryptionProvider::HasFactory());

	DuckLakeEncryptionProvider::RegisterProvider("first_kms", StubFactory("first"));
	DuckLakeEncryptionProvider::RegisterProvider("second_kms", StubFactory("second"));

	auto message = ThrownMessage([&] { DuckLakeEncryptionProvider::Create(*db.instance, "", LakeConfig("lake")); });
	REQUIRE_THAT(message, Catch::Contains("has 2 DuckLake encryption providers registered ('first_kms', "
	                                      "'second_kms') and none is named 'default'"));
	// Naming one still works: the refusal is about the unnamed ATTACH only.
	REQUIRE(DuckLakeEncryptionProvider::Create(*db.instance, "second_kms", LakeConfig("lake"))->SelfTest() == "second");
}

// FRESH PROCESS ONLY (hidden). A host that registered 'default' BEFORE DuckLake
// loads must not make the load throw, and must keep its own registration.
TEST_CASE("DuckLake load tolerates a host-registered 'default'", "[.][ducklake_kms_registry_host_default]") {
	REQUIRE(!DuckLakeEncryptionProvider::HasFactory());
	DuckLakeEncryptionProvider::RegisterFactory(LegacyStubFactory("host-default"));

	auto message = ThrownMessage([] { DuckDB db(nullptr); });
	REQUIRE(message == "");

	DuckDB db(nullptr);
	REQUIRE(DuckLakeEncryptionProvider::Create(*db.instance, "", LakeConfig("lake"))->SelfTest() == "host-default");
	// DuckLake's other provider still registered beside the host's.
	REQUIRE(DuckLakeEncryptionProvider::HasProvider("test_kms_alt"));
}

// FRESH PROCESS ONLY (hidden). A host that took a name DuckLake registers
// SECOND: the first load must not throw, and - the defect this pins - a second
// load must not report a collision on 'default', a name DuckLake itself
// registered on the first attempt, in place of whatever really happened.
TEST_CASE("DuckLake load tolerates a host-registered second provider name", "[.][ducklake_kms_registry_host_alt]") {
	REQUIRE(!DuckLakeEncryptionProvider::HasFactory());
	DuckLakeEncryptionProvider::RegisterProvider("test_kms_alt", StubFactory("host-alt"));

	auto first = ThrownMessage([] { DuckDB db(nullptr); });
	auto second = ThrownMessage([] { DuckDB db(nullptr); });
	INFO("first load: " << first);
	INFO("second load: " << second);
	REQUIRE(first == "");
	REQUIRE(second == "");

	DuckDB db(nullptr);
	REQUIRE(DuckLakeEncryptionProvider::HasProvider("default"));
	REQUIRE(DuckLakeEncryptionProvider::Create(*db.instance, "test_kms_alt", LakeConfig("lake"))->SelfTest() ==
	        "host-alt");
}
