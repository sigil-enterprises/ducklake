"""
pgwire gateway over an ENCRYPTED DuckLake.

Mirrors what `opvance/teras-ext-pgwire` deploys, reduced to what this scenario
needs: open DuckDB, load the sigil fork extension, attach the encrypted lake,
and serve it on the Postgres wire so an ordinary client can read it.

Installation and the key dance are declared in opvance. This exists so the sigil
side can PROVE the consumption path without depending on a deployment it does
not own - which also means keeping the two in step on the details below.
"""

import logging
import os
import sys
import traceback

import duckdb
from buenavista import bv_dialects, postgres, rewrite
from buenavista.backends.duckdb import DuckDBConnection, DuckDBSession

LAKE_ALIAS = "lake"

# buenavista logs through `logging`. Without a configured handler its errors are
# dropped, and a failed handshake reaches the client as nothing more than
# "server closed the connection unexpectedly" - which is unactionable. Configure
# it, and print handler tracebacks explicitly, so a wire-protocol failure names
# itself instead of being inferred.
logging.basicConfig(level=logging.INFO, format="%(levelname)s %(name)s: %(message)s")

# ---------------------------------------------------------------------------
# Compatibility shim: the GSSENCRequest startup code.
#
# Modern libpq (psql 16, recent psycopg, JDBC) opens with 80877104
# (GSSENCRequest) BEFORE the SSL request. buenavista 0.5.0 knows only SSL
# (80877103) and raises "Unsupported startup message", closing the socket - the
# client sees "server closed the connection unexpectedly".
#
# Decline GSS with a single notice byte and recurse, so the client falls back to
# the StartupMessage. Same fix as the teras gateway; kept in step deliberately,
# because a scenario that passed with a client the real gateway rejects would
# prove nothing about the deployed path.
# ---------------------------------------------------------------------------
_orig_handle_startup = postgres.BuenaVistaHandler.handle_startup


def _handle_startup_gss_aware(self, conn):
    msglen = self.r.read_uint32() - 4
    code = self.r.read_uint32()
    if code in (80877104, 80877103):  # GSSENCRequest / SSLRequest - decline both
        self.send_notice()
        return self.handle_startup(conn)
    if code == 196608:  # protocol 3.0 StartupMessage
        msg = [x.decode("utf-8") for x in self.r.read_bytes(msglen - 4).split(b"\x00")]
        params = dict(zip(msg[::2], msg[1::2]))
        from buenavista.postgres import BVContext

        ctx = BVContext(conn.create_session(), self.server.rewriter, params)
        self.send_auth_request(ctx)
        return ctx
    if code == 80877102:  # CancelRequest
        process_id, secret_key = self.r.read_uint32(), self.r.read_uint32()
        cancelled = self.server.ctxts.get(process_id)
        if cancelled and cancelled.secret_key == secret_key:
            self.server.conn.close_session(cancelled.session)
            del self.server.ctxts[cancelled.process_id]
        return None
    raise Exception(f"Unsupported startup message: {code}")


postgres.BuenaVistaHandler.handle_startup = _handle_startup_gss_aware


class LakeDuckDBConnection(DuckDBConnection):
    """
    A connection whose SESSIONS default to the lake, not to `memory.main`.

    buenavista opens every client session on a fresh DuckDB cursor and runs
    `SET search_path='main'` on it unconditionally. That silently discards the
    `USE lake` done on the parent connection, so an ordinary client query -
    `SELECT * FROM person` - fails with "Table with name person does not exist.
    Did you mean lake.person?" even though the lake is attached and readable.

    Point each session at the lake instead. Overriding the session factory is
    the only place this can be fixed: it is not a connection-level setting, and
    the client cannot be asked to fully qualify every name - a stock client that
    must know the lake's alias is not the consumption path this scenario claims
    to prove.
    """

    def new_session(self) -> DuckDBSession:
        cursor = self.db.cursor()
        cursor.execute(f"USE {LAKE_ALIAS}")
        return DuckDBSession(cursor)


def build_connection() -> duckdb.DuckDBPyConnection:
    """
    Open DuckDB with the fork extension and the encrypted lake attached.
    """
    ext = os.environ["DUCKLAKE_EXTENSION"]

    # allow_unsigned_extensions is decided HERE, at database configuration time.
    # DuckDB reads it when the database is configured, so no later SET can turn
    # it on. The sigil fork cannot carry a valid signature - signatures verify
    # against DuckDB's own key, which a private fork cannot obtain.
    con = duckdb.connect(config={"allow_unsigned_extensions": True})
    con.execute(f"LOAD '{ext}'")
    con.execute("INSTALL postgres; LOAD postgres")

    catalog = os.environ["LAKE_CATALOG_DSN"]
    data_path = os.environ["LAKE_DATA_PATH"]
    con.execute(f"ATTACH 'ducklake:postgres:{catalog}' AS {LAKE_ALIAS} (DATA_PATH '{data_path}')")
    con.execute(f"USE {LAKE_ALIAS}")

    # Prove the lake is READABLE before serving, not merely attached.
    #
    # On an encrypted lake the attach succeeds and the catalog answers even when
    # the data cannot be decrypted - only the first real Parquet read fails. And
    # count(*) will NOT catch that: on a DuckLake table it is answered from the
    # catalog's record_count without opening a Parquet file. So read a real row.
    row = con.execute("SELECT count(*) FROM (SELECT * FROM person LIMIT 1) s").fetchone()
    if not row or row[0] != 1:
        print("gateway: self-test read returned nothing", file=sys.stderr)
        raise SystemExit(1)
    print("gateway: self-test passed - read one real row from the encrypted lake", flush=True)

    return con


def main() -> None:
    con = build_connection()
    host = os.environ.get("GATEWAY_HOST", "0.0.0.0")  # noqa: S104 - inside a container
    port = int(os.environ.get("GATEWAY_PORT", "5433"))

    # buenavista 0.5.0 refuses every connection whose source address is not
    # 127.0.0.1 unless BUENAVISTA_HOST is set - "until auth is in place", per its
    # own comment. socketserver rejects in verify_request(), BEFORE the handler,
    # so the refusal is completely silent: no log, no traceback, and the client
    # sees only "server closed the connection unexpectedly". Any containerised
    # gateway is off-loopback by construction, so this must be set.
    #
    # Setting it removes the ONLY access control buenavista applies by default,
    # so put real authentication in its place rather than leaving the port open:
    # GATEWAY_USER/GATEWAY_PASSWORD enable its md5 handshake. Unset means no
    # auth - acceptable only on a private network. The production deployment and
    # its credential handling are declared in opvance.
    os.environ.setdefault("BUENAVISTA_HOST", host)

    password = os.environ.get("GATEWAY_PASSWORD")
    auth = {os.environ.get("GATEWAY_USER", "postgres"): password} if password else None
    if auth is None:
        print("gateway: WARNING - no GATEWAY_PASSWORD, serving unauthenticated", file=sys.stderr, flush=True)

    rewriter = rewrite.Rewriter(bv_dialects.BVPostgres(), bv_dialects.BVDuckDB())
    server = postgres.BuenaVistaServer(
        (host, port), LakeDuckDBConnection(con), rewriter=rewriter, auth=auth
    )

    # socketserver swallows handler exceptions by default, which is how a
    # handshake bug becomes an unexplained closed socket.
    def _handle_error(request, client_address):
        print(f"gateway: handler error from {client_address}", file=sys.stderr, flush=True)
        traceback.print_exc()

    server.handle_error = _handle_error
    print(f"gateway: serving pgwire on {host}:{port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
