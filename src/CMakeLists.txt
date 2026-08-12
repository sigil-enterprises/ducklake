add_subdirectory(common)
add_subdirectory(functions)
add_subdirectory(metadata_manager)
add_subdirectory(storage)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/crypta-provider")
  message(STATUS "DuckLake: crypta-provider found, adding")
  add_subdirectory(crypta-provider)
else()
  message(STATUS "DuckLake: no crypta-provider (KMS-agnostic build)")
endif()

add_library(ducklake_library OBJECT ducklake_extension.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:ducklake_library>
    PARENT_SCOPE)
