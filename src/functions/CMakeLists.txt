add_library(ducklake_functions OBJECT base_metadata_function.cpp
                                      ducklake_snapshots.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:ducklake_functions>
    PARENT_SCOPE)
