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

## [Unreleased]

### Fixed

- **An EMPTY wrapped key was accepted, written as SQL `NULL`, and the data file
  lost forever** (#55). `CryptaClient::IsBase64("")` returned `true` - the loop
  that validates the alphabet never executes on a zero-length string, so the
  predicate fell out the bottom as *vacuously* in the alphabet. That was the one
  input #33's alphabet check admitted, and the only value that reached
  `DuckLakeUtil::WrappedEncryptionKeyLiteral`'s `return "NULL"` branch.

  The outcome was worse than the injection class #33 closed, because an
  injection is loud. Here the data file was written to storage **encrypted with
  a real DEK**, the wrapped form of that DEK was **discarded**, the row was
  written with no key, and **the commit reported success**. No error, no
  warning; the only symptom is a read that fails later, long after the key is
  gone. The existing empty-batch guards (`no_empty_shortcut_wrap`,
  `no_empty_shortcut_unwrap`) are on the **request** side and never saw it, and
  `LooksWrapped` - which would have rejected it - is not consulted on the write
  path at all.

  Two guards, both failing closed, one per layer. `IsBase64` now refuses the
  empty string, in the **predicate** rather than at one call site, so every
  caller present and future inherits the refusal; the reply reader's existing
  behaviour then covers it unchanged, refusing the **whole batch** rather than
  dropping the bad value. `WrappedEncryptionKeyLiteral` now **throws** instead of
  writing `NULL` when the file has a key, and takes an explicit `file_has_key`
  from the caller because an empty value means two different rows: a file with no
  key at all - `ducklake_add_data_files` never sets one - still writes `NULL`,
  and that path is unchanged.

  Proven red-first: the three cases were pushed failing against the unfixed tree
  (`06e0be5b`) before the guards landed. Each guard has its own mutant in the
  standalone roster - `no_empty_reply_value_refusal` and
  `no_empty_wrapped_key_refusal` - and every multi-name mutant was verified per
  case rather than by its combined exit status. Found in adversarial review of
  the `v0.1.0-rc.1` release candidate.

- **The crypta reply was zipped onto the caller's file list by ARRAY POSITION**
  (#31, #54). crypta already says which file each returned value belongs to - its
  reply item is an identity beside the value - and the client threw that identity
  away. The reply-count check from #24 was the only thing between that and key
  confusion, and a count is a LENGTH check standing in for a BINDING check: it
  catches a reply with the wrong *number* of items, and a REORDER has exactly the
  right number. Measured against the pre-fix client, a two-item reply with its
  items swapped handed `t/alpha.parquet` the DEK crypta minted for
  `t/beta.parquet` - a wrong-key defect, not the refused-batch availability
  defect #28 bounded.

  Both batch paths now go through one reader that walks the reply item by item
  and requires item `i` to echo `identities[i]`, all four fields, compared as
  decoded values rather than as bytes. The value and the identity binding it come
  out of the same structurally delimited item, so a value nested inside the
  echoed identity or duplicated beside it cannot be substituted for the item's
  own. The count check stays: it is what answers for a reply with no items at
  all. Both fakes were corrected to echo the identity first - a guard verified
  against a fake that never sends the field it checks passes without running.

  Proven red-first against the pre-fix client, then green; the standalone
  mutation roster is 40 guards, 40 red, no survivors, and every multi-name mutant
  was verified per case rather than by its combined exit status. That per-case
  pass found and fixed a defect in this change's own evidence, recorded in the
  pull request.

- **The `v0.1.0-rc.1` entry below understated the release** (#52). As published
  at the `v0.1.0-rc.1` tag, that entry listed **#33** and **#26** under *Known
  limitations* while both were **already fixed at the very commit the tag points
  at** (`fb314fb6`). The cause: the pull request that introduced this file was
  authored before those two fixes and squash-merged after them, so its text
  described a tree that no longer existed by the time it landed.

  The consequence was not cosmetic. A client pinning `v0.1.0-rc.1` and reading
  its CHANGELOG - which the release body explicitly directs them to - would
  conclude that the SQL-injection write path was still live and that the
  inlined-deletion flush still consumed a wrapped key raw. Neither was true at
  that commit.

  The entry below is corrected in place, because leaving a false security
  statement standing is worse than editing a historical entry. **The published
  `v0.1.0-rc.1` tag still carries the uncorrected text** and is not retagged.
  Found in adversarial review of the release candidate, not by its author.

- **A second overclaim in the same entry**: the storage-layer mutation suite was
  described as "proven in CI". It was proven LOCALLY. That gate fires only when
  its subject moves, and its last CI green (`59176c04`) predates every file the
  #33 and #26 fixes touched, so it does not cover the tagged commit. Corrected
  in place with the evidence that does exist named explicitly.

- **Two residuals found while verifying that correction** are now recorded
  against the entry below rather than left implicit: **#55** (an empty wrapped
  key becomes SQL `NULL` and the data file is unreadable forever) and **#53**
  (the flush call site skips the resolve on a NULL key instead of refusing).

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
  delete each storage-layer guard and RED the cases that claim it. This is what
  makes the refusal assertions falsifiable rather than decorative.

  **Proven LOCALLY at this commit, not in CI.** Said precisely, because the
  distinction is the whole point of the suite. This gate fires only when its
  subject moves, and its last CI green is `59176c04` - which predates the #33
  and #26 fixes and every other file they touched. The evidence for this commit
  is therefore a local run: 5/5 storage mutants RED and 37/37 crypta mutants
  RED, re-derived independently on a second checkout. GitHub Actions was
  degraded during this release (jobs returning `steps=0`, unacquired by any
  runner), so no CI verdict exists for the tag in either direction.

### Fixed

- **Fail-closed on three configured-crypta paths** (#19, #20, #21). A configured
  provider that is silently bypassed now refuses rather than writing plaintext
  key material; an unconfigured reader refuses a crypta-wrapped key rather than
  decoding it as a raw DEK; a crypta write failure no longer kills the embedding
  host.
- **The wrapped blob is escaped at BOTH splice sites** (#24, #33). The READ site
  escapes it into the crypta request JSON; the WRITE site
  (`DuckLakeUtil::WrappedEncryptionKeyLiteral`) routes through
  `SQLLiteralToString` like every sibling value on the same row. Additionally
  `CryptaClient::ExtractBase64Field` refuses any reply value carrying a
  character outside the base64 alphabet, which makes the safety of that value an
  assertion about the VALUE rather than an assumption about the PEER. Exactly
  one input escapes the alphabet check - see #55 below.
- **The inlined-deletion flush unwraps the delete file's stored key** (#26). It
  previously consumed the stored crypta-wrapped blob as if it were a raw DEK; it
  now resolves it through `DuckLakeCatalog::ResolveStoredEncryptionKey`. A
  residual remains at that call site - see #53 below.
- **Key confusion** (#18) - a bare `|` join let a key be re-read across fields.

### Known limitations

Stated rather than implied. These are **not** closed by this release:

- **The envelope does NOT yet fail closed on every path** - milestone M2 is open.
  #33 and #26 are FIXED at this commit and are recorded under Fixed above. What
  remains open is:
  - **#55 (open, security)** - `IsBase64("")` returns true vacuously, so an
    EMPTY wrapped value passes #33's alphabet check, becomes SQL `NULL` in
    `WrappedEncryptionKeyLiteral`, and the data file is written encrypted with a
    DEK whose wrapped form was discarded. The commit reports success and the
    file is unreadable forever, by anyone. This is the single input #33's check
    admits, and it is the more dangerous residual on this list.
  - **#53 (open)** - #26's fix moved the resolve onto the catalog, but the flush
    CALL SITE skips the resolve on a NULL key rather than refusing it, so two of
    three halves moved and the third did not.
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
