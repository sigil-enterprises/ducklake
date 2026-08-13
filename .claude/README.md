# ducklake (sigil-enterprises fork)

PRIVATE fork of duckdb/ducklake. Never cherry-pick fork-local files upstream.

## What this repo is

DuckLake: a data lake on Parquet with a metadata catalog (DuckDB / Postgres /
SQLite) that stores per-file and table-wide column statistics, partition values
and file metadata. This fork adds KMS-agnostic envelope encryption and, on an
enveloped lake, forbids column VALUES from reaching the catalog.

## The envelope and the confidentiality rule

- `DuckLakeEncryptionProvider` is the abstract KMS interface; the concrete
  provider is registered by a SEPARATE repo (`sigil-enterprises/crypta`). This
  repo alone has no `RegisterFactory` call, so `EncryptionProvider()` is always
  null here and no crypta test can run in this tree alone.
- Detection point: `DuckLakeCatalog::EncryptionProvider()` returns the provider,
  or null when no `ENCRYPTION_SOCKET` / `ENCRYPTION_LAKE_ID` is configured.

## Fork-local confidentiality guards

The invariant is one line: an enveloped lake writes no column VALUE into the
catalog. Two guards implement it, and they are the pattern any new guard must
follow.

1. Inlining refusal (NOT in this tree): a crypta lake forces
   `DataInliningRowLimit()` to 0 so small writes go to encrypted files instead
   of cleartext inlined rows.
2. Column-statistics redaction (this tree):
   - `DuckLakeColumnStats::RedactValues()` - the ONE primitive that decides what
     is value-bearing. Drops min/max and extra_stats; KEEPS value_count,
     null_count, column_size_bytes, contains_nan.
   - `DuckLakeTransaction::RedactStatsOnEnvelopedLake(file)` - gated on
     `EncryptionProvider()`, called at BOTH producers of a catalog data-file
     row: `AppendFiles` (INSERT/UPDATE/MERGE/CTAS/flush/add_data_files) and
     `AddCompaction` (merge_adjacent_files).

The rule for adding a new guard: one primitive + call sites at every producer,
each gated on `EncryptionProvider()`. A guard written at only one producer leaks
on the next.

## Build / test

Build inside the devcontainer only (`docker compose up app`); the image is
GCC 14 + vcpkg at the CI pin. See `.devcontainer/Dockerfile` for why GCC 14 is
load-bearing. The column-stats redaction is PROVEN by the bench repo
(`ducklake-bench`) use-case `column_stats_redacted_on_crypta_lake`, not by any
test in this tree - this repo cannot construct a crypta provider.
