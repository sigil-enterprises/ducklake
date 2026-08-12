//===----------------------------------------------------------------------===//
//                         DuckDB
//
// common/ducklake_options.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "common/ducklake_encryption.hpp"
#include "common/ducklake_version.hpp"
#include "common/index.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/access_mode.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"

namespace duckdb {

using option_map_t = unordered_map<string, string>;

struct DuckLakeOptions {
  string metadata_database;
  string metadata_path;
  string metadata_schema;
  string data_path;
  bool override_data_path = false;
  AccessMode access_mode = AccessMode::AUTOMATIC;
  DuckLakeEncryption encryption = DuckLakeEncryption::AUTOMATIC;
  bool create_if_not_exists = true;
  bool automatic_migration = false;
  bool hide_metadata_catalog = true;
  unique_ptr<BoundAtClause> at_clause;
  case_insensitive_map_t<Value> metadata_parameters;
  option_map_t config_options;
  map<SchemaIndex, option_map_t> schema_options;
  map<TableIndex, option_map_t> table_options;
  idx_t busy_timeout = 5000;
  DuckLakeVersion ducklake_version = DuckLakeVersion::UNSET;

  //! KMS envelope encryption: Unix socket of the key service. NOT SUPPLIED
  //! = no envelope, and the encryption_key column holds a plaintext key
  //! exactly as upstream. Supplied but empty is a misconfiguration and is
  //! refused - see the flags below.
  string encryption_socket;
  //! Compartment name that scopes every key in this lake. Required whenever
  //! encryption_socket is set; without it keys are interchangeable between
  //! lakes.
  string encryption_lake_id;
  //! Whether each option was SUPPLIED at ATTACH, which is NOT the same as
  //! it being non-empty - and the difference is the whole of #19.
  //! `ENCRYPTION_SOCKET ''`, the shape an unexpanded `${VAR}` takes in a
  //! templated ATTACH, is indistinguishable from "no envelope wanted" if you
  //! only look at the value, so it used to silently disable the envelope and
  //! write PLAINTEXT per-file keys.
  bool encryption_socket_supplied = false;
  bool encryption_lake_id_supplied = false;
  //! How long an unwrapped plaintext DEK may stay in the reader's cache.
  //! Held with a `_supplied` flag rather than pre-set to the default so the
  //! default lives in exactly ONE place - DuckLakeEncryptionProvider, which
  //! is also where the range is enforced.
  int64_t encryption_cache_ttl_seconds = 0;
  bool encryption_cache_ttl_seconds_supplied = false;
};

} // namespace duckdb
