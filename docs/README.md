<div align="center">
  <picture>
    <source media="(prefers-color-scheme: light)" srcset="logo/DuckLake_Logo-horizontal.svg">
    <source media="(prefers-color-scheme: dark)" srcset="logo/DuckLake_Logo-horizontal.svg">
    <img alt="DuckLake logo" src="logo/DuckLake_Logo-horizontal.svg" height="100">
  </picture>
</div>
<br>

# DuckDB DuckLake Extension

DuckLake is an open Lakehouse format that is built on SQL and Parquet. DuckLake stores metadata in a [catalog database](https://ducklake.select/docs/stable/duckdb/usage/choosing_a_catalog_database), and stores data in Parquet files. The DuckLake extension allows DuckDB to directly read and write data from DuckLake.

See the [DuckLake website](https://ducklake.select) for more information.

## Usage

DuckLake databases can be attached using the  [`ATTACH`](https://duckdb.org/docs/stable/sql/statements/attach.html) syntax, after which tables can be created, modified and queried using standard SQL.

Below is a short usage example that stores the metadata in a DuckDB database file called `metadata.ducklake`, and the data in Parquet files in the `file_path` directory:

```sql
ATTACH 'ducklake:metadata.ducklake' AS my_ducklake (DATA_PATH 'file_path/');
USE my_ducklake;
CREATE TABLE my_ducklake.my_table(id INTEGER, val VARCHAR);
INSERT INTO my_ducklake.my_table VALUES (1, 'Hello'), (2, 'World');
FROM my_ducklake.my_table;
┌───────┬─────────┐
│  id   │   val   │
│ int32 │ varchar │
├───────┼─────────┤
│     1 │ Hello   │
│     2 │ World   │
└───────┴─────────┘
-- updates
UPDATE my_ducklake.my_table SET val='DuckLake' WHERE id=2;
FROM my_ducklake.my_table;
┌───────┬──────────┐
│  id   │   val    │
│ int32 │ varchar  │
├───────┼──────────┤
│     1 │ Hello    │
│     2 │ DuckLake │
└───────┴──────────┘
-- time travel
FROM my_ducklake.my_table AT (VERSION => 2);
┌───────┬─────────┐
│  id   │   val   │
│ int32 │ varchar │
├───────┼─────────┤
│     1 │ Hello   │
│     2 │ World   │
└───────┴─────────┘
-- schema evolution
ALTER TABLE my_ducklake.my_table ADD COLUMN new_column VARCHAR;
FROM my_ducklake.my_table;
┌───────┬──────────┬────────────┐
│  id   │   val    │ new_column │
│ int32 │ varchar  │  varchar   │
├───────┼──────────┼────────────┤
│     1 │ Hello    │ NULL       │
│     2 │ DuckLake │ NULL       │
└───────┴──────────┴────────────┘
-- change data feed
FROM my_ducklake.table_changes('my_table', 2, 2);
┌─────────────┬───────┬─────────────┬───────┬─────────┐
│ snapshot_id │ rowid │ change_type │  id   │   val   │
│    int64    │ int64 │   varchar   │ int32 │ varchar │
├─────────────┼───────┼─────────────┼───────┼─────────┤
│           2 │     0 │ insert      │     1 │ Hello   │
│           2 │     1 │ insert      │     2 │ World   │
└─────────────┴───────┴─────────────┴───────┴─────────┘
```

See the [Usage](https://ducklake.select/docs/stable/duckdb/introduction) guide for more information.

## Building & Loading the Extension

To build, type
```
# to build with multiple cores, use `make GEN=ninja release`
make pull
make
```

To run, run the bundled `duckdb` shell:
```
 ./build/release/duckdb
```
