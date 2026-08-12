//===----------------------------------------------------------------------===//
//                         DuckLake
//
// storage/ducklake_encryption_provider.cpp
//
// Static helpers and factory storage for the abstract KMS-agnostic provider.
//
//===----------------------------------------------------------------------===//

#include "storage/ducklake_encryption_provider.hpp"

#include "duckdb/common/types/blob.hpp"

#include <cstdint>

namespace duckdb {

bool DuckLakeEncryptionProvider::LooksWrapped(const string &base64_value) {
  // The magic "DLK1" comes first in the blob, so it survives base64
  // verbatim as "RExL". Any blob starting with those four bytes is a
  // wrapped key; anything else is not.
  if (base64_value.size() < 4) {
    return false;
  }
  return base64_value[0] == 'R' && base64_value[1] == 'E' &&
         base64_value[2] == 'x' && base64_value[3] == 'L';
}

bool DuckLakeEncryptionProvider::IsBase64(const string &value) {
  for (size_t i = 0; i < value.size(); i++) {
    unsigned char c = static_cast<unsigned char>(value[i]);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=') {
      continue;
    }
    return false;
  }
  return true;
}

//! Process-wide factory for creating concrete KMS providers.
//!
//! Static storage, not a function-local static: a registered factory must
//! survive across ATTACH/DETACH cycles within the same process. This is a
//! raw pointer (never deleted) because the factory is registered once at
//! extension init and lives for the life of the process.
namespace {
DuckLakeEncryptionProvider::Factory *g_factory = nullptr;
} // namespace

void DuckLakeEncryptionProvider::RegisterFactory(Factory factory) {
  if (!g_factory) {
    g_factory = new Factory(std::move(factory));
    return;
  }
  *g_factory = std::move(factory);
}

const DuckLakeEncryptionProvider::Factory &
DuckLakeEncryptionProvider::GetFactory() {
  static Factory empty;
  if (!g_factory) {
    return empty;
  }
  return *g_factory;
}

DuckLakeEncryptionProvider::Factory DuckLakeEncryptionProvider::factory_;

} // namespace duckdb
