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

bool CryptaClient::IsBase64(const string &value) {
	// The ALPHABET only, deliberately - not the length, and not the padding.
	//
	// `LooksWrapped` is a four-character discriminator between "wrapped" and
	// "plaintext key", and it is fine at that job; what it is NOT is validation,
	// and it was being leaned on as if it were. This is the validation: a value
	// carrying a character outside the base64 alphabet can never decode to a
	// wrapped key, so nothing is lost by refusing it - and the characters it
	// excludes are exactly the ones that end a JSON string early.
	//
	// Length and padding are crypta's business. A rule here would refuse blobs
	// that are perfectly forwardable, and over-refusal on the read path locks an
	// operator out of their own lake.
	for (auto c : value) {
		auto u = static_cast<unsigned char>(c);
		bool in_alphabet = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '+' ||
		                   u == '/' || u == '=';
		if (!in_alphabet) {
			return false;
		}
	}
	return true;
}

//! JSON string escaping. Only the two characters that can break a JSON string,
//! plus control characters, need handling.
//!
//! Applied to EVERY value interpolated into a frame, including the wrapped blob.
//! It used to skip the blob on the reasoning that base64 needs no escaping -
//! true of base64, and beside the point: the blob is a catalog column value, and
//! nothing guarantees a catalog column holds base64. That is issue #24, and it
//! is why "this value is a safe shape" is asserted by `IsBase64` where the value
//! enters, not assumed by the encoder that writes it out.
//!
//! `stored_path` is the other attacker-influenced value, and it is not optional
//! either - but be precise about HOW it is reached, because three
//! plausible-sounding routes are all dead ends and citing any of them would
//! misdescribe the threat:
//!
//!  - NOT a table name. Measured: a table named `weird|name` yields a generated
//!    basename. `CanGeneratePathFromName` rejects any character outside
//!    alphanumerics, `_` and `-`, so such a name is substituted by the table UUID
//!    and reaches the path NOWHERE - not merely in a stripped directory part.
//!  - NOT `ducklake_add_data_files`. It does store an operator-supplied path
//!    verbatim, but the row it writes carries NO encryption key: `AddFileToTable`
//!    never sets one, both wrap sites skip empty-key files, and on read
//!    `ReadDataFile` throws "Database is encrypted, but file ... does not have an
//!    encryption key" BEFORE the identity is built. So that row never reaches this
//!    function at all.
//!  - NOT a hive partition value: `HivePartitioning::Escape` is
//!    `StringUtil::URLEncode`, which keeps only unreserved characters and `/`.
//!
//! The route that IS real is the threat model the envelope already assumes: an
//! attacker with catalog write access sets both `path` and a wrapped
//! `encryption_key` on the same `ducklake_data_file` row. That gives full control
//! of `stored_path`, so an unescaped `"` here would inject into the request frame.
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

//===----------------------------------------------------------------------===//
// Reading crypta's reply
//
// This used to be one narrow scan for `"<field>":"`, collecting every value it
// found in order and handing them back POSITIONALLY. That is the defect #31
// names, and it has two halves.
//
// The BINDING half: crypta already says which file each value belongs to. Its
// reply item is a `PlainKey` / `WrappedKeyEntry`, an identity beside the value,
// and the client threw the identity away and zipped by array index instead. The
// count check (#24) bounded that, but a count is a LENGTH check standing in for
// a BINDING check: it catches only a misalignment that changes the number of
// items, and a REORDER preserves it exactly. A reordered reply therefore handed
// a file another file's DEK - a wrong-key defect, not an availability one.
//
// The STRUCTURE half: a binding is only worth what the reader's item boundaries
// are worth. A flat scan and an item-wise walk disagree about which text is a
// value, and every place they disagree is a place the check can be walked
// around - a `"dek":"..."` nested INSIDE the echoed identity object, a second
// `dek` member beside the first, an item carrying none at all so the next one's
// value slides into its place. So the value and the identity it is bound to are
// now read out of the SAME structurally-delimited item, by the same walk. A
// binding read from a different slice than the value it binds is not a binding.
//
// Still not a general JSON parser, and deliberately not: it walks objects,
// strings and escapes because it has to, and refuses anything it would have to
// guess at.
//===----------------------------------------------------------------------===//

static bool IsJsonSpace(char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

//! Advance past ONE JSON value starting at `at`, reporting the index just past
//! it. False when the value does not end inside `text`.
//!
//! String-, escape- and nesting-aware. That is the whole difference between
//! reading a reply and grepping one: an escaped quote inside a value closes
//! nothing, and a brace inside a nested object is not the enclosing object's.
static bool ScanJsonValue(const string &text, idx_t at, idx_t &end_out) {
	idx_t depth = 0;
	bool in_string = false;
	bool escaped = false;
	for (idx_t i = at; i < text.size(); i++) {
		char c = text[i];
		if (in_string) {
			if (escaped) {
				escaped = false;
			} else if (c == '\\') {
				escaped = true;
			} else if (c == '"') {
				in_string = false;
				if (depth == 0) {
					end_out = i + 1;
					return true;
				}
			}
			continue;
		}
		if (c == '"') {
			in_string = true;
		} else if (c == '{' || c == '[') {
			depth++;
		} else if (c == '}' || c == ']') {
			if (depth == 0) {
				// The ENCLOSING object or array closed, so a bare value - a number,
				// a boolean, a null - ended just before it.
				end_out = i;
				return true;
			}
			depth--;
			if (depth == 0) {
				end_out = i + 1;
				return true;
			}
		} else if (c == ',' && depth == 0) {
			end_out = i;
			return true;
		}
	}
	return false;
}

//! Decode a JSON string LITERAL - quotes included - into the bytes it stands
//! for. False when it is not a string, or carries an escape this client will not
//! guess at.
static bool DecodeJsonString(const string &literal, string &out) {
	if (literal.size() < 2 || literal[0] != '"' || literal[literal.size() - 1] != '"') {
		return false;
	}
	string decoded;
	idx_t last = literal.size() - 1;
	for (idx_t i = 1; i < last; i++) {
		char c = literal[i];
		if (c != '\\') {
			decoded += c;
			continue;
		}
		if (i + 1 >= last) {
			return false;
		}
		char escape = literal[i + 1];
		i++;
		switch (escape) {
		case '"':
			decoded += '"';
			break;
		case '\\':
			decoded += '\\';
			break;
		case '/':
			decoded += '/';
			break;
		case 'b':
			decoded += '\b';
			break;
		case 'f':
			decoded += '\f';
			break;
		case 'n':
			decoded += '\n';
			break;
		case 'r':
			decoded += '\r';
			break;
		case 't':
			decoded += '\t';
			break;
		case 'u': {
			if (i + 4 >= last) {
				return false;
			}
			uint32_t code = 0;
			for (idx_t digit_at = 1; digit_at <= 4; digit_at++) {
				auto hex = static_cast<unsigned char>(literal[i + digit_at]);
				uint32_t digit;
				if (hex >= '0' && hex <= '9') {
					digit = static_cast<uint32_t>(hex - '0');
				} else if (hex >= 'a' && hex <= 'f') {
					digit = static_cast<uint32_t>(hex - 'a' + 10);
				} else if (hex >= 'A' && hex <= 'F') {
					digit = static_cast<uint32_t>(hex - 'A' + 10);
				} else {
					return false;
				}
				code = (code << 4) | digit;
			}
			// A JSON writer \u-escapes CONTROL characters and emits everything above
			// 0x1f as raw UTF-8 - crypta's serde does, this client's own JsonEscape
			// does. So an escape naming anything wider than one byte is not
			// something a healthy reply contains, and GUESSING at it - a lone
			// surrogate, a codepoint this client would have to re-encode - would
			// mean comparing an identity against bytes we invented. Refuse instead:
			// a refused batch is recoverable, a mis-decoded identity is not.
			if (code > 0xFF) {
				return false;
			}
			decoded += static_cast<char>(code);
			i += 4;
			break;
		}
		default:
			return false;
		}
	}
	out = decoded;
	return true;
}

//! Parse a JSON integer literal. Strict: digits and an optional leading `-`,
//! nothing else, and it must fit in an int64.
static bool ParseJsonInteger(const string &text, int64_t &out) {
	//! |INT64_MIN|, the largest magnitude an int64 can carry.
	static const uint64_t LIMIT = 9223372036854775808ULL;
	if (text.empty()) {
		return false;
	}
	bool negative = text[0] == '-';
	idx_t at = negative ? 1 : 0;
	if (at >= text.size()) {
		return false;
	}
	uint64_t magnitude = 0;
	for (idx_t i = at; i < text.size(); i++) {
		if (text[i] < '0' || text[i] > '9') {
			return false;
		}
		auto digit = static_cast<uint64_t>(text[i] - '0');
		if (magnitude > (LIMIT - digit) / 10) {
			return false;
		}
		magnitude = magnitude * 10 + digit;
	}
	if (negative) {
		if (magnitude == LIMIT) {
			out = -9223372036854775807LL - 1;
			return true;
		}
		out = -static_cast<int64_t>(magnitude);
		return true;
	}
	if (magnitude >= LIMIT) {
		return false;
	}
	out = static_cast<int64_t>(magnitude);
	return true;
}

//! One top-level member of a JSON object: its decoded NAME, and the RAW slice of
//! its value.
struct JsonMember {
	string name;
	string value;
};

//! The TOP-LEVEL members of a JSON object, in order.
//!
//! Nested objects and arrays are skipped whole rather than descended into, which
//! is the property the whole binding rests on: an item's `dek` is the item's own
//! member, never one buried inside the identity object beside it. A reader that
//! searched the item's TEXT would find either, and the attacker would pick.
static vector<JsonMember> SplitObjectMembers(const string &object, const string &what) {
	if (object.size() < 2 || object[0] != '{') {
		throw IOException("crypta response carries %s that is not an object", what);
	}
	vector<JsonMember> members;
	idx_t i = 1;
	while (i < object.size()) {
		while (i < object.size() && (IsJsonSpace(object[i]) || object[i] == ',')) {
			i++;
		}
		if (i >= object.size() || object[i] == '}') {
			break;
		}
		if (object[i] != '"') {
			throw IOException("crypta response carries a member of %s whose name is not a string", what);
		}
		idx_t name_end;
		if (!ScanJsonValue(object, i, name_end)) {
			throw IOException("crypta response is truncated inside a member name of %s", what);
		}
		JsonMember member;
		if (!DecodeJsonString(object.substr(i, name_end - i), member.name)) {
			throw IOException("crypta response carries a member name of %s this client will not decode", what);
		}
		i = name_end;
		while (i < object.size() && IsJsonSpace(object[i])) {
			i++;
		}
		if (i >= object.size() || object[i] != ':') {
			throw IOException("crypta response carries a member of %s with no value", what);
		}
		i++;
		while (i < object.size() && IsJsonSpace(object[i])) {
			i++;
		}
		idx_t value_end;
		if (!ScanJsonValue(object, i, value_end)) {
			throw IOException("crypta response is truncated inside a member value of %s", what);
		}
		member.value = object.substr(i, value_end - i);
		while (!member.value.empty() && IsJsonSpace(member.value[member.value.size() - 1])) {
			member.value.resize(member.value.size() - 1);
		}
		members.push_back(member);
		i = value_end;
	}
	return members;
}

//! The value of the ONE member named `name`.
//!
//! A DUPLICATE is a refusal, not a first-wins or last-wins choice, and that is
//! load-bearing rather than pedantic: under a first-wins reader a hostile reply
//! echoes the identity the caller expects and puts a SECOND `dek` beside it, and
//! which member the reader happens to pick becomes the attacker's to choose.
//! crypta's own parser refuses a duplicate member on the way in (measured in
//! #28), so refusing one on the way back costs a healthy service nothing.
static const string &RequiredMember(const vector<JsonMember> &members, const string &name, const string &what) {
	const JsonMember *found = nullptr;
	for (idx_t i = 0; i < members.size(); i++) {
		if (members[i].name != name) {
			continue;
		}
		if (found) {
			throw IOException("crypta answered %s with more than one %s member", what, name);
		}
		found = &members[i];
	}
	if (!found) {
		throw IOException("crypta answered %s with no %s member", what, name);
	}
	return found->value;
}

static string RequiredStringMember(const vector<JsonMember> &members, const string &name, const string &what) {
	string value;
	if (!DecodeJsonString(RequiredMember(members, name, what), value)) {
		throw IOException("crypta answered %s with a %s that is not a JSON string this client can decode", what, name);
	}
	return value;
}

//! Split the top-level objects out of a reply's `items` array.
//!
//! It FAILS rather than guesses. An items array that never closes is a truncated
//! reply, and the refusal for that lives HERE rather than in the field reader it
//! used to live in (#31): once the walk is string-aware, a value whose quote
//! never closes swallows the rest of the frame, so "truncated inside a value"
//! and "truncated inside the array" stopped being two conditions - the array is
//! the one that can still be observed.
static vector<string> SplitReplyItems(const string &response) {
	static const string OPENER = "\"items\":[";
	auto opener_at = response.find(OPENER);
	if (opener_at == string::npos) {
		throw IOException("crypta response carries no items array");
	}
	vector<string> items;
	idx_t depth = 0;
	bool in_string = false;
	bool escaped = false;
	bool closed = false;
	bool in_item = false;
	idx_t item_start = 0;
	for (idx_t i = opener_at + OPENER.size(); i < response.size(); i++) {
		char c = response[i];
		if (in_string) {
			if (escaped) {
				escaped = false;
			} else if (c == '\\') {
				escaped = true;
			} else if (c == '"') {
				in_string = false;
			}
			continue;
		}
		if (c == '"') {
			in_string = true;
		} else if (c == '{' || c == '[') {
			if (depth == 0 && c == '{') {
				in_item = true;
				item_start = i;
			}
			depth++;
		} else if (c == '}') {
			if (depth == 0) {
				throw IOException("crypta response closes an object that never opened, inside its items array");
			}
			depth--;
			if (depth == 0 && in_item) {
				items.push_back(response.substr(item_start, i - item_start + 1));
				in_item = false;
			}
		} else if (c == ']') {
			if (depth == 0) {
				closed = true;
				break;
			}
			depth--;
		}
	}
	if (!closed) {
		throw IOException("crypta response is truncated inside its items array");
	}
	return items;
}

//! THE BINDING, on the way back. Issue #31.
//!
//! Compared as DECODED VALUES, never as bytes, and that is not fastidiousness.
//! There is more than one correct way to write the same string in JSON: crypta's
//! serde escapes a backspace as `\b` where this client's own encoder writes the
//! six-character numeric form, and a re-serialising service is free to emit the
//! four members in any order it likes - the SQL fixture's python stand-in sorts
//! them. A comparison against the bytes this client SENT would refuse a healthy
//! service for spelling the same identity differently, and over-refusal on the
//! read path locks an operator out of their own lake. What the key is bound to
//! is the four VALUES; their spelling is not part of the binding.
static void RequireEchoedIdentity(const vector<JsonMember> &members, const CryptaFileIdentity &expected,
                                  const string &what) {
	auto identity = SplitObjectMembers(RequiredMember(members, "identity", what), what + "'s identity");
	auto catalog_uuid = RequiredStringMember(identity, "catalog_uuid", what);
	auto file_kind = RequiredStringMember(identity, "file_kind", what);
	auto file_path = RequiredStringMember(identity, "file_path", what);
	int64_t table_id = 0;
	if (!ParseJsonInteger(RequiredMember(identity, "table_id", what), table_id)) {
		throw IOException("crypta answered %s with a table_id that is not an integer", what);
	}
	string expected_kind = expected.is_delete_file ? "delete" : "data";
	// ALL FOUR, and each on its own. Three of them checked and one taken on trust
	// is the same defect one layer down - a delete file's key row and a data
	// file's are not interchangeable, and neither are two lakes' - so a narrowed
	// comparison is its own mutant in the roster rather than a matter of taste.
	if (catalog_uuid != expected.lake_id || table_id != expected.table_id || file_kind != expected_kind ||
	    file_path != expected.stored_path) {
		throw IOException("crypta answered %s with a different file's identity: asked for lake %s table %lld %s file "
		                  "%s, answered lake %s table %lld %s file %s. The reply is matched to the request by the "
		                  "identity crypta echoes, never by position",
		                  what, expected.lake_id, static_cast<long long>(expected.table_id), expected_kind,
		                  expected.stored_path, catalog_uuid, static_cast<long long>(table_id), file_kind, file_path);
	}
}

vector<string> CryptaClient::ExtractBoundBase64Field(const string &response, const string &field,
                                                     const vector<CryptaFileIdentity> &identities) {
	auto items = SplitReplyItems(response);
	// The COUNT check, kept exactly where #24 put it and for the reason it was put
	// there. It is not made redundant by the binding below: it is what refuses a
	// reply that carries the RIGHT identities and the wrong number of them, and it
	// is the check that answers first for a reply with nothing in it at all, where
	// there is no item to bind anything to.
	if (items.size() != identities.size()) {
		throw IOException("crypta returned %llu %s values for %llu requested items",
		                  static_cast<uint64_t>(items.size()), field, static_cast<uint64_t>(identities.size()));
	}
	vector<string> out;
	out.reserve(items.size());
	for (idx_t i = 0; i < items.size(); i++) {
		auto what = StringUtil::Format("reply item %llu", static_cast<uint64_t>(i));
		auto members = SplitObjectMembers(items[i], what);
		RequireEchoedIdentity(members, identities[i], what);
		string value;
		if (!DecodeJsonString(RequiredMember(members, field, what), value)) {
			throw IOException("crypta answered %s with a %s value that is not a JSON string", what, field);
		}
		// The DECODED value, not the raw slice: the decoded bytes are what reaches
		// the catalog and the decoder, so they are what has to be in the alphabet.
		// Fails CLOSED, and refuses the WHOLE batch rather than dropping the bad
		// value: a partial answer would leave some files keyed and the rest not,
		// which is a worse outcome than a refused commit.
		if (!IsBase64(value)) {
			throw IOException("crypta returned a %s value that is not base64 - the reply is either corrupt or is not "
			                  "crypta on the other end of this socket, and neither is worth writing to the catalog",
			                  field);
		}
		out.push_back(value);
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
	// Bound to the identities, not zipped onto them. The wrap reply echoes the
	// identity too (`WrappedKeyEntry`), and a mis-zipped wrap is the same defect
	// with a slower fuse: file A's row stores the blob crypta minted for B, and
	// the lake reads fine until the day A is read and its key will not unwrap.
	return ExtractBoundBase64Field(response, "wrapped", identities);
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
		// The blob is escaped for the same reason every identity field beside it
		// is: it is a catalog column value, so it is attacker-influenced, and
		// splicing it raw let it close its own JSON string and write protocol -
		// a second `identity` member, or a whole extra array element. The caller
		// asks for N files; the frame must say N files.
		body += StringUtil::Format("{\"identity\":%s,\"wrapped\":\"%s\"}", IdentityJson(identities[i]),
		                           JsonEscape(blobs[i]));
	}
	body += "]}";

	auto response = Request(body);
	ThrowIfError(response);
	auto encoded = ExtractBoundBase64Field(response, "dek", identities);

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
