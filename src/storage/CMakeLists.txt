add_library(
  ducklake_storage OBJECT
ducklake_catalog.cpp			ducklake_insert.cpp			ducklake_schema_entry.cpp		ducklake_transaction_manager.cpp
ducklake_catalog_set.cpp		ducklake_multi_file_list.cpp		ducklake_storage.cpp
ducklake_delete.cpp			ducklake_multi_file_reader.cpp		ducklake_table_entry.cpp
ducklake_initializer.cpp		ducklake_scan.cpp			ducklake_transaction.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:ducklake_storage>
    PARENT_SCOPE)
