# PRIVATE-FORK ONLY. Never cherry-pick to the public upstream fork.
#
# A STANDALONE CMake project, deliberately not wired into the extension's own
# CMakeLists.txt.
#
# Two reasons, both fork-hygiene:
#
#   1. Zero divergence. `CMakeLists.txt` at the repo root also exists upstream,
#      so every line added there is re-resolved on every upstream merge. This
#      project adds none.
#   2. Cheap mutants. The red-then-green evidence for each refusal case needs the
#      client rebuilt with one guard deleted, once per guard. Hanging off the
#      DuckDB build would mean re-configuring a 700-target tree for each; here a
#      mutant configures and links in seconds, because it compiles two source
#      files against an ALREADY BUILT libduckdb.
#
# It consumes the artifacts of a normal `make release`:
#
#   cmake -S test/cpp/crypta -B build/crypta-test \
#     -DDUCKLAKE_ROOT=<repo> -DDUCKDB_BUILD_DIR=<repo>/build/release
#   cmake --build build/crypta-test
#
# `run_crypta_tests.sh` drives that, and then the mutants.

cmake_minimum_required(VERSION 3.5)
project(ducklake_crypta_test CXX)

# DuckDB pins CMAKE_CXX_STANDARD to 11 in its own top-level CMakeLists, so the
# client is COMPILED as C++11 in the shipped extension. Building the same
# translation unit under a newer standard here would be testing something the
# fork does not ship.
#
# Verified at EVERY patch `.github/duckdb-versions` publishes, not just one -
# v1.5.3 and v1.5.5 both `set(CMAKE_CXX_STANDARD "11" ...)`. If a future patch
# raises it, this line silently starts testing a different translation unit than
# the one that ships, so re-check it when adding a patch there.
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(NOT DEFINED DUCKLAKE_ROOT)
  get_filename_component(DUCKLAKE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
endif()
if(NOT DEFINED DUCKDB_BUILD_DIR)
  set(DUCKDB_BUILD_DIR "${DUCKLAKE_ROOT}/build/release")
endif()
# The sources under test. Overridden by the mutant runner to point at a copy with
# exactly one guard removed - which is the whole mechanism behind the red-first
# evidence, so it is a knob rather than a hardcoded path.
if(NOT DEFINED CRYPTA_SRC_DIR)
  set(CRYPTA_SRC_DIR "${DUCKLAKE_ROOT}/src/crypta")
endif()

set(DUCKDB_DIR "${DUCKLAKE_ROOT}/duckdb")
find_library(DUCKDB_LIBRARY NAMES duckdb PATHS "${DUCKDB_BUILD_DIR}/src" NO_DEFAULT_PATH)
if(NOT DUCKDB_LIBRARY)
  message(FATAL_ERROR "libduckdb not found in ${DUCKDB_BUILD_DIR}/src - run `make release` first")
endif()

add_executable(
  crypta_test
  crypta_test_main.cpp
  fake_crypta_server.cpp
  crypta_refusal_test.cpp
  crypta_cache_test.cpp
  crypta_injection_test.cpp
  "${CRYPTA_SRC_DIR}/crypta_client.cpp"
  "${CRYPTA_SRC_DIR}/ducklake_crypta.cpp")

# Mirrors the flags the extension build uses for these exact files (taken from
# build/release/compile_commands.json). DUCKDB_BUILD_LIBRARY in particular
# governs the DUCKDB_API visibility macros; compiling the client without it would
# be compiling a different translation unit than the one that ships.
target_compile_definitions(crypta_test PRIVATE DUCKDB_BUILD_LIBRARY)
target_include_directories(
  crypta_test
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
          "${DUCKLAKE_ROOT}/src/include"
          "${DUCKDB_DIR}/src/include"
          "${DUCKDB_DIR}/third_party/catch"
          "${DUCKDB_DIR}/third_party/fmt/include"
          "${DUCKDB_DIR}/third_party/fsst"
          "${DUCKDB_DIR}/third_party/hyperloglog"
          "${DUCKDB_DIR}/third_party/fastpforlib"
          "${DUCKDB_DIR}/third_party/skiplist"
          "${DUCKDB_DIR}/third_party/fast_float"
          "${DUCKDB_DIR}/third_party/re2"
          "${DUCKDB_DIR}/third_party/miniz"
          "${DUCKDB_DIR}/third_party/utf8proc/include"
          "${DUCKDB_DIR}/third_party/concurrentqueue"
          "${DUCKDB_DIR}/third_party/pcg"
          "${DUCKDB_DIR}/third_party/pdqsort"
          "${DUCKDB_DIR}/third_party/tdigest"
          "${DUCKDB_DIR}/third_party/mbedtls/include"
          "${DUCKDB_DIR}/third_party/httplib"
          "${DUCKDB_DIR}/third_party/jaro_winkler"
          "${DUCKDB_DIR}/third_party/vergesort"
          "${DUCKDB_DIR}/third_party/yyjson/include"
          "${DUCKDB_DIR}/third_party/zstd/include")

find_package(Threads REQUIRED)
target_link_libraries(crypta_test "${DUCKDB_LIBRARY}" Threads::Threads)
set_target_properties(crypta_test PROPERTIES BUILD_RPATH "${DUCKDB_BUILD_DIR}/src")

# Opt-in coverage for THIS binary, mirroring DUCKLAKE_ENABLE_COVERAGE in the
# top-level CMakeLists but scoped to the two files under test.
#
# It has to be separate, because the two builds are separate: the extension
# coverage build instruments src/crypta/ as compiled into `unittest`, and this
# project compiles the same sources into a standalone binary against
# libduckdb.so. Only the latter runs the refusal cases, so only the latter can
# measure them - the refusal branches are unreachable from anything the
# `unittest` binary runs, which is the whole premise of this suite.
#
# -O0 -g for the same reason the other block gives: once the optimiser has
# inlined and reordered, gcov's line attribution stops meaning anything.
option(CRYPTA_TEST_COVERAGE "Instrument the crypta sources under test for gcov" OFF)
if(CRYPTA_TEST_COVERAGE)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "CRYPTA_TEST_COVERAGE is GCC-only, got '${CMAKE_CXX_COMPILER_ID}'")
  endif()
  target_compile_options(crypta_test PRIVATE --coverage -O0 -g -fprofile-update=atomic)
  target_link_options(crypta_test PRIVATE --coverage)
endif()
