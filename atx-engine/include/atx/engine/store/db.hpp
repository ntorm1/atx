#pragma once
// atx::engine::store::StoreDb — one SQLite connection per environment file
// (atx_dev/uat/prod.sqlite). Opens WAL, runs the v2 schema, exposes the Database.

#include <string>
#include <string_view>
#include <utility>

#include "atx/core/db/sqlite.hpp" // db::Database, OpenMode
#include "atx/core/error.hpp"     // Result, Status, Ok, ATX_TRY*
#include "atx/engine/store/schema.hpp"

namespace atx::engine::store {

class StoreDb {
public:
  [[nodiscard]] static atx::core::Result<StoreDb> open(std::string_view path) {
    ATX_TRY(auto db, atx::core::db::Database::open(path, atx::core::db::OpenMode::ReadWriteCreate));
    return bootstrap(std::move(db));
  }
  [[nodiscard]] static atx::core::Result<StoreDb> open_memory() {
    ATX_TRY(auto db, atx::core::db::Database::open_memory());
    return bootstrap(std::move(db));
  }

  [[nodiscard]] atx::core::db::Database& db() noexcept { return db_; }

  [[nodiscard]] atx::core::Result<int> schema_version() {
    ATX_TRY(auto* stmt, db_.prepare_cached("SELECT schema_version FROM schema_meta LIMIT 1"));
    ATX_TRY(const auto step, stmt->step());
    if (step == atx::core::db::Statement::Step::Done) {
      return atx::core::Err(atx::core::ErrorCode::NotFound, "StoreDb: schema_meta empty");
    }
    return atx::core::Ok(static_cast<int>(stmt->column_int(0)));
  }

private:
  explicit StoreDb(atx::core::db::Database db) : db_{std::move(db)} {}

  [[nodiscard]] static atx::core::Result<StoreDb> bootstrap(atx::core::db::Database db) {
    // WAL: concurrent readers under a single writer. NORMAL sync is the WAL norm.
    ATX_TRY_VOID(db.pragma("journal_mode", "WAL"));
    ATX_TRY_VOID(db.pragma("synchronous", "NORMAL"));
    ATX_TRY_VOID(db.pragma("foreign_keys", "ON"));
    ATX_TRY_VOID(schema::create_all(db));
    return atx::core::Ok(StoreDb{std::move(db)});
  }

  atx::core::db::Database db_;
};

}  // namespace atx::engine::store
