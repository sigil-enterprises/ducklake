#!/bin/sh
# PRIVATE-FORK ONLY. Never cherry-pick.
#
# The crypta envelope proof: an encrypted DuckLake that cannot be read without
# the key service, and whose key rows cannot be swapped, re-scoped, or stripped.
#
# Run from the repo root, on a machine where `docker compose build app` has been
# done in BOTH this repo and ../crypta.
#
# The socket is shared over a NAMED DOCKER VOLUME, not a bind mount: macOS bind
# mounts do not carry Unix sockets, and the failure is a confusing ENOENT.
#
# This proof uses the REAL Parquet cipher. It does not set
# `force_mbedtls_unsafe`, and it must never be changed to.
#
# That flag was used here once, because a locally built DuckDB reports v0.0.1 so
# `INSTALL httpfs` 404s against the extension repository, leaving the full mbedtls
# crypto module unavailable. The honest fix is to BUILD httpfs rather than to
# weaken the cipher: the devcontainer now pins the vcpkg sha that can resolve the
# full extension deps, so `BUILD_EXTENSION_TEST_DEPS=full make release` statically
# links httpfs and the flag is unnecessary.
#
# It matters that this proof runs unweakened. A lake that claims encryption while
# using a deliberately unsafe cipher is precisely the vacuous-green outcome the
# envelope exists to prevent, and a proof script is the last place to accept one.
set -eu

CRYPTA_REPO=${CRYPTA_REPO:-../crypta}
SOCK_VOLUME=crypta-mvp-sock
LAKE_VOLUME=ducklake-mvp-lake
# The SoftHSM token and the root-wrapped KEK blob live here, OUTSIDE the
# container filesystem, so that stopping and starting crypta RECOVERS the same
# KEK from the same root of trust instead of minting a new one.
#
# This matters more than it looks. An earlier version of this script
# re-initialised the token and re-provisioned on every container start, so step 4
# below was not a restart at all - it was a brand-new root of trust, under which
# the existing wrapped blobs correctly refuse to unwrap. The step failed, and it
# was right to: the test was wrong, not the code. A restart that silently
# regenerates the KEK proves nothing about restart behaviour and would mask a
# real KEK-persistence bug.
STATE_VOLUME=crypta-mvp-state
SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT

docker volume create "$SOCK_VOLUME" >/dev/null
docker volume rm -f "$LAKE_VOLUME" >/dev/null 2>&1 || true
docker volume create "$LAKE_VOLUME" >/dev/null
# Fresh root of trust for a fresh proof run, then stable across restarts within it.
docker volume rm -f "$STATE_VOLUME" >/dev/null 2>&1 || true
docker volume create "$STATE_VOLUME" >/dev/null

cat > "$SCRATCH/serve.sh" <<'SERVE'
#!/bin/sh
set -eu
export PATH=/usr/local/cargo/bin:$PATH
cargo build --release --quiet
# /state is a named volume, so this survives docker stop/start.
W=/state/crypta-mvp
mkdir -p "$W/tokens"
printf 'directories.tokendir = %s\nobjectstore.backend = file\n' "$W/tokens" > "$W/softhsm2.conf"
export SOFTHSM2_CONF="$W/softhsm2.conf" CRYPTA_PIN=123456
# Provision ONCE. On a restart the blob is already there and the KEK is recovered
# from the device by C_Decrypt - which is the property step 4 is meant to prove.
if [ ! -f "$W/kek.rotblob" ]; then
  softhsm2-util --init-token --free --label crypta-mvp --so-pin 87654321 --pin 123456 >/dev/null
  ./target/release/crypta provision-device-key --profile softhsm --token-label crypta-mvp
  ./target/release/crypta provision --profile softhsm --token-label crypta-mvp \
    --kek-id "softhsm:mvp:kek-v1" --kek-blob "$W/kek.rotblob"
  echo "provisioned a new root of trust"
else
  echo "recovering the existing KEK from the root of trust"
fi
rm -f /run/crypta/crypta.sock
exec ./target/release/crypta serve --profile softhsm --token-label crypta-mvp \
  --kek-blob "$W/kek.rotblob" --socket /run/crypta/crypta.sock
SERVE

ATTACH_OPTS="DATA_PATH '/lake/data/', ENCRYPTED, DATA_INLINING_ROW_LIMIT 0,
    CRYPTA_SOCKET '/run/crypta/crypta.sock', CRYPTA_LAKE_ID 'teras-mvp'"

emit() { # $1 = file, $2 = body
	{
		echo "ATTACH 'ducklake:/lake/meta.db' AS lake ($ATTACH_OPTS);"
		echo "$2"
	} > "$SCRATCH/$1"
}
emit write.sql "CREATE TABLE lake.patients AS SELECT i AS id, 'NHS-' || i AS nhs FROM range(2000) t(i);
CREATE TABLE lake.other AS SELECT i AS id, 'X-' || i AS v FROM range(2000) t(i);
SELECT sum(id) AS checksum FROM lake.patients;"
# A genuine column read. SELECT count(*) is answered from record_count in the
# catalog and never touches the Parquet file, so it does not test decryption.
emit read.sql "SELECT sum(id) AS checksum, min(nhs) AS sample FROM lake.patients;"
# The blob as stored, to show the column holds a wrapped blob and not a key.
emit inspect.sql "SELECT length(encryption_key) AS len, substr(encryption_key,1,4) AS magic
    FROM __ducklake_metadata_lake.ducklake_data_file ORDER BY data_file_id LIMIT 1;"

# Same lake, same blobs, a DIFFERENT compartment name. Proves the lake id is part
# of the binding and not decoration: keys do not travel between compartments even
# with the same KEK and the same root of trust.
{
	echo "ATTACH 'ducklake:/lake/meta.db' AS lake (DATA_PATH '/lake/data/', ENCRYPTED,"
	echo "    DATA_INLINING_ROW_LIMIT 0, CRYPTA_SOCKET '/run/crypta/crypta.sock',"
	echo "    CRYPTA_LAKE_ID 'teras-WRONG');"
	echo "SELECT sum(id) AS checksum FROM lake.patients;"
} > "$SCRATCH/wrong_lake.sql"

dl() {
	docker run --rm -v "$(pwd)":/app -v "$SOCK_VOLUME":/run/crypta -v "$LAKE_VOLUME":/lake \
		-v "$SCRATCH/$1":/q.sql:ro --entrypoint sh ducklake-app \
		-c 'mkdir -p /lake/data && ./build/release/duckdb -unsigned < /q.sql' 2>&1
}

docker rm -f crypta-mvp >/dev/null 2>&1 || true
docker run -d --name crypta-mvp -v "$(cd "$CRYPTA_REPO" && pwd)":/app \
	-v "$SOCK_VOLUME":/run/crypta -v "$STATE_VOLUME":/state \
	-v "$SCRATCH/serve.sh":/serve.sh:ro \
	--entrypoint sh crypta-app /serve.sh >/dev/null
i=0; while ! docker logs crypta-mvp 2>&1 | grep -q "listening socket"; do
	i=$((i+1)); [ $i -gt 120 ] && { docker logs crypta-mvp; echo "FAIL: crypta never listened"; exit 1; }
	sleep 1
done

echo "=== 1. write, crypta up ==="                    ; dl write.sql | tail -5
echo "=== 2. genuine read, crypta up (MUST succeed) ="; dl read.sql  | tail -6
echo "=== 3. crypta DOWN (MUST fail) ==="             ; docker stop crypta-mvp >/dev/null; dl read.sql | head -2
echo "=== 4. crypta UP again (MUST succeed) ==="      ; docker start crypta-mvp >/dev/null
i=0; while ! docker logs crypta-mvp 2>&1 | grep -q "listening socket"; do
	i=$((i+1)); [ $i -gt 120 ] && { echo "FAIL: crypta never relistened"; exit 1; }
	sleep 1
done
dl read.sql | tail -6

# Substitution: swap the two files' key rows, proving the swap by md5 first.
emit swap.sql "SELECT 1;"
cat > "$SCRATCH/swap.sql" <<'SWAP'
ATTACH '/lake/meta.db' AS raw;
SELECT data_file_id, md5(encryption_key) AS key_md5 FROM raw.ducklake_data_file ORDER BY data_file_id;
CREATE TEMP TABLE k AS SELECT data_file_id, encryption_key FROM raw.ducklake_data_file;
UPDATE raw.ducklake_data_file SET encryption_key=(SELECT encryption_key FROM k WHERE k.data_file_id=1) WHERE data_file_id=0;
UPDATE raw.ducklake_data_file SET encryption_key=(SELECT encryption_key FROM k WHERE k.data_file_id=0) WHERE data_file_id=1;
SELECT data_file_id, md5(encryption_key) AS key_md5 FROM raw.ducklake_data_file ORDER BY data_file_id;
SWAP
echo "=== 5. what the catalog actually holds ==="
dl inspect.sql | tail -6

echo "=== 6. wrong CRYPTA_LAKE_ID (MUST fail) ==="
dl wrong_lake.sql | head -3

echo "=== 7. substitution: swap the key rows, then READ REAL COLUMNS ==="
dl swap.sql | tail -12
echo "--- the read after the swap MUST fail ---"
dl read.sql | head -3

# Downgrade: strip the envelope and leave a plaintext-looking key, as a DBA with
# catalog write access could. Must be refused locally, without a crypta call -
# otherwise a lake could be quietly reverted to plaintext keys.
cat > "$SCRATCH/downgrade.sql" <<'DOWNGRADE'
ATTACH '/lake/meta.db' AS raw;
UPDATE raw.ducklake_data_file SET encryption_key = 'AAAAAAAAAAAAAAAAAAAAAAAA';
SELECT DISTINCT encryption_key FROM raw.ducklake_data_file;
DOWNGRADE
echo "=== 8. plaintext downgrade (MUST be refused) ==="
dl downgrade.sql | tail -5
dl read.sql | head -3

echo
echo "Expected: 1,2,4 succeed with checksum 1999000 (4 after a restart that"
echo "RECOVERS the KEK, not one that regenerates it); 5 shows a long wrapped"
echo "blob, not a 24-char key; 3 refuses with 'cannot reach the crypta key"
echo "service'; 6 and 7 refuse with 'unwrap failed'; 8 refuses with the"
echo "downgrade message."
