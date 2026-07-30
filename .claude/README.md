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
docker compose run --rm --entrypoint sh app -c 'make release'   # 30-70 min cold, minutes warm
docker compose run --rm --entrypoint sh app -c 'make test'
```

vcpkg is pinned to `a42af01b72c28a8e1d7b48107b33e4f286a55ef6` - the **same sha as
CI**. Do not float it: a local build resolving a different `roaring` than CI
proves nothing.

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

### Known gap

The **server-side commit path** (`ducklake_server_side_commit.cpp`) builds its
commit context without a catalog, so `context.catalog` is null there and the
crypta provider is unreachable. On that path keys would be written **plaintext**.
The read side refuses them, so it fails loudly rather than silently - but it is
not supported yet and must not be used with crypta until the provider is threaded
through that context.

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
   while not being securely encrypted. Treat that setting as forbidden.

Note for reproducing: `SELECT count(*)` on a DuckLake table is answered from
`record_count` in the catalog and **never touches the Parquet file**. A test that
counts rows does not exercise decryption at all. Always read real columns
(`sum(id)`, `min(nhs)`) when testing the key path - this cost us a false pass.

## The proof, and how to re-run it

`scripts/mvp_crypta_proof.sh` documents the sequence. The socket is shared
between the two containers over a **named Docker volume**, not a bind mount -
macOS bind mounts do not carry Unix sockets.

Verified results:

| case | outcome |
|---|---|
| write + genuine read, crypta up | 2000 rows, checksum 1999000 |
| catalog contents | 208-char wrapped blob (`RExL...` = `DLK1`), not a 24-char key |
| crypta stopped | ATTACH refused: "cannot reach the crypta key service ... an encrypted DuckLake cannot be read without it" |
| crypta restarted | reads again, checksum 1999000 |
| key rows swapped between two files (md5-proven) | `unwrap failed: not valid for this KEK and file identity` |
| wrong `CRYPTA_LAKE_ID` | `unwrap failed` - compartments are isolated |
| blob replaced with a plaintext key | refused, with the downgrade explained |
