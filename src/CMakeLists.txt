add_library(
  ducklake_library OBJECT
  ducklake_catalog.cpp ducklake_catalog_set.cpp ducklake_extension.cpp ducklake_initializer.cpp ducklake_insert.cpp ducklake_schema_entry.cpp ducklake_storage.cpp ducklake_table_entry.cpp ducklake_transaction.cpp ducklake_transaction_manager.cpp
  ducklake_multi_file_list.cpp ducklake_multi_file_reader.cpp ducklake_scan.cpp
)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:ducklake_library>
    PARENT_SCOPE)
