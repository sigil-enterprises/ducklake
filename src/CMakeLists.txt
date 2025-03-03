add_library(
  ducklake_library OBJECT
  ducklake_catalog.cpp ducklake_extension.cpp ducklake_storage.cpp ducklake_transaction_manager.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:ducklake_library>
    PARENT_SCOPE)
