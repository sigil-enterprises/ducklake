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
# NOTE on force_mbedtls_unsafe below: it is set because a locally built DuckDB
# reports version v0.0.1, so `INSTALL httpfs` 404s against the extension
# repository and the full mbedtls crypto module is unavailable. It weakens the
# PARQUET cipher only. The envelope path under test - key generated -> wrapped by
# crypta -> stored -> unwrapped by crypta -> handed to the Parquet reader - is
# completely unaffected. Do NOT copy this flag into anything that is not this test;
# see .claude/README.md.
set -eu

CRYPTA_REPO=${CRYPTA_REPO:-../crypta}
SOCK_VOLUME=crypta-mvp-sock
LAKE_VOLUME=ducklake-mvp-lake
SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT

docker volume create "$SOCK_VOLUME" >/dev/null
docker volume rm -f "$LAKE_VOLUME" >/dev/null 2>&1 || true
docker volume create "$LAKE_VOLUME" >/dev/null

cat > "$SCRATCH/serve.sh" <<'SERVE'
#!/bin/sh
set -eu
export PATH=/usr/local/cargo/bin:$PATH
cargo build --release --quiet
W=/tmp/crypta-mvp
rm -rf "$W"; mkdir -p "$W/tokens"
printf 'directories.tokendir = %s\nobjectstore.backend = file\n' "$W/tokens" > "$W/softhsm2.conf"
export SOFTHSM2_CONF="$W/softhsm2.conf" CRYPTA_PIN=123456
softhsm2-util --init-token --free --label crypta-mvp --so-pin 87654321 --pin 123456 >/dev/null
./target/release/crypta provision-device-key --profile softhsm --token-label crypta-mvp
./target/release/crypta provision --profile softhsm --token-label crypta-mvp \
  --kek-id "softhsm:mvp:kek-v1" --kek-blob "$W/kek.rotblob"
rm -f /run/crypta/crypta.sock
exec ./target/release/crypta serve --profile softhsm --token-label crypta-mvp \
  --kek-blob "$W/kek.rotblob" --socket /run/crypta/crypta.sock
SERVE

ATTACH_OPTS="DATA_PATH '/lake/data/', ENCRYPTED, DATA_INLINING_ROW_LIMIT 0,
    CRYPTA_SOCKET '/run/crypta/crypta.sock', CRYPTA_LAKE_ID 'teras-mvp'"

emit() { # $1 = file, $2 = body
	{
		echo "SET force_mbedtls_unsafe='true';"
		echo "ATTACH 'ducklake:/lake/meta.db' AS lake ($ATTACH_OPTS);"
		echo "$2"
	} > "$SCRATCH/$1"
}
emit write.sql "CREATE TABLE lake.patients AS SELECT i AS id, 'NHS-' || i AS nhs FROM range(2000) t(i);
SELECT sum(id) AS checksum FROM lake.patients;"
# A genuine column read. SELECT count(*) is answered from record_count in the
# catalog and never touches the Parquet file, so it does not test decryption.
emit read.sql "SELECT sum(id) AS checksum, min(nhs) AS sample FROM lake.patients;"

dl() {
	docker run --rm -v "$(pwd)":/app -v "$SOCK_VOLUME":/run/crypta -v "$LAKE_VOLUME":/lake \
		-v "$SCRATCH/$1":/q.sql:ro --entrypoint sh ducklake-app \
		-c 'mkdir -p /lake/data && ./build/release/duckdb -unsigned < /q.sql' 2>&1
}

docker rm -f crypta-mvp >/dev/null 2>&1 || true
docker run -d --name crypta-mvp -v "$(cd "$CRYPTA_REPO" && pwd)":/app \
	-v "$SOCK_VOLUME":/run/crypta -v "$SCRATCH/serve.sh":/serve.sh:ro \
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
echo "=== 5. substitution (needs 2 files; skipped if only 1) ==="
dl swap.sql | tail -12

echo
echo "Expected: 1,2,4 succeed with checksum 1999000; 3 refuses with"
echo "'cannot reach the crypta key service'; a substituted or re-scoped or"
echo "de-enveloped row refuses with 'unwrap failed' / the downgrade message."
