//===----------------------------------------------------------------------===//
//                         DuckLake (sigil fork)
//
// test/cpp/crypta/crypta_test_support.hpp
//
// PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.
//===----------------------------------------------------------------------===//

#pragma once

#include "catch.hpp"
#include "crypta/crypta_client.hpp"
#include "fake_crypta_server.hpp"

#include <functional>
#include <string>

namespace ducklake_crypta_test {

//! Run `fn` and return the message it threw, or the empty string if it did not
//! throw at all.
//!
//! Every case in this suite asserts that the client REFUSES something, and the
//! failure mode that matters is "it did not refuse". Returning "" for a
//! non-throwing call makes that failure land on the same assertion as a wrong
//! message, so a mutant that deletes a guard reddens the case it is supposed to
//! redden rather than aborting the run with an unexpected-exception error.
inline std::string ThrownMessage(const std::function<void()> &fn) {
	try {
		fn();
	} catch (const std::exception &e) {
		return e.what();
	}
	return "";
}

//! A plausible identity, so a test that is not about identities does not have
//! to invent one.
inline duckdb::CryptaFileIdentity SampleIdentity(const std::string &path = "t/f.parquet") {
	duckdb::CryptaFileIdentity identity;
	identity.lake_id = "test-lake";
	identity.table_id = 7;
	identity.is_delete_file = false;
	identity.stored_path = path;
	return identity;
}

//! 32 raw bytes, the size this fork mints (see commit 94144c31).
inline std::string SampleDek(char fill = 'k') {
	return std::string(32, fill);
}

} // namespace ducklake_crypta_test
