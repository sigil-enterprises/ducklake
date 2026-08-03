add_library(ducklake_crypta OBJECT crypta_client.cpp ducklake_crypta.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:ducklake_crypta>
    PARENT_SCOPE)
