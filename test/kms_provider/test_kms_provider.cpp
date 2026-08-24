//===----------------------------------------------------------------------===//
//                    DuckLake TEST-ONLY KMS envelope provider
//
// test/kms_provider/test_kms_provider.cpp
//
// PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.
//
// WHY THIS EXISTS
// ---------------
// The envelope's only concrete provider is out of tree and PRIVATE, so this
// repository's CI cannot build one. That is why `run_envelope_e2e.sh` and
// `run_sql_crypta_tests.sh` were never invoked by any workflow, why their
// require-env fixtures SKIPPED, and why a skip - which exits ZERO - read as a
// pass for months. That is issue #52.
//
// This file removes the excuse. It is a real DuckLakeEncryptionProvider that
// speaks CryptaWireManifest@v3 over a Unix socket, so it can be driven by
// test/sql/encryption/fake_kms.py, which is in this tree. It performs NO
// cryptography and is NOT a KMS: the wrapping is done by whatever is behind the
// socket. Its whole job is to make the ATTACH-time factory non-null so that
// every DuckLake-side envelope branch - WrapKeys on commit, UnwrapKey on read,
// the unenveloped-reader refusal - is REACHED and can be asserted on.
//
// IT IS NEVER IN A RELEASE BUILD. It lives under test/, and the only thing that
// ever copies it to src/crypta-provider/ (the pickup hook) is the Envelope
// workflow. Nothing in the shipped build system references this directory.
//
//===----------------------------------------------------------------------===//

#include "storage/ducklake_encryption_provider.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/helper.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace duckdb {

namespace {

constexpr const char *WIRE_SCHEMA = "CryptaWireManifest@v3";

//! JSON string escaping, restricted to what a lake id and a stored path can
//! actually contain. A path with a quote or a backslash in it must not be able
//! to break the frame into two identities.
string JsonEscape(const string &value) {
	string out;
	for (auto c : value) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				char buffer[7];
				snprintf(buffer, sizeof(buffer), "\\u%04x", c);
				out += buffer;
			} else {
				out += c;
			}
		}
	}
	return out;
}

string JsonUnescape(const string &value) {
	string out;
	for (idx_t i = 0; i < value.size(); i++) {
		if (value[i] != '\\' || i + 1 >= value.size()) {
			out += value[i];
			continue;
		}
		i++;
		switch (value[i]) {
		case 'n':
			out += '\n';
			break;
		case 'r':
			out += '\r';
			break;
		case 't':
			out += '\t';
			break;
		case 'u': {
			if (i + 4 >= value.size()) {
				throw IOException("test kms provider: truncated \\u escape in reply");
			}
			auto code = std::stoi(value.substr(i + 1, 4), nullptr, 16);
			out += static_cast<char>(code);
			i += 4;
			break;
		}
		default:
			out += value[i];
		}
	}
	return out;
}

//! Every string value stored under `key`, in the order they appear.
//!
//! Deliberately positional rather than a real parser: the reply's items come
//! back in request order, and pairing by position is what lets the caller check
//! that the echoed identity in slot i is the identity it asked about in slot i.
vector<string> ExtractStrings(const string &body, const string &key) {
	vector<string> values;
	string needle = "\"" + key + "\":\"";
	idx_t position = 0;
	while (true) {
		auto found = body.find(needle, position);
		if (found == string::npos) {
			return values;
		}
		auto start = found + needle.size();
		auto cursor = start;
		while (cursor < body.size()) {
			if (body[cursor] == '\\') {
				cursor += 2;
				continue;
			}
			if (body[cursor] == '"') {
				break;
			}
			cursor++;
		}
		if (cursor >= body.size()) {
			throw IOException("test kms provider: unterminated string for key '%s' in reply", key);
		}
		values.push_back(JsonUnescape(body.substr(start, cursor - start)));
		position = cursor + 1;
	}
}

class TestKmsProvider : public DuckLakeEncryptionProvider {
public:
	TestKmsProvider(string socket_path_p, string lake_id_p, int64_t ttl_seconds_p)
	    : socket_path(std::move(socket_path_p)), lake_id(std::move(lake_id_p)), ttl_seconds(ttl_seconds_p) {
		if (socket_path.empty()) {
			throw InvalidInputException("test kms provider: ENCRYPTION_SOCKET is empty");
		}
		// sun_path is a fixed 108-byte field; a longer path would be silently
		// TRUNCATED by the copy below and connect to a different socket.
		if (socket_path.size() >= sizeof(sockaddr_un().sun_path)) {
			throw InvalidInputException("test kms provider: socket path is %llu bytes, the maximum is %llu",
			                            static_cast<uint64_t>(socket_path.size()),
			                            static_cast<uint64_t>(sizeof(sockaddr_un().sun_path) - 1));
		}
		if (lake_id.empty()) {
			throw InvalidInputException("test kms provider: ENCRYPTION_LAKE_ID is empty - every key is scoped "
			                            "by it, so an empty one would bind keys to nothing");
		}
		// This provider caches nothing, so the TTL bounds nothing here. It is
		// still REFUSED when out of range rather than ignored: a fixture that
		// set an out-of-range TTL and saw it accepted would be learning
		// something false about the option.
		int64_t ttl_ceiling = MAX_CACHE_TTL_SECONDS;
		if (ttl_seconds <= 0 || ttl_seconds > ttl_ceiling) {
			throw InvalidInputException("test kms provider: encryption_cache_ttl_seconds is %s, which is "
			                            "outside 1..%s",
			                            std::to_string(ttl_seconds), std::to_string(ttl_ceiling));
		}
	}

	vector<string> WrapKeys(const vector<DuckLakeFileIdentity> &identities, const vector<string> &deks) override {
		if (identities.size() != deks.size()) {
			throw IOException("test kms provider: %llu identities for %llu keys",
			                  static_cast<uint64_t>(identities.size()), static_cast<uint64_t>(deks.size()));
		}
		string request = "{\"items\":[";
		for (idx_t i = 0; i < identities.size(); i++) {
			if (i > 0) {
				request += ",";
			}
			request += "{\"dek\":\"" + Blob::ToBase64(string_t(deks[i])) + "\",\"identity\":\"" +
			           JsonEscape(IdentityString(identities[i])) + "\"}";
		}
		request += "],\"op\":\"wrap_batch\",\"schema\":\"";
		request += WIRE_SCHEMA;
		request += "\"}";

		auto reply = Roundtrip(request);
		auto wrapped = ExtractStrings(reply, "wrapped");
		CheckEchoedIdentities(reply, identities, "wrap_batch");
		if (wrapped.size() != identities.size()) {
			throw IOException("test kms provider: wrap_batch returned %llu blobs for %llu keys",
			                  static_cast<uint64_t>(wrapped.size()), static_cast<uint64_t>(identities.size()));
		}
		for (auto &blob : wrapped) {
			// The service is what makes a blob a blob. If it handed back
			// something that would read as a plaintext key, every downstream
			// refusal that keys on the magic would fail OPEN.
			if (!LooksWrapped(blob)) {
				throw IOException("test kms provider: wrap_batch returned a blob that does not carry the "
				                  "wrapped-key magic - it would be read back as a plaintext key");
			}
		}
		return wrapped;
	}

	string UnwrapKey(const DuckLakeFileIdentity &identity, const string &base64_value) override {
		// Refusing a plaintext row on an enveloped lake is REQUIRED of a
		// provider by the abstract interface: reading one would let a
		// downgraded catalog serve its data as if the envelope were intact.
		if (!LooksWrapped(base64_value)) {
			throw IOException("test kms provider: refusing a stored key that is not an envelope blob - on an "
			                  "enveloped lake that is a pre-envelope leftover or a downgrade");
		}
		string request = "{\"items\":[{\"identity\":\"";
		request += JsonEscape(IdentityString(identity));
		request += "\",\"wrapped\":\"" + JsonEscape(base64_value) + "\"}],\"op\":\"unwrap_batch\",\"schema\":\"";
		request += WIRE_SCHEMA;
		request += "\"}";

		auto reply = Roundtrip(request);
		auto deks = ExtractStrings(reply, "dek");
		if (deks.size() != 1) {
			throw IOException("test kms provider: unwrap_batch returned %llu keys for 1 file",
			                  static_cast<uint64_t>(deks.size()));
		}
		vector<DuckLakeFileIdentity> one {identity};
		CheckEchoedIdentities(reply, one, "unwrap_batch");
		return Blob::FromBase64(string_t(deks[0]));
	}

	vector<DuckLakeRewrapResult> RewrapKeys(const vector<DuckLakeFileIdentity> &identities,
	                                        const vector<string> &blobs) override {
		if (identities.size() != blobs.size()) {
			throw IOException("test kms provider: %llu identities for %llu blobs",
			                  static_cast<uint64_t>(identities.size()), static_cast<uint64_t>(blobs.size()));
		}
		// The fake service has one key and no rotation, so a rewrap here can
		// only ever be a no-op. It reports `rewrapped = false` rather than
		// inventing movement: a sweep that claimed to have rotated a row it did
		// not touch is exactly the false green this fork exists to remove.
		vector<DuckLakeRewrapResult> results;
		for (idx_t i = 0; i < blobs.size(); i++) {
			if (!LooksWrapped(blobs[i])) {
				throw IOException("test kms provider: refusing to rewrap a stored key that is not an envelope blob");
			}
			// Prove the blob is genuinely ours and genuinely bound to this
			// identity before reporting it converged.
			UnwrapKey(identities[i], blobs[i]);
			DuckLakeRewrapResult result;
			result.wrapped = blobs[i];
			result.rewrapped = false;
			results.push_back(result);
		}
		return results;
	}

	string SelfTest() override {
		string request = "{\"op\":\"health\",\"schema\":\"";
		request += WIRE_SCHEMA;
		request += "\"}";
		auto reply = Roundtrip(request);
		if (reply.find("\"ok\":true") == string::npos) {
			throw IOException("test kms provider: the key service at %s did not report healthy: %s", socket_path,
			                  reply);
		}
		auto roots = ExtractStrings(reply, "root");
		return roots.empty() ? string("test-kms") : roots[0];
	}

	const string &LakeId() const override {
		return lake_id;
	}

private:
	//! Injective by construction: the three fixed-shape fields come first and
	//! the only free-form one - the path - comes LAST, so no two distinct
	//! identities can render to the same string.
	string IdentityString(const DuckLakeFileIdentity &identity) const {
		return lake_id + "\x1f" + std::to_string(identity.table_id) + "\x1f" +
		       (identity.is_delete_file ? "delete" : "data") + "\x1f" + identity.stored_path;
	}

	//! The reply ECHOES the identity it answers for. Comparing it is what makes
	//! a substitution - the right blob returned for the wrong file - a failure
	//! rather than a silent key mix-up.
	void CheckEchoedIdentities(const string &reply, const vector<DuckLakeFileIdentity> &identities,
	                           const char *op) const {
		auto echoed = ExtractStrings(reply, "identity");
		if (echoed.size() != identities.size()) {
			throw IOException("test kms provider: %s echoed %llu identities for %llu files", op,
			                  static_cast<uint64_t>(echoed.size()), static_cast<uint64_t>(identities.size()));
		}
		for (idx_t i = 0; i < identities.size(); i++) {
			if (echoed[i] != IdentityString(identities[i])) {
				throw IOException("test kms provider: %s answered for '%s' where '%s' was asked", op, echoed[i],
				                  IdentityString(identities[i]));
			}
		}
	}

	//! One request, one reply, one connection. Short-lived connections keep the
	//! provider free of any cross-request state that a stale socket could
	//! corrupt; this is a test provider, not a performance one.
	string Roundtrip(const string &request) {
		std::lock_guard<std::mutex> lock(wire_lock);
		auto fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0) {
			throw IOException("test kms provider: cannot create a unix socket: %s", string(strerror(errno)));
		}
		sockaddr_un address;
		memset(&address, 0, sizeof(address));
		address.sun_family = AF_UNIX;
		memcpy(address.sun_path, socket_path.c_str(), socket_path.size());
		if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
			auto reason = string(strerror(errno));
			::close(fd);
			throw IOException("test kms provider: cannot reach the key service at %s: %s", socket_path, reason);
		}

		string frame;
		uint32_t length = static_cast<uint32_t>(request.size());
		frame += static_cast<char>((length >> 24) & 0xFF);
		frame += static_cast<char>((length >> 16) & 0xFF);
		frame += static_cast<char>((length >> 8) & 0xFF);
		frame += static_cast<char>(length & 0xFF);
		frame += request;

		idx_t sent = 0;
		while (sent < frame.size()) {
			auto written = ::write(fd, frame.data() + sent, frame.size() - sent);
			if (written <= 0) {
				::close(fd);
				throw IOException("test kms provider: short write to the key service at %s", socket_path);
			}
			sent += static_cast<idx_t>(written);
		}

		string header;
		if (!ReadExact(fd, 4, header)) {
			::close(fd);
			throw IOException("test kms provider: the key service at %s closed before replying", socket_path);
		}
		uint32_t reply_length = (static_cast<uint32_t>(static_cast<unsigned char>(header[0])) << 24) |
		                        (static_cast<uint32_t>(static_cast<unsigned char>(header[1])) << 16) |
		                        (static_cast<uint32_t>(static_cast<unsigned char>(header[2])) << 8) |
		                        static_cast<uint32_t>(static_cast<unsigned char>(header[3]));
		string body;
		if (!ReadExact(fd, reply_length, body)) {
			::close(fd);
			throw IOException("test kms provider: truncated reply from the key service at %s", socket_path);
		}
		::close(fd);

		if (body.find("\"schema\":\"" + string(WIRE_SCHEMA) + "\"") == string::npos) {
			throw IOException("test kms provider: the key service does not speak %s: %s", string(WIRE_SCHEMA), body);
		}
		if (body.find("\"status\":\"ok\"") == string::npos) {
			throw IOException("test kms provider: the key service refused the request: %s", body);
		}
		return body;
	}

	static bool ReadExact(int fd, idx_t count, string &out) {
		out.clear();
		out.reserve(count);
		char buffer[4096];
		while (out.size() < count) {
			auto want = MinValue<idx_t>(sizeof(buffer), count - out.size());
			auto got = ::read(fd, buffer, want);
			if (got <= 0) {
				return false;
			}
			out.append(buffer, static_cast<idx_t>(got));
		}
		return true;
	}

	string socket_path;
	string lake_id;
	int64_t ttl_seconds;
	std::mutex wire_lock;
};

} // namespace

//! Declared by ducklake_extension.cpp under DUCKLAKE_KMS_PROVIDER and called
//! from its Load(). A file-scope static initializer would NOT do: in the
//! `duckdb` binary DuckLake is linked as libducklake_extension.a, and an
//! archive member no symbol references is dropped, so the provider would
//! register nothing while the build reported success. This external definition
//! is the reference that obliges the linker to pull the member in.
void DuckLakeRegisterKmsProvider() {
	DuckLakeEncryptionProvider::RegisterFactory(
	    [](string socket, string lake_id, int64_t ttl) -> unique_ptr<DuckLakeEncryptionProvider> {
		    return make_uniq<TestKmsProvider>(std::move(socket), std::move(lake_id), ttl);
	    });
}

} // namespace duckdb
