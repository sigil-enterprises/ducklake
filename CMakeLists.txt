cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME ducklake)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(src/include)

# Roaring is installed via vcpkg and used for deletion vectors
find_package(roaring CONFIG REQUIRED)
if(TARGET roaring::roaring-headers)
  get_target_property(ROARING_INCLUDE_DIRS roaring::roaring-headers INTERFACE_INCLUDE_DIRECTORIES)
else()
  get_target_property(ROARING_INCLUDE_DIRS roaring::roaring INTERFACE_INCLUDE_DIRECTORIES)
endif()
if(NOT ROARING_INCLUDE_DIRS OR ROARING_INCLUDE_DIRS STREQUAL "ROARING_INCLUDE_DIRS-NOTFOUND")
  set(ROARING_INCLUDE_DIRS "${roaring_DIR}/../../include")
endif()
include_directories(${ROARING_INCLUDE_DIRS})

# >>> FORK-LOCAL (sigil-enterprises): opt-in coverage instrumentation. >>>
# PRIVATE-FORK ONLY. Never cherry-pick this block upstream.
#
# OFF by default, and off means OFF: with DUCKLAKE_ENABLE_COVERAGE unset, not a
# single line below emits anything, so the stock build is byte-for-byte what
# upstream produces. Turn it on explicitly:
#
#   cmake ... -DDUCKLAKE_ENABLE_COVERAGE=ON
#
# The options are directory-scoped on purpose - they reach this CMakeLists and
# everything under src/, and nothing else. DuckDB's own translation units are
# compiled by their own directories and stay uninstrumented, so the report
# covers this repo rather than the engine it links against.
option(DUCKLAKE_ENABLE_COVERAGE
       "Instrument DuckLake's own sources for gcov/gcovr coverage" OFF)
if(DUCKLAKE_ENABLE_COVERAGE)
  # GCC only, deliberately narrow. Clang would accept --coverage too, but its
  # runtime is libclang_rt.profile, not libgcov - and the second block below
  # links gcov by name, so a Clang build would configure cleanly and then die at
  # link on `cannot find -lgcov`. This fork pins GCC 14 (see .devcontainer), so
  # widening it would only advertise a path nothing has ever compiled.
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "DUCKLAKE_ENABLE_COVERAGE is GCC-only, got '${CMAKE_CXX_COMPILER_ID}'")
  endif()
  # -O0 -g, appended after the build-type flags so they win: gcov attributes
  #   hits to lines, and once the optimiser has inlined and reordered them the
  #   line numbers no longer mean anything.
  # -fprofile-update=atomic: DuckDB's test runner is multi-threaded, and
  #   non-atomic counter updates lose hits and can corrupt the .gcda outright.
  add_compile_options(--coverage -O0 -g -fprofile-update=atomic)
  add_link_options(--coverage)
endif()
# <<< FORK-LOCAL (sigil-enterprises) <<<

add_subdirectory(src)
set(EXTENSION_SOURCES ${ALL_OBJECT_FILES})

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# >>> FORK-LOCAL (sigil-enterprises): opt-in coverage instrumentation. >>>
# PRIVATE-FORK ONLY. Never cherry-pick this block upstream.
#
# The static extension is archived, never linked, so add_link_options above
# never reaches it. libgcov has to arrive at whatever binary consumes the
# archive - DuckDB's `unittest` - or that link fails on __gcov_* . The plain
# target_link_libraries signature propagates gcov as a usage requirement, which
# is exactly that. Still inside the guard: default builds gain no dependency.
if(DUCKLAKE_ENABLE_COVERAGE)
  target_link_libraries(${EXTENSION_NAME} gcov)
endif()
# <<< FORK-LOCAL (sigil-enterprises) <<<

if(TARGET roaring::roaring-headers)
  target_link_libraries(${EXTENSION_NAME} roaring::roaring roaring::roaring-headers roaring::roaring-headers-cpp)
  target_link_libraries(${TARGET_NAME}_loadable_extension roaring::roaring roaring::roaring-headers roaring::roaring-headers-cpp)
else()
  target_link_libraries(${EXTENSION_NAME} roaring::roaring)
  target_link_libraries(${TARGET_NAME}_loadable_extension roaring::roaring)
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
