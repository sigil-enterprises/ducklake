# DuckDB DuckLake Extension

The DuckLake extension allows DuckDB to directly read and write data from DuckLake. See the [DuckLake website](https://ducklake.select) for more information.

## Usage

See the [Usage](https://ducklake.select/docs/stable/duckdb/introduction) guide.

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
