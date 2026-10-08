#pragma once
#include "library_validation.hpp"
#include <sqlite3.h>

namespace atx::server::library_detail {
// Borrowed SQLite handle: callers hold the Store mutex throughout each operation.
void migrate(sqlite3* db);
[[nodiscard]] AlphaSnapshot create(sqlite3* db, const Input& input, std::int64_t now);
[[nodiscard]] AlphaSnapshot update(sqlite3* db, std::string_view id,
    std::uint64_t expected_revision, const Input& input, std::int64_t now);
[[nodiscard]] AlphaSnapshot get(sqlite3* db, std::string_view id, std::uint64_t revision);
[[nodiscard]] AlphaPage list(sqlite3* db, std::uint32_t limit, std::uint32_t offset,
    std::string_view search, std::string_view tag, bool include_archived);
[[nodiscard]] AlphaPage history(sqlite3* db, std::string_view id,
    std::uint32_t limit, std::uint32_t offset);
[[nodiscard]] AlphaSnapshot archive(sqlite3* db, std::string_view id,
    std::uint64_t expected_revision, bool archived, std::int64_t now, std::string_view actor);
} // namespace atx::server::library_detail
