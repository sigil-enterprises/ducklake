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
