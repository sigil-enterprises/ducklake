//===----------------------------------------------------------------------===//
//                         DuckLake (sigil fork)
//
// test/cpp/crypta/crypta_refusal_test.cpp
//
// Every case here asserts that `CryptaClient` REFUSES a malformed, truncated,
// oversized, or impossible response from the key service.
//
// PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.
//
// None of these are reachable from `scripts/mvp_crypta_proof.sh` or from
// anything under `test/sql/`: both run against a HEALTHY crypta, and a healthy
// crypta never emits any of this. That is the whole reason the fake server
// exists.
//
// Each case names the mutant in `mutants.py` that must make it FAIL. A refusal
// test nobody has seen fail is indistinguishable from one that asserts nothing,
// so `run_crypta_tests.sh --mutants` deletes the guard and requires the red.
//===----------------------------------------------------------------------===//

#include "crypta/ducklake_crypta.hpp"
#include "crypta_test_support.hpp"
#include "duckdb/common/exception.hpp"

#include <chrono>
#include <csignal>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace duckdb;              // NOLINT
using namespace ducklake_crypta_test; // NOLINT

//===----------------------------------------------------------------------===//
// Constructing the client
//===----------------------------------------------------------------------===//

// mutant: no_empty_path_check
TEST_CASE("crypta: an empty socket path is refused at construction", "[crypta][refusal][socket_path]") {
	REQUIRE_THROWS_AS(CryptaClient(""), InvalidInputException);
	REQUIRE_THAT(ThrownMessage([]() { CryptaClient client(""); }), Catch::Contains("socket path is empty"));
}

// mutant: no_path_length_check
TEST_CASE("crypta: an over-long socket path is refused instead of silently truncated",
          "[crypta][refusal][socket_path]") {
	// sun_path is 108 bytes on Linux. A path that does not fit would be truncated
	// by memcpy and connect to a DIFFERENT socket - which is the failure mode
	// worth refusing, because it succeeds against the wrong service.
	const size_t SUN_PATH_SIZE = 108;

	SECTION("exactly at the limit is refused") {
		CryptaClient client(std::string(SUN_PATH_SIZE, 'x'));
		REQUIRE_THAT(ThrownMessage([&]() { client.Health(); }), Catch::Contains("socket path is too long"));
	}
	SECTION("one byte under the limit passes the length check and fails on connect instead") {
		// The discriminating half: if the guard were merely "always throw", this
		// would throw too. It must not - it must get as far as connect().
		CryptaClient client(std::string(SUN_PATH_SIZE - 1, 'x'));
		auto message = ThrownMessage([&]() { client.Health(); });
		REQUIRE_THAT(message, Catch::Contains("cannot reach the crypta key service"));
		REQUIRE_THAT(message, !Catch::Contains("too long"));
	}
}

TEST_CASE("crypta: an unreachable socket names the socket in the error", "[crypta][refusal][socket_path]") {
	CryptaClient client("/tmp/ducklake_crypta_test_definitely_not_here.sock");
	auto message = ThrownMessage([&]() { client.Health(); });
	REQUIRE_THAT(message, Catch::Contains("cannot reach the crypta key service"));
	REQUIRE_THAT(message, Catch::Contains("/tmp/ducklake_crypta_test_definitely_not_here.sock"));
	REQUIRE_THAT(message, Catch::Contains("cannot be read without it"));
}

//===----------------------------------------------------------------------===//
// Empty input - neither batch path may touch the socket
//===----------------------------------------------------------------------===//

// mutants: no_empty_shortcut_wrap, no_empty_shortcut_unwrap
TEST_CASE("crypta: an empty batch returns empty WITHOUT contacting the service", "[crypta][refusal][empty_input]") {
	FakeCryptaServer server;
	server.Start([](FakeConnection &connection, int) {
		// Any connection at all is the defect. Reply with something valid so the
		// failure is a connection-count assertion, not a hang.
		connection.ReadFrame();
		connection.WriteFrame(OkUnwrapResponse({SampleDek()}));
	});
	CryptaClient client(server.Path());

	SECTION("wrap") {
		auto result = client.WrapBatch({}, {});
		REQUIRE(result.empty());
	}
	SECTION("unwrap") {
		auto result = client.UnwrapBatch({}, {});
		REQUIRE(result.empty());
	}

	// Give a stray connection time to land before we look.
	std::this_thread::sleep_for(std::chrono::milliseconds(60));
	REQUIRE(server.Connections() == 0);
}

// mutants: no_size_mismatch_wrap, no_size_mismatch_unwrap
//
// OVER-supplied on purpose, and the mutants point here rather than at the
// under-supplied case below. With the guard removed, the loop runs over
// `identities.size()` and indexes IN BOUNDS of the longer vector, so the mutant
// completes the call and reddens on the missing throw and on the connection
// count - evidence about the guard. Removing it in the under-supplied direction
// would index past the end of the shorter vector, and a crash is not evidence
// about anything; it is undefined behaviour that happens to exit non-zero.
TEST_CASE("crypta: a batch with more keys than identities is refused", "[crypta][refusal][size_mismatch]") {
	FakeCryptaServer server;
	server.Start([](FakeConnection &connection, int) {
		connection.ReadFrame();
		connection.WriteFrame(OkUnwrapResponse({SampleDek(), SampleDek()}));
	});
	CryptaClient client(server.Path());
	vector<CryptaFileIdentity> two {SampleIdentity("a"), SampleIdentity("b")};
	vector<string> three {SampleDek('a'), SampleDek('b'), SampleDek('c')};

	SECTION("wrap") {
		REQUIRE_THAT(ThrownMessage([&]() { client.WrapBatch(two, three); }),
		             Catch::Contains("2 identities for 3 keys"));
	}
	SECTION("unwrap") {
		REQUIRE_THAT(ThrownMessage([&]() { client.UnwrapBatch(two, three); }),
		             Catch::Contains("2 identities for 3 blobs"));
	}
	// It refused before building a body, so nothing reached the service.
	std::this_thread::sleep_for(std::chrono::milliseconds(60));
	REQUIRE(server.Connections() == 0);
}

// No mutant points here - see the note above. It is the same single `if` in each
// batch path, already proven by the over-supplied case; this one exists because
// under-supply is the direction that would actually read off the end.
TEST_CASE("crypta: a batch with fewer keys than identities is refused", "[crypta][refusal][size_mismatch]") {
	FakeCryptaServer server;
	server.Start([](FakeConnection &connection, int) {
		connection.ReadFrame();
		connection.WriteFrame(OkUnwrapResponse({SampleDek()}));
	});
	CryptaClient client(server.Path());
	vector<CryptaFileIdentity> two {SampleIdentity("a"), SampleIdentity("b")};
	vector<string> one {SampleDek()};

	SECTION("wrap") {
		REQUIRE_THAT(ThrownMessage([&]() { client.WrapBatch(two, one); }),
		             Catch::Contains("2 identities for 1 keys"));
	}
	SECTION("unwrap") {
		REQUIRE_THAT(ThrownMessage([&]() { client.UnwrapBatch(two, one); }),
		             Catch::Contains("2 identities for 1 blobs"));
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(60));
	REQUIRE(server.Connections() == 0);
}

//===----------------------------------------------------------------------===//
// Malformed responses
//===----------------------------------------------------------------------===//

// mutant: no_truncation_check
TEST_CASE("crypta: a response truncated inside a value is refused", "[crypta][refusal][truncated]") {
	SECTION("unwrap - the dek value never closes its quote") {
		FakeCryptaServer server;
		server.Start([](FakeConnection &connection, int) {
			connection.ReadFrame();
			// Well-framed: the length header matches the body exactly. The damage is
			// INSIDE the JSON, which is what makes this unreachable from a healthy
			// service and undetectable by the framing checks.
			connection.WriteFrame("{\"status\":\"ok\",\"items\":[{\"dek\":\"QUJD");
		});
		CryptaClient client(server.Path());
		REQUIRE_THAT(ThrownMessage([&]() { client.UnwrapBatch({SampleIdentity()}, {"RExLblob"}); }),
		             Catch::Contains("truncated inside a dek value"));
	}
	SECTION("wrap - the wrapped value never closes its quote") {
		FakeCryptaServer server;
		server.Start([](FakeConnection &connection, int) {
			connection.ReadFrame();
			connection.WriteFrame("{\"status\":\"ok\",\"items\":[{\"wrapped\":\"RExLQUJD");
		});
		CryptaClient client(server.Path());
		REQUIRE_THAT(ThrownMessage([&]() { client.WrapBatch({SampleIdentity()}, {SampleDek()}); }),
		             Catch::Contains("truncated inside a wrapped value"));
	}
}

// mutant: no_count_check
TEST_CASE("crypta: a value count that does not match the request is refused", "[crypta][refusal][count]") {
	SECTION("too many - two deks for one file") {
		FakeCryptaServer server;
		server.Start([](FakeConnection &connection, int) {
			connection.ReadFrame();
			connection.WriteFrame(OkUnwrapResponse({SampleDek('a'), SampleDek('b')}));
		});
		CryptaClient client(server.Path());
		REQUIRE_THAT(ThrownMessage([&]() { client.UnwrapBatch({SampleIdentity()}, {"RExLblob"}); }),
		             Catch::Contains("crypta returned 2 dek values for 1 requested items"));
	}
	SECTION("too few - one dek for two files") {
		FakeCryptaServer server;
		server.Start([](FakeConnection &connection, int) {
			connection.ReadFrame();
			connection.WriteFrame(OkUnwrapResponse({SampleDek('a')}));
		});
		CryptaClient client(server.Path());
		vector<CryptaFileIdentity> identities {SampleIdentity("a"), SampleIdentity("b")};
		vector<string> blobs {"RExLone", "RExLtwo"};
		REQUIRE_THAT(ThrownMessage([&]() { client.UnwrapBatch(identities, blobs); }),
		             Catch::Contains("crypta returned 1 dek values for 2 requested items"));
	}
	SECTION("none at all - an ok status with no items") {
		FakeCryptaServer server;
		server.Start([](FakeConnection &connection, int) {
			connection.ReadFrame();
			connection.WriteFrame("{\"status\":\"ok\",\"items\":[]}");
		});
		CryptaClient client(server.Path());
		REQUIRE_THAT(ThrownMessage([&]() { client.UnwrapBatch({SampleIdentity()}, {"RExLblob"}); }),
		             Catch::Contains("crypta returned 0 dek values for 1 requested items"));
	}
	SECTION("wrap - two blobs for one key") {
		FakeCryptaServer server;
		server.Start([](FakeConnection &connection, int) {
			connection.ReadFrame();
			connection.WriteFrame(OkWrapResponse({"RExLone", "RExLtwo"}));
		});
		CryptaClient client(server.Path());
		REQUIRE_THAT(ThrownMessage([&]() { client.WrapBatch({SampleIdentity()}, {SampleDek()}); }),
		             Catch::Contains("crypta returned 2 wrapped values for 1 requested items"));
	}
}

// The two bounds are separate cases so each can be mutated on its own. They are
// also deliberately NOT tested with an absurd announced length (0xFFFFFFFF):
// with the upper bound mutated away, that would ask for a 4 GB allocation and
// take the machine down instead of reddening a test. One byte past the limit
// proves the bound; four gigabytes proves nothing further.

// mutant: no_frame_upper_bound
TEST_CASE("crypta: an oversized response frame is refused", "[crypta][refusal][frame]") {
	const uint32_t MAX_FRAME = 64u * 1024 * 1024;
	FakeCryptaServer server;
	server.Start([&](FakeConnection &connection, int) {
		connection.ReadFrame();
		// Announce a body larger than the limit and send none of it. A client that
		// trusted the header would try to allocate it.
		connection.WriteLengthHeader(MAX_FRAME + 1);
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	});
	CryptaClient client(server.Path());
	auto message = ThrownMessage([&]() { client.UnwrapBatch({SampleIdentity()}, {"RExLblob"}); });
	REQUIRE_THAT(message, Catch::Contains("byte response, outside 1.."));
	REQUIRE_THAT(message, Catch::Contains("67108865"));
}

// mutant: no_frame_lower_bound
TEST_CASE("crypta: a zero-length response frame is refused", "[crypta][refusal][frame]") {
	FakeCryptaServer server;
	server.Start([](FakeConnection &connection, int) {
		connection.ReadFrame();
		connection.WriteLengthHeader(0);
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	});
	CryptaClient client(server.Path());
	REQUIRE_THAT(ThrownMessage([&]() { client.UnwrapBatch({SampleIdentity()}, {"RExLblob"}); }),
	             Catch::Contains("crypta announced a 0 byte response"));
}

// mutant: no_request_frame_limit
TEST_CASE("crypta: a request larger than the frame limit is refused before it is sent",
          "[crypta][refusal][frame][slow]") {
	const size_t MAX_FRAME = 64ull * 1024 * 1024;
	FakeCryptaServer server;
	server.Start([&](FakeConnection &connection, int) {
		server.Record(connection.ReadFrame());
		connection.WriteFrame(OkUnwrapResponse({SampleDek()}));
	});
	CryptaClient client(server.Path());

	// One identity with an enormous stored path is the cheapest way past the
	// limit; the alternative is half a million files, which is slower and proves
	// the same thing.
	auto identity = SampleIdentity(std::string(MAX_FRAME + 4096, 'p'));
	auto message = ThrownMessage([&]() { client.UnwrapBatch({identity}, {"RExLblob"}); });
	REQUIRE_THAT(message, Catch::Contains("exceeds the frame limit"));
	// It refused, so nothing reached the service.
	std::this_thread::sleep_for(std::chrono::milliseconds(60));
	REQUIRE(server.Requests().empty());
}

// mutant: no_eof_check
TEST_CASE("crypta: a connection closed mid-read is refused", "[crypta][refusal][closed]") {
	SECTION("closed before the length header") {
		FakeCryptaServer server;
		server.Start([](FakeConnection &connection, int) {
			connection.ReadFrame();
			connection.Close();
		});
		CryptaClient client(server.Path());
		REQUIRE_THAT(ThrownMessage([&]() { client.UnwrapBatch({SampleIdentity()}, {"RExLblob"}); }),
		             Catch::Contains("closed the connection while reading the response length"));
	}
	SECTION("closed halfway through the header") {
		FakeCryptaServer server;
		server.Start([](FakeConnection &connection, int) {
			connection.ReadFrame();
			connection.WriteRaw(std::string("\x00\x00", 2));
			connection.Close();
		});
		CryptaClient client(server.Path());
		REQUIRE_THAT(ThrownMessage([&]() { client.UnwrapBatch({SampleIdentity()}, {"RExLblob"}); }),
		             Catch::Contains("closed the connection while reading the response length"));
	}
	SECTION("closed halfway through the body") {
		FakeCryptaServer server;
		server.Start([](FakeConnection &connection, int) {
			connection.ReadFrame();
			auto body = OkUnwrapResponse({SampleDek()});
			// The header promises the whole body; only half of it arrives. This is
			// exactly the shape a crashed service produces.
			connection.WriteLengthHeader(static_cast<uint32_t>(body.size()));
			connection.WriteRaw(body.substr(0, body.size() / 2));
			connection.Close();
		});
		CryptaClient client(server.Path());
		REQUIRE_THAT(ThrownMessage([&]() { client.UnwrapBatch({SampleIdentity()}, {"RExLblob"}); }),
		             Catch::Contains("closed the connection while reading the response body"));
	}
}

// mutant: no_socket_creation_check
TEST_CASE("crypta: a socket that cannot be created is refused, not used", "[crypta][refusal][io]") {
	// The fd table is exhausted for the duration of one call. Without this the
	// `socket() < 0` branch is unreachable from any test: every other failure in
	// this file happens AFTER a socket exists.
	//
	// It matters because the guard is what stops an fd of -1 being carried into
	// connect(). Deleting it does not produce a silent success - it produces the
	// WRONG diagnosis, "could not connect to crypta at <path>", pointing an
	// operator at a service that is running perfectly well while the real fault
	// is a process out of descriptors.
	struct RlimitGuard {
		struct rlimit saved;
		bool held = false;
		~RlimitGuard() {
			if (held) {
				setrlimit(RLIMIT_NOFILE, &saved);
			}
		}
	} guard;
	REQUIRE(getrlimit(RLIMIT_NOFILE, &guard.saved) == 0);

	// The path is never reached, but it still has to pass the constructor's own
	// checks - non-empty and under sun_path - or this would measure those instead.
	CryptaClient client("/tmp/ducklake-crypta-no-fds.sock");

	struct rlimit squeezed = guard.saved;
	squeezed.rlim_cur = 0;
	REQUIRE(setrlimit(RLIMIT_NOFILE, &squeezed) == 0);
	guard.held = true;
	auto message = ThrownMessage([&]() { client.Health(); });

	// Restore through the GUARD, and disarm it before asserting. Asserting first
	// would leave `held` true when the REQUIRE throws, so the destructor would
	// retry the same failing call and every later case in this binary would run
	// at soft limit 0. Unreachable in practice - restoring a soft limit at or
	// below an unchanged hard limit always succeeds - but the ordering costs
	// nothing and the failure mode is silent and global.
	int restored = setrlimit(RLIMIT_NOFILE, &guard.saved);
	guard.held = false;
	REQUIRE(restored == 0);

	REQUIRE_THAT(message, Catch::Contains("could not create a socket for crypta"));
}

// mutant: no_read_error_check
TEST_CASE("crypta: a socket read failure is refused", "[crypta][refusal][io]") {
	FakeCryptaServer server;
	server.Start([](FakeConnection &connection, int) {
		// Wait for the request to land, then close WITHOUT draining it. On Linux
		// that sets ECONNRESET on the client's socket, so its read fails with an
		// error instead of reporting a clean EOF - the `rc < 0` branch of
		// ReadExact, which no graceful close can reach. (Reading the frame first,
		// as the other cases do, gives EOF instead.)
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
		connection.CloseWithoutReading();
	});
	CryptaClient client(server.Path());
	auto message = ThrownMessage([&]() { client.UnwrapBatch({SampleIdentity()}, {"RExLblob"}); });
	REQUIRE_THAT(message, Catch::Contains("crypta read failed while reading the response length"));
	REQUIRE_THAT(message, Catch::Contains("Connection reset by peer"));
}

// mutant: no_write_error_check
TEST_CASE("crypta: a socket write failure is refused", "[crypta][refusal][io]") {
	FakeCryptaServer server;
	server.Start([](FakeConnection &connection, int) {
		// Close immediately, without reading: the client's request has nowhere to
		// go and its next write to the dead socket returns EPIPE.
		connection.CloseWithoutReading();
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	});
	CryptaClient client(server.Path());

	// The body has to be bigger than the socket buffer, or the whole request
	// disappears into it and the write succeeds against a dead peer.
	vector<CryptaFileIdentity> identities;
	vector<string> blobs;
	for (int i = 0; i < 8000; i++) {
		identities.push_back(SampleIdentity("table/file_" + std::to_string(i) + ".parquet"));
		blobs.push_back("RExLblob" + std::to_string(i));
	}
	REQUIRE_THAT(ThrownMessage([&]() { client.UnwrapBatch(identities, blobs); }),
	             Catch::Contains("crypta write failed"));
}

namespace {

//! The exit codes the forked child below reports its outcome with. They are
//! distinct on purpose: "it threw the WRONG exception" must not be readable as
//! success, because a request that slips past the write and dies on the READ
//! instead would otherwise turn this case into a green that proves nothing
//! about SIGPIPE.
enum SigpipeChildOutcome : int {
	CHILD_WRITE_FAILURE_RAISED = 0,
	CHILD_NO_THROW = 2,
	CHILD_WRONG_EXCEPTION = 3,
	CHILD_UNKNOWN_EXCEPTION = 4,
};

//! Fill a batch big enough that the request cannot vanish into the socket
//! buffer - the same reason the case above needs one.
void FillOversizedBatch(vector<CryptaFileIdentity> &identities, vector<string> &blobs) {
	for (int i = 0; i < 8000; i++) {
		identities.push_back(SampleIdentity("table/file_" + std::to_string(i) + ".parquet"));
		blobs.push_back("RExLblob" + std::to_string(i));
	}
}

} // namespace

// mutant: no_sigpipe_suppression
TEST_CASE("crypta: a socket write failure does not kill a host that leaves SIGPIPE at its default",
          "[crypta][refusal][io][sigpipe]") {
	// The case above proves the client RAISES on a write failure. It cannot prove
	// the host lives long enough to see it, because `crypta_test_main.cpp` ignores
	// SIGPIPE process-wide so the branch is observable at all - which is exactly
	// the production disposition an embedding host does NOT have.
	//
	// So this case forks, restores SIG_DFL in the child, and asserts on how the
	// child DIED. A client that lets the signal fire kills the child (WIFSIGNALED,
	// WTERMSIG == SIGPIPE); a client that suppresses it locally raises the
	// IOException and the child exits 0. Nothing observable inside the runner can
	// tell those two apart - only the wait status can.
	FakeCryptaServer server;

	// The fork happens BEFORE Start(), while this process is still single-
	// threaded: forking a process with a live accept thread would give the child a
	// heap whose allocator lock may be held by a thread that does not exist in it.
	// connect() succeeds against a listening socket that nobody has accepted yet,
	// so the child does not need the server thread to be running first.
	auto child = fork();
	REQUIRE(child >= 0);
	if (child == 0) {
		// CHILD. No Catch2 assertions past this point - it does not own the
		// reporter - and _exit rather than exit, so no parent destructor (the
		// server's socket unlink among them) runs twice.
		signal(SIGPIPE, SIG_DFL);
		// A client that neither raises nor dies would otherwise hang the suite.
		alarm(60);
		int outcome = CHILD_NO_THROW;
		try {
			CryptaClient client(server.Path());
			vector<CryptaFileIdentity> identities;
			vector<string> blobs;
			FillOversizedBatch(identities, blobs);
			client.UnwrapBatch(identities, blobs);
		} catch (const std::exception &e) {
			outcome = std::string(e.what()).find("crypta write failed") != std::string::npos
			              ? CHILD_WRITE_FAILURE_RAISED
			              : CHILD_WRONG_EXCEPTION;
		} catch (...) {
			outcome = CHILD_UNKNOWN_EXCEPTION;
		}
		_exit(outcome);
	}

	server.Start([](FakeConnection &connection, int) {
		connection.CloseWithoutReading();
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	});
	int status = 0;
	auto waited = waitpid(child, &status, 0);
	server.Stop();
	REQUIRE(waited == child);

	// Reported rather than merely asserted: on the pre-fix client the useful fact
	// is WHICH signal killed the child, and Catch2 prints these only on failure.
	INFO("WIFSIGNALED=" << (WIFSIGNALED(status) ? 1 : 0)
	                    << " WTERMSIG=" << (WIFSIGNALED(status) ? WTERMSIG(status) : 0) << " (SIGPIPE is "
	                    << SIGPIPE << ") WIFEXITED=" << (WIFEXITED(status) ? 1 : 0)
	                    << " WEXITSTATUS=" << (WIFEXITED(status) ? WEXITSTATUS(status) : -1));
	REQUIRE_FALSE(WIFSIGNALED(status));
	REQUIRE(WIFEXITED(status));
	// Not merely "it survived": 3 would mean the write slipped through and the
	// read raised instead, which would leave the write path unproven.
	REQUIRE(WEXITSTATUS(status) == CHILD_WRITE_FAILURE_RAISED);
}

//===----------------------------------------------------------------------===//
// EINTR - a signal in the middle of a syscall is retried, not an error
//===----------------------------------------------------------------------===//

namespace {

std::atomic<int> g_signal_count {0};

void CountingSignalHandler(int) {
	g_signal_count.fetch_add(1);
}

//! Install a handler WITHOUT SA_RESTART, so a signal actually interrupts a
//! blocked read/write instead of the kernel restarting it transparently.
struct InterruptingSignal {
	InterruptingSignal() {
		g_signal_count.store(0);
		struct sigaction action;
		memset(&action, 0, sizeof(action));
		action.sa_handler = CountingSignalHandler;
		action.sa_flags = 0;
		sigemptyset(&action.sa_mask);
		sigaction(SIGUSR1, &action, &previous);
	}
	~InterruptingSignal() {
		sigaction(SIGUSR1, &previous, nullptr);
	}
	struct sigaction previous;
};

//! Repeatedly signal `target` once `gate` opens, until stopped.
struct SignalPoker {
	SignalPoker(pthread_t target_p, std::atomic<bool> &gate_p) : target(target_p), gate(gate_p) {
		thread = std::thread([this]() {
			// This thread must not eat the signals it is sending.
			sigset_t block;
			sigemptyset(&block);
			sigaddset(&block, SIGUSR1);
			pthread_sigmask(SIG_BLOCK, &block, nullptr);
			while (!stop.load()) {
				if (gate.load()) {
					pthread_kill(target, SIGUSR1);
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
		});
	}
	~SignalPoker() {
		stop.store(true);
		thread.join();
	}
	pthread_t target;
	std::atomic<bool> &gate;
	std::atomic<bool> stop {false};
	std::thread thread;
};

//! Block SIGUSR1 for the duration, so threads created inside inherit the block
//! and only the thread that unblocks it can be interrupted.
struct BlockSignalHere {
	BlockSignalHere() {
		sigset_t block;
		sigemptyset(&block);
		sigaddset(&block, SIGUSR1);
		pthread_sigmask(SIG_BLOCK, &block, &previous);
	}
	~BlockSignalHere() {
		pthread_sigmask(SIG_SETMASK, &previous, nullptr);
	}
	sigset_t previous;
};

void UnblockSignalHere() {
	sigset_t block;
	sigemptyset(&block);
	sigaddset(&block, SIGUSR1);
	pthread_sigmask(SIG_UNBLOCK, &block, nullptr);
}

} // namespace

// mutant: no_eintr_retry_read
TEST_CASE("crypta: a signal during the response read is retried, not reported as failure",
          "[crypta][refusal][eintr]") {
	InterruptingSignal signal_guard;
	std::atomic<bool> request_seen {false};
	auto dek = SampleDek('e');

	FakeCryptaServer server;
	pthread_t client_thread = pthread_self();
	{
		// Server and poker threads inherit a blocked SIGUSR1 from here, so the
		// signal is delivered to the thread that is actually inside read().
		BlockSignalHere blocked;
		server.Start([&](FakeConnection &connection, int) {
			connection.ReadFrame();
			request_seen.store(true);
			// Hold the response back so the client is parked in read() with the
			// signals arriving.
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			connection.WriteFrame(OkUnwrapResponse({dek}));
		});
		SignalPoker poker(client_thread, request_seen);
		UnblockSignalHere();

		CryptaClient client(server.Path());
		auto keys = client.UnwrapBatch({SampleIdentity()}, {"RExLblob"});
		REQUIRE(keys.size() == 1);
		REQUIRE(keys[0] == dek);
	}
	// This counter proves signals were DELIVERED, not that the EINTR branch ran -
	// a signal arriving between the syscalls would count without interrupting
	// one. What proves the branch ran is the mutant: deleting `if (errno ==
	// EINTR) continue;` makes this case fail. If the read were never interrupted,
	// removing the retry would change nothing and the mutant would survive.
	REQUIRE(g_signal_count.load() > 0);
	REQUIRE(server.HandlerError().empty());
}

// mutant: no_eintr_retry_write
TEST_CASE("crypta: a signal during the request write is retried, not reported as failure",
          "[crypta][refusal][eintr][slow]") {
	InterruptingSignal signal_guard;
	std::atomic<bool> accepted {false};

	vector<CryptaFileIdentity> identities;
	vector<string> blobs;
	vector<string> deks;
	for (int i = 0; i < 8000; i++) {
		identities.push_back(SampleIdentity("table/file_" + std::to_string(i) + ".parquet"));
		blobs.push_back("RExLblob" + std::to_string(i));
		deks.push_back(SampleDek(static_cast<char>('a' + (i % 26))));
	}

	FakeCryptaServer server;
	pthread_t client_thread = pthread_self();
	{
		BlockSignalHere blocked;
		server.Start([&](FakeConnection &connection, int) {
			accepted.store(true);
			// Do not drain the socket. The client fills the buffer and blocks in
			// write(), which is the only place the write-side EINTR can happen.
			std::this_thread::sleep_for(std::chrono::milliseconds(400));
			connection.ReadFrame();
			connection.WriteFrame(OkUnwrapResponse(std::vector<std::string>(deks.begin(), deks.end())));
		});
		SignalPoker poker(client_thread, accepted);
		UnblockSignalHere();

		CryptaClient client(server.Path());
		auto keys = client.UnwrapBatch(identities, blobs);
		REQUIRE(keys.size() == identities.size());
		REQUIRE(keys[0] == deks[0]);
	}
	// As on the read side: delivery is what this counts, and the
	// no_eintr_retry_write mutant is what proves the branch was entered.
	REQUIRE(g_signal_count.load() > 0);
	REQUIRE(server.HandlerError().empty());
}

//===----------------------------------------------------------------------===//
// The error status crypta itself sends
//===----------------------------------------------------------------------===//

// mutant: no_error_status_check
TEST_CASE("crypta: an error status is surfaced, never parsed for keys", "[crypta][refusal][error_status]") {
	SECTION("the service's own message is surfaced") {
		FakeCryptaServer server;
		server.Start([](FakeConnection &connection, int) {
			connection.ReadFrame();
			connection.WriteFrame(ErrorResponse("unwrap failed: not valid for this KEK and file identity"));
		});
		CryptaClient client(server.Path());
		REQUIRE_THAT(ThrownMessage([&]() { client.UnwrapBatch({SampleIdentity()}, {"RExLblob"}); }),
		             Catch::Contains("crypta refused the request: unwrap failed"));
	}
	SECTION("an error with no message is still an error") {
		FakeCryptaServer server;
		server.Start([](FakeConnection &connection, int) {
			connection.ReadFrame();
			connection.WriteFrame("{\"status\":\"error\"}");
		});
		CryptaClient client(server.Path());
		REQUIRE_THAT(ThrownMessage([&]() { client.UnwrapBatch({SampleIdentity()}, {"RExLblob"}); }),
		             Catch::Contains("crypta refused the request: unknown error"));
	}
	SECTION("an error response carrying a dek is still refused, and the dek is not used") {
		// The ordering matters: ThrowIfError runs BEFORE the values are read. A
		// service that reports an error while also emitting a key must not have
		// that key harvested.
		FakeCryptaServer server;
		server.Start([](FakeConnection &connection, int) {
			connection.ReadFrame();
			connection.WriteFrame("{\"status\":\"error\",\"message\":\"denied\",\"items\":[{\"dek\":\"" +
			                      Base64Encode(SampleDek()) + "\"}]}");
		});
		CryptaClient client(server.Path());
		REQUIRE_THAT(ThrownMessage([&]() { client.UnwrapBatch({SampleIdentity()}, {"RExLblob"}); }),
		             Catch::Contains("crypta refused the request: denied"));
	}
}

// mutant: no_reply_alphabet_check
TEST_CASE("crypta: a dek value that is not valid base64 is refused", "[crypta][refusal][base64]") {
	// THE ORDER, WRITTEN DOWN - and it MOVED, which is why this case is now two
	// sections instead of one assertion.
	//
	// It used to name `Blob::FromBase64` alone, and that was the whole story: the
	// reader returned any bytes it found and the DECODER was the first thing to
	// object. #33 put an alphabet check in `ExtractBase64Field`, upstream of the
	// decode, because the WRAP reply's value is decoded by nobody - it goes to the
	// catalog as SQL text - so the reader had to become the guard. The unwrap path
	// inherits it, and the diagnosis on this path therefore changed hands.
	//
	// Both guards are still load-bearing and the discriminator is exact: the
	// reader owns the ALPHABET, the decoder owns LENGTH and PADDING, which
	// `IsBase64` deliberately says nothing about. Pinning the pair here means the
	// next guard added to this path changes a RED test rather than quietly
	// stealing the other's coverage.
	//
	// The assertions name their guard rather than testing that "it threw
	// something": swap either reply for `{"items":[]}` and the COUNT check throws
	// instead, with nothing base64 anywhere in the picture, and a bare non-empty
	// check would still be green.
	SECTION("a value outside the ALPHABET is refused by the reader, before the decode") {
		FakeCryptaServer server;
		server.Start([](FakeConnection &connection, int) {
			connection.ReadFrame();
			connection.WriteFrame("{\"status\":\"ok\",\"items\":[{\"dek\":\"not-base64!!\"}]}");
		});
		CryptaClient client(server.Path());
		// Deliberately NOT the bare fragment "not base64": the decoder's message
		// quotes the offending VALUE back, and this fixture's own bytes read
		// `not-base64!!`, so a loose match would find the fixture inside the OTHER
		// guard's message and pass whichever guard answered.
		REQUIRE_THAT(ThrownMessage([&]() { client.UnwrapBatch({SampleIdentity()}, {"RExLblob"}); }),
		             Catch::Contains("crypta returned a dek value that is not base64"));
	}

	SECTION("a value INSIDE the alphabet but of an impossible length is refused by the decoder") {
		// Three characters: every one of them in the alphabet, so `IsBase64` says
		// yes and must - length and padding are crypta's business, and a rule here
		// would refuse blobs that are perfectly forwardable. The decode is what
		// objects, and this is the case that keeps it in the picture.
		FakeCryptaServer server;
		server.Start([](FakeConnection &connection, int) {
			connection.ReadFrame();
			connection.WriteFrame("{\"status\":\"ok\",\"items\":[{\"dek\":\"QQQ\"}]}");
		});
		CryptaClient client(server.Path());
		REQUIRE_THAT(ThrownMessage([&]() { client.UnwrapBatch({SampleIdentity()}, {"RExLblob"}); }),
		             Catch::Contains("as base64: length must be a multiple of 4"));
	}
}

//===----------------------------------------------------------------------===//
// JSON escaping of the identity
//===----------------------------------------------------------------------===//

// mutant: no_json_escape
TEST_CASE("crypta: quotes and control characters in an identity are escaped on the wire",
          "[crypta][refusal][json_escape]") {
	// A table name reaches stored_path, so this is attacker-influenced input. An
	// unescaped quote would end the JSON string early and let the rest of the path
	// be read as protocol - the identity the service binds against would then be
	// something the caller never asked for.
	std::string hostile_path = "t/he said \"hi\"\\back\nnew\rcr\ttab\x01soh\x1funit.parquet";
	std::string hostile_lake = "lake\"; \"op\":\"health";

	FakeCryptaServer server;
	server.Start([&](FakeConnection &connection, int) {
		server.Record(connection.ReadFrame());
		connection.WriteFrame(OkUnwrapResponse({SampleDek()}));
	});
	CryptaClient client(server.Path());

	CryptaFileIdentity identity;
	identity.lake_id = hostile_lake;
	identity.table_id = 42;
	identity.is_delete_file = true;
	identity.stored_path = hostile_path;
	client.UnwrapBatch({identity}, {"RExLblob"});

	auto requests = server.Requests();
	REQUIRE(requests.size() == 1);
	auto &body = requests[0];

	// 1. The escapes are present as escapes.
	REQUIRE(body.find("\\\"") != std::string::npos);
	REQUIRE(body.find("\\\\") != std::string::npos);
	REQUIRE(body.find("\\n") != std::string::npos);
	REQUIRE(body.find("\\r") != std::string::npos);
	REQUIRE(body.find("\\t") != std::string::npos);
	REQUIRE(body.find("\\u0001") != std::string::npos);
	REQUIRE(body.find("\\u001f") != std::string::npos);

	// 2. No raw control character survived into the frame.
	for (auto c : body) {
		REQUIRE(static_cast<unsigned char>(c) >= 0x20);
	}

	// 3. The strongest form: decoding the field by scanning to the next UNESCAPED
	//    quote gives back exactly what went in. If a quote had been emitted raw,
	//    the value would end early and this would not round-trip.
	std::string decoded_path;
	REQUIRE(DecodeJsonStringField(body, "file_path", decoded_path));
	REQUIRE(decoded_path == hostile_path);

	std::string decoded_lake;
	REQUIRE(DecodeJsonStringField(body, "catalog_uuid", decoded_lake));
	REQUIRE(decoded_lake == hostile_lake);

	// 4. The injected `"op":"health"` did not become a second op. The only
	//    unescaped occurrence of an op key is the real one.
	size_t op_count = 0;
	for (size_t i = 0; (i = body.find("\"op\":", i)) != std::string::npos; i++) {
		if (i == 0 || body[i - 1] != '\\') {
			op_count++;
		}
	}
	REQUIRE(op_count == 1);
	REQUIRE(body.find("\"op\":\"unwrap_batch\"") != std::string::npos);
}

TEST_CASE("crypta: the delete-file kind is carried on the wire", "[crypta][refusal][json_escape]") {
	// Invariant 3 in .claude/README.md: a delete file's key row must not be
	// interchangeable with a data file's. That starts with the kind reaching the
	// service at all.
	FakeCryptaServer server;
	server.Start([&](FakeConnection &connection, int) {
		server.Record(connection.ReadFrame());
		connection.WriteFrame(OkUnwrapResponse({SampleDek()}));
	});
	CryptaClient client(server.Path());

	auto identity = SampleIdentity();
	identity.is_delete_file = true;
	client.UnwrapBatch({identity}, {"RExLblob"});
	REQUIRE(server.Requests().size() == 1);
	REQUIRE(server.Requests()[0].find("\"file_kind\":\"delete\"") != std::string::npos);
	REQUIRE(server.Requests()[0].find("\"file_kind\":\"data\"") == std::string::npos);
}

//===----------------------------------------------------------------------===//
// The happy path, so the fake server is not proving refusals by being broken
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// The ATTACH-time self-test
//===----------------------------------------------------------------------===//

// mutant: no_self_test_ok_check
TEST_CASE("crypta: a service that answers but does not report ok fails the self-test",
          "[crypta][refusal][self_test]") {
	// This is the gate that decides whether the envelope provider is installed on
	// the catalog at all, so a fail-open here is not a degraded read - it is an
	// ATTACH that reports success and then writes PLAINTEXT keys.
	//
	// Reachability is the easy half and the SQL fixture already covers it (a dead
	// socket refuses the ATTACH). The half nothing covered is a service that is
	// alive, answers the frame, and says it is NOT ok - a crypta whose KEK has
	// not been unsealed answers exactly like this.
	FakeCryptaServer server;
	server.Start([&](FakeConnection &connection, int) {
		server.Record(connection.ReadFrame());
		connection.WriteFrame("{\"schema\":\"CryptaWireManifest@v2\",\"status\":\"ok\",\"ok\":false,"
		                      "\"detail\":\"kek is sealed\"}");
	});
	DuckLakeCryptaProvider provider(server.Path(), "some-lake");
	auto message = ThrownMessage([&]() { provider.SelfTest(); });
	REQUIRE_THAT(message, Catch::Contains("did not report ok"));
	// The refusal carries what the service actually said, so an operator can tell
	// a sealed KEK from a wrong socket without reading the service's own log.
	REQUIRE_THAT(message, Catch::Contains("kek is sealed"));
	// `status` is "ok" on purpose: the response is NOT an error frame, so
	// ThrowIfError passes it through and only the self-test's own check can
	// refuse it. Without that, this case would be re-testing the error path.
	REQUIRE(server.Connections() == 1);
}

TEST_CASE("crypta: a health probe answered with an error frame fails the self-test",
          "[crypta][refusal][self_test]") {
	// Distinct from the case above, and the distinction is the whole point: there
	// the service answered "ok":false inside a SUCCESSFUL frame, here it answers
	// an ERROR frame. The two take different code paths out of SelfTest - the
	// first through the self-test's own check, this one through ThrowIfError
	// inside Health - and only this one unwinds an exception through Health.
	//
	// It is the shape crypta returns when the request is refused rather than when
	// the service is unwell, and an ATTACH must fail closed on both.
	FakeCryptaServer server;
	server.Start([](FakeConnection &connection, int) {
		connection.ReadFrame();
		connection.WriteFrame(ErrorResponse("health is not available to this caller"));
	});
	DuckLakeCryptaProvider provider(server.Path(), "some-lake");
	auto message = ThrownMessage([&]() { provider.SelfTest(); });
	REQUIRE_THAT(message, Catch::Contains("crypta refused the request"));
	REQUIRE_THAT(message, Catch::Contains("health is not available to this caller"));
	// And NOT the self-test's own wording: reporting "did not report ok" here
	// would send an operator looking at a sealed KEK when the service is telling
	// them the caller is not permitted.
	REQUIRE(message.find("did not report ok") == std::string::npos);
}

TEST_CASE("crypta: a service reporting ok passes the self-test", "[crypta][happy]") {
	FakeCryptaServer server;
	server.Start([](FakeConnection &connection, int) {
		connection.ReadFrame();
		connection.WriteFrame("{\"schema\":\"CryptaWireManifest@v2\",\"status\":\"ok\",\"ok\":true,"
		                      "\"kek\":\"yubihsm\"}");
	});
	DuckLakeCryptaProvider provider(server.Path(), "some-lake");
	// The health text is RETURNED rather than swallowed, which is what lets ATTACH
	// report what the lake is rooted in. A self-test that only threw or did not
	// would leave the operator no way to tell one crypta from another.
	REQUIRE_THAT(provider.SelfTest(), Catch::Contains("yubihsm"));
}

TEST_CASE("crypta: a multi-item wrap is one request with well-formed separators", "[crypta][happy]") {
	// The unwrap side of this is covered by the round-trip case below; the wrap
	// side was not, and its item separator is the difference between valid JSON
	// and a body crypta cannot parse. A commit writing two or more files is the
	// ordinary case, not an edge one - WrapKeys batches a whole commit.
	FakeCryptaServer server;
	server.Start([&](FakeConnection &connection, int) {
		server.Record(connection.ReadFrame());
		connection.WriteFrame(OkWrapResponse({"RExLone", "RExLtwo"}));
	});
	CryptaClient client(server.Path());
	vector<CryptaFileIdentity> identities {SampleIdentity("a.parquet"), SampleIdentity("b.parquet")};
	vector<string> deks {SampleDek('a'), SampleDek('b')};
	auto blobs = client.WrapBatch(identities, deks);
	REQUIRE(blobs.size() == 2);
	REQUIRE(blobs[0] == "RExLone");
	REQUIRE(blobs[1] == "RExLtwo");
	REQUIRE(server.Connections() == 1);
	auto request = server.Requests().at(0);
	REQUIRE_THAT(request, Catch::Contains("},{\"identity\":"));
	// Both files are in the ONE body, in order - not two calls, and not one file
	// silently dropped.
	REQUIRE_THAT(request, Catch::Contains("a.parquet"));
	REQUIRE_THAT(request, Catch::Contains("b.parquet"));
	REQUIRE(request.find("a.parquet") < request.find("b.parquet"));
}

TEST_CASE("crypta: a well-formed response round-trips", "[crypta][happy]") {
	auto dek_a = SampleDek('a');
	auto dek_b = SampleDek('b');
	FakeCryptaServer server;
	server.Start([&](FakeConnection &connection, int) {
		server.Record(connection.ReadFrame());
		connection.WriteFrame(OkUnwrapResponse({dek_a, dek_b}));
	});
	CryptaClient client(server.Path());
	vector<CryptaFileIdentity> identities {SampleIdentity("a"), SampleIdentity("b")};
	vector<string> blobs {"RExLone", "RExLtwo"};
	auto keys = client.UnwrapBatch(identities, blobs);
	REQUIRE(keys.size() == 2);
	REQUIRE(keys[0] == dek_a);
	REQUIRE(keys[1] == dek_b);
	REQUIRE(server.Connections() == 1);
}

TEST_CASE("crypta: LooksWrapped tells a blob from a plaintext key", "[crypta][happy]") {
	// A real wrapped blob is ~208-280 base64 characters. Nothing shorter than a
	// raw DEK's 44 can be one, so the prefix alone is not the test - see the
	// length-floor case below for why that matters.
	REQUIRE(CryptaClient::LooksWrapped("RExLMQAAAA" + string(256, 'A')));
	REQUIRE_FALSE(CryptaClient::LooksWrapped(""));
	REQUIRE_FALSE(CryptaClient::LooksWrapped("RExK"));
	REQUIRE_FALSE(CryptaClient::LooksWrapped("REx"));
	// A 24-character plaintext key, the shape a pre-envelope lake stores.
	REQUIRE_FALSE(CryptaClient::LooksWrapped("AAAAAAAAAAAAAAAAAAAAAA=="));
}

TEST_CASE("crypta: LooksWrapped does not misread a plaintext DEK that happens to "
          "start with the magic",
          "[crypta][refusal]") {
	// THE OVER-REFUSAL THIS FLOOR EXISTS TO PREVENT.
	//
	// LooksWrapped is now called on EVERY stored key of EVERY plain-ENCRYPTED
	// lake - the upstream, no-crypta path - because that is where the
	// unconfigured-reader refusal lives. A prefix-only test therefore has a
	// false-positive rate on random key material: a 32-byte CSPRNG DEK whose
	// first three bytes are 0x44 0x4C 0x4B base64-encodes to "RExL...", and
	// would be refused forever as "crypta-wrapped" on a lake that has no crypta
	// and never had any. The advice in that refusal - re-attach with the crypta
	// options - would be wrong AND unactionable, and the file unreadable.
	//
	// ~6e-8 per file is small and is NOT zero, and the failure is unrecoverable,
	// so the discriminator must be more than four characters.
	//
	// 44 characters is base64 of exactly 32 bytes, the largest DEK this fork
	// mints (94144c31). Anything at or below that cannot be a wrapped blob.
	const string plaintext_dek_that_looks_wrapped = "RExL" + string(40, 'A');
	REQUIRE(plaintext_dek_that_looks_wrapped.size() == 44);
	REQUIRE_FALSE(CryptaClient::LooksWrapped(plaintext_dek_that_looks_wrapped));

	// The 24-character pre-envelope shape, same prefix, same answer.
	REQUIRE_FALSE(CryptaClient::LooksWrapped("RExL" + string(20, 'A')));

	// One character past the floor is admitted: the floor must not be so greedy
	// that it starts rejecting genuine blobs.
	REQUIRE(CryptaClient::LooksWrapped("RExL" + string(41, 'A')));
}
