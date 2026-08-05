//===----------------------------------------------------------------------===//
//                         DuckLake (sigil fork)
//
// test/cpp/crypta/fake_crypta_server.cpp
//
// PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.
//===----------------------------------------------------------------------===//

#include "fake_crypta_server.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace ducklake_crypta_test {

static void WriteAllOrThrow(int fd, const char *data, size_t n) {
	size_t sent = 0;
	while (sent < n) {
		auto rc = ::write(fd, data + sent, n - sent);
		if (rc < 0) {
			if (errno == EINTR) {
				continue;
			}
			throw std::runtime_error(std::string("fake crypta write failed: ") + strerror(errno));
		}
		sent += static_cast<size_t>(rc);
	}
}

FakeConnection::FakeConnection(int fd_p) : fd(fd_p) {
}

FakeConnection::~FakeConnection() {
	if (fd >= 0) {
		::close(fd);
		fd = -1;
	}
}

std::string FakeConnection::ReadFrame() {
	unsigned char header[4];
	size_t got = 0;
	while (got < 4) {
		auto rc = ::read(fd, header + got, 4 - got);
		if (rc == 0) {
			throw std::runtime_error("fake crypta: peer closed before sending a length header");
		}
		if (rc < 0) {
			if (errno == EINTR) {
				continue;
			}
			throw std::runtime_error(std::string("fake crypta read failed: ") + strerror(errno));
		}
		got += static_cast<size_t>(rc);
	}
	uint32_t length = (static_cast<uint32_t>(header[0]) << 24) | (static_cast<uint32_t>(header[1]) << 16) |
	                  (static_cast<uint32_t>(header[2]) << 8) | static_cast<uint32_t>(header[3]);
	std::string body;
	body.resize(length);
	got = 0;
	while (got < length) {
		auto rc = ::read(fd, &body[0] + got, length - got);
		if (rc == 0) {
			throw std::runtime_error("fake crypta: peer closed mid-body");
		}
		if (rc < 0) {
			if (errno == EINTR) {
				continue;
			}
			throw std::runtime_error(std::string("fake crypta read failed: ") + strerror(errno));
		}
		got += static_cast<size_t>(rc);
	}
	return body;
}

void FakeConnection::WriteLengthHeader(uint32_t length) {
	char header[4] = {static_cast<char>((length >> 24) & 0xFF), static_cast<char>((length >> 16) & 0xFF),
	                  static_cast<char>((length >> 8) & 0xFF), static_cast<char>(length & 0xFF)};
	WriteAllOrThrow(fd, header, 4);
}

void FakeConnection::WriteFrame(const std::string &body) {
	WriteLengthHeader(static_cast<uint32_t>(body.size()));
	WriteAllOrThrow(fd, body.data(), body.size());
}

void FakeConnection::WriteRaw(const std::string &bytes) {
	WriteAllOrThrow(fd, bytes.data(), bytes.size());
}

void FakeConnection::Close() {
	if (fd >= 0) {
		::close(fd);
		fd = -1;
	}
}

void FakeConnection::CloseWithoutReading() {
	// Deliberately no read() first, and deliberately no SO_LINGER: see the header
	// for why linger is a TCP notion that buys nothing on AF_UNIX, and why the
	// unread receive queue is what actually sets ECONNRESET on the peer.
	if (fd >= 0) {
		::close(fd);
		fd = -1;
	}
}

FakeCryptaServer::FakeCryptaServer() {
	char tmpl[] = "/tmp/ducklake_crypta_test_XXXXXX";
	auto dir = mkdtemp(tmpl);
	if (!dir) {
		throw std::runtime_error(std::string("mkdtemp failed: ") + strerror(errno));
	}
	temp_dir = dir;
	socket_path = temp_dir + "/s";

	listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		throw std::runtime_error(std::string("socket() failed: ") + strerror(errno));
	}
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, socket_path.c_str(), socket_path.size());
	if (::bind(listen_fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
		throw std::runtime_error(std::string("bind() failed: ") + strerror(errno));
	}
	if (::listen(listen_fd, 64) < 0) {
		throw std::runtime_error(std::string("listen() failed: ") + strerror(errno));
	}
}

FakeCryptaServer::~FakeCryptaServer() {
	Stop();
	if (listen_fd >= 0) {
		::close(listen_fd);
		listen_fd = -1;
	}
	::unlink(socket_path.c_str());
	::rmdir(temp_dir.c_str());
}

void FakeCryptaServer::Start(Handler handler_p) {
	handler = std::move(handler_p);
	thread = std::thread(&FakeCryptaServer::AcceptLoop, this);
}

void FakeCryptaServer::Stop() {
	if (!thread.joinable()) {
		return;
	}
	stop_flag.store(true);
	thread.join();
}

void FakeCryptaServer::AcceptLoop() {
	// The accept is polled rather than blocking so Stop() does not need a
	// self-connect to unwedge it. 20 ms is a local, in-process test loop - the
	// >= 3600 s poller floor guards shared services, and there is none here.
	while (!stop_flag.load()) {
		struct pollfd pfd;
		pfd.fd = listen_fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		auto rc = ::poll(&pfd, 1, 20);
		if (rc <= 0) {
			continue;
		}
		auto fd = ::accept(listen_fd, nullptr, nullptr);
		if (fd < 0) {
			continue;
		}
		auto index = connections.fetch_add(1);
		FakeConnection connection(fd);
		try {
			handler(connection, index);
		} catch (const std::exception &e) {
			std::lock_guard<std::mutex> guard(lock);
			if (handler_error.empty()) {
				handler_error = e.what();
			}
		}
	}
}

std::vector<std::string> FakeCryptaServer::Requests() const {
	std::lock_guard<std::mutex> guard(lock);
	return requests;
}

void FakeCryptaServer::Record(const std::string &request) {
	std::lock_guard<std::mutex> guard(lock);
	requests.push_back(request);
}

std::string FakeCryptaServer::HandlerError() const {
	std::lock_guard<std::mutex> guard(lock);
	return handler_error;
}

std::string Base64Encode(const std::string &raw) {
	static const char *ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	size_t i = 0;
	while (i + 2 < raw.size()) {
		uint32_t chunk = (static_cast<unsigned char>(raw[i]) << 16) | (static_cast<unsigned char>(raw[i + 1]) << 8) |
		                 static_cast<unsigned char>(raw[i + 2]);
		out += ALPHABET[(chunk >> 18) & 0x3F];
		out += ALPHABET[(chunk >> 12) & 0x3F];
		out += ALPHABET[(chunk >> 6) & 0x3F];
		out += ALPHABET[chunk & 0x3F];
		i += 3;
	}
	auto remaining = raw.size() - i;
	if (remaining == 1) {
		uint32_t chunk = static_cast<unsigned char>(raw[i]) << 16;
		out += ALPHABET[(chunk >> 18) & 0x3F];
		out += ALPHABET[(chunk >> 12) & 0x3F];
		out += "==";
	} else if (remaining == 2) {
		uint32_t chunk = (static_cast<unsigned char>(raw[i]) << 16) | (static_cast<unsigned char>(raw[i + 1]) << 8);
		out += ALPHABET[(chunk >> 18) & 0x3F];
		out += ALPHABET[(chunk >> 12) & 0x3F];
		out += ALPHABET[(chunk >> 6) & 0x3F];
		out += "=";
	}
	return out;
}

std::string OkUnwrapResponse(const std::vector<std::string> &raw_deks) {
	std::ostringstream out;
	out << "{\"schema\":\"CryptaWireManifest@v2\",\"status\":\"ok\",\"items\":[";
	for (size_t i = 0; i < raw_deks.size(); i++) {
		if (i > 0) {
			out << ",";
		}
		out << "{\"dek\":\"" << Base64Encode(raw_deks[i]) << "\"}";
	}
	out << "]}";
	return out.str();
}

std::string OkWrapResponse(const std::vector<std::string> &base64_blobs) {
	std::ostringstream out;
	out << "{\"schema\":\"CryptaWireManifest@v2\",\"status\":\"ok\",\"items\":[";
	for (size_t i = 0; i < base64_blobs.size(); i++) {
		if (i > 0) {
			out << ",";
		}
		out << "{\"wrapped\":\"" << base64_blobs[i] << "\"}";
	}
	out << "]}";
	return out.str();
}

std::string ErrorResponse(const std::string &message) {
	return "{\"schema\":\"CryptaWireManifest@v2\",\"status\":\"error\",\"message\":\"" + message + "\"}";
}

bool DecodeJsonStringField(const std::string &json, const std::string &field, std::string &out) {
	auto key = "\"" + field + "\":\"";
	auto at = json.find(key);
	if (at == std::string::npos) {
		return false;
	}
	size_t i = at + key.size();
	std::string decoded;
	while (i < json.size()) {
		char c = json[i];
		if (c == '"') {
			out = decoded;
			return true;
		}
		if (c != '\\') {
			decoded += c;
			i++;
			continue;
		}
		if (i + 1 >= json.size()) {
			return false;
		}
		char esc = json[i + 1];
		switch (esc) {
		case '"':
			decoded += '"';
			i += 2;
			break;
		case '\\':
			decoded += '\\';
			i += 2;
			break;
		case '/':
			decoded += '/';
			i += 2;
			break;
		case 'b':
			decoded += '\b';
			i += 2;
			break;
		case 'f':
			decoded += '\f';
			i += 2;
			break;
		case 'n':
			decoded += '\n';
			i += 2;
			break;
		case 'r':
			decoded += '\r';
			i += 2;
			break;
		case 't':
			decoded += '\t';
			i += 2;
			break;
		case 'u': {
			if (i + 5 >= json.size()) {
				return false;
			}
			auto hex = json.substr(i + 2, 4);
			for (auto h : hex) {
				if (!isxdigit(static_cast<unsigned char>(h))) {
					return false;
				}
			}
			auto code = std::stoul(hex, nullptr, 16);
			// Only the control range is expected here; anything wider would mean
			// the encoder escaped something it was not asked to.
			if (code > 0xFF) {
				return false;
			}
			decoded += static_cast<char>(code);
			i += 6;
			break;
		}
		default:
			return false;
		}
	}
	return false;
}

} // namespace ducklake_crypta_test
