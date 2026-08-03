add_subdirectory(common)
# PRIVATE-FORK ONLY: the crypta envelope key provider. Not upstream-eligible.
add_subdirectory(crypta)
add_subdirectory(functions)
add_subdirectory(metadata_manager)
add_subdirectory(storage)

add_library(ducklake_library OBJECT ducklake_extension.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:ducklake_library>
    PARENT_SCOPE)
