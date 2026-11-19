# postgres_scanner depends on libpq/OpenSSL, so it is built but never statically linked.
# DuckDB v2 removed DONT_LINK: extensions are linked only when listed with
# duckdb_extension_statically_link() or STATICALLY_LINK_EXTENSIONS, so omission is enough.
if (NOT MINGW AND NOT ${WASM_ENABLED})
    duckdb_extension_load(postgres_scanner
            GIT_URL https://github.com/duckdb/duckdb-postgres
            GIT_TAG 3fade09fdc00f42f419cdded76f3cc0c354366dd
            )
endif()
