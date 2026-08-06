# Changelog

This file covers the **sigil fork's own release line** - the crypta
envelope-encryption work carried on `release/v1.5-variegata`. It does not
restate upstream `duckdb/ducklake` releases, which have their own history.

## Versioning

The fork is versioned **fork-native**: `v0.1.0`, `v0.2.0`, ... `v1.0.0`.

The earlier `v<duckdb>-crypta.<n>` scheme was abandoned because it could never
promote: the promotion of `v1.5.3-crypta.1` would be plain `v1.5.3`, which is
upstream's tag and not the fork's. Under that scheme no milestone could ever
honestly flip `shipped`. The existing `v1.5.3-crypta.1` prerelease is left in
place - nothing was retagged or deleted.

Which DuckDB version an artifact is built for is carried by the build, not by
the fork's version number, and a runtime refuses any other patch.

## [v0.1.0-rc.1] - 2026-08-06

Pre-release. Milestone **M1 - Envelope MVP: identity-bound wrapped DEKs on the
existing catalog column**. Soaking before promotion to `v0.1.0`; an rc is soak,
not ship.

### Added

- **Identity-bound wrapped-blob provider on the existing column.** Every
  data-file key is stored in the existing opaque `encryption_key VARCHAR` as a
  self-describing, identity-bound crypta-wrapped blob, with **zero upstream
  schema change**.
- **CSPRNG-minted, 256-bit data-file keys** (#4, #5). Previously 128-bit.
- **Per-DuckDB-version extension build.** The extension declares the DuckDB
  version it was built for and a runtime refuses any other patch, so a binary
  cannot be silently loaded against a DuckDB it was not built against.
- **Encrypted-lake enablement**: a migration path from a plaintext lake and an
  end-to-end path from a native Postgres source.
- **Storage-layer mutation suite** (#45, #46) - full-extension mutants that
  delete each storage-layer guard and are proven in CI to RED the cases that
  claim it. This is what makes the refusal assertions falsifiable rather than
  decorative.

### Fixed

- **Fail-closed on three configured-crypta paths** (#19, #20, #21). A configured
  provider that is silently bypassed now refuses rather than writing plaintext
  key material; an unconfigured reader refuses a crypta-wrapped key rather than
  decoding it as a raw DEK; a crypta write failure no longer kills the embedding
  host.
- **The wrapped blob is escaped on the READ splice site** (#24), where it is
  spliced into the crypta request JSON. Said precisely rather than as "both
  splice sites": the WRITE site is still open, see below.
- **Key confusion** (#18) - a bare `|` join let a key be re-read across fields.

### Known limitations

Stated rather than implied. These are **not** closed by this release:

- **The envelope does NOT yet fail closed on every path** - milestone M2 is open,
  and three of its four epics have open blocking issues. Specifically:
  - **#33 (open, security)** - `WrappedEncryptionKeyLiteral` splices crypta's
    reply into SQL. The READ splice site is escaped (#24); the **WRITE** site is
    not. Escaping is therefore incomplete, and an operator should not read this
    release as closing the injection class.
  - **#26 (open)** - the inlined-deletion flush consumes a delete file's
    crypta-wrapped key WITHOUT unwrapping it.
  - **#31 (open)** - the unwrap reply is zipped by array POSITION rather than
    matched against the identity crypta echoes, so an injected or reordered
    element can hand a caller the wrong DEK.
  - **#41 (open)** - #25 leaves #34's second-decode-site refusal unconstructible
    from public operations, so that guard is live but not provable end to end.

- **Reader-side residuals are unbounded** (M3): the DEK cache has no TTL and
  does not zeroize, DuckDB temp spill is not encrypted, and there is no rewrap
  sweep for KEK retirement.
- **Plaintext column statistics**, **catalog DSN exposure** via
  `duckdb_databases()`, and the **compromised-live-reader** boundary remain open
  exposures. They are deliberately asserted as residuals in `ducklake-bench` so
  that the day one of them stops being true, the bench reds.
- **The root of trust is not proven to be hardware.** The stack has not been
  stood up against a real device in a way that DISTINGUISHES it from SoftHSM, so
  no hardware-rooted claim is made here.
- **Inlining is unreachable on a crypta lake** (#25): `DataInliningRowLimit()`
  returns 0 unconditionally ahead of every option scope, so the inline decode
  site is not constructible from public operations.
- **No upstream contribution** has been proposed (M6). This is a fork release.

### Evidence

`ducklake-bench` executes the full 18-case use-case matrix against a live
containerised stack - Postgres catalog, crypta key service, this extension, and
a pgwire gateway - and at `ducklake-bench@3fbc5d0` reports **18 passed, zero
FAIL-REGRESSION**, with this fork pinned at `59176c04`.

Fork CI on `59176c04` is green across all suites, including `Storage-Layer
Mutants` and `Crypta Refusal Tests`.
