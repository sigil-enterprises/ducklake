# Scenario: native Postgres -> ENCRYPTED DuckLake -> consumed over the Postgres wire

End-to-end proof that a real client can import a native Postgres database into a
DuckLake created `ENCRYPTED` and read it back with a stock Postgres client, with
the data at rest encrypted the whole way.

The FEATURES are sigil's. The deployment that runs this - and the key dance
around the KEK - is declared in opvance. This scenario exists so the feature can
be proven without depending on a deployment this repo does not own, which also
means the two have to stay in step on the details below.

## Running it

```sh
# a linux extension build for this arch, from the release assets
DUCKLAKE_EXTENSION_HOST=/path/to/ducklake-v1.5.3-linux_arm64.duckdb_extension \
  sh scripts/scenario_pg_to_ducklake/run.sh
```

Needs Docker. Exits non-zero on any failed assertion; cleans up its containers
and network on exit. The lake volume is recreated from scratch each run.

## What it actually asserts

Nothing here is printed for a human to eyeball - every claim is an assertion that
can fail the run.

| leg | assertion |
|-----|-----------|
| 1 IMPORT | `person` and `visit` copied from a native Postgres into a lake created `ENCRYPTED`, with a Postgres catalog |
| 2 VERIFY | rows reconcile source vs lake, and the lake side is counted by **reading rows** |
| 2 VERIFY | every data file carries a key, and `min_key_bytes = 32` - no file slipped through as plaintext, none at the old 128-bit size |
| 3 CONSUME | `person=5000` and `visit=12000` read back through a **stock psql** over the wire |
| 3 CONSUME | three real `person` rows returned, decrypted from encrypted Parquet |
| 3 CONSUME | negative control: a **wrong password is refused** |

Two of these are subtle and were chosen deliberately:

- **The lake side is counted by reading rows, never by bare `count(*)`.** On a
  DuckLake table `count(*)` is answered from the catalog's `record_count` and
  never opens a Parquet file - so it reports success on a lake whose data cannot
  be decrypted. `SELECT count(*) FROM (SELECT * FROM t) x` forces the read.
- **The wrong-password control is not decoration.** Serving off-loopback means
  setting `BUENAVISTA_HOST` (below), which removes buenavista's only default
  access control. A password check never observed rejecting anything is
  indistinguishable from no password check.

## Two buenavista 0.5.0 traps, both of which cost real time

Both produce the same useless client-side symptom - `server closed the
connection unexpectedly` - and neither logs anything server-side.

**1. It refuses every non-loopback client, silently.**

```python
def verify_request(self, request, client_address) -> bool:
    """Ensure all requests come from localhost until auth is in place"""
    return client_address[0] == "127.0.0.1" or "BUENAVISTA_HOST" in os.environ
```

`socketserver` rejects in `verify_request()`, which runs **before** the handler,
so there is no handler invocation, no traceback, and no log line. Any
containerised gateway is off-loopback by construction, so `BUENAVISTA_HOST` must
be set. Doing that removes the only access control buenavista applies by default,
so `gateway.py` puts its md5 auth (`GATEWAY_USER` / `GATEWAY_PASSWORD`) in its
place rather than leaving the port open.

**2. It resets `search_path` on every session, discarding `USE lake`.**

```python
def new_session(self) -> Session:
    cursor = self.db.cursor()
    cursor.execute("SET search_path='main'")   # unconditional
    return DuckDBSession(cursor)
```

So an ordinary `SELECT * FROM person` fails with `Table with name person does
not exist. Did you mean "lake.person"?` even though the lake is attached and
readable. `gateway.py` subclasses the connection and points each new session at
the lake. Overriding the session factory is the only place this can be fixed: it
is not a connection-level setting, and requiring the client to fully qualify
every name would not be the consumption path this scenario claims to prove.

## Pins that are load-bearing

- **`duckdb==1.5.3`, exactly.** A DuckDB extension loads only under the exact
  patch it was built against - not the minor. A 1.5.5 runtime refuses a 1.5.3
  build outright. Same reason `opvance/teras-ext-pgwire` fixes `DUCKDB_VERSION`.
- **`pyarrow`, `python-dateutil`, `sqlglot`.** buenavista 0.5.0's undeclared
  runtime imports - it does `import dateutil.parser` at module scope without
  requiring it, so a bare `pip install buenavista` fails on first import. The
  same set is pinned in opvance; kept in step deliberately.
- **`ENCRYPTED` is fixed at lake CREATION.** It cannot be added to an existing
  lake - re-attaching one with `ENCRYPTED` is refused. Hence the import creates
  the lake encrypted from the first attach. Migrating an existing plaintext lake
  is a data copy: see `scripts/migrate_lake_to_encrypted.sh`.

## What this does NOT prove

- The gateway holds a live, readable handle on the lake. This is catalog-at-rest
  protection: it defends against a DBA reading the catalog, a stolen backup, or a
  replica. It does **not** protect against a compromised live reader.
- The scenario uses the lake's own key handling, not crypta. The KEK dance is
  declared in opvance and proven separately by `scripts/mvp_crypta_proof.sh`.
- The scenario password is a throwaway fixture. Real credential handling belongs
  to the opvance deployment.
