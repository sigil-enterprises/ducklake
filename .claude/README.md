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
catalog. Three guards implement it, and they are the pattern any new guard must
follow.

1. Inlining refusal (this tree, `ducklake_catalog.cpp`): an enveloped lake forces
   `DataInliningRowLimit()` (both overloads) to 0 so small writes go to
   encrypted files instead of cleartext inlined rows. The gate sits on
   `DataInliningRowLimit` itself, not `GetInliningLimit`, because several call
   sites read the former directly and would otherwise bypass the guard. That is
   the SILENT half, and it is the only half that fires for the shipped default.
   The LOUD half refuses an EXPLICIT non-zero limit, so an operator who spelled
   one out learns it did not take effect rather than silently getting 0: at
   ATTACH (the catalog constructor) and at `ducklake_set_option` (in the value
   branch, before scope resolution, so one check covers global, schema and table
   scope alike). Setting the limit to 0 stays legal at both surfaces.
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
3. Resolved-to-unencrypted refusal (`ducklake_catalog.cpp`, `FinalizeLoad`): a
   lake attached with `ENCRYPTION_SOCKET` but `ENCRYPTED` omitted (AUTOMATIC)
   that RESOLVES to unencrypted is refused. It cannot live in the constructor -
   at construction the mode is still AUTOMATIC, and only the initializer
   resolves it (`InitializeNewDuckLake` defaults a fresh lake to UNENCRYPTED;
   `LoadExistingDuckLake` adopts what the catalog records) - so it runs
   immediately after `initializer.Initialize()`. It tests the RESOLVED mode
   rather than banning AUTOMATIC, so an existing enveloped lake re-attached
   without repeating `ENCRYPTED` still works.

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
