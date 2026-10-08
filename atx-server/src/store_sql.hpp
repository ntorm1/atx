#pragma once
#include <atx/server/core.hpp>
#include <sqlite3.h>
#include <iomanip>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <string_view>

namespace atx::server::store_detail {
struct DbCloser { void operator()(sqlite3* db) const noexcept { sqlite3_close_v2(db); } };
struct StatementCloser {
    void operator()(sqlite3_stmt* stmt) const noexcept { sqlite3_finalize(stmt); }
};
using Database = std::unique_ptr<sqlite3, DbCloser>;
using Statement = std::unique_ptr<sqlite3_stmt, StatementCloser>;

[[noreturn]] inline void database_error(sqlite3* db) {
    throw Error(ErrorCode::unavailable, std::string("database: ") + sqlite3_errmsg(db));
}
inline Statement prepare(sqlite3* db, const char* sql) {
    sqlite3_stmt* statement = nullptr; // Borrowed until adopted by the RAII wrapper.
    if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK) {
        database_error(db);
    }
    return Statement(statement);
}
inline void execute(sqlite3* db, const char* sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) database_error(db);
}
class Transaction final {
public:
    explicit Transaction(sqlite3* db) : db_(db) { execute(db_, "BEGIN IMMEDIATE"); }
    ~Transaction() {
        if (!committed_) {
            static_cast<void>(sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr));
        }
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void commit() { execute(db_, "COMMIT"); committed_ = true; }
private:
    sqlite3* db_; // Borrowed; owner outlives the transaction scope.
    bool committed_{};
};
inline void bind_value(Statement& stmt, int index, std::string_view value) {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw Error(ErrorCode::invalid, "database field exceeds length limit");
    }
    const auto* bytes = value.empty() ? "" : value.data();
    if (sqlite3_bind_text(stmt.get(), index, bytes, static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
        database_error(sqlite3_db_handle(stmt.get()));
    }
}
inline void bind_value(Statement& stmt, int index, std::int64_t value) {
    if (sqlite3_bind_int64(stmt.get(), index, value) != SQLITE_OK) {
        database_error(sqlite3_db_handle(stmt.get()));
    }
}
inline int step(Statement& stmt) {
    const auto result = sqlite3_step(stmt.get());
    if (result != SQLITE_ROW && result != SQLITE_DONE) {
        database_error(sqlite3_db_handle(stmt.get()));
    }
    return result;
}
inline std::string column(Statement& stmt, int index) {
    const auto* text = sqlite3_column_text(stmt.get(), index);
    if (text == nullptr) return {};
    // SAFETY: SQLite exposes UTF-8 bytes; character aliasing is permitted by C++.
    return {reinterpret_cast<const char*>(text),
            static_cast<std::size_t>(sqlite3_column_bytes(stmt.get(), index))};
}
inline std::string identifier() {
    std::random_device random;
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i) out << std::setw(8) << random();
    return out.str();
}
} // namespace atx::server::store_detail
