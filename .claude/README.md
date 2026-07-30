# ducklake (sigil fork) - agent notes

Private vendored fork of `duckdb/ducklake`. Feeds the teras DuckLake image in
Nexus. Two things live here that are **not upstream**: a devcontainer, and the
crypta envelope key provider.

Programme of record: **issue #1**. Read it before changing anything in `src/crypta/`.

## Upstream hygiene - the rule that matters most

`origin` is `sigil-enterprises/ducklake` (private). `upstream` is `duckdb/ducklake`.

**Nothing here goes upstream by mirroring.** When a change is upstream-eligible,
it is cherry-picked commit by commit into a separate public upstream-only fork
with `git cherry-pick -s` (the sign-off is DCO). Before any public push, grep the
diff for `crypta`, `coffer`, `HSM`, `Utimaco`, `YubiHSM`, `teras`.

Files marked `PRIVATE-FORK ONLY` in their header comment must **never** be
cherry-picked:

- `.devcontainer/`, `docker-compose.yaml`
- `src/crypta/`, `src/include/crypta/`
- the crypta hunks inside `ducklake_metadata_manager.cpp`, `ducklake_catalog.*`,
  `ducklake_util.*`, `ducklake_storage.cpp`, `ducklake_options.hpp`,
  `ducklake_transaction*.cpp`, `ducklake_transaction_state.hpp`

Upstream-eligible work so far: the CSPRNG fix on `fix/csprng-key-generation`
(one file, `ducklake_insert.cpp`). No PR has been opened - that is deliberate and
gated on the human.

## Building

Always in the devcontainer. There was none before; it is hand-built rather than
tc-managed because a tc layer owns the Makefile and this tree's Makefile is
upstream's (it includes `extension-ci-tools/makefiles/duckdb_extension.Makefile`).

```bash
git submodule update --init --recursive --depth 1     # once
docker compose build app
# BUILD_EXTENSION_TEST_DEPS=full statically links httpfs, which is what supplies
# the real mbedtls crypto module for encrypted Parquet. Without it, encrypted
# writes fail closed and the only way forward is the forbidden
# force_mbedtls_unsafe - so build the deps, do not weaken the cipher.
docker compose run --rm --entrypoint bash app -lc 'BUILD_EXTENSION_TEST_DEPS=full make release'
docker compose run --rm --entrypoint bash app -lc './build/release/test/unittest "test/sql/*"'
```

Note the entrypoint override: the image's entrypoint is `make`, so
`docker compose run app <target>` works but `... app bash -lc '...'` needs
`--entrypoint bash`.

vcpkg is pinned to `84bab45d415d22042bd0b9081aea57f362da3f35`. Do not float it,
and do not "correct" it to the other sha in CI - see the `httpfs` note under the
`ENCRYPTED` findings for why that one cannot build the full deps.

The image builds **native**, so arm64 on an Apple Silicon workstation vs amd64 in
CI. Deliberate: forcing amd64 locally means QEMU and an all-day build for a binary
nobody ships. **CI is the only artifact source.** Confirm anything
architecture-sensitive on a CI run, never here.

## The crypta envelope provider

DuckLake stores the per-data-file key in `ducklake_data_file.encryption_key` as
plaintext base64. Anyone who can read the catalog holds every key. With crypta
configured, that column holds an identity-bound wrapped blob instead.

Enable per-ATTACH; absent these options nothing in the crypta path runs and
behaviour is byte-identical to upstream:

```sql
ATTACH 'ducklake:...' AS lake (
    DATA_PATH '...', ENCRYPTED,
    DATA_INLINING_ROW_LIMIT 0,                       -- see the inlining warning
    CRYPTA_SOCKET '/run/crypta/crypta.sock',
    CRYPTA_LAKE_ID 'teras-prod');
```

`CRYPTA_LAKE_ID` is mandatory with `CRYPTA_SOCKET` and names the **compartment**.
There is nothing inside DuckLake to derive it from: `ducklake_metadata` holds only
`version` / `created_by` / `data_path` / `encrypted`, and
`DuckLakeCatalog::instance_id` is regenerated on every ATTACH. Two lakes sharing
a crypta with the same id would have interchangeable keys.

### The four touch points

| site | file:line | what happens |
|---|---|---|
| **unwrap (the choke point)** | `ducklake_metadata_manager.cpp` `ReadDataFile` | every read of every encrypted file funnels here |
| wrap, data files (SQL) | `ducklake_metadata_manager.cpp` `WriteNewDataFilesSqlBatch` | whole commit wrapped in ONE call |
| wrap, delete files (SQL) | `ducklake_metadata_manager.cpp` `WriteNewDeleteFiles` | same, `file_kind=delete` |
| wrap, appender path | `ducklake_metadata_manager.cpp` `WriteNewDataFilesWithAppender` | per-file; the appender loop has no batch shape |

Identity is `(lake_id, table_id, file_kind, stored_path)`. **`stored_path` is the
path as persisted** - `path.path`, before `FromRelativePath` resolves it into
`data.path`. Do not "fix" this to use the resolved path: it would bind every key
to a mutable table property, so changing a data path would orphan the lot. And do
not try to use `data_file_id`: it is not in scope at the choke point, and adding
it to `GetFileSelectList` shifts positional column indices in 8+ queries.

### Invariants - do not regress

1. **Fail closed, always.** No crypta -> no lake. `FinalizeLoad` self-tests at
   ATTACH so a down service surfaces there, not as apparent corruption mid-scan.
2. **A plaintext key row on a crypta lake is refused**, never used. That is the
   downgrade attack.
3. **Delete files bind with `file_kind=delete`.** Their key rows must not be
   interchangeable with data-file rows.
4. **Wrap the whole commit in one call** wherever a `vector` of files exists.
5. **Never re-encode a wrapped blob.** It arrives base64 from crypta;
   `WrappedEncryptionKeyLiteral` only quotes it. Double-encoding produces rows
   nothing can read.

### The staged / server-side commit path refuses, rather than degrading

`ducklake_commit()` runs as a table function against the metadata schema alone,
with no attached DuckLake catalog and therefore no reachable crypta provider - so
the wrap cannot happen there. It has to happen at **staging** time, in
`DuckLakeStagedCommit::Build`, which does have the transaction and the catalog.

It does not happen there: the `Emit*` helpers serialise `file.encryption_key`
through the upstream plaintext encoder `DuckLakeUtil::EncryptionKeyLiteral`. Left
alone, a crypta-configured lake committed this way would write **plaintext keys**
into `ducklake_data_file`, silently, because the rows look ordinary. The read side
would later refuse them - but by then the plaintext keys are committed and have
reached every replica, WAL archive and backup of the catalog.

So `Build` **throws** when `CryptaProvider()` is set. Placement matters: it is the
single point where this is preventable rather than merely detectable.

Supporting it properly means threading the identity through `EmitDataFiles` /
`EmitDeleteFiles` / `EmitCompactions` and wrapping against the staged stored path
(`file.file_name`, staged with `path_is_relative = false` - note that differs from
the regular path, which stores a resolved-or-relative path). That is mechanical.

**Honest limit on the guard:** it compiles, and its only caller is
`QuackMetadataManager::FlushChangesServerSide` (verified by grep - nothing else
constructs a `DuckLakeStagedCommit`). It has **not been executed**, because
`quack` is a metadata backend absent from this tree, so there is no way to reach
that path here. What is proven is that the full suite still passes with it in
place, i.e. it does not fire on a non-crypta lake. Whether it fires correctly on a
quack lake is untested.

## Two findings about `ENCRYPTED` itself - read these

Both reproduced on an **unmodified** build. Neither is caused by our changes, and
both are on issue #1 with the reproductions.

1. **`ENCRYPTED` does not encrypt inlined data.** A small write goes into
   `ducklake_inlined_data_<t>_<n>` in the metadata catalog, in **plaintext** -
   `ENCRYPTED` governs the Parquet path only. Verified: a lake with
   `encrypted = true`, zero data files, and the value readable with an ordinary
   `SELECT`. On teras the catalog is Postgres, so that is plaintext PHI in
   Postgres, the WAL, every replica and every backup. **crypta cannot help** -
   there is no key involved. Set `DATA_INLINING_ROW_LIMIT 0`.
2. **Encrypted Parquet writes require `httpfs`** (the full mbedtls crypto
   module). Without it the write fails closed, which is right. But
   `force_mbedtls_unsafe` exists and would produce a lake that claims encryption
   while not being securely encrypted. **Treat that setting as forbidden**, and do
   not reach for it to get a local build working - build `httpfs` instead:

   ```sh
   docker compose run --rm --entrypoint bash app -lc 'BUILD_EXTENSION_TEST_DEPS=full make release'
   ```

   This needs the vcpkg pin in `.devcontainer/Dockerfile` to be
   `84bab45d415d22042bd0b9081aea57f362da3f35`, not the other sha CI uses. CI has
   two: the GCC-12 compatibility job pins `a42af01b...` and builds with **no**
   extension deps, while the relassert job pins `84bab45d...`, which is also the
   `builtin-baseline` duckdb's own `merge_vcpkg_deps.py` writes into the generated
   manifest. Under `a42af01b` the full-deps build cannot configure at all - the
   merged manifest asks for curl 8.17.0 / openssl 3.6.0 / roaring 4.5.0 and that
   older version database has no entry for any of them.

   The proof script used to set `force_mbedtls_unsafe` for exactly this reason. It
   no longer does, and must not again: a lake that claims encryption while running
   a deliberately unsafe cipher is the vacuous-green outcome the envelope exists to
   prevent, and a proof is the last place to accept one.

Note for reproducing: `SELECT count(*)` on a DuckLake table is answered from
`record_count` in the catalog and **never touches the Parquet file**. A test that
counts rows does not exercise decryption at all. Always read real columns
(`sum(id)`, `min(nhs)`) when testing the key path - this cost us a false pass.

## The proof, and how to re-run it

`scripts/mvp_crypta_proof.sh` runs the whole matrix - all eight steps, no manual
follow-up. The socket is shared between the two containers over a **named Docker
volume**, not a bind mount: macOS bind mounts do not carry Unix sockets, and the
failure is a confusing ENOENT.

Observed, from `sh scripts/mvp_crypta_proof.sh`, against the **real** Parquet
cipher (no `force_mbedtls_unsafe`):

| # | case | outcome |
|---|---|---|
| 1-2 | write + genuine read, crypta up | checksum 1999000, sample `NHS-0` |
| 3 | crypta stopped | ATTACH refused: "cannot reach the crypta key service ... an encrypted DuckLake cannot be read without it" |
| 4 | crypta restarted, KEK **recovered** | reads again, checksum 1999000 |
| 5 | what the catalog holds | `len 208`, `magic RExL` - a wrapped blob, not a 24-char key |
| 6 | wrong `CRYPTA_LAKE_ID` | `unwrap failed` - compartments are isolated |
| 7 | key rows swapped between two files (md5-proven both ways) | `unwrap failed: not valid for this KEK and file identity` |
| 8 | blob replaced with a plaintext key | refused, with the downgrade explained |

Regression suite alongside it: **18943 assertions in 506 test cases, 0 failures**,
13 skipped. Worth noting against the earlier figure of 18818 / 498 / 21: building
`httpfs` did not just remove the unsafe flag, it un-skipped **8 test cases** that
had been silently sitting out for want of the extension. A skip is not a pass.

**Step 4 is the one to be careful with, and it caught us.** An earlier version of
the script re-initialised the SoftHSM token and re-provisioned on every container
start, so "restart" minted a **brand-new KEK** - under which the existing wrapped
blobs correctly refuse to unwrap. The step failed, and it was right to fail: the
test was wrong, not the code. The token and the root-wrapped KEK blob now live on
a named volume (`crypta-mvp-state`) so a restart genuinely recovers the same KEK
via `C_Decrypt`; crypta's own log distinguishes the two cases
("provisioned a new root of trust" vs "recovering the existing KEK from the root
of trust"), and step 4 must show the second one.

A restart that silently regenerates the KEK proves nothing about restart
behaviour and would mask a real KEK-persistence bug. Do not "simplify" the state
volume away.
