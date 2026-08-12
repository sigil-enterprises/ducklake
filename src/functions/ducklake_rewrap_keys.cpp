//===----------------------------------------------------------------------===//
//                         DuckLake
//
// functions/ducklake_rewrap_keys.cpp
//
// The consumer half of a KMS key rotation: `CALL ducklake_rewrap_keys(lake)`.
//
// KMS-agnostic — calls the abstract EncryptionProvider interface, so every
// concrete provider (crypta, cloud KMS, local HSM) gets a sweep for free as
// long as it implements RewrapKeys.
//
//===----------------------------------------------------------------------===//
//
// WHY THIS EXISTS
// ---------------
// A rotation's sequence is: mint → serve both → SWEEP → retire. Without
// step 3, step 4 does not expire stale copies, it STRANDS LIVE ROWS: every
// `encryption_key` in the catalog still names the outgoing KEK, the KMS no
// longer holds it, and every read fails with an unknown-kek error. There is no
// recovery, because by design the plaintext DEK exists nowhere else.
//
// THE FOUR PROPERTIES THIS FILE IS BUILT AROUND
// ---------------------------------------------
//  1. NO PLAINTEXT DEK, ANYWHERE. The sweep never unwraps. It hands the KMS the
//     stored blob and takes back a re-wrapped one (RewrapKeys), so the DEK
//     never leaves the KMS, never enters this process, and never reaches a log,
//     a temp file, an error message or the catalog. The unwrap-then-wrap
//     alternative would have pulled the plaintext key of EVERY FILE IN THE LAKE
//     through this loop to accomplish an operation whose entire point is that
//     the DEK does not change.
//
//  2. IDENTITY IS PRESERVED, BY CONSTRUCTION. Every blob is bound to
//     (lake, table, kind, path). The identity handed to the KMS is built from
//     THE SAME ROW the blob was read from and the row is written back BY ITS
//     PRIMARY KEY, so there is no arrangement of the loop in which file A's key
//     is rewrapped under file B's identity. On the reply side the provider
//     verifies the identity it was asked about, so a reordered or substituted
//     reply cannot get past it either.
//
//  3. BOTH KEY COLUMNS, AND SUPERSEDED ROWS TOO. `ducklake_delete_file` carries
//     its own `encryption_key`, and a sweep that covered only `ducklake_data_file`
//     would leave every delete-file read broken at retirement. Rows with
//     `end_snapshot` set are still live for TIME TRAVEL and still name the old
//     KEK, so the walk is deliberately snapshot-BLIND: no `end_snapshot IS NULL`
//     filter anywhere. Both omissions are silent until the day the old KEK goes
//     away, which is the day nothing can be done about them.
//
//  4. RESUMABLE AND CRASH-SAFE, BY THE SHAPE OF THE WRITES rather than by a
//     checkpoint file.
//
//       * Each batch is committed on ITS OWN connection, in autocommit. A crash
//         leaves a committed PREFIX, never a torn row: the smallest unit anything
//         can observe is one row's `encryption_key` going from one intact blob to
//         another intact blob.
//       * A half-swept lake is FULLY READABLE, and this is a property of the
//         rotation rather than of this loop: during the overlap window the KMS
//         serves BOTH KEKs, so the swept rows unwrap under the incoming one and
//         the unswept rows under the outgoing one. That is why the sweep goes
//         BETWEEN "serve both" and "retire" and not anywhere else.
//       * There is NO cursor to persist, because the sweep is CONVERGENT. The KMS
//         answers `rewrapped: false` for a row already on the active KEK, so a
//         re-run walks the whole lake and writes nothing for the part already
//         done. Resuming is simply running it again. A cursor would be a second
//         source of truth about progress, and the catalog already is one.
//       * The write is a COMPARE-AND-SWAP - `WHERE <id> = ? AND encryption_key =
//         <the blob we read>` - so a row another writer changed under us is left
//         alone rather than clobbered with a blob rewrapped from a stale value.
//
// WHAT IT DOES NOT DO
// -------------------
// It takes no snapshot and writes no `ducklake_snapshot` row. Re-wrapping a key
// is not a data change: the files on disk are untouched, every row keeps its
// `begin_snapshot`/`end_snapshot`, and time travel reads exactly what it read
// before. A sweep that minted a snapshot would show up in every consumer's change
// feed as a modification that changed no data.
//
//===----------------------------------------------------------------------===//

#include "common/ducklake_util.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "functions/ducklake_table_functions.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_encryption_provider.hpp"
#include "storage/ducklake_transaction.hpp"

namespace duckdb {

//! One row of the sweep's report. Deliberately carries no key material of any
//! kind - not the old blob, not the new one. The path is the identity's own
//! `stored_path`, which is what makes the binding assertable from SQL.
struct RewrapReportRow {
	string file_kind;
	int64_t table_id = 0;
	int64_t file_id = 0;
	string stored_path;
	bool rewrapped = false;
};

struct RewrapKeysBindData : public TableFunctionData {
	explicit RewrapKeysBindData(Catalog &catalog) : catalog(catalog) {
	}

	Catalog &catalog;
	bool dry_run = false;
	idx_t batch_size = 128;
};

struct RewrapKeysGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;
	bool executed = false;
	vector<RewrapReportRow> report;
};

//! One of the two catalog tables the sweep walks. The pair is written down HERE,
//! once, and the loop below iterates it - so "did you remember the delete files"
//! is answered by a list rather than by a second copy of the body.
//!
//! That shape is deliberate and it is this repo's recurring defect it guards
//! against: a guard written for one call site reads as complete. A second
//! hand-written block for delete files would be free to drift from the first,
//! and the drift would be invisible until a retirement.
struct RewrapTarget {
	const char *table_name;
	const char *id_column;
	bool is_delete_file;
};

static const RewrapTarget REWRAP_TARGETS[] = {
    {"ducklake_data_file", "data_file_id", false},
    {"ducklake_delete_file", "delete_file_id", true},
};

static unique_ptr<FunctionData> DuckLakeRewrapKeysBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<string> &names) {
	auto &catalog = DuckLakeBaseMetadataFunction::GetCatalog(context, input.inputs[0]);
	auto &ducklake_catalog = catalog.Cast<DuckLakeCatalog>();
	auto result = make_uniq<RewrapKeysBindData>(catalog);

	for (auto &entry : input.named_parameters) {
		if (StringUtil::CIEquals(entry.first, "dry_run")) {
			result->dry_run = BooleanValue::Get(entry.second);
		} else if (StringUtil::CIEquals(entry.first, "batch_size")) {
			auto requested = entry.second.GetValue<int64_t>();
			if (requested <= 0) {
				throw InvalidInputException("ducklake_rewrap_keys: batch_size must be positive, got %lld",
				                            static_cast<long long>(requested));
			}
			result->batch_size = static_cast<idx_t>(requested);
		} else {
			throw InternalException("Unsupported named parameter for ducklake_rewrap_keys");
		}
	}

	// REFUSE A NON-ENVELOPED LAKE — it is a refusal rather than a no-op on purpose.
	//
	// A lake attached without ENCRYPTION_SOCKET / ENCRYPTION_LAKE_ID holds PLAINTEXT
	// keys in `encryption_key`, exactly as upstream does. There is nothing here for a
	// sweep to move, and every action this function could take on such a lake is
	// wrong: rewriting the column would destroy working keys, and returning zero
	// rows would tell an operator running a rotation that the sweep RAN and found
	// nothing - which is the exact reading under which they would then go and
	// retire a KEK. Silence is the dangerous answer here, so it says so instead.
	//
	// Raised at BIND, before any row is read or any socket is opened, so a
	// mis-aimed sweep costs nothing and touches nothing.
	if (!ducklake_catalog.EncryptionProvider()) {
		throw InvalidInputException(
		    "ducklake_rewrap_keys: this lake was attached without ENCRYPTION_SOCKET / ENCRYPTION_LAKE_ID, so its "
		    "encryption keys are plaintext and there is no KEK to rewrap them onto. Refusing rather "
		    "than reporting an empty sweep: an empty sweep reads as 'nothing left to do', which is exactly the "
		    "reading under which a KEK gets retired and every row in the lake is stranded");
	}

	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("file_kind");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("table_id");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("file_id");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("path");
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("rewrapped");
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> DuckLakeRewrapKeysInit(ClientContext &context,
                                                                   TableFunctionInitInput &input) {
	return make_uniq<RewrapKeysGlobalState>();
}

//! `"catalog"."schema"` - the same text `{METADATA_CATALOG}` expands to.
//!
//! Spelled out rather than reached through the metadata manager's substitution
//! because the sweep runs on its OWN connection (see below), so it never goes
//! near `DuckLakeMetadataManager::Query`.
static string MetadataCatalogPrefix(DuckLakeCatalog &catalog) {
	return DuckLakeUtil::SQLIdentifierToString(catalog.MetadataDatabaseName()) + "." +
	       DuckLakeUtil::SQLIdentifierToString(catalog.MetadataSchemaName());
}

static void RequireSuccess(QueryResult &result, const string &what) {
	if (result.HasError()) {
		throw IOException("ducklake_rewrap_keys: %s failed: %s", what, result.GetError());
	}
}

//! Sweep ONE of the two catalog tables, appending to `report`.
//!
//! Reads and writes go through `connection`, which is the caller's OWN
//! connection in AUTOCOMMIT - not the DuckLakeTransaction's, which sits inside
//! an explicit transaction that does not commit until the statement ends. That
//! difference is the whole of the crash-safety claim: on this connection each
//! batch's writes are durable the moment they return, so an interrupted sweep
//! leaves a committed prefix. On the shared connection an interrupted sweep
//! would leave NOTHING, and a lake with millions of files would never make
//! progress at all.
static void SweepTable(Connection &connection, DuckLakeCatalog &catalog, DuckLakeEncryptionProvider &provider,
                       const RewrapTarget &target, const RewrapKeysBindData &bind_data,
                       vector<RewrapReportRow> &report) {
	auto prefix = MetadataCatalogPrefix(catalog);
	// KEYSET pagination on the primary key, not OFFSET.
	//
	// The predicate and the ordering key are both untouched by what the sweep
	// writes - it only ever replaces `encryption_key` with another non-empty
	// blob - so paging by "id greater than the last one I saw" cannot skip a row
	// or serve one twice, and it does not degrade as the walk gets deeper. An
	// OFFSET walk would be correct here too, and quadratic.
	int64_t last_id = std::numeric_limits<int64_t>::min();
	bool first_page = true;
	while (true) {
		auto select = StringUtil::Format(
		    "SELECT %s, table_id, path, encryption_key FROM %s.%s WHERE encryption_key IS NOT NULL AND "
		    "encryption_key <> '' AND %s > %lld ORDER BY %s LIMIT %llu",
		    target.id_column, prefix, target.table_name, target.id_column,
		    static_cast<long long>(first_page ? std::numeric_limits<int64_t>::min() : last_id), target.id_column,
		    static_cast<uint64_t>(bind_data.batch_size));
		first_page = false;
		auto page = connection.Query(select);
		RequireSuccess(*page, StringUtil::Format("reading a page of %s", target.table_name));

		vector<int64_t> file_ids;
		vector<int64_t> table_ids;
		vector<string> stored_paths;
		vector<string> blobs;
		vector<DuckLakeFileIdentity> identities;
		for (auto &row : *page) {
			auto file_id = row.GetValue<int64_t>(0);
			auto table_id = row.GetValue<int64_t>(1);
			auto stored_path = row.GetValue<string>(2);
			auto blob = row.GetValue<string>(3);
			// A PLAINTEXT row on an enveloped lake is refused, not walked past.
			//
			// It is either a leftover from before the envelope or a downgrade
			// attempt — the provider's UnwrapKey refuses it in those words on every
			// read — and either way there is no KEK it belongs to, so a sweep cannot
			// move it. Skipping it quietly is the failure this whole function is
			// about, one row wide: the sweep would report success, the operator would
			// retire the outgoing KEK, and this row would be the one that is stranded.
			// Naming it is the only useful answer.
			if (!DuckLakeEncryptionProvider::LooksWrapped(blob)) {
				throw IOException(
				    "ducklake_rewrap_keys: %s file %s (table %lld) carries a plaintext encryption key, not a "
				    "KMS-wrapped one. Refusing to report this sweep as complete: there is no KEK this row can "
				    "be moved from, so retiring the outgoing KEK after a sweep that walked past it would strand "
				    "it forever. Resolve the row first, then re-run the sweep",
				    target.is_delete_file ? "delete" : "data", stored_path, static_cast<long long>(table_id));
			}
			// The alphabet floor, in the same order and for the same reason
			// the provider's UnwrapKey asks it: `LooksWrapped` above is a
			// four-character discriminator between "wrapped" and "plaintext key" and
			// nothing more, and leaning on it as validation is what let a catalog
			// value carrying JSON punctuation reach the request builder. A
			// value outside the base64 alphabet can never be a wrapped key, so it
			// has no business on the wire - this is the layer that stops it being
			// sent at all.
			if (!DuckLakeEncryptionProvider::IsBase64(blob)) {
				throw IOException("ducklake_rewrap_keys: %s file %s (table %lld) carries an encryption key that is "
				                  "not base64. A wrapped key that cannot even be decoded is either corruption or an "
				                  "attempt to write something other than a key into the catalog, and neither is "
				                  "worth sending to the KMS",
				                  target.is_delete_file ? "delete" : "data", stored_path,
				                  static_cast<long long>(table_id));
			}
			file_ids.push_back(file_id);
			table_ids.push_back(table_id);
			stored_paths.push_back(stored_path);
			blobs.push_back(blob);
			// The identity is built from THE SAME ROW the blob came from - its
			// table id, its stored path, and the kind fixed by which of the two
			// tables we are walking. `stored_path` is the `path` COLUMN, never a
			// path resolved against the table's data path, because that is what
			// the key was bound to when it was minted.
			identities.push_back(
			    catalog.BuildEncryptionIdentity(TableIndex(static_cast<idx_t>(table_id)), stored_path, target.is_delete_file));
			last_id = file_id;
		}
		if (identities.empty()) {
			return;
		}

		auto results = provider.RewrapKeys(identities, blobs);
		if (results.size() != identities.size()) {
			throw IOException("ducklake_rewrap_keys: KMS answered %llu items for %llu files",
			                  static_cast<uint64_t>(results.size()), static_cast<uint64_t>(identities.size()));
		}

		for (idx_t i = 0; i < results.size(); i++) {
			RewrapReportRow entry;
			entry.file_kind = target.is_delete_file ? "delete" : "data";
			entry.table_id = table_ids[i];
			entry.file_id = file_ids[i];
			entry.stored_path = stored_paths[i];
			entry.rewrapped = false;
			// `rewrapped` is what decides whether a row is rewritten, and the KMS is
			// the only thing that can answer it - it is the only party that knows
			// which KEK minted the blob. A converged sweep therefore writes nothing
			// at all, which is what makes re-running one free and makes "resume"
			// mean nothing more than "run it again".
			if (results[i].rewrapped && !bind_data.dry_run) {
				// COMPARE-AND-SWAP. The row is written by its PRIMARY KEY - so the
				// blob can only ever land on the row it was read from - and only
				// while `encryption_key` is still the value that was sent to the KMS.
				// If another writer replaced the row underneath us its new blob
				// stands, and this pass simply reports the row as not moved; the
				// next pass picks it up. Without the second predicate the sweep
				// would overwrite a fresh key with one rewrapped from a stale read.
				auto update = StringUtil::Format(
				    "UPDATE %s.%s SET encryption_key = %s WHERE %s = %lld AND encryption_key = %s", prefix,
				    target.table_name, DuckLakeUtil::WrappedEncryptionKeyLiteral(results[i].wrapped, true),
				    target.id_column, static_cast<long long>(file_ids[i]),
				    DuckLakeUtil::SQLLiteralToString(blobs[i]));
				auto written = connection.Query(update);
				RequireSuccess(*written, StringUtil::Format("rewriting a key in %s", target.table_name));
				entry.rewrapped = true;
			} else if (results[i].rewrapped) {
				// dry_run: the KMS was asked and said it WOULD move this row. Report
				// it as such and write nothing.
				entry.rewrapped = true;
			}
			report.push_back(entry);
		}
	}
}

static void DuckLakeRewrapKeysExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->Cast<RewrapKeysBindData>();
	auto &state = data_p.global_state->Cast<RewrapKeysGlobalState>();

	if (!state.executed) {
		auto &ducklake_catalog = data.catalog.Cast<DuckLakeCatalog>();
		auto provider = ducklake_catalog.EncryptionProvider();
		if (!provider) {
			// Unreachable through bind, which refuses first. Kept because "the bind
			// already checked" is precisely the reasoning that produces a guard
			// that lives at one site and is assumed at another.
			throw InvalidInputException("ducklake_rewrap_keys: this lake has no encryption provider");
		}
		// The sweep's OWN connection - see SweepTable. Autocommit, so each
		// statement is durable when it returns.
		auto &db = DatabaseInstance::GetDatabase(context);
		Connection connection(db);
		// BOTH TABLES, from one list, in one loop. Not two hand-written blocks.
		for (auto &target : REWRAP_TARGETS) {
			SweepTable(connection, ducklake_catalog, *provider, target, data, state.report);
		}
		state.executed = true;
	}

	idx_t count = 0;
	while (state.offset < state.report.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = state.report[state.offset++];
		output.SetValue(0, count, Value(entry.file_kind));
		output.SetValue(1, count, Value::BIGINT(entry.table_id));
		output.SetValue(2, count, Value::BIGINT(entry.file_id));
		output.SetValue(3, count, Value(entry.stored_path));
		output.SetValue(4, count, Value::BOOLEAN(entry.rewrapped));
		count++;
	}
	output.SetCardinality(count);
}

DuckLakeRewrapKeysFunction::DuckLakeRewrapKeysFunction()
    : TableFunction("ducklake_rewrap_keys", {LogicalType::VARCHAR}, DuckLakeRewrapKeysExecute, DuckLakeRewrapKeysBind,
                    DuckLakeRewrapKeysInit) {
	named_parameters["dry_run"] = LogicalType::BOOLEAN;
	named_parameters["batch_size"] = LogicalType::BIGINT;
}

} // namespace duckdb
