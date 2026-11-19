# DuckDB v2 removed DONT_LINK: extensions are linked only when listed with
# duckdb_extension_statically_link() or STATICALLY_LINK_EXTENSIONS.
# Static linking on windows does not properly work due to symbol collision,
# so sqlite_scanner is statically linked everywhere but windows.
duckdb_extension_load(sqlite_scanner
        LOAD_TESTS
        GIT_URL https://github.com/duckdb/duckdb-sqlite
        GIT_TAG e223dc01493414cef6d5960744f1e048d59fc592
        SUBMODULES database-connector
        )
if (NOT WIN32)
    duckdb_extension_statically_link(sqlite_scanner)
endif()
