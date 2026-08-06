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

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

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

//! The `identity` object a reply echoes beside its value, spelled INDEPENDENTLY
//! of the client's own encoder.
//!
//! Two things about it are deliberate rather than incidental:
//!
//!   * The four members come out in a DIFFERENT ORDER than
//!     `CryptaClient::IdentityJson` writes them. A real service re-serialises the
//!     identity it parsed - crypta's `IdentityWire::from`, and the SQL fixture's
//!     python stand-in with `sort_keys=True` - so member order is not a property
//!     of the wire. A reader that compared the reply's BYTES to the bytes it
//!     sent would refuse both of them, and over-refusal on the read path locks
//!     an operator out of their own lake. Every canned reply built here
//!     therefore doubles as the control against that.
//!   * It escapes with its own escaper. Building the expectation with the
//!     client's own encoder would make a bug in that encoder invisible - the
//!     same reason `Base64Encode` above is written out by hand.
inline std::string JsonEscapeForEcho(const std::string &input) {
	std::string out;
	for (size_t i = 0; i < input.size(); i++) {
		auto c = static_cast<unsigned char>(input[i]);
		if (c == '"') {
			out += "\\\"";
		} else if (c == '\\') {
			out += "\\\\";
		} else if (c == '\b') {
			// `\b` and `\f` on purpose: these are the two control characters serde
			// writes as a SHORT escape where this client writes the six-character
			// numeric one. If the reader ever compared spellings, they are the
			// bytes that would show it.
			out += "\\b";
		} else if (c == '\f') {
			out += "\\f";
		} else if (c == '\n') {
			out += "\\n";
		} else if (c == '\r') {
			out += "\\r";
		} else if (c == '\t') {
			out += "\\t";
		} else if (c < 0x20) {
			char buffer[8];
			snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<int>(c));
			out += buffer;
		} else {
			out += static_cast<char>(c);
		}
	}
	return out;
}

inline std::string IdentityEcho(const duckdb::CryptaFileIdentity &identity) {
	std::ostringstream out;
	out << "{\"file_path\":\"" << JsonEscapeForEcho(identity.stored_path) << "\",\"file_kind\":\""
	    << (identity.is_delete_file ? "delete" : "data") << "\",\"table_id\":" << identity.table_id
	    << ",\"catalog_uuid\":\"" << JsonEscapeForEcho(identity.lake_id) << "\"}";
	return out.str();
}

//! 32 raw bytes, the size this fork mints (see commit 94144c31).
inline std::string SampleDek(char fill = 'k') {
	return std::string(32, fill);
}

//! A synthetic wrapped blob carrying `tag`, of REALISTIC LENGTH.
//!
//! Length is not cosmetic here. CryptaClient::LooksWrapped requires more than
//! 44 base64 characters - the size of a raw 32-byte DEK - before it will call
//! anything wrapped, because on the no-crypta read path a prefix-only test
//! would refuse a legitimate plaintext DEK that happened to start with the
//! magic. A fixture blob shorter than that floor is read as PLAINTEXT and
//! refused by UnwrapKey before the cache is ever reached, so a cache test using
//! one would be measuring the wrong refusal.
//!
//! Real crypta blobs run 208-280 characters. `tag` keeps distinct fixtures
//! distinct, which is all the cache-key tests need of the content.
inline std::string WrappedBlob(const std::string &tag) {
	return "RExL" + tag + std::string(64, 'A');
}

} // namespace ducklake_crypta_test
