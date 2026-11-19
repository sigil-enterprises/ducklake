# DuckDB v2 removed DONT_LINK: extensions are linked only when listed with
# duckdb_extension_statically_link() or STATICALLY_LINK_EXTENSIONS.
# Static linking on windows does not properly work due to symbol collision,
# so sqlite_scanner is statically linked everywhere but windows.
duckdb_extension_load(sqlite_scanner
        LOAD_TESTS
        GIT_URL https://github.com/duckdb/duckdb-sqlite
        GIT_TAG d4ee2bcdd2678239bf84d0754900dce5293c0b65
        )
if (NOT WIN32)
    duckdb_extension_statically_link(sqlite_scanner)
endif()
