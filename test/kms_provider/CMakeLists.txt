cmake_policy(SET CMP0079 NEW)
# TEST-ONLY KMS provider. Never present in a release build.
#
# It is copied into src/crypta-provider/ by the Envelope workflow and picked up
# by the SAME `if(EXISTS crypta-provider)` hook a real out-of-tree provider uses
# - so this build exercises the production pickup path rather than a second one
# invented for tests.
#
# OBJECT, not STATIC, and its objects are appended to ALL_OBJECT_FILES: a
# separate archive would simply never be linked. OBJECT is necessary and NOT
# sufficient - for the `duckdb` binary DuckLake arrives as
# libducklake_extension.a, and an archive member no symbol references is
# dropped. What closes that is the explicit DuckLakeRegisterKmsProvider() call
# from ducklake_extension.cpp's Load().
add_library(ducklake_test_kms_provider OBJECT
  test_kms_provider.cpp
)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:ducklake_test_kms_provider>
    PARENT_SCOPE)

# The registry's Catch cases (issue #120) go into the `unittest` binary - and
# ONLY there: the provider objects above are also linked into the shell and the
# loadable extension, which carry no Catch. `unittest` does not exist yet when
# this file is read (duckdb adds extensions before test/), so the source is
# attached once the top-level directory has finished configuring.
set_property(GLOBAL PROPERTY DUCKLAKE_TEST_KMS_REGISTRY_UNITTEST
                             "${CMAKE_CURRENT_SOURCE_DIR}/test_kms_registry_unittest.cpp")
function(ducklake_test_kms_attach_registry_unittest)
  if(TARGET unittest)
    get_property(source GLOBAL PROPERTY DUCKLAKE_TEST_KMS_REGISTRY_UNITTEST)
    target_sources(unittest PRIVATE "${source}")
    # DuckDB v2.0 links `unittest` against the SHARED libduckdb, which does not
    # carry DuckLake: the registry cases call DuckLakeEncryptionProvider
    # directly, so the static extension archive must be linked in explicitly.
    if(TARGET ducklake_extension)
      target_link_libraries(unittest ducklake_extension)
    endif()
  endif()
endfunction()
# cmake_language(DEFER) needs CMake >= 3.19 although the project floor (cmake_minimum_required) is
# 3.5: say so here instead of failing with "unknown sub-command DEFER".
if(CMAKE_VERSION VERSION_LESS 3.19)
  message(FATAL_ERROR "the test KMS provider needs CMake >= 3.19 (cmake_language(DEFER)); found ${CMAKE_VERSION}")
endif()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL
               ducklake_test_kms_attach_registry_unittest)
