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

## Coverage - opt-in, and off means off

`kaoyan` ingests C++ coverage as gcovr's Cobertura XML, and gcovr needs an
**instrumented** build. This tree had none, which is why coverage was the one
open acceptance box in kaoyan#197 / kaoyan PR #199. Issue #14 here.

The switch is `DUCKLAKE_ENABLE_COVERAGE`, default **OFF**, in the top-level
`CMakeLists.txt`. Two fenced `FORK-LOCAL` blocks in that one file; nothing else
in the tree knows about it. Keep it that way - see the upstream-hygiene rule
above. **GCC only**: Clang's coverage runtime is `libclang_rt.profile`, not
`libgcov`, so a Clang build would configure and then fail at link; the guard
refuses it rather than advertising a path nothing has compiled.

**No CI job sets it.** Checkable: `-DDUCKLAKE_ENABLE_COVERAGE` appears nowhere
under `.github/`, and neither does `EXT_FLAGS`. There is exactly one `EXT_FLAGS`
in the reusable workflow this fork calls
(`extension-ci-tools/.github/workflows/_extension_distribution.yml:949`) and it
sets only ccache launchers, on the `shell: cmd` Windows job that
`MainDistributionPipeline.yml` excludes anyway.

### Build it in its OWN directory - `build/coverage`, never `build/release`

`make release` hardcodes `build/release`, so `EXT_FLAGS=-D...=ON make release`
reconfigures the release tree **in place**. And a CMake cache entry is sticky:
`option()` never overrides one that already exists, so a later plain
`make release` in that directory stays instrumented until somebody passes an
explicit `=OFF`. Forget that and you have shipped an `-O0 --coverage` binary
called "release".

So do not use `EXT_FLAGS` for this. Configure a separate build directory, which
removes the failure class rather than documenting it. The one-time
`build/extension_configuration` (the merged vcpkg manifest) comes from an
ordinary `make release`, so run that once first if the tree is fresh.

```bash
# 0. once, if build/extension_configuration does not exist yet
docker compose run --rm --entrypoint bash app -lc \
  'BUILD_EXTENSION_TEST_DEPS=full make release'

# 1. instrumented build, in its own directory. Same flags make release uses,
#    plus the option. build/release is never touched.
docker compose run --rm --entrypoint bash app -lc '
  cmake -G Ninja -DEXTENSION_STATIC_BUILD=1 \
    -DDUCKDB_EXTENSION_CONFIGS=/app/extension_config.cmake \
    -DCORE_EXTENSIONS=";httpfs" -DVCPKG_BUILD=1 \
    -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DUNITTEST_ROOT_DIRECTORY=/app/ -DBENCHMARK_ROOT_DIRECTORY=/app/ \
    -DENABLE_UNITTEST_CPP_TESTS=FALSE -DBUILD_EXTENSION_TEST_DEPS=full \
    -DVCPKG_MANIFEST_DIR=/app/build/extension_configuration \
    -DCMAKE_BUILD_TYPE=Release -DDUCKLAKE_ENABLE_COVERAGE=ON \
    -S ./duckdb/ -B build/coverage
  cmake --build build/coverage'

# 2. run the suite - this is what writes the .gcda counters
docker compose run --rm --entrypoint bash app -lc \
  './build/coverage/test/unittest "test/sql/*"'

# 3. Cobertura for kaoyan. The --json-summary/--txt are not decoration: the
#    function rate is NOT in the Cobertura XML (gcovr emits <methods/> empty),
#    so without them it exists only in stdout that nobody kept.
docker compose run --rm --entrypoint bash app -lc \
  'gcovr --gcov-executable gcov-14 --root /app --filter /app/src/ \
     build/coverage/extension/ducklake \
     --cobertura /app/build/coverage.xml --cobertura-pretty \
     --json-summary /app/build/coverage-summary.json \
     --txt /app/build/coverage.txt --print-summary'
```

There is no step 4. `build/release` was never instrumented, so there is nothing
to undo; `rm -rf build/coverage` when you are finished with it.

### "Off means off" is measured, not asserted

Three checks, all run on this change:

1. **Configure the pristine tree and the tree carrying the block with the option
   unset, then diff every emitted build command.** All **713** identical -
   `sha256 78a43f8c708bcd3bd1a8c2d14e9c15dea998ff6df0e2a10ebd6df797f2721e2c` for
   both command sets. Zero `--coverage`, zero `-lgcov`, zero `-fprofile`, zero
   `.gcno`.
2. **Compare the whole generated build system, not just the commands.**
   `build.ninja` identical, `cmake_install.cmake` identical, and **1068 of 1077**
   generated files identical after normalising the build-dir name. The 9 that
   differ are environmental noise plus the cache entry itself: `CMakeCache.txt`,
   `CMakeFiles/CMakeOutput.log` (random `cmTC_*` try-compile ids),
   `third_party/re2/DartConfiguration.tcl` (hostname),
   `vcpkg-manifest-install.log` (timings), a `.ninja_log`, and four `.git`
   metadata files of the fetched `httpfs` clone. The `CMakeCache.txt` diff is
   three lines - the `DUCKLAKE_ENABLE_COVERAGE:BOOL=OFF` entry with its comment,
   and the container's own `SITE:STRING=` hostname. Nothing else.
3. **A positive control on the absence check itself.** "Zero `--coverage`" is an
   absence assertion, and a broken grep and a genuinely clean scan both report
   zero. `grep -cE -- "--coverage|-lgcov|-fprofile"` over the three command
   sets: **73** on the option-ON `build/coverage`, **0** on `build/release`,
   **0** on the pristine tree. It is shown firing on a known-instrumented input
   before either zero is believed.

And because the coverage build now lives in its own directory, `build/release`
is not merely restored afterwards - it is **never touched**. Measured across a
full instrumented build and suite run: `libducklake_extension.a` still
`e010f577...`, `ducklake.duckdb_extension` still `d8a0f387...`, and zero `.gcno`
under `build/release` against 68 under `build/coverage`.

### The three ways this goes wrong

1. **`--gcov-executable gcov-14` is not optional.** gcov and the compiler that
   wrote the `.gcno` share a format version; the image's default `gcov` is 11
   and the build is GCC 14. Measured, not assumed: omitting the flag makes
   `gcov` **segfault** on every `.gcda` (`GCOV returncode was -11`), and gcovr
   then aborts and writes **no file at all**. That is the good outcome - it
   fails loudly. Do not "fix" it with `--gcov-ignore-errors`, which is what
   turns this into the silent, well-formed, entirely-empty report.
2. **`-O0` is part of the instrumentation, not a preference.** gcov attributes
   hits to line numbers; at `-O3` the optimiser has inlined and reordered them
   and the attribution is fiction. The block appends `-O0 -g` after the
   build-type flags so they win, for ducklake's own translation units only.
3. **`-fprofile-update=atomic` likewise.** DuckDB's runner is multi-threaded and
   non-atomic counter updates lose hits and can corrupt a `.gcda` outright.

### Scope, and why the numbers are about this repo

The options are **directory-scoped** - they reach this `CMakeLists.txt` and
everything under `src/`, and nothing else. Measured on the instrumented compile
database: of 672 compile entries, exactly **68** carry `--coverage`, **all** of
them under `/app/src`, and the other **604** - DuckDB's own - carry none. So the
report covers this repo, not the engine it links against.

The static extension is *archived*, never linked, so `add_link_options` never
reaches it. `libgcov` has to arrive at whatever binary consumes the archive -
DuckDB's `unittest` - which is what the second fenced block does, via the plain
`target_link_libraries` signature's usage-requirement propagation.

### What it measured, 2026-08-05, at `06296c89` + this change

`./build/coverage/test/unittest "test/sql/*"` - **all 481 files green**, 473
test cases, 95419 assertions, 8 skipped (postgres_scanner 2, sqlite_scanner 2,
spatial 1, `LOCAL_EXTENSION_REPO` 1, `S3_TEST_SERVER_AVAILABLE` 2). 3m56s. The
481 reconciles independently: 472 `.test` + 9 `.test_slow` under `test/sql`,
nothing excluded from the glob.

| | |
|---|---|
| lines, everything in the report | **82.3%** - 14878 / 18076, 103 files |
| lines, the 68 `.cpp` alone | 82.2% - 14367 / 17488 |
| lines, the 35 `.hpp` alone | 86.9% - 511 / 588 |
| branches | 47.2% - 16971 / 35939 |
| functions | 84.4% - 1268 / 1503 |

Two caveats on that table, both mattering to a reader:

- **"103 files" is 68 translation units plus 35 headers.** Headers are 3.3% of
  measurable lines and gcov attributes their inlined code to them; that is why
  the `.cpp`-only figure is slightly lower.
- **The functions row is NOT in the Cobertura XML.** gcovr's Cobertura writer
  emits `<methods/>` empty - verified, 0 `method` elements across all 103
  classes. It exists only in gcovr's stdout, kept at
  `build/_proof/gcovr-stdout.txt` alongside `--json-summary` and `--txt`
  renderings. **kaoyan ingests the Cobertura XML, so it will show lines and
  branches and no function rate.** Do not report a function rate as something
  kaoyan measured.
- The numbers are from an `-O0` build. That is correct for gcov and wrong for
  anything else: **the measured binary is not the optimisation level of the
  shipped one.**

Named, so any of it is checkable against the file:
`src/storage/ducklake_delete.cpp` **98.5%** (383/389),
`ducklake_transaction_state.cpp` 94.3% (1094/1160),
`ducklake_table_entry.cpp` 94.0% (943/1003),
`ducklake_metadata_manager.cpp` 83.4% (2573/3084).

**The 14 zero-hit files are the interesting result, and none of them is an
instrumentation defect** - each has a `.gcno` and a `.gcda` (68 of each, one per
translation unit), so they are instrumented-and-never-executed, not unmeasured.
Seven are `.cpp`, seven are their headers. They split into two very different
kinds, and conflating them would be dishonest:

**Structurally unreachable by this suite** - no test could hit them as the tree
stands:

- `src/crypta/crypta_client.cpp` (0/167) and `ducklake_crypta.cpp` (0/36). The
  envelope provider needs `CRYPTA_SOCKET` + `CRYPTA_LAKE_ID` on ATTACH, and
  `grep -ril crypta test/` returns **zero files** - the suite does not mention
  crypta at all. **The fork's own security-critical code is at 0% under the
  standard suite.** Issue #15. `scripts/mvp_crypta_proof.sh` is the only thing
  in the tree that exercises it - see the next section for what it measures and,
  more importantly, what it does not.
- `ducklake_server_side_commit.cpp` (0/622) and `ducklake_staged_commit.cpp`
  (0/381). `test/sql/quack/server_side_commit_atomicity.test` is inside the glob
  and does run, but under the default file-backed catalog; the server-side path
  only engages against the quack RPC backend (`test/configs/quack.json`,
  `quack:localhost:19999`), which is absent here. Independent corroboration of
  the honest limit recorded under the staged-commit guard below.
- `quack_metadata_manager.cpp` (0/97) - same reason.

**Merely skipped in THIS build, and recoverable** - do not call these
structural:

- `postgres_metadata_manager.cpp` (0/97) and `sqlite_metadata_manager.cpp`
  (0/18). Dedicated tests for them exist inside the glob -
  `test/sql/metadata/ducklake_settings_postgres.test`,
  `ducklake_settings_sqlite.test`, `test/sql/issues/issue_sqlite_snapshot_time.test`,
  `test/sql/data_inlining/postgres_identifier_limit.test` - and they are exactly
  4 of the 8 declared skips, skipped only because `postgres_scanner` and
  `sqlite_scanner` were not built. Building with `ENABLE_POSTGRES_SCANNER` /
  `ENABLE_SQLITE_SCANNER` (and a Postgres to talk to) recovers ~115 lines.

### Measuring `src/crypta/` - and what the number does NOT prove

`scripts/mvp_crypta_proof.sh` is the only thing in the tree that drives the
envelope provider, so it is the only thing that can measure it. It now takes two
overrides; the defaults are the stock values, so leaving them unset is
byte-identical to before they existed:

```bash
CRYPTA_REPO=/path/to/crypta \
DUCKLAKE_IMAGE=<the image built from THIS tree> \
DUCKLAKE_BUILD=build/coverage \
  sh scripts/mvp_crypta_proof.sh
```

`.gcda` counters **accumulate**, so run it after the suite and re-run gcovr. The
delta on `src/crypta/` is attributable to the script alone, because the suite
leaves those files at exactly zero.

All eight proof cases behaved as specified against the instrumented build, with
the real Parquet cipher and no `force_mbedtls_unsafe`: checksum 1999000 on 1, 2
and 4, `len 228 / magic RExL` on 5, and the four refusals on 3, 6, 7 and 8.

| file | suite alone | + proof script |
|---|---|---|
| `src/crypta/crypta_client.cpp` | 0 / 167 (0%) | **128 / 167 (76.6%)** |
| `src/crypta/ducklake_crypta.cpp` | 0 / 36 (0%) | **30 / 36 (83.3%)** |
| `src/include/crypta/crypta_client.hpp` | 0 / 2 | 0 / 2 - still zero |
| whole report | 14878 / 18076 (82.3%) | 15058 / 18076 (83.3%) |

**This does not mean the provider is tested, and the number must not be read
that way.** A script-driven happy path plus four deliberate abuses is not a test
suite. The useful part is what the 39 still-uncovered lines in
`crypta_client.cpp` are:

- **Protocol error handling** - response truncated inside a field, wrong number
  of values returned for the items requested, connection closed mid-read, socket
  read/write failure, `EINTR` retry, oversized frame, socket path too long,
  empty socket path.
- **JSON escaping** of quotes and control characters in an identity string.
- **Empty-input early returns** on both the wrap and the unwrap batch path.
- In `ducklake_crypta.cpp`, line 58 `return entry->second;` - **the DEK cache
  HIT path never executes**, nor does the cache clear on line 72. That is the
  code issue #10 is about.

So the 76.6% is "the paths a working service and four deliberate abuses take".
The uncovered quarter is "what happens when the wire is malformed or the socket
misbehaves" - which, for a security boundary, is the half that matters most.
**Issue #15 stays open.** Real unit tests for the refusal paths - a malformed
blob, a wrong identity, a plaintext key row on an encrypted lake - are separate
work. This measurement does not do that work; it makes its shape visible.

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
   there is no key involved.

   **A crypta lake now forces this off itself** (issue #13 item 1). When
   `CRYPTA_SOCKET` is set, both `DuckLakeCatalog::DataInliningRowLimit` overloads
   return 0, so no option at any scope - table, schema, catalog-global, persisted,
   ATTACH, or the process-wide `SET` - can re-enable inlining. The guard is at the
   **read**, not at the write, because the write paths cannot be exhaustively
   guarded: `DuckLakeInitializer` overwrites the ATTACH-supplied value with the
   persisted lake option in the same map *after* the catalog is constructed
   (`src/storage/ducklake_initializer.cpp:213`), and `ducklake_set_option` can set
   it at any scope at any time. Asking for a non-zero limit explicitly - at ATTACH
   or through `set_option` - is a hard error rather than a silent no-op, so nobody
   learns a request took effect when it did not. Setting it to 0 still works.

   Three things this does **not** do, named so nobody assumes otherwise:
   - **An `ENCRYPTED` lake attached *without* `CRYPTA_SOCKET` still inlines
     cleartext.** The guard keys on crypta, not on `ENCRYPTED`, because upstream
     `test/sql/data_inlining/data_inlining_encryption.test` deliberately combines
     `ENCRYPTED` with inlining. There, `DATA_INLINING_ROW_LIMIT 0` is still yours
     to set.
   - **Rows already inlined before crypta was configured are not scrubbed.** This
     is write-side only. Remediation is an explicit
     `CALL ducklake_flush_inlined_data(...)`; nothing warns about it at ATTACH.
   - **Deleting rows in a pre-existing inlined table takes an ungated branch**
     (`src/storage/ducklake_delete.cpp:497-500`). It stamps `end_snapshot` on rows
     that already exist in `ducklake_inlined_data_<id>_<v>` (via
     `WriteNewInlinedDeletes`, `ducklake_metadata_manager.cpp:2812-2830`). It writes
     **no new cleartext** and does **not** touch `ducklake_inlined_delete_*` - that
     table is reached only through the *gated* `AddNewInlinedFileDeletes` at
     `ducklake_delete.cpp:512`. Unreachable on a lake crypta-configured from its
     first write, since no inlined data can exist there.
   - **`SET ducklake_default_data_inlining_row_limit = N` still succeeds silently**
     on a crypta lake. It is process-wide, so refusing it would break a non-crypta
     lake attached in the same process; the read-path guard makes it harmless, and
     `options()` is where the effective value can be seen.
   - **Small deletes now cost a Parquet delete file and a `WrapKeys` call.** Forcing
     the limit to 0 also disables inlined *file* deletions, so a single-row DELETE
     on a crypta lake writes an enveloped delete file where it previously wrote rows
     into `ducklake_inlined_delete_*`. Correct - the delete file is enveloped, the
     inlined positions were not - but it is real per-delete I/O that did not exist.

   Proven by `test/sql/crypta/crypta_inlining_refusals.test`, which carries its own
   positive control - the same insert on a non-crypta lake must still show the
   sentinel readable as cleartext out of the catalog. If that case goes quiet,
   every absence assertion in the file is worthless.
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

## The refusal suite - what the proof structurally cannot reach

`scripts/mvp_crypta_proof.sh` runs against a **healthy** crypta, so every path
that only executes when the service MISBEHAVES is unreachable from it, and from
anything under `test/sql/`. Those paths are the whole security surface of
`CryptaClient`: truncated frames, wrong value counts, oversized frames, a
connection closed mid-read, socket errors, EINTR, JSON escaping. A coverage run
found them at zero while the proof reported green - a healthy 76.6% covering the
paths that WORK, with 0% on the paths that must REFUSE.

Two suites close that, and neither touches shared source:

```bash
# unit: a fake crypta socket that misbehaves on purpose, PLUS the red-first
# evidence for every case (see below - --mutants is the point, not an extra)
docker compose run --rm --entrypoint bash app -lc \
  'test/cpp/crypta/run_crypta_tests.sh --mutants'

# sql: the ATTACH-level and key-row refusals, with a fake key service behind them
docker compose run --rm --entrypoint bash app -lc \
  'test/sql/crypta/run_sql_crypta_tests.sh'

# what either suite actually reaches, both arms - see "Measuring it honestly"
docker compose run --rm --entrypoint bash app -lc \
  'scripts/measure_crypta_refusal_coverage.sh'
```

Both suites run in CI, in `.github/workflows/CryptaRefusals.yml`, inside this
repo's own devcontainer image. A surviving mutant FAILS the job - proven, not
assumed: a no-op mutant (a semantically identical rewrite, so its case still
passes) makes the runner print `SURVIVED` and exit 1, and the workflow invokes it
as the last command in the container so that status is what the step sees.

The measurement is deliberately NOT in that job, and it therefore gates nothing -
it needs a second `-O0` build of DuckDB, roughly doubling a 30-70 minute job, for
a number the mutants already gate. That is the same criticism this file levels at
an unrun mutant suite; it is stated in the script's own header rather than left
for a reader to notice.

`test/cpp/crypta/` is a **standalone CMake project**, deliberately not wired into
the extension's `CMakeLists.txt`. It compiles the two `src/crypta/` files against
an already-built `libduckdb.so`. Two reasons: zero divergence in a file that also
exists upstream, and a mutant rebuild that takes seconds instead of
re-configuring a 700-target tree.

### --mutants, and why a green here is worthless without it

Every case in the suite asserts a REFUSAL, and **an unrun refusal test and a
passing one look identical**. So `mutants.py` removes exactly one guard at a
time - the truncation check, the count check, the EINTR retry, EACH HALF of the
cache key - rebuilds, and requires the cases naming that guard to go RED. 28
mutants, 28 reds. A guard whose removal changes nothing means the case naming it
is not testing it, and the runner says so by name rather than counting it.

Both halves of the cache key need their own mutant, and finding out why is worth
the warning: reducing the key to the blob alone cannot redden the
two-blobs-one-identity case, because two different blobs still give two different
keys. That case carried a `// mutant:` annotation naming evidence that did not
exist until a review caught it.

Three controls sit in front of each mutant, because every failure mode here is
silent:

- the roster is read into a VARIABLE, not a process substitution - `done < <(...)`
  hides the generator's exit status from both `set -e` and `pipefail`, so a
  `mutants.py` that crashed would leave an empty loop printing "greens are
  earned" and exiting 0;
- a mutant's spec must match EXACTLY the cases it names. Counting "more than
  zero" would let one renamed case in a multi-name list go unproven while the
  mutant still reddened off the others, and a spec matching nothing runs nothing
  and exits zero;
- those cases must be green BEFORE the guard is removed, or the red proves
  nothing about this mutant.

The first two are themselves positive-controlled: a crashing roster generator and
an empty one were both shown to stop the run.

### The cache HIT path, and 7df67912

`ducklake_crypta.cpp:135` had never executed in the EXTENSION build - i.e. through
a real ATTACH. (The standalone cache suite covers it too; the looser claim
"never executed in this tree" is false once that suite exists, and was corrected
after a review caught it.) Every unwrap in the proof is a MISS,
so the `(identity, blob)` cache keying - which IS the fix for the key-confusion
hole - was carried by nothing at all. `crypta_cache_test.cpp` exercises a genuine
hit, the wholesale clear at the 4096 cap, and each identity component
independently. The load-bearing assertion throughout is the **connection count**:
a hit is precisely "the client did not go to crypta again", and comparing
returned DEKs cannot tell a hit from a miss.

Mutating the key to the blob alone (`cache_key_blob_only`) reddens three cases,
which is the first executable evidence that 7df67912 fixed anything.

### The fakes are fakes, and the boundary is exact

`fake_crypta.py` and the C++ `FakeCryptaServer` perform **no cryptography** - no
KEK, no AEAD; the "blob" is a reversible encoding. They do enforce the identity
binding, which is the only property the refusal cases depend on. The real cipher,
the real KEK and KEK recovery across a restart are proven by
`scripts/mvp_crypta_proof.sh` and by nothing here.

One detail that will bite: `SelfTest` looks for the literal substring
`"ok":true`, so a health response pretty-printed as `"ok": true` is rejected as
not-ok. The fake emits compact JSON for that reason.

### What it found, and did not fix

Six defects, every one FILED rather than changed here - this suite tests the
envelope, it does not modify it. The PR carrying it touches no `src/`.

| # | issue | what | state |
|---|-------|------|-------|
| 2 | [#18](https://github.com/sigil-enterprises/ducklake/issues/18) | SECURITY - the cache key delimiter is ambiguous | **FIXED** |
| 3,4,5 | [#19](https://github.com/sigil-enterprises/ducklake/issues/19) | three ATTACH-time fail-opens | open |
| 1 | [#20](https://github.com/sigil-enterprises/ducklake/issues/20) | an unconfigured reader dies on an assertion, not a diagnostic | open |
| - | [#21](https://github.com/sigil-enterprises/ducklake/issues/21) | SIGPIPE kills an embedding host | open |

Number 2 has since been FIXED, in its own change (this one) with its own
red-first cases and its own mutant - so that row is the one place in this section
where `src/` did move. The rest are still open.

1. **A wrapped lake read by an UNCONFIGURED reader is not refused - it hits an
   assertion failure.** ([#20](https://github.com/sigil-enterprises/ducklake/issues/20)) `crypta_client.hpp` says `LooksWrapped` fails closed "in
   both directions", but its only call site is inside the provider, which only
   exists when crypta is configured. Attach an enveloped lake with the crypta
   options omitted and the read dies with `INTERNAL Error: Invalid AES key length
   for GCM` and a stack trace, not a diagnostic. That is the most likely operator
   mistake there is - forgetting the options on re-attach. Found by review, not
   by this suite; no test added, because the fix changes behaviour.
2. **FIXED - the cache key delimiter was ambiguous**
   ([#18](https://github.com/sigil-enterprises/ducklake/issues/18) - the most
   important result here). The
   key joined the identity fields and the blob with a bare `|` and escaped
   nothing, so a path ending `|RExLZZZZ` and a blob beginning `RExLZZZZ|`
   produced the SAME key - reintroducing, through the delimiter, the exact bypass
   the (identity, blob) keying exists to prevent. On reachability the route is
   catalog write access itself: the attacker sets `path` and a wrapped
   `encryption_key` on the same `ducklake_data_file` row, which is full control of
   `stored_path`. Three plausible-sounding routes are NOT it, and #18's own
   reachability paragraph got this wrong - a table name (`CanGeneratePathFromName`
   substitutes the table UUID, so the name reaches the path nowhere),
   `ducklake_add_data_files` (stores a path verbatim, but the row carries no
   encryption key and a keyless row throws in `ReadDataFile` before an identity is
   built, so it never reaches the cache), and a hive partition value
   (`HivePartitioning::Escape` is `StringUtil::URLEncode`). Fixed by
   LENGTH-PREFIXING each of the five components as
   `<decimal-byte-length>:<raw-bytes>` and concatenating them with no separator
   at all - injective by construction, because a decoder consumes each component
   BY COUNT instead of scanning for a delimiter, so no field content can be
   re-read as structure. Not escaping (that invites the next escaping bug) and
   not a rarer separator (no byte is safe when the field is operator-supplied).
   Carried by two cases in `crypta_cache_test.cpp` and by the
   `cache_key_unprefixed_join` mutant, which restores the bare join and must
   redden both. The completeness half of the property is `wire-set == key-set`:
   `CryptaFileIdentity` has exactly four fields, `IdentityJson` puts exactly those
   four on the wire, and the five components are those four plus the blob - so a
   field crypta binds to cannot be silently missing from the key. The
   justification comment at `crypta_client.cpp` was corrected in the same change,
   and then corrected AGAIN once the `ducklake_add_data_files` route was measured
   and found dead.
3. **`CRYPTA_SOCKET ''` disables the envelope instead of being refused.**
   ([#19](https://github.com/sigil-enterprises/ducklake/issues/19), with 4 and 5)
   `DuckLakeCatalog` guards the block with `!crypta_socket.empty()`, so an unset
   variable expands to `''` and the lake writes 44-character PLAINTEXT keys with
   no error. `CryptaClient`'s own empty-path refusal is unreachable from ATTACH.
4. **`CRYPTA_LAKE_ID` with no `CRYPTA_SOCKET`** is silently ignored the same way.
   Note the asymmetry: the opposite omission is refused, loudly.
5. **`CRYPTA_SOCKET` on a lake that RESOLVES TO UNENCRYPTED** - a fresh lake
   attached without `ENCRYPTED` - attaches, self-tests, and then encrypts
   nothing. The "crypta_socket on an UNENCRYPTED DuckLake" guard only fires on an
   explicit `ENCRYPTED false`. Stated carefully because a looser version of this
   claim is false: re-attaching an EXISTING enveloped lake under AUTOMATIC works
   fully, blobs and all (measured).

The three fail-opens are characterised in
`test/sql/crypta/crypta_config_fail_open.test`, the only file in the group that
asserts current behaviour rather than a refusal - read its header before reading
its assertions. Each turns red the day its defect is fixed, which is the point.

### Measuring it honestly - both arms, or the number is not evidence

`scripts/measure_crypta_refusal_coverage.sh`, against the opt-in
`-DDUCKLAKE_ENABLE_COVERAGE=ON` build:

| file | before | now | still dark |
|------|--------|-----|------------|
| `src/crypta/crypta_client.cpp` | 128/167 | **166/167** | `:98` |
| `src/crypta/ducklake_crypta.cpp` | 30/36 | **35/36** | `:143` |

Both ratios were measured BEFORE the #18 cache-key fix and have NOT been
re-measured since. They are left as the last real measurement rather than
adjusted by hand, because the fix adds executable lines to `ducklake_crypta.cpp`
(the `AppendLengthPrefixed` helper and its five calls), so that row's denominator
has certainly moved and its numerator probably has. Treat `35/36` as historical
until `measure_crypta_refusal_coverage.sh` is re-run; the `still dark` column IS
current, re-derived against the fixed files.

Every number there is a PAIR, because gcov's `.gcda` counters accumulate and a
lone non-zero proves nothing about which run produced it:

- **the cache HIT** (`ducklake_crypta.cpp:135`) reads **0** with only
  `crypta_attach_refusals.test` run - which exercises the same file, so the
  instrument is demonstrably live - and **4** with the full SQL group. It moves
  when and only when the cache case runs.
- **the dark-line check** is a subset test with its own positive control: with
  the unreachable list emptied it flags `:98` and `:143`, with the list in place
  it passes. A check that cannot fail is not a check.

There are **three** hardcoded line pins in that script and they drift
independently - `CACHE_HIT_LINE` at the top, plus the two in the `UNREACHABLE`
dict. The #18 fix moved all three (`58 -> 135`, `79 -> 98`, `66 -> 143`) and
repaired all three; repairing only the dict is the easy mistake, because
`CACHE_HIT_LINE` is 130 lines away from it. What a stale pin actually does is
worth stating precisely, since it is not silent:

- the `UNREACHABLE` dict is a subset test (`unexpected = dark - UNREACHABLE`), so
  a stale entry **always** false-flags the real line once it moves off the list -
  a visible red. It excuses the line now at the stale number only if that line is
  itself dark, which is a coincidence rather than the default.
- a stale `CACHE_HIT_LINE` also fails loud, but **with a misleading diagnosis**.
  If it lands on a comment, gcovr never reports the line, `line_count` returns the
  string `unreachable`, and the negative arm's `!= "0"` trips. The message used to
  offer only "a stale `.gcda`" or "something else reaches that line" - neither
  true - so the reader chases a phantom. The message now names the drift first.
  Credit to the #24 branch for finding this one; the first pass here fixed the
  dict and missed it.

Two traps this script exists to document, both of which it walked into first:

- `run_sql_crypta_tests.sh` hardcoded `build/release/test/unittest`, so the
  positive arm ran an UNINSTRUMENTED binary and wrote no counters - and read
  LOWER than the negative arm. Hence `DUCKLAKE_UNITTEST`.
- `gcovr --gcov-object-directory` does **not** scope the search - it ADDS a
  search path while `--root` keeps scanning the whole tree, so a second build's
  counters merged into the first's number (12 vs 4, measured). Only the trailing
  positional search path isolates.

Two things are honestly NOT proven and were not contrived into looking proven:

- `ducklake_crypta.cpp:142-144` (`crypta returned N keys for one file`) is
  **unreachable**. `ExtractBase64Field` already enforces exactly-`expected`
  values and `UnwrapKey` always requests one, so no response can reach it. It is
  defence in depth, and it stays dark.
- `crypta_client.cpp:98`, the closing brace of `JsonEscape`, is its
  exception-unwind block. gcov counts a closing brace on both the return and the
  unwind path - measured, not assumed: `ExtractBase64Field`'s brace reads 4143
  against 4131 returns, the excess being its throws unwinding through. Nothing in
  `JsonEscape` can throw but an allocation. Worth reading rather than dismissing,
  though: the same signal on `Health`'s brace was a REAL missing case - a health
  probe answered with an error frame - and is now covered.

And one that is proven only under a disposition DuckLake does not set:

- The write-failure branch depends on the HOST's SIGPIPE disposition. The CLI
  installs a handler so the branch is reached; a plain libduckdb embedding takes
  the default and the PROCESS DIES instead
  ([#21](https://github.com/sigil-enterprises/ducklake/issues/21)). The unit
  runner ignores SIGPIPE to observe the branch, and says so in its `main`.

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

## The consumption scenario: native Postgres -> encrypted lake -> psql

`scripts/scenario_pg_to_ducklake/` proves the path a client actually takes:
import a native Postgres database into a lake created `ENCRYPTED`, then read it
back with a **stock psql** over the Postgres wire. Every claim is asserted, and
it exits non-zero on any failure. Details and the two buenavista traps that cost
real time are in that directory's `README.md`; the two worth knowing before
touching any pgwire gateway:

- buenavista 0.5.0 **silently refuses every non-loopback client** in
  `verify_request()`, which runs before the handler - no log, no traceback, and
  the client sees only "server closed the connection unexpectedly". Any
  containerised gateway must set `BUENAVISTA_HOST`, which removes its only
  default access control, so real auth has to replace it.
- buenavista runs `SET search_path='main'` on **every new session**, discarding
  the `USE lake` done on the parent connection. Fixed by overriding the session
  factory, not by a connection-level setting.

The features are sigil's; installation and the key dance are declared in
opvance. Kept deliberately in step with `opvance/teras-ext-pgwire` - a scenario
that passed with a client the real gateway rejects would prove nothing.

## Installing the built extension - it is pinned to the exact PATCH

A DuckDB extension is loadable **only** by the exact `duckdb` patch it was built
against. Not the minor - the patch. Measured on 2026-08-03 with the fork
extension built for v1.5.3, loaded into a stock v1.5.5 CLI:

```
Invalid Input Error: Failed to load '.../ducklake.duckdb_extension',
The file was built specifically for DuckDB version 'v1.5.3' and can only be
loaded with that version of DuckDB. (this version of DuckDB is 'v1.5.5')
```

Both are 1.5.x. Refused anyway. So "converge the fleet on one DuckDB minor" is
NOT a convergence criterion - it has to be one exact patch, or the extension has
to be built and published per patch. Today `luma-server` bundles `=1.10503.1`
(v1.5.3) while the `lypto` image ships `1.5.5`, so one build cannot serve both.
Tracked as an open decision on the epic (sigil-enterprises/ducklake#1).

Two more things that bite when installing locally:

- **Install into a SEPARATE extension directory, never `~/.duckdb`.** The fork
  extension has the same name as the official one and will overwrite it. Use
  `~/.duckdb-crypta/extensions/v<patch>/<platform>/`. (This was learned the hard
  way: the official extension was overwritten and had to be restored, then
  verified byte-identically - the `.info` hash is the md5 of the **`.gz`**, not
  of the extension binary.)
- **The fork cannot mint a valid signature.** Extension signatures verify
  against DuckDB's own key, so a private fork's build is unsigned by
  construction. `allow_unsigned_extensions` is read at **database configuration
  time**, so no `SET` can turn it on afterwards - the CLI needs `-unsigned`, and
  an embedding host has to decide at connection-open. In `luma-server` that is
  `DataSource::in_memory_with(..., allow_unsigned_extensions)` behind
  `LUMA_ALLOW_UNSIGNED_EXTENSIONS`, default OFF and fail-closed.
