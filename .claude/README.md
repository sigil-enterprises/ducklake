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

1. Inlining refusal (this tree, `ducklake_catalog.cpp`): a crypta lake forces
   `DataInliningRowLimit()` (both overloads) to 0 so small writes go to
   encrypted files instead of cleartext inlined rows. The gate sits on
   `DataInliningRowLimit` itself, not `GetInliningLimit`, because several call
   sites read the former directly and would otherwise bypass the guard.
2. Column-statistics redaction (this tree):
   - `DuckLakeColumnStats::RedactValues()` - the ONE primitive that decides what
     is value-bearing. Drops min/max and extra_stats; KEEPS value_count,
     null_count, column_size_bytes, contains_nan.
   - `DuckLakeTransaction::RedactStatsOnEnvelopedLake(file)` - gated on
     `EncryptionProvider()`, called at BOTH producers of a catalog data-file
     row: `AppendFiles` (INSERT/UPDATE/MERGE/CTAS/flush/add_data_files) and
     `AddCompaction` (merge_adjacent_files).
   - The transient `DuckLakeColumnStats::redacted` flag (never serialised)
     marks that RedactValues ran, so the commit-time table-wide merge can tell a
     redacted-EMPTY extra_stats (which must CLEAR the accumulated bound) from a
     legitimately-empty one. Geometry's extra_stats merges as a UNION (an empty
     bbox is the identity), so without the flag a stale pre-envelope bbox would
     survive every write; the flag makes a redacted file REPLACE instead.
     Variant merges as an intersection and clears on its own, so it does not
     depend on the flag.

The rule for adding a new guard: one primitive + call sites at every producer,
each gated on `EncryptionProvider()`. A guard written at only one producer leaks
on the next.

## Build / test

Build inside the devcontainer only (`docker compose up app`); the image is
GCC 14 + vcpkg at the CI pin. See `.devcontainer/Dockerfile` for why GCC 14 is
load-bearing. The inlining refusal and column-stats redaction are DECLARED by
the bench repo (`ducklake-bench`) use-cases `inlining_forced_off_on_crypta_lake`
and `column_stats_redacted_on_crypta_lake`, not by any test in this tree - this
repo cannot construct a crypta provider. "Declared", not "proven": those
use-cases only fire on the bench's live-stack replay, which is not yet wired to
CI (ducklake-bench#10).
