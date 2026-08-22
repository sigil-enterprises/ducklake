add_subdirectory(common)
add_subdirectory(functions)
add_subdirectory(metadata_manager)
add_subdirectory(storage)

# A concrete KMS encryption provider, if this build was overlaid with one.
# DuckLake itself carries only the abstract DuckLakeEncryptionProvider; the
# directory below is supplied out of tree by whoever owns the KMS.
set(DUCKLAKE_KMS_PROVIDER OFF)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/crypta-provider")
  message(STATUS "DuckLake: crypta-provider found, adding")
  add_subdirectory(crypta-provider)
  set(DUCKLAKE_KMS_PROVIDER ON)
else()
  message(STATUS "DuckLake: no crypta-provider (KMS-agnostic build)")
endif()

add_library(ducklake_library OBJECT ducklake_extension.cpp)
if(DUCKLAKE_KMS_PROVIDER)
  # Turns on the declaration AND the call in ducklake_extension.cpp. The call is
  # what forces the provider's objects into the link: without a reference to
  # them, the duckdb binary drops them out of libducklake_extension.a and the
  # provider registers nothing. See the comment on DuckLakeRegisterKmsProvider.
  target_compile_definitions(ducklake_library PRIVATE DUCKLAKE_KMS_PROVIDER)
endif()

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:ducklake_library>
    PARENT_SCOPE)
