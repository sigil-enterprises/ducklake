add_subdirectory(common)
add_subdirectory(functions)
add_subdirectory(metadata_manager)
add_subdirectory(storage)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/crypta-provider")
  message(STATUS "DuckLake: crypta-provider found, adding")
  add_subdirectory(crypta-provider)
  # Gate the DuckLakeRegisterKmsProvider() call in ducklake_extension.cpp.
  # The provider's objects link in either way (OBJECT library, appended to
  # ALL_OBJECT_FILES above) - this definition is what makes DuckLake actually
  # CALL its registration entry point instead of silently linking dead code.
  add_compile_definitions(DUCKLAKE_KMS_PROVIDER)
else()
  message(STATUS "DuckLake: no crypta-provider (KMS-agnostic build)")
endif()

add_library(ducklake_library OBJECT ducklake_extension.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:ducklake_library>
    PARENT_SCOPE)
