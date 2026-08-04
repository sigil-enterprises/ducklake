#!/bin/sh
# Scenario: native Postgres -> ENCRYPTED DuckLake -> consumed over the Postgres wire.
#
# Three legs, each proven rather than assumed:
#
#   1. IMPORT   a native Postgres database into a DuckLake created ENCRYPTED,
#               with a Postgres catalog (what teras actually runs).
#   2. VERIFY   rows reconcile AND every data file carries a 32-byte key.
#   3. CONSUME  serve the lake over pgwire and read it back with an ordinary
#               Postgres client, so the round trip is end to end.
#
# The sigil side owns the FEATURES here. Installation - and the key dance around
# the KEK - is declared in opvance; this scenario exists so the feature can be
# proven without depending on a deployment this repo does not own.
#
# The gateway pins DuckDB 1.5.3, which is exactly the patch the sigil fork
# extension is built for. That is load-bearing: an extension loads only under the
# patch it was built against, so a 1.5.5 gateway could not load this build.
set -eu

EXT_HOST=${DUCKLAKE_EXTENSION_HOST:-}
if [ -z "$EXT_HOST" ]; then
  echo "set DUCKLAKE_EXTENSION_HOST to a linux extension build for this arch" >&2
  echo "  (download ducklake-v1.5.3-linux_<arch>.duckdb_extension from the release)" >&2
  exit 2
fi

NET=ducklake-scenario-net
PG=ducklake-scenario-pg
APP=ducklake-scenario-app
LAKE_VOL=ducklake-scenario-lake
PGPASS=scenario
HERE=$(cd "$(dirname "$0")" && pwd)

# Leave nothing behind, including the volume. The lake volume holds the encrypted
# Parquet, so an abandoned one is both clutter and a stray copy of the data.
cleanup() {
  docker rm -f "$PG" "$APP" >/dev/null 2>&1 || true
  docker network rm "$NET" >/dev/null 2>&1 || true
  docker volume rm -f "$LAKE_VOL" >/dev/null 2>&1 || true
}
trap cleanup EXIT
cleanup

docker network create "$NET" >/dev/null
docker volume create "$LAKE_VOL" >/dev/null

echo "=== 0. native Postgres: source data AND the lake catalog ==="
docker run -d --name "$PG" --network "$NET" \
  -e POSTGRES_PASSWORD="$PGPASS" -e POSTGRES_DB=source \
  postgres:16 >/dev/null

# Wait for the CONDITION WE NEED, not for a proxy. `pg_isready` is not it: the
# postgres entrypoint runs initdb against a TEMPORARY server on the unix socket
# before it creates POSTGRES_DB and restarts for real, and pg_isready answers yes
# to that temporary server. The next command then fails with
# 'database "source" does not exist' - a race that hides whenever the image is
# already cached and the timing shifts. Wait until the target database actually
# answers a query.
i=0
until docker exec "$PG" psql -U postgres -d source -c 'SELECT 1' >/dev/null 2>&1; do
  i=$((i+1)); [ "$i" -gt 90 ] && { echo "postgres did not become ready" >&2; exit 1; }
  sleep 1
done

docker exec -i "$PG" psql -U postgres -d source -v ON_ERROR_STOP=1 >/dev/null <<'SQL'
CREATE TABLE person (person_id int primary key, birth_year int, sex text);
INSERT INTO person SELECT g, 1940 + (g % 60), CASE WHEN g % 2 = 0 THEN 'F' ELSE 'M' END
  FROM generate_series(1, 5000) g;
CREATE TABLE visit (visit_id int primary key, person_id int, kind text);
INSERT INTO visit SELECT g, 1 + (g % 5000), CASE WHEN g % 3 = 0 THEN 'inpatient' ELSE 'outpatient' END
  FROM generate_series(1, 12000) g;
SQL
docker exec "$PG" psql -U postgres -d source -c "CREATE DATABASE lakecatalog" >/dev/null
echo "  seeded: person=5000 visit=12000, and a separate lakecatalog database"

# One image for both legs: the duckdb wheel pinned to the patch the extension
# needs, plus the pgwire shim the teras gateway uses.
echo
echo "=== building the scenario image (duckdb 1.5.3 + buenavista) ==="
docker build -q -t ducklake-scenario -f "$HERE/Dockerfile" "$HERE" >/dev/null

SRC_DSN="dbname=source host=$PG port=5432 user=postgres password=$PGPASS"
CAT_DSN="dbname=lakecatalog host=$PG port=5432 user=postgres password=$PGPASS"

echo
echo "=== 1+2. import into an ENCRYPTED lake, and verify it ==="
docker run --rm --name "$APP" --network "$NET" \
  -v "$EXT_HOST":/ext/ducklake.duckdb_extension:ro \
  -v "$LAKE_VOL":/lake \
  -e DUCKLAKE_EXTENSION=/ext/ducklake.duckdb_extension \
  -e SOURCE_PG_DSN="$SRC_DSN" \
  -e LAKE_CATALOG_DSN="$CAT_DSN" \
  -e LAKE_DATA_PATH=/lake/data/ \
  -e IMPORT_TABLES=person,visit \
  ducklake-scenario python /app/import_lake.py

echo
echo "=== 3. serve over pgwire and consume with an ordinary Postgres client ==="
docker run -d --name "$APP" --network "$NET" \
  -v "$EXT_HOST":/ext/ducklake.duckdb_extension:ro \
  -v "$LAKE_VOL":/lake \
  -e DUCKLAKE_EXTENSION=/ext/ducklake.duckdb_extension \
  -e LAKE_CATALOG_DSN="$CAT_DSN" \
  -e LAKE_DATA_PATH=/lake/data/ \
  -e GATEWAY_USER=postgres \
  -e GATEWAY_PASSWORD="$PGPASS" \
  ducklake-scenario python /app/gateway.py >/dev/null

i=0
until docker logs "$APP" 2>&1 | grep -q "serving pgwire"; do
  i=$((i+1))
  if [ "$i" -gt 60 ]; then
    echo "gateway did not start:" >&2
    docker logs "$APP" 2>&1 | tail -20 >&2
    exit 1
  fi
  sleep 1
done
docker logs "$APP" 2>&1 | grep "self-test" || true

# The consumer is a stock psql - no DuckDB, no extension, no knowledge that the
# data is encrypted Parquet behind a lake. That is the point of the leg.
echo
echo "  querying the lake through psql over the Postgres wire protocol:"
# The counts are ASSERTED, not printed for a human to eyeball. A leg that prints
# whatever it gets and exits 0 proves only that psql connected.
#
# The tables are named UNQUALIFIED on purpose. A stock client that had to know
# the lake's alias would not be the consumption path this scenario claims.
if OUT=$(docker exec "$PG" env PGPASSWORD="$PGPASS" psql -h "$APP" -p 5433 -U postgres -d lake -At -F'|' -c \
  "SELECT 'person' AS tbl, count(*) AS n FROM (SELECT * FROM person) x
   UNION ALL
   SELECT 'visit', count(*) FROM (SELECT * FROM visit) x
   ORDER BY tbl;" 2>&1)
then
  echo "$OUT" | sed 's/^/    /'
  fail=0
  for want in "person|5000" "visit|12000"; do
    echo "$OUT" | grep -qx "$want" || { echo "  ASSERTION FAILED: expected '$want' over the wire" >&2; fail=1; }
  done
  [ "$fail" -eq 0 ] || exit 1
  echo "  asserted: person=5000 visit=12000 read back over the Postgres wire"
else
  echo "$OUT" | sed 's/^/    /' >&2
  # A wire-protocol failure is opaque on the client side - "server closed the
  # connection unexpectedly" is all libpq will say, whatever went wrong. The
  # gateway's traceback is the only useful evidence, so surface it rather than
  # letting the cleanup trap destroy it.
  echo
  echo "  psql failed - gateway log follows:" >&2
  docker logs "$APP" 2>&1 | tail -30 >&2
  exit 1
fi

echo
echo "  a real row, decrypted end to end:"
docker exec "$PG" env PGPASSWORD="$PGPASS" psql -h "$APP" -p 5433 -U postgres -d lake -A -F' | ' -c \
  "SELECT person_id, birth_year, sex FROM person ORDER BY person_id LIMIT 3;"

# Negative control. Serving off-loopback means setting BUENAVISTA_HOST, which
# removes the only access control buenavista applies by default, so the md5 auth
# that replaces it has to be shown REFUSING - a password check that was never
# observed rejecting anything is indistinguishable from no password check.
echo
echo "  negative control - a wrong password must be refused:"
if docker exec "$PG" env PGPASSWORD="definitely-not-the-password" \
  psql -h "$APP" -p 5433 -U postgres -d lake -c "SELECT 1" >/dev/null 2>&1
then
  echo "  ASSERTION FAILED: the gateway accepted a wrong password" >&2
  exit 1
fi
echo "  asserted: refused"

echo
echo "PROVEN: 5000 person and 12000 visit rows imported from native Postgres into"
echo "a lake created ENCRYPTED, every data file carrying a 32-byte key, read back"
echo "through a stock psql over the Postgres wire - decrypted end to end - with a"
echo "wrong password refused. All of it asserted, none of it eyeballed."
