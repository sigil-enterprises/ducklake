//===----------------------------------------------------------------------===//
//                         DuckLake (sigil fork)
//
// test/cpp/crypta/fake_crypta_server.hpp
//
// A scripted Unix-domain-socket server that speaks - and deliberately
// mis-speaks - CryptaWireManifest@v2.
//
// PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.
//
// Why this exists: a healthy crypta never emits a truncated frame, never
// announces a 64 MB response, and never closes mid-body. Every refusal path in
// `CryptaClient` is therefore unreachable from `scripts/mvp_crypta_proof.sh` or
// from anything under `test/sql/`, because both run against a service that
// behaves. The only way to observe those branches is to put a server on the
// other end of the socket that misbehaves on purpose.
//
// The handler is handed the raw accepted connection rather than a response
// string, because several cases have to fail BEFORE a well-formed response
// exists: a hard reset before the request is drained, a length header with no
// body behind it, a body that stops halfway.
//===----------------------------------------------------------------------===//

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ducklake_crypta_test {

//! One accepted connection. Owns the fd and closes it on destruction unless the
//! handler already disposed of it.
class FakeConnection {
public:
	explicit FakeConnection(int fd_p);
	~FakeConnection();
	FakeConnection(const FakeConnection &) = delete;
	FakeConnection &operator=(const FakeConnection &) = delete;

	int Fd() const {
		return fd;
	}

	//! Read one 4-byte big-endian length header plus that many bytes. Throws
	//! std::runtime_error if the peer goes away first.
	std::string ReadFrame();
	//! Write a well-formed frame: 4-byte big-endian length, then the body.
	void WriteFrame(const std::string &body);
	//! Write exactly these bytes, whatever they are.
	void WriteRaw(const std::string &bytes);
	//! Write a 4-byte length header announcing `length`, and nothing else.
	void WriteLengthHeader(uint32_t length);
	//! Graceful close after the request has been drained - the peer sees EOF and
	//! read() returns 0.
	void Close();
	//! Close while the peer's request is still sitting UNREAD in this socket's
	//! receive queue.
	//!
	//! This is the only way to make read() fail with an error rather than report
	//! EOF on a Unix domain socket, and it is worth being precise about why:
	//! AF_UNIX has no RST, so SO_LINGER does nothing here. Linux's
	//! unix_release_sock() sets the PEER's sk_err to ECONNRESET only when the
	//! closing socket's receive queue is non-empty. Drain the request first and
	//! the peer gets a clean EOF (the rc == 0 branch); leave it unread and the
	//! peer gets ECONNRESET (the rc < 0 branch). The two produce different
	//! messages, and only this call reaches the second.
	//!
	//! Used from both sides of the client's loop: called AFTER the request has
	//! landed it reddens the read path; called IMMEDIATELY on accept it reddens
	//! the write path, because the peer's next write to a closed socket is EPIPE.
	void CloseWithoutReading();

private:
	int fd;
};

//! A Unix socket in a private temp directory, with a caller-supplied handler
//! run once per accepted connection on the server thread.
class FakeCryptaServer {
public:
	using Handler = std::function<void(FakeConnection &, int connection_index)>;

	//! Binds and listens immediately, so Path() is usable before Start(). The
	//! two-phase construction is what lets a handler capture the server itself.
	FakeCryptaServer();
	~FakeCryptaServer();
	FakeCryptaServer(const FakeCryptaServer &) = delete;
	FakeCryptaServer &operator=(const FakeCryptaServer &) = delete;

	void Start(Handler handler);
	//! Stop accepting and join the server thread. Idempotent.
	void Stop();

	const std::string &Path() const {
		return socket_path;
	}
	//! How many connections were accepted. The load-bearing assertion for every
	//! cache test: a cache HIT is exactly "the client did not connect again".
	int Connections() const {
		return connections.load();
	}
	//! Request bodies recorded by handlers that chose to record them.
	std::vector<std::string> Requests() const;
	void Record(const std::string &request);
	//! Whatever the handler threw, if anything. A handler that dies silently
	//! would otherwise turn into a confusing client-side timeout.
	std::string HandlerError() const;

private:
	void AcceptLoop();

	std::string temp_dir;
	std::string socket_path;
	int listen_fd = -1;
	std::thread thread;
	std::atomic<bool> stop_flag {false};
	std::atomic<int> connections {0};
	Handler handler;
	mutable std::mutex lock;
	std::vector<std::string> requests;
	std::string handler_error;
};

//! Base64, written here rather than reused from DuckDB on purpose: these tests
//! assert on the exact bytes DuckDB's own encoder produced on the wire, so
//! encoding the expectation with that same encoder would make a bug in it
//! invisible.
std::string Base64Encode(const std::string &raw);

//! The RAW `identity` value of every top-level item in a REQUEST frame, in
//! order - the JSON object exactly as the client wrote it.
//!
//! Exposed rather than kept private because REORDERING or REPLACING one of these
//! before handing it back is the only way to exercise the reply-side binding
//! (#31), and a test that built the reply's identities from scratch would be
//! testing its own spelling rather than the client's.
//!
//! An item with no `identity` member contributes the empty string, which is the
//! shape that reddens the binding rather than crashing the fake.
std::vector<std::string> RequestIdentities(const std::string &request);

//! A well-formed unwrap_batch response: one item per DEK, each carrying the
//! identity slice at the same position beside it.
//!
//! When there are fewer identities than values the identities CYCLE, and that is
//! for the count cases alone - a reply with more items than the request had is
//! refused on the count before any identity is looked at, so what the surplus
//! items echo is not what those cases are measuring.
std::string UnwrapResponse(const std::vector<std::string> &identities, const std::vector<std::string> &raw_deks);
//! The same, for a wrap_batch reply carrying already-base64 blobs.
std::string WrapResponse(const std::vector<std::string> &identities, const std::vector<std::string> &base64_blobs);

//! A well-formed unwrap_batch response carrying these raw DEKs, in order, each
//! echoing the identity of the REQUEST item at the same position.
//!
//! The echo is what the real service does - crypta's reply item is a `PlainKey`,
//! an identity beside the dek (`sigil-enterprises/crypta` src/server.rs) - and
//! until #31 neither fake here echoed anything. A reply-side binding tested
//! against a fake that never echoes an identity passes without ever running the
//! guard, so correcting the fakes is a PRECONDITION of the guard, not a
//! follow-up.
std::string OkUnwrapResponse(const std::string &request, const std::vector<std::string> &raw_deks);
//! A well-formed wrap_batch response carrying these already-base64 blobs, each
//! echoing the identity of the REQUEST item at the same position.
std::string OkWrapResponse(const std::string &request, const std::vector<std::string> &base64_blobs);
//! The error shape crypta emits when a binding does not hold.
std::string ErrorResponse(const std::string &message);

//! Pull the value of a JSON string field out of `json` by scanning for the next
//! UNESCAPED closing quote, and decode its escapes. Deliberately independent of
//! `CryptaClient::ExtractBase64Field` (which may not assume escapes exist) so it
//! can be used to prove the encoder's output round-trips.
//! Returns false when the field is absent or the string is unterminated.
bool DecodeJsonStringField(const std::string &json, const std::string &field, std::string &out);

} // namespace ducklake_crypta_test
