//===----------------------------------------------------------------------===//
//                         DuckLake (sigil fork)
//
// crypta/crypta_client.cpp
//
// PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.
//
//===----------------------------------------------------------------------===//

#include "crypta/crypta_client.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/string_type.hpp"

#ifndef _WIN32
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace duckdb {

static constexpr const char *WIRE_SCHEMA = "CryptaWireManifest@v2";
//! Matches crypta's MAX_FRAME. A response larger than this is a bug or an
//! attack, not a big batch.
static constexpr idx_t MAX_FRAME = 64ULL * 1024 * 1024;
//! Base64 of the crypta blob header. The blob starts with the 4 bytes "DLK1",
//! which base64-encodes to "RExL" followed by a character encoding the version
//! nibble - so only the first 4 characters are stable.
static constexpr const char *WRAPPED_PREFIX = "RExL";

CryptaClient::CryptaClient(string socket_path_p) : socket_path(std::move(socket_path_p)) {
	if (socket_path.empty()) {
		throw InvalidInputException("crypta socket path is empty");
	}
}

bool CryptaClient::LooksWrapped(const string &base64_value) {
	// The prefix ALONE is not enough, and the reason is a false positive on
	// random key material rather than anything about crypta.
	//
	// This is now called on every stored key of every plain-ENCRYPTED lake - the
	// upstream, no-crypta path - because that is where the unconfigured-reader
	// refusal lives. A 32-byte CSPRNG DEK whose first three bytes happen to be
	// 0x44 0x4C 0x4B ("DLK") base64-encodes to "RExL..." like a real blob does.
	// On a prefix-only test such a file would be refused forever as
	// crypta-wrapped, on a lake that has no crypta and never had any, with
	// advice to re-attach with options that do not apply. ~6e-8 per file: small,
	// not zero, and unrecoverable for whoever draws it.
	//
	// So require a length no plaintext key can reach. MAX_PLAINTEXT_KEY_BASE64 is
	// base64 of 32 bytes, the largest DEK this fork mints (94144c31); a real
	// wrapped blob runs 208-280 characters, so the two ranges do not overlap and
	// the floor costs nothing.
	static constexpr idx_t MAX_PLAINTEXT_KEY_BASE64 = 44;
	if (base64_value.size() <= MAX_PLAINTEXT_KEY_BASE64) {
		return false;
	}
	return StringUtil::StartsWith(base64_value, WRAPPED_PREFIX);
}

//! JSON string escaping. Only the two characters that can appear in a DuckLake
//! path and break a JSON string, plus control characters, need handling; base64
//! never does. Paths are attacker-influenced in the sense that a table name
//! reaches them, so this is not optional.
static string JsonEscape(const string &input) {
	string out;
	out.reserve(input.size() + 8);
	for (auto c : input) {
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
				out += StringUtil::Format("\\u%04x", static_cast<int>(static_cast<unsigned char>(c)));
			} else {
				out += c;
			}
		}
	}
	return out;
}

string CryptaClient::IdentityJson(const CryptaFileIdentity &identity) {
	return StringUtil::Format("{\"catalog_uuid\":\"%s\",\"table_id\":%lld,\"file_kind\":\"%s\",\"file_path\":\"%s\"}",
	                          JsonEscape(identity.lake_id), static_cast<long long>(identity.table_id),
	                          identity.is_delete_file ? "delete" : "data", JsonEscape(identity.stored_path));
}

void CryptaClient::ThrowIfError(const string &response) {
	if (response.find("\"status\":\"error\"") == string::npos) {
		return;
	}
	// Surface crypta's own message. It is deliberately coarse on the crypto side
	// - every binding failure collapses to one `unwrap_failed` there so a caller
	// cannot use this as a decryption oracle - so there is nothing to redact.
	string message = "unknown error";
	auto key = string("\"message\":\"");
	auto at = response.find(key);
	if (at != string::npos) {
		auto start = at + key.size();
		auto end = response.find('"', start);
		if (end != string::npos) {
			message = response.substr(start, end - start);
		}
	}
	throw IOException("crypta refused the request: %s", message);
}

vector<string> CryptaClient::ExtractBase64Field(const string &response, const string &field, idx_t expected) {
	// A deliberately narrow reader rather than a JSON parser.
	//
	// It is safe for exactly this input because the values it reads are base64,
	// whose alphabet (A-Za-z0-9+/=) contains neither a quote nor a backslash - so
	// there is no escape sequence to mishandle and no way for a value to end its
	// own string early. Everything else about the response is ignored.
	//
	// The strictness below is what makes it trustworthy: the count must match the
	// request exactly, and any surprise throws. Results are positional, matching
	// crypta's documented request-order guarantee; a misalignment would hand a
	// file the wrong DEK, and the Parquet reader would then fail to decrypt it.
	// That fails closed - it cannot silently return wrong data.
	vector<string> out;
	auto key = "\"" + field + "\":\"";
	idx_t at = 0;
	while (true) {
		auto found = response.find(key, at);
		if (found == string::npos) {
			break;
		}
		auto start = found + key.size();
		auto end = response.find('"', start);
		if (end == string::npos) {
			throw IOException("crypta response is truncated inside a %s value", field);
		}
		out.push_back(response.substr(start, end - start));
		at = end;
	}
	if (out.size() != expected) {
		throw IOException("crypta returned %llu %s values for %llu requested items", static_cast<uint64_t>(out.size()),
		                  field.c_str(), static_cast<uint64_t>(expected));
	}
	return out;
}

#ifdef _WIN32

string CryptaClient::Request(const string &json_body) {
	throw NotImplementedException("crypta uses a Unix domain socket and is not supported on Windows");
}

#else

//! Read exactly n bytes or fail. A short read is a protocol violation, not a
//! partial success.
static void ReadExact(int fd, char *buffer, idx_t n, const string &what) {
	idx_t got = 0;
	while (got < n) {
		auto rc = read(fd, buffer + got, n - got);
		if (rc == 0) {
			throw IOException("crypta closed the connection while reading %s", what);
		}
		if (rc < 0) {
			if (errno == EINTR) {
				continue;
			}
			throw IOException("crypta read failed while reading %s: %s", what, strerror(errno));
		}
		got += static_cast<idx_t>(rc);
	}
}

//! Make this socket's writes fail with EPIPE instead of KILLING the process,
//! and return the send() flags that finish the job.
//!
//! Writing to a socket whose peer has gone away returns EPIPE *and* raises
//! SIGPIPE. Under the default disposition SIGPIPE terminates the process before
//! send() returns, so the IOException in WriteAll below is unreachable: the
//! DuckDB CLI installs a handler and survives, a plain libduckdb embedding host
//! installs nothing and dies. A crypta that restarts mid-request would take the
//! whole host down rather than fail one query.
//!
//! Suppressed locally so no host cooperation is needed. Linux has MSG_NOSIGNAL
//! on the send; macOS and the BSDs have no MSG_NOSIGNAL and use SO_NOSIGPIPE on
//! the socket instead. Neither platform has both, and both arms are compiled in.
static int SuppressSigpipe(int fd) {
#ifdef SO_NOSIGPIPE
	int enabled = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#else
	(void)fd;
#endif
#ifdef MSG_NOSIGNAL
	return MSG_NOSIGNAL;
#else
	return 0;
#endif
}

static void WriteAll(int fd, const char *buffer, idx_t n, int send_flags) {
	idx_t sent = 0;
	while (sent < n) {
		auto rc = send(fd, buffer + sent, n - sent, send_flags);
		if (rc < 0) {
			if (errno == EINTR) {
				continue;
			}
			throw IOException("crypta write failed: %s", strerror(errno));
		}
		sent += static_cast<idx_t>(rc);
	}
}

//! RAII for the socket fd. An exception between connect and close would
//! otherwise leak a descriptor on every failed scan.
struct SocketHandle {
	explicit SocketHandle(int fd_p) : fd(fd_p) {
	}
	~SocketHandle() {
		if (fd >= 0) {
			close(fd);
		}
	}
	SocketHandle(const SocketHandle &) = delete;
	SocketHandle &operator=(const SocketHandle &) = delete;
	int fd;
};

string CryptaClient::Request(const string &json_body) {
	struct sockaddr_un addr;
	// sun_path is a fixed-size array; an over-long path would be silently
	// truncated and connect to the wrong place, so refuse it instead.
	if (socket_path.size() >= sizeof(addr.sun_path)) {
		throw InvalidInputException("crypta socket path is too long (max %llu bytes)",
		                            static_cast<uint64_t>(sizeof(addr.sun_path) - 1));
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, socket_path.c_str(), socket_path.size());

	SocketHandle handle(socket(AF_UNIX, SOCK_STREAM, 0));
	if (handle.fd < 0) {
		throw IOException("could not create a socket for crypta: %s", strerror(errno));
	}
	auto send_flags = SuppressSigpipe(handle.fd);
	if (connect(handle.fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
		// The message names the socket on purpose. This is the error an operator
		// sees when the key service is down, and "no such file" without the path
		// is a support ticket.
		throw IOException("cannot reach the crypta key service at %s: %s - an encrypted DuckLake cannot be read "
		                  "without it",
		                  socket_path, strerror(errno));
	}

	if (json_body.size() > MAX_FRAME) {
		throw IOException("crypta request of %llu bytes exceeds the frame limit",
		                  static_cast<uint64_t>(json_body.size()));
	}
	uint32_t length = static_cast<uint32_t>(json_body.size());
	char header[4] = {static_cast<char>((length >> 24) & 0xFF), static_cast<char>((length >> 16) & 0xFF),
	                  static_cast<char>((length >> 8) & 0xFF), static_cast<char>(length & 0xFF)};
	WriteAll(handle.fd, header, 4, send_flags);
	WriteAll(handle.fd, json_body.c_str(), json_body.size(), send_flags);

	char response_header[4];
	ReadExact(handle.fd, response_header, 4, "the response length");
	uint32_t response_length = (static_cast<uint32_t>(static_cast<unsigned char>(response_header[0])) << 24) |
	                           (static_cast<uint32_t>(static_cast<unsigned char>(response_header[1])) << 16) |
	                           (static_cast<uint32_t>(static_cast<unsigned char>(response_header[2])) << 8) |
	                           static_cast<uint32_t>(static_cast<unsigned char>(response_header[3]));
	if (response_length == 0 || response_length > MAX_FRAME) {
		throw IOException("crypta announced a %u byte response, outside 1..%llu", response_length,
		                  static_cast<uint64_t>(MAX_FRAME));
	}
	string response;
	response.resize(response_length);
	ReadExact(handle.fd, &response[0], response_length, "the response body");
	return response;
}

#endif

vector<string> CryptaClient::WrapBatch(const vector<CryptaFileIdentity> &identities, const vector<string> &deks) {
	if (identities.size() != deks.size()) {
		throw InternalException("crypta WrapBatch: %llu identities for %llu keys",
		                        static_cast<uint64_t>(identities.size()), static_cast<uint64_t>(deks.size()));
	}
	if (identities.empty()) {
		return {};
	}
	string body = StringUtil::Format("{\"schema\":\"%s\",\"op\":\"wrap_batch\",\"items\":[", WIRE_SCHEMA);
	for (idx_t i = 0; i < identities.size(); i++) {
		if (i > 0) {
			body += ",";
		}
		body += StringUtil::Format("{\"identity\":%s,\"dek\":\"%s\"}", IdentityJson(identities[i]),
		                           Blob::ToBase64(string_t(deks[i])));
	}
	body += "]}";

	auto response = Request(body);
	ThrowIfError(response);
	return ExtractBase64Field(response, "wrapped", identities.size());
}

vector<string> CryptaClient::UnwrapBatch(const vector<CryptaFileIdentity> &identities, const vector<string> &blobs) {
	if (identities.size() != blobs.size()) {
		throw InternalException("crypta UnwrapBatch: %llu identities for %llu blobs",
		                        static_cast<uint64_t>(identities.size()), static_cast<uint64_t>(blobs.size()));
	}
	if (identities.empty()) {
		return {};
	}
	string body = StringUtil::Format("{\"schema\":\"%s\",\"op\":\"unwrap_batch\",\"items\":[", WIRE_SCHEMA);
	for (idx_t i = 0; i < identities.size(); i++) {
		if (i > 0) {
			body += ",";
		}
		body += StringUtil::Format("{\"identity\":%s,\"wrapped\":\"%s\"}", IdentityJson(identities[i]), blobs[i]);
	}
	body += "]}";

	auto response = Request(body);
	ThrowIfError(response);
	auto encoded = ExtractBase64Field(response, "dek", identities.size());

	vector<string> keys;
	keys.reserve(encoded.size());
	for (auto &value : encoded) {
		keys.push_back(Blob::FromBase64(string_t(value)));
	}
	return keys;
}

string CryptaClient::Health() {
	auto response = Request(StringUtil::Format("{\"schema\":\"%s\",\"op\":\"health\"}", WIRE_SCHEMA));
	ThrowIfError(response);
	return response;
}

} // namespace duckdb
