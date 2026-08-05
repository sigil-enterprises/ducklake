//===----------------------------------------------------------------------===//
//                         DuckLake (sigil fork)
//
// test/cpp/crypta/crypta_test_main.cpp
//
// PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.
//===----------------------------------------------------------------------===//

#define CATCH_CONFIG_RUNNER
#include "catch.hpp"

#include <csignal>

int main(int argc, char *argv[]) {
	// SIGPIPE is ignored so the write-failure case can be OBSERVED rather than
	// killing the runner.
	//
	// Stated honestly, because it is a difference from production: what happens
	// when crypta resets the connection mid-write depends on the HOST process's
	// SIGPIPE disposition, which DuckLake does not set. The DuckDB CLI installs a
	// handler (duckdb/tools/shell/shell.cpp), so there the write returns EPIPE and
	// `CryptaClient` raises its IOException as this suite asserts. A plain
	// libduckdb embedding that sets nothing takes the default disposition and the
	// PROCESS DIES instead. That gap is reported on the issue, not papered over
	// here - this main only makes the client's own branch observable.
	signal(SIGPIPE, SIG_IGN);
	return Catch::Session().run(argc, argv);
}
