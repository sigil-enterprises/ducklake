# Scenario image: the import leg and the pgwire gateway leg.
#
# duckdb is pinned to 1.5.3 and that is load-bearing, not tidiness. A DuckDB
# extension loads only under the EXACT patch it was built against - the sigil
# fork extension is built for v1.5.3, and a 1.5.5 runtime refuses it with
# "built specifically for DuckDB version 'v1.5.3'". The same pin is why
# opvance/teras-ext-pgwire fixes DUCKDB_VERSION=1.5.3.
FROM python:3.11-slim

# buenavista is the pgwire shim the teras gateway uses, so the scenario exercises
# the same consumption path rather than a lookalike.
#
# pyarrow / python-dateutil / sqlglot are buenavista 0.5.0's UNDECLARED runtime
# imports - it does `import dateutil.parser` at module scope without requiring it,
# so a bare `pip install buenavista` yields a package that fails on first import.
# The same set is pinned in opvance/teras-ext-pgwire; kept in step deliberately.
RUN pip install --no-cache-dir \
      "duckdb==1.5.3" \
      "buenavista==0.5.0" \
      "pyarrow>=14" \
      "python-dateutil>=2.8" \
      "sqlglot>=20"

WORKDIR /app
COPY import_lake.py gateway.py /app/
