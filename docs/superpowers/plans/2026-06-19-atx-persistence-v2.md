# atx-engine Persistence Layer v2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a greenfield SQLite persistence layer (`atx::engine::store`) that tracks the full alpha lifecycle — identity, lineage, runs, fingerprints, per-alpha event history, lifecycle state, and Dev/UAT/PROD environments — with heavy PnL/weight arrays staying in existing binary segments.

**Architecture:** One SQLite file per environment, opened via the existing `atx::core::db` RAII wrapper. A single `Database` connection per env (one writer per thread, WAL). All units are header-only namespaces of free functions operating on a shared `db::Database&`, mirroring the existing header-only style of `library/lifecycle.hpp`. `canon_hash` (u64) is the universal join key. Heavy time-series stays in mmap binary segments; the DB indexes them.

**Tech Stack:** C++17, CMake/Ninja, vendored SQLite via `atx::core::db` (`Database`/`Statement`/`Transaction`, `Result<T>`/`Status`, `ATX_TRY*`), GoogleTest.

## Global Constraints

- **C++ standard / build:** match the engine target; build with `cmake --preset ninja`, binary dir is `build/`.
- **Threading:** `SQLITE_THREADSAFE=2` — one `db::Database` per thread, never shared across threads.
- **No wall-clock reads in the engine.** Every timestamp (`created_at`, `ts`, `started_at`, …) and every `run_id` is **supplied by the caller**, never read inside `store`. This preserves replay/reproducibility.
- **Error handling:** use `Result<T>`/`Status` + `ATX_TRY`/`ATX_TRY_VOID`/`Ok`/`Err(ErrorCode::…, "msg")`. Never throw for control flow. Bind indices are 1-based; column indices 0-based.
- **u64 → INTEGER:** hashes/ids are u64; store via `static_cast<atx::i64>(value)` and read back via `static_cast<atx::u64>(stmt.column_int(col))` (bit-pattern preserving for the values produced here, mirroring `lifecycle.hpp`).
- **Append-only:** `alpha_event` and `lifecycle_journal` are INSERT-only — never UPDATE/DELETE. `seq`/`event_id` are `INTEGER PRIMARY KEY AUTOINCREMENT` for deterministic tie-break.
- **Namespace:** `atx::engine::store`. Headers under `atx-engine/include/atx/engine/store/`. Tests under `atx-engine/tests/store/`.
- **Header-only:** units are inline header-only (no `.cpp`), matching `library/lifecycle.hpp`. The engine include path already exposes `include/`, so no `atx-engine/CMakeLists.txt` change is needed for headers.

## File Structure

| File | Responsibility |
|---|---|
| `include/atx/engine/store/schema.hpp` | DDL for all tables + `schema_meta`; `create_all(db)`; `kSchemaVersion`. |
| `include/atx/engine/store/db.hpp` | `StoreDb`: per-env open (file/memory), WAL pragma, runs `schema::create_all`. |
| `include/atx/engine/store/alpha_catalog.hpp` | Alpha identity + lineage upsert; dedup via PK. |
| `include/atx/engine/store/universe_registry.hpp` | `universe`, `universe_member`, `data_snapshot`. |
| `include/atx/engine/store/fingerprint.hpp` | Pure `compute(RunInputs) -> u64`; replay lookup. |
| `include/atx/engine/store/run_recorder.hpp` | `RunRecorder` RAII: run row + params + run_alpha + metrics, status running→committed. |
| `include/atx/engine/store/event_log.hpp` | `alpha_event` append + `lifecycle_journal` projection (dual-write); history + state_as_of. |
| `include/atx/engine/store/segment_index.hpp` | `segment` + `segment_alpha` (locate PnL/weight arrays). |
| `include/atx/engine/store/env_config.hpp` | `env_config` key/value for this env. |
| `include/atx/engine/store/promotion.hpp` | Cross-env promote via ATTACH + `promotion_ledger`. |
| `tests/store/*_test.cpp` | One test file per unit + one integration test. |
| `tests/CMakeLists.txt` | Add `store` to `ATX_ALL_TEST_GROUPS`. |

---

## Task 1: Schema + StoreDb + build wiring

**Files:**
- Create: `atx-engine/include/atx/engine/store/schema.hpp`
- Create: `atx-engine/include/atx/engine/store/db.hpp`
- Create: `atx-engine/tests/store/store_db_test.cpp`
- Modify: `atx-engine/tests/CMakeLists.txt:16` (add `store` to group list)

**Interfaces:**
- Produces:
  - `namespace atx::engine::store::schema { constexpr int kSchemaVersion = 1; atx::core::Status create_all(atx::core::db::Database& db); }`
  - `class atx::engine::store::StoreDb` with `static Result<StoreDb> open(std::string_view path);`, `static Result<StoreDb> open_memory();`, `atx::core::db::Database& db() noexcept;`, `Result<int> schema_version();`

- [ ] **Step 1: Add the `store` test group**

In `atx-engine/tests/CMakeLists.txt:16`, change:
```cmake
set(ATX_ALL_TEST_GROUPS
    alpha risk data factory parallel learn eval library combine fund book core regime)
```
to:
```cmake
set(ATX_ALL_TEST_GROUPS
    alpha risk data factory parallel learn eval library combine fund book core regime store)
```

- [ ] **Step 2: Write the failing test**

Create `atx-engine/tests/store/store_db_test.cpp`:
```cpp
// store_db_test.cpp — StoreDb open + schema bootstrap (persistence v2 Task 1).
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "atx/engine/store/db.hpp"
#include "atx/engine/store/schema.hpp"

namespace atxtest_store_db_test {

using atx::engine::store::StoreDb;

[[nodiscard]] std::string tmpdir() {
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "atx_store_v2" /
      (std::string(info->test_suite_name()) + "_" + info->name());
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

TEST(StoreDb, MemoryOpenSetsSchemaVersion) {
  auto store = StoreDb::open_memory();
  ASSERT_TRUE(store.has_value());
  auto v = store->schema_version();
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, atx::engine::store::schema::kSchemaVersion);
}

TEST(StoreDb, FileOpenIsIdempotentAcrossReopen) {
  const std::string path = tmpdir() + "/atx_dev.sqlite";
  { auto s1 = StoreDb::open(path); ASSERT_TRUE(s1.has_value()); }
  auto s2 = StoreDb::open(path); // re-open existing file: CREATE IF NOT EXISTS is a no-op
  ASSERT_TRUE(s2.has_value());
  auto v = s2->schema_version();
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, atx::engine::store::schema::kSchemaVersion);
}

}  // namespace atxtest_store_db_test
```

- [ ] **Step 3: Run test to verify it fails (no header yet)**

Run: `cmake --preset ninja -DATX_TEST_GROUPS=store && cmake --build build --target atx-engine-store-tests`
Expected: FAIL — `fatal error: atx/engine/store/db.hpp: No such file`.

- [ ] **Step 4: Write `schema.hpp`**

Create `atx-engine/include/atx/engine/store/schema.hpp`:
```cpp
#pragma once
// atx::engine::store::schema — greenfield v2 DDL for the alpha-lifecycle persistence
// layer. All tables are CREATE TABLE IF NOT EXISTS so create_all is idempotent across
// reopen. Append-only tables (alpha_event, lifecycle_journal) use AUTOINCREMENT PK.

#include "atx/core/db/sqlite.hpp" // db::Database
#include "atx/core/error.hpp"     // Status, Ok, ATX_TRY_VOID

namespace atx::engine::store::schema {

inline constexpr int kSchemaVersion = 1;

// Create every v2 table if absent and stamp schema_meta. Idempotent.
[[nodiscard]] inline atx::core::Status create_all(atx::core::db::Database& db) {
  ATX_TRY_VOID(db.exec(
    "CREATE TABLE IF NOT EXISTS alpha ("
    " canon_hash INTEGER PRIMARY KEY, alpha_id INTEGER UNIQUE, expr_source TEXT NOT NULL,"
    " created_at INTEGER NOT NULL, first_run_id TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS alpha_lineage ("
    " child_hash INTEGER NOT NULL, parent_hash INTEGER NOT NULL,"
    " mutation_op INTEGER NOT NULL, seed INTEGER NOT NULL,"
    " PRIMARY KEY(child_hash, parent_hash));"
    "CREATE TABLE IF NOT EXISTS universe ("
    " universe_id TEXT PRIMARY KEY, name TEXT, as_of INTEGER, rule TEXT, content_hash INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS universe_member ("
    " universe_id TEXT NOT NULL, instrument_id INTEGER NOT NULL,"
    " PRIMARY KEY(universe_id, instrument_id));"
    "CREATE TABLE IF NOT EXISTS data_snapshot ("
    " snapshot_id TEXT PRIMARY KEY, source TEXT, as_of INTEGER, content_hash INTEGER);"
    "CREATE TABLE IF NOT EXISTS run ("
    " run_id TEXT PRIMARY KEY, run_fingerprint INTEGER UNIQUE, kind TEXT NOT NULL,"
    " status TEXT NOT NULL, engine_git_sha TEXT, master_seed INTEGER,"
    " universe_id TEXT, snapshot_id TEXT, fit_start INTEGER, fit_end INTEGER,"
    " bt_start INTEGER, bt_end INTEGER, position_mode TEXT, sector_neutral INTEGER,"
    " rebalance_every INTEGER, cost_model TEXT, manifest_version_id INTEGER,"
    " result_digest INTEGER, started_at INTEGER, finished_at INTEGER);"
    "CREATE TABLE IF NOT EXISTS run_param ("
    " run_id TEXT NOT NULL, key TEXT NOT NULL, value TEXT, PRIMARY KEY(run_id, key));"
    "CREATE TABLE IF NOT EXISTS run_alpha ("
    " run_id TEXT NOT NULL, canon_hash INTEGER NOT NULL, role TEXT NOT NULL,"
    " PRIMARY KEY(run_id, canon_hash));"
    "CREATE TABLE IF NOT EXISTS alpha_metrics ("
    " run_id TEXT NOT NULL, canon_hash INTEGER NOT NULL, sharpe REAL, returns REAL,"
    " drawdown REAL, turnover REAL, margin REAL, fitness REAL,"
    " PRIMARY KEY(run_id, canon_hash));"
    "CREATE TABLE IF NOT EXISTS eval_fold ("
    " run_id TEXT NOT NULL, canon_hash INTEGER NOT NULL, fold_id INTEGER NOT NULL,"
    " sharpe REAL, returns REAL, n_test INTEGER, PRIMARY KEY(run_id, canon_hash, fold_id));"
    "CREATE TABLE IF NOT EXISTS conviction ("
    " run_id TEXT NOT NULL, canon_hash INTEGER NOT NULL, dsr REAL, pbo REAL,"
    " stability REAL, explain_flag INTEGER, score REAL, PRIMARY KEY(run_id, canon_hash));"
    "CREATE TABLE IF NOT EXISTS alpha_event ("
    " event_id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER NOT NULL,"
    " canon_hash INTEGER NOT NULL, event_type TEXT NOT NULL, run_id TEXT,"
    " actor TEXT NOT NULL, payload TEXT);"
    "CREATE TABLE IF NOT EXISTS lifecycle_journal ("
    " seq INTEGER PRIMARY KEY AUTOINCREMENT, canon_hash INTEGER NOT NULL,"
    " from_state INTEGER NOT NULL, to_state INTEGER NOT NULL,"
    " as_of_period INTEGER NOT NULL, run_id TEXT);"
    "CREATE TABLE IF NOT EXISTS env_config (key TEXT PRIMARY KEY, value TEXT);"
    "CREATE TABLE IF NOT EXISTS promotion_ledger ("
    " promo_id INTEGER PRIMARY KEY AUTOINCREMENT, canon_hash INTEGER NOT NULL,"
    " from_env TEXT, to_env TEXT, justifying_run_id TEXT, approved_by TEXT, ts INTEGER);"
    "CREATE TABLE IF NOT EXISTS segment ("
    " segment_id TEXT PRIMARY KEY, path TEXT, content_hash INTEGER, base_alpha_id INTEGER,"
    " n_alphas INTEGER, crc INTEGER, format_version INTEGER, created_by_run_id TEXT);"
    "CREATE TABLE IF NOT EXISTS segment_alpha ("
    " canon_hash INTEGER NOT NULL, segment_id TEXT NOT NULL, dir_index INTEGER NOT NULL,"
    " PRIMARY KEY(canon_hash, segment_id));"
    "CREATE INDEX IF NOT EXISTS ix_alpha_event_hash ON alpha_event(canon_hash, ts, event_id);"
    "CREATE INDEX IF NOT EXISTS ix_run_alpha_hash ON run_alpha(canon_hash);"
    "CREATE TABLE IF NOT EXISTS schema_meta ("
    " schema_version INTEGER NOT NULL, engine_version TEXT, applied_at INTEGER);"));
  // Stamp the version once (only if empty).
  ATX_TRY(auto* stmt, db.prepare_cached("SELECT COUNT(*) FROM schema_meta"));
  ATX_TRY(const auto step, stmt->step());
  (void)step;
  if (stmt->column_int(0) == 0) {
    ATX_TRY_VOID(db.exec("INSERT INTO schema_meta(schema_version, engine_version, applied_at)"
                         " VALUES (" + std::to_string(kSchemaVersion) + ", 'v2', 0)"));
  }
  return atx::core::Ok();
}

}  // namespace atx::engine::store::schema
```
(Add `#include <string>` at the top for `std::to_string`.)

- [ ] **Step 5: Write `db.hpp`**

Create `atx-engine/include/atx/engine/store/db.hpp`:
```cpp
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
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `cmake --build build --target atx-engine-store-tests && ctest --test-dir build -R 'StoreDb' --output-on-failure`
Expected: PASS — `MemoryOpenSetsSchemaVersion`, `FileOpenIsIdempotentAcrossReopen`.

- [ ] **Step 7: Commit**

```bash
git add atx-engine/include/atx/engine/store/schema.hpp atx-engine/include/atx/engine/store/db.hpp atx-engine/tests/store/store_db_test.cpp atx-engine/tests/CMakeLists.txt
git commit -m "feat(store): v2 persistence schema + StoreDb bootstrap"
```

---

## Task 2: Alpha catalog (identity + lineage + dedup)

**Files:**
- Create: `atx-engine/include/atx/engine/store/alpha_catalog.hpp`
- Create: `atx-engine/tests/store/store_alpha_catalog_test.cpp`

**Interfaces:**
- Consumes: `StoreDb` (Task 1).
- Produces (`namespace atx::engine::store::alpha_catalog`):
  - `Status upsert(db::Database&, atx::u64 canon_hash, atx::u64 alpha_id, std::string_view expr, atx::i64 created_at, std::string_view first_run_id);`
  - `Result<bool> exists(db::Database&, atx::u64 canon_hash);`
  - `Status add_lineage(db::Database&, atx::u64 child, atx::u64 parent, atx::u64 mutation_op, atx::u64 seed);`
  - `Result<std::vector<atx::u64>> parents(db::Database&, atx::u64 child);`

- [ ] **Step 1: Write the failing test**

Create `atx-engine/tests/store/store_alpha_catalog_test.cpp`:
```cpp
// store_alpha_catalog_test.cpp — identity + lineage + dedup (Task 2).
#include <gtest/gtest.h>
#include "atx/engine/store/db.hpp"
#include "atx/engine/store/alpha_catalog.hpp"

namespace atxtest_store_alpha_catalog_test {
using atx::engine::store::StoreDb;
namespace cat = atx::engine::store::alpha_catalog;

TEST(AlphaCatalog, UpsertThenExistsAndDedup) {
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  auto& db = s->db();
  ASSERT_TRUE(cat::upsert(db, 0xABCDull, 1, "rank(close)", 100, "run1").has_value());
  auto ex = cat::exists(db, 0xABCDull); ASSERT_TRUE(ex.has_value()); EXPECT_TRUE(*ex);
  // re-upsert same canon_hash is a no-op (dedup), not an error
  EXPECT_TRUE(cat::upsert(db, 0xABCDull, 1, "rank(close)", 100, "run1").has_value());
  auto missing = cat::exists(db, 0x9999ull); ASSERT_TRUE(missing.has_value()); EXPECT_FALSE(*missing);
}

TEST(AlphaCatalog, LineageParentsRoundTrip) {
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  auto& db = s->db();
  ASSERT_TRUE(cat::add_lineage(db, /*child*/5, /*parent*/2, /*op*/7, /*seed*/42).has_value());
  ASSERT_TRUE(cat::add_lineage(db, 5, 3, 7, 42).has_value());
  auto ps = cat::parents(db, 5); ASSERT_TRUE(ps.has_value());
  ASSERT_EQ(ps->size(), 2u);
}

}  // namespace atxtest_store_alpha_catalog_test
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target atx-engine-store-tests`
Expected: FAIL — `atx/engine/store/alpha_catalog.hpp: No such file`.

- [ ] **Step 3: Write `alpha_catalog.hpp`**

Create `atx-engine/include/atx/engine/store/alpha_catalog.hpp`:
```cpp
#pragma once
// atx::engine::store::alpha_catalog — alpha identity (canon_hash PK = dedup) + lineage.

#include <string_view>
#include <vector>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::store::alpha_catalog {

// Insert the alpha identity if its canon_hash is new; a duplicate canon_hash is a
// no-op (INSERT OR IGNORE) — canon_hash PK IS the dedup index.
[[nodiscard]] inline atx::core::Status
upsert(atx::core::db::Database& db, atx::u64 canon_hash, atx::u64 alpha_id,
       std::string_view expr, atx::i64 created_at, std::string_view first_run_id) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "INSERT OR IGNORE INTO alpha(canon_hash, alpha_id, expr_source, created_at, first_run_id)"
      " VALUES (?1, ?2, ?3, ?4, ?5)"));
  ATX_TRY_VOID(stmt->bind(1, static_cast<atx::i64>(canon_hash)));
  ATX_TRY_VOID(stmt->bind(2, static_cast<atx::i64>(alpha_id)));
  ATX_TRY_VOID(stmt->bind(3, expr));
  ATX_TRY_VOID(stmt->bind(4, created_at));
  ATX_TRY_VOID(stmt->bind(5, first_run_id));
  ATX_TRY(const auto step, stmt->step());
  if (step != atx::core::db::Statement::Step::Done) {
    return atx::core::Err(atx::core::ErrorCode::Internal, "alpha_catalog::upsert: insert incomplete");
  }
  return atx::core::Ok();
}

[[nodiscard]] inline atx::core::Result<bool>
exists(atx::core::db::Database& db, atx::u64 canon_hash) {
  ATX_TRY(auto* stmt, db.prepare_cached("SELECT 1 FROM alpha WHERE canon_hash = ?1 LIMIT 1"));
  ATX_TRY_VOID(stmt->bind(1, static_cast<atx::i64>(canon_hash)));
  ATX_TRY(const auto step, stmt->step());
  return atx::core::Ok(step == atx::core::db::Statement::Step::Row);
}

[[nodiscard]] inline atx::core::Status
add_lineage(atx::core::db::Database& db, atx::u64 child, atx::u64 parent,
            atx::u64 mutation_op, atx::u64 seed) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "INSERT OR IGNORE INTO alpha_lineage(child_hash, parent_hash, mutation_op, seed)"
      " VALUES (?1, ?2, ?3, ?4)"));
  ATX_TRY_VOID(stmt->bind(1, static_cast<atx::i64>(child)));
  ATX_TRY_VOID(stmt->bind(2, static_cast<atx::i64>(parent)));
  ATX_TRY_VOID(stmt->bind(3, static_cast<atx::i64>(mutation_op)));
  ATX_TRY_VOID(stmt->bind(4, static_cast<atx::i64>(seed)));
  ATX_TRY(const auto step, stmt->step());
  if (step != atx::core::db::Statement::Step::Done) {
    return atx::core::Err(atx::core::ErrorCode::Internal, "alpha_catalog::add_lineage: incomplete");
  }
  return atx::core::Ok();
}

[[nodiscard]] inline atx::core::Result<std::vector<atx::u64>>
parents(atx::core::db::Database& db, atx::u64 child) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "SELECT parent_hash FROM alpha_lineage WHERE child_hash = ?1 ORDER BY parent_hash"));
  ATX_TRY_VOID(stmt->bind(1, static_cast<atx::i64>(child)));
  std::vector<atx::u64> out;
  for (;;) {
    ATX_TRY(const auto step, stmt->step());
    if (step == atx::core::db::Statement::Step::Done) break;
    out.push_back(static_cast<atx::u64>(stmt->column_int(0)));
  }
  return atx::core::Ok(std::move(out));
}

}  // namespace atx::engine::store::alpha_catalog
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build build --target atx-engine-store-tests && ctest --test-dir build -R 'AlphaCatalog' --output-on-failure`
Expected: PASS — `UpsertThenExistsAndDedup`, `LineageParentsRoundTrip`.

- [ ] **Step 5: Commit**

```bash
git add atx-engine/include/atx/engine/store/alpha_catalog.hpp atx-engine/tests/store/store_alpha_catalog_test.cpp
git commit -m "feat(store): alpha identity + lineage catalog with PK dedup"
```

---

## Task 3: Universe registry + data snapshots

**Files:**
- Create: `atx-engine/include/atx/engine/store/universe_registry.hpp`
- Create: `atx-engine/tests/store/store_universe_registry_test.cpp`

**Interfaces:**
- Consumes: `StoreDb` (Task 1).
- Produces (`namespace atx::engine::store::universe_registry`):
  - `Status define(db::Database&, std::string_view universe_id, std::string_view name, atx::i64 as_of, std::string_view rule, atx::u64 content_hash);`
  - `Status add_member(db::Database&, std::string_view universe_id, atx::u64 instrument_id);`
  - `Result<std::vector<atx::u64>> members(db::Database&, std::string_view universe_id);`
  - `Status record_snapshot(db::Database&, std::string_view snapshot_id, std::string_view source, atx::i64 as_of, atx::u64 content_hash);`

- [ ] **Step 1: Write the failing test**

Create `atx-engine/tests/store/store_universe_registry_test.cpp`:
```cpp
// store_universe_registry_test.cpp — universe defs + members + data snapshot (Task 3).
#include <gtest/gtest.h>
#include "atx/engine/store/db.hpp"
#include "atx/engine/store/universe_registry.hpp"

namespace atxtest_store_universe_registry_test {
using atx::engine::store::StoreDb;
namespace ur = atx::engine::store::universe_registry;

TEST(UniverseRegistry, DefineAddMembersRoundTrip) {
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  auto& db = s->db();
  ASSERT_TRUE(ur::define(db, "uni_sp500", "SP500", 20260101, "top500_by_cap", 0xFEEDull).has_value());
  ASSERT_TRUE(ur::add_member(db, "uni_sp500", 10).has_value());
  ASSERT_TRUE(ur::add_member(db, "uni_sp500", 20).has_value());
  auto m = ur::members(db, "uni_sp500"); ASSERT_TRUE(m.has_value());
  ASSERT_EQ(m->size(), 2u);
}

TEST(UniverseRegistry, RecordSnapshotOk) {
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  EXPECT_TRUE(ur::record_snapshot(s->db(), "snap_orats_0619", "ORATS", 20260619, 0x1234ull).has_value());
}

}  // namespace atxtest_store_universe_registry_test
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target atx-engine-store-tests`
Expected: FAIL — `universe_registry.hpp: No such file`.

- [ ] **Step 3: Write `universe_registry.hpp`**

Create `atx-engine/include/atx/engine/store/universe_registry.hpp`:
```cpp
#pragma once
// atx::engine::store::universe_registry — point-in-time universe definitions, their
// members, and external data snapshots. universe_id / snapshot_id are content hashes
// (TEXT) supplied by the caller; both inserts are INSERT OR REPLACE (idempotent define).

#include <string_view>
#include <vector>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::store::universe_registry {

[[nodiscard]] inline atx::core::Status
define(atx::core::db::Database& db, std::string_view universe_id, std::string_view name,
       atx::i64 as_of, std::string_view rule, atx::u64 content_hash) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "INSERT OR REPLACE INTO universe(universe_id, name, as_of, rule, content_hash)"
      " VALUES (?1, ?2, ?3, ?4, ?5)"));
  ATX_TRY_VOID(stmt->bind(1, universe_id));
  ATX_TRY_VOID(stmt->bind(2, name));
  ATX_TRY_VOID(stmt->bind(3, as_of));
  ATX_TRY_VOID(stmt->bind(4, rule));
  ATX_TRY_VOID(stmt->bind(5, static_cast<atx::i64>(content_hash)));
  ATX_TRY(const auto step, stmt->step());
  if (step != atx::core::db::Statement::Step::Done) {
    return atx::core::Err(atx::core::ErrorCode::Internal, "universe_registry::define: incomplete");
  }
  return atx::core::Ok();
}

[[nodiscard]] inline atx::core::Status
add_member(atx::core::db::Database& db, std::string_view universe_id, atx::u64 instrument_id) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "INSERT OR IGNORE INTO universe_member(universe_id, instrument_id) VALUES (?1, ?2)"));
  ATX_TRY_VOID(stmt->bind(1, universe_id));
  ATX_TRY_VOID(stmt->bind(2, static_cast<atx::i64>(instrument_id)));
  ATX_TRY(const auto step, stmt->step());
  if (step != atx::core::db::Statement::Step::Done) {
    return atx::core::Err(atx::core::ErrorCode::Internal, "universe_registry::add_member: incomplete");
  }
  return atx::core::Ok();
}

[[nodiscard]] inline atx::core::Result<std::vector<atx::u64>>
members(atx::core::db::Database& db, std::string_view universe_id) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "SELECT instrument_id FROM universe_member WHERE universe_id = ?1 ORDER BY instrument_id"));
  ATX_TRY_VOID(stmt->bind(1, universe_id));
  std::vector<atx::u64> out;
  for (;;) {
    ATX_TRY(const auto step, stmt->step());
    if (step == atx::core::db::Statement::Step::Done) break;
    out.push_back(static_cast<atx::u64>(stmt->column_int(0)));
  }
  return atx::core::Ok(std::move(out));
}

[[nodiscard]] inline atx::core::Status
record_snapshot(atx::core::db::Database& db, std::string_view snapshot_id, std::string_view source,
                atx::i64 as_of, atx::u64 content_hash) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "INSERT OR REPLACE INTO data_snapshot(snapshot_id, source, as_of, content_hash)"
      " VALUES (?1, ?2, ?3, ?4)"));
  ATX_TRY_VOID(stmt->bind(1, snapshot_id));
  ATX_TRY_VOID(stmt->bind(2, source));
  ATX_TRY_VOID(stmt->bind(3, as_of));
  ATX_TRY_VOID(stmt->bind(4, static_cast<atx::i64>(content_hash)));
  ATX_TRY(const auto step, stmt->step());
  if (step != atx::core::db::Statement::Step::Done) {
    return atx::core::Err(atx::core::ErrorCode::Internal, "universe_registry::record_snapshot: incomplete");
  }
  return atx::core::Ok();
}

}  // namespace atx::engine::store::universe_registry
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build build --target atx-engine-store-tests && ctest --test-dir build -R 'UniverseRegistry' --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add atx-engine/include/atx/engine/store/universe_registry.hpp atx-engine/tests/store/store_universe_registry_test.cpp
git commit -m "feat(store): universe registry + data snapshot tables"
```

---

## Task 4: Run fingerprint (pure hash + replay detection)

**Files:**
- Create: `atx-engine/include/atx/engine/store/fingerprint.hpp`
- Create: `atx-engine/tests/store/store_fingerprint_test.cpp`

**Interfaces:**
- Consumes: `StoreDb` (Task 1).
- Produces (`namespace atx::engine::store`):
  - `struct RunInputs { std::string engine_git_sha; std::string config_normalized; atx::u64 universe_content_hash; atx::u64 snapshot_content_hash; atx::u64 master_seed; std::string gate_config; };`
  - `atx::u64 fingerprint::compute(const RunInputs&);`
  - `Result<bool> fingerprint::is_replay(db::Database&, atx::u64 fingerprint);` // true iff a committed run already has this fingerprint

- [ ] **Step 1: Write the failing test**

Create `atx-engine/tests/store/store_fingerprint_test.cpp`:
```cpp
// store_fingerprint_test.cpp — run fingerprint determinism + replay lookup (Task 4).
#include <gtest/gtest.h>
#include "atx/engine/store/fingerprint.hpp"

namespace atxtest_store_fingerprint_test {
using atx::engine::store::RunInputs;
namespace fp = atx::engine::store::fingerprint;

RunInputs sample() {
  return RunInputs{"sha_abc", "cfg{mode:long}", 0xAAAAull, 0xBBBBull, 7, "gates{pbo<0.5}"};
}

TEST(Fingerprint, IdenticalInputsSameHash) {
  EXPECT_EQ(fp::compute(sample()), fp::compute(sample()));
}

TEST(Fingerprint, PerturbedInputDiffersOnEveryField) {
  const atx::u64 base = fp::compute(sample());
  { auto x = sample(); x.engine_git_sha = "sha_xyz";       EXPECT_NE(base, fp::compute(x)); }
  { auto x = sample(); x.config_normalized = "cfg{mode:short}"; EXPECT_NE(base, fp::compute(x)); }
  { auto x = sample(); x.universe_content_hash = 0xCCCCull;  EXPECT_NE(base, fp::compute(x)); }
  { auto x = sample(); x.snapshot_content_hash = 0xDDDDull;  EXPECT_NE(base, fp::compute(x)); }
  { auto x = sample(); x.master_seed = 8;                    EXPECT_NE(base, fp::compute(x)); }
  { auto x = sample(); x.gate_config = "gates{pbo<0.4}";     EXPECT_NE(base, fp::compute(x)); }
}

}  // namespace atxtest_store_fingerprint_test
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target atx-engine-store-tests`
Expected: FAIL — `fingerprint.hpp: No such file`.

- [ ] **Step 3: Write `fingerprint.hpp`**

Create `atx-engine/include/atx/engine/store/fingerprint.hpp`:
```cpp
#pragma once
// atx::engine::store::fingerprint — content hash over all run inputs. Equal fingerprint
// ⇒ identical inputs ⇒ a re-launch is a replay. FNV-1a 64 fold over a canonical byte
// stream (length-prefixed fields so concatenation is unambiguous). No compile-time
// seeds / wall-clock — the same inputs hash the same on every platform and run.

#include <string>
#include <string_view>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::store {

struct RunInputs {
  std::string engine_git_sha;
  std::string config_normalized;
  atx::u64 universe_content_hash{0};
  atx::u64 snapshot_content_hash{0};
  atx::u64 master_seed{0};
  std::string gate_config;
};

namespace fingerprint {

inline constexpr atx::u64 kFnvOffset = 1469598103934665603ull;
inline constexpr atx::u64 kFnvPrime  = 1099511628211ull;

[[nodiscard]] inline atx::u64 fold_bytes(atx::u64 h, std::string_view bytes) noexcept {
  for (const char c : bytes) {
    h ^= static_cast<atx::u8>(c);
    h *= kFnvPrime;
  }
  return h;
}

// Length-prefix each string field (8 raw length bytes) so "ab"+"c" != "a"+"bc".
[[nodiscard]] inline atx::u64 fold_string(atx::u64 h, std::string_view s) noexcept {
  atx::u64 n = s.size();
  for (int i = 0; i < 8; ++i) { h ^= static_cast<atx::u8>(n & 0xFF); h *= kFnvPrime; n >>= 8; }
  return fold_bytes(h, s);
}

[[nodiscard]] inline atx::u64 fold_u64(atx::u64 h, atx::u64 v) noexcept {
  for (int i = 0; i < 8; ++i) { h ^= static_cast<atx::u8>(v & 0xFF); h *= kFnvPrime; v >>= 8; }
  return h;
}

[[nodiscard]] inline atx::u64 compute(const RunInputs& in) noexcept {
  atx::u64 h = kFnvOffset;
  h = fold_string(h, in.engine_git_sha);
  h = fold_string(h, in.config_normalized);
  h = fold_u64(h, in.universe_content_hash);
  h = fold_u64(h, in.snapshot_content_hash);
  h = fold_u64(h, in.master_seed);
  h = fold_string(h, in.gate_config);
  return h;
}

// True iff a COMMITTED run already carries this fingerprint (a replay).
[[nodiscard]] inline atx::core::Result<bool>
is_replay(atx::core::db::Database& db, atx::u64 fp_value) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "SELECT 1 FROM run WHERE run_fingerprint = ?1 AND status = 'committed' LIMIT 1"));
  ATX_TRY_VOID(stmt->bind(1, static_cast<atx::i64>(fp_value)));
  ATX_TRY(const auto step, stmt->step());
  return atx::core::Ok(step == atx::core::db::Statement::Step::Row);
}

}  // namespace fingerprint
}  // namespace atx::engine::store
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build build --target atx-engine-store-tests && ctest --test-dir build -R 'Fingerprint' --output-on-failure`
Expected: PASS — `IdenticalInputsSameHash`, `PerturbedInputDiffersOnEveryField`.

- [ ] **Step 5: Commit**

```bash
git add atx-engine/include/atx/engine/store/fingerprint.hpp atx-engine/tests/store/store_fingerprint_test.cpp
git commit -m "feat(store): deterministic run fingerprint + replay lookup"
```

---

## Task 5: RunRecorder (run row + params + run_alpha + metrics, transactional)

**Files:**
- Create: `atx-engine/include/atx/engine/store/run_recorder.hpp`
- Create: `atx-engine/tests/store/store_run_recorder_test.cpp`

**Interfaces:**
- Consumes: `StoreDb` (Task 1), `fingerprint::compute`/`is_replay` (Task 4).
- Produces (`namespace atx::engine::store`):
  - `struct RunRow { std::string run_id; atx::u64 fingerprint; std::string kind; std::string engine_git_sha; atx::u64 master_seed; std::string universe_id; std::string snapshot_id; atx::i64 fit_start, fit_end, bt_start, bt_end; std::string position_mode; bool sector_neutral; atx::i64 rebalance_every; std::string cost_model; atx::i64 started_at; };`
  - `struct AlphaMetricsRow { atx::u64 canon_hash; double sharpe, returns, drawdown, turnover, margin, fitness; };`
  - `class RunRecorder` with:
    - `static Result<RunRecorder> begin(db::Database&, const RunRow&);` // Err(AlreadyExists) if fingerprint is a committed replay; inserts run status='running'
    - `Status set_param(std::string_view key, std::string_view value);`
    - `Status link_alpha(atx::u64 canon_hash, std::string_view role);`
    - `Status record_metrics(const AlphaMetricsRow&);`
    - `Status commit(atx::i64 finished_at, atx::u64 result_digest);` // flips status='committed' in one txn

- [ ] **Step 1: Write the failing test**

Create `atx-engine/tests/store/store_run_recorder_test.cpp`:
```cpp
// store_run_recorder_test.cpp — run registry write path + replay guard (Task 5).
#include <gtest/gtest.h>
#include "atx/engine/store/db.hpp"
#include "atx/engine/store/run_recorder.hpp"
#include "atx/engine/store/fingerprint.hpp"

namespace atxtest_store_run_recorder_test {
using atx::engine::store::StoreDb;
using atx::engine::store::RunRow;
using atx::engine::store::RunRecorder;
using atx::engine::store::AlphaMetricsRow;
namespace fp = atx::engine::store::fingerprint;

RunRow make_run(const std::string& id, atx::u64 finger) {
  RunRow r;
  r.run_id = id; r.fingerprint = finger; r.kind = "backtest";
  r.engine_git_sha = "sha"; r.master_seed = 1; r.universe_id = "u"; r.snapshot_id = "s";
  r.fit_start = 0; r.fit_end = 100; r.bt_start = 100; r.bt_end = 200;
  r.position_mode = "book"; r.sector_neutral = true; r.rebalance_every = 5;
  r.cost_model = "flat"; r.started_at = 10;
  return r;
}

TEST(RunRecorder, FullWritePathCommits) {
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  auto& db = s->db();
  auto rec = RunRecorder::begin(db, make_run("run1", 0x111ull));
  ASSERT_TRUE(rec.has_value());
  ASSERT_TRUE(rec->set_param("n_folds", "8").has_value());
  ASSERT_TRUE(rec->link_alpha(0xABCull, "admitted").has_value());
  ASSERT_TRUE(rec->record_metrics(AlphaMetricsRow{0xABCull, 1.5, 0.2, 0.1, 0.3, 0.5, 2.0}).has_value());
  ASSERT_TRUE(rec->commit(/*finished_at*/99, /*result_digest*/0xDEADull).has_value());

  auto replay = fp::is_replay(db, 0x111ull);  // now committed
  ASSERT_TRUE(replay.has_value()); EXPECT_TRUE(*replay);
}

TEST(RunRecorder, ReplayOfCommittedFingerprintRejected) {
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  auto& db = s->db();
  auto r1 = RunRecorder::begin(db, make_run("run1", 0x222ull));
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r1->commit(99, 0).has_value());
  // same fingerprint, different run_id -> replay -> rejected
  auto r2 = RunRecorder::begin(db, make_run("run2", 0x222ull));
  EXPECT_FALSE(r2.has_value());
}

}  // namespace atxtest_store_run_recorder_test
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target atx-engine-store-tests`
Expected: FAIL — `run_recorder.hpp: No such file`.

- [ ] **Step 3: Write `run_recorder.hpp`**

Create `atx-engine/include/atx/engine/store/run_recorder.hpp`:
```cpp
#pragma once
// atx::engine::store::RunRecorder — write path for one run. begin() rejects a replay
// (committed fingerprint already present) and inserts the run with status='running';
// commit() flips it to 'committed'. A crash before commit leaves status='running'
// (GC-able). One writer; the caller owns the Database (one per thread).

#include <string>
#include <string_view>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/store/fingerprint.hpp"

namespace atx::engine::store {

struct RunRow {
  std::string run_id;
  atx::u64 fingerprint{0};
  std::string kind;
  std::string engine_git_sha;
  atx::u64 master_seed{0};
  std::string universe_id;
  std::string snapshot_id;
  atx::i64 fit_start{0}, fit_end{0}, bt_start{0}, bt_end{0};
  std::string position_mode;
  bool sector_neutral{false};
  atx::i64 rebalance_every{0};
  std::string cost_model;
  atx::i64 started_at{0};
};

struct AlphaMetricsRow {
  atx::u64 canon_hash{0};
  double sharpe{0}, returns{0}, drawdown{0}, turnover{0}, margin{0}, fitness{0};
};

class RunRecorder {
public:
  [[nodiscard]] static atx::core::Result<RunRecorder>
  begin(atx::core::db::Database& db, const RunRow& r) {
    ATX_TRY(const bool replay, fingerprint::is_replay(db, r.fingerprint));
    if (replay) {
      return atx::core::Err(atx::core::ErrorCode::AlreadyExists,
                            "RunRecorder::begin: fingerprint already committed (replay)");
    }
    ATX_TRY(auto* stmt, db.prepare_cached(
        "INSERT INTO run(run_id, run_fingerprint, kind, status, engine_git_sha, master_seed,"
        " universe_id, snapshot_id, fit_start, fit_end, bt_start, bt_end, position_mode,"
        " sector_neutral, rebalance_every, cost_model, started_at)"
        " VALUES (?1,?2,?3,'running',?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16)"));
    ATX_TRY_VOID(stmt->bind(1, r.run_id));
    ATX_TRY_VOID(stmt->bind(2, static_cast<atx::i64>(r.fingerprint)));
    ATX_TRY_VOID(stmt->bind(3, r.kind));
    ATX_TRY_VOID(stmt->bind(4, r.engine_git_sha));
    ATX_TRY_VOID(stmt->bind(5, static_cast<atx::i64>(r.master_seed)));
    ATX_TRY_VOID(stmt->bind(6, r.universe_id));
    ATX_TRY_VOID(stmt->bind(7, r.snapshot_id));
    ATX_TRY_VOID(stmt->bind(8, r.fit_start));
    ATX_TRY_VOID(stmt->bind(9, r.fit_end));
    ATX_TRY_VOID(stmt->bind(10, r.bt_start));
    ATX_TRY_VOID(stmt->bind(11, r.bt_end));
    ATX_TRY_VOID(stmt->bind(12, r.position_mode));
    ATX_TRY_VOID(stmt->bind(13, static_cast<atx::i64>(r.sector_neutral ? 1 : 0)));
    ATX_TRY_VOID(stmt->bind(14, r.rebalance_every));
    ATX_TRY_VOID(stmt->bind(15, r.cost_model));
    ATX_TRY_VOID(stmt->bind(16, r.started_at));
    ATX_TRY(const auto step, stmt->step());
    if (step != atx::core::db::Statement::Step::Done) {
      return atx::core::Err(atx::core::ErrorCode::Internal, "RunRecorder::begin: insert incomplete");
    }
    return atx::core::Ok(RunRecorder{db, r.run_id});
  }

  [[nodiscard]] atx::core::Status set_param(std::string_view key, std::string_view value) {
    ATX_TRY(auto* stmt, db_.prepare_cached(
        "INSERT OR REPLACE INTO run_param(run_id, key, value) VALUES (?1, ?2, ?3)"));
    ATX_TRY_VOID(stmt->bind(1, run_id_));
    ATX_TRY_VOID(stmt->bind(2, key));
    ATX_TRY_VOID(stmt->bind(3, value));
    return step_done(*stmt, "set_param");
  }

  [[nodiscard]] atx::core::Status link_alpha(atx::u64 canon_hash, std::string_view role) {
    ATX_TRY(auto* stmt, db_.prepare_cached(
        "INSERT OR REPLACE INTO run_alpha(run_id, canon_hash, role) VALUES (?1, ?2, ?3)"));
    ATX_TRY_VOID(stmt->bind(1, run_id_));
    ATX_TRY_VOID(stmt->bind(2, static_cast<atx::i64>(canon_hash)));
    ATX_TRY_VOID(stmt->bind(3, role));
    return step_done(*stmt, "link_alpha");
  }

  [[nodiscard]] atx::core::Status record_metrics(const AlphaMetricsRow& m) {
    ATX_TRY(auto* stmt, db_.prepare_cached(
        "INSERT OR REPLACE INTO alpha_metrics(run_id, canon_hash, sharpe, returns, drawdown,"
        " turnover, margin, fitness) VALUES (?1,?2,?3,?4,?5,?6,?7,?8)"));
    ATX_TRY_VOID(stmt->bind(1, run_id_));
    ATX_TRY_VOID(stmt->bind(2, static_cast<atx::i64>(m.canon_hash)));
    ATX_TRY_VOID(stmt->bind(3, m.sharpe));
    ATX_TRY_VOID(stmt->bind(4, m.returns));
    ATX_TRY_VOID(stmt->bind(5, m.drawdown));
    ATX_TRY_VOID(stmt->bind(6, m.turnover));
    ATX_TRY_VOID(stmt->bind(7, m.margin));
    ATX_TRY_VOID(stmt->bind(8, m.fitness));
    return step_done(*stmt, "record_metrics");
  }

  [[nodiscard]] atx::core::Status commit(atx::i64 finished_at, atx::u64 result_digest) {
    ATX_TRY(auto* stmt, db_.prepare_cached(
        "UPDATE run SET status='committed', finished_at=?2, result_digest=?3 WHERE run_id=?1"));
    ATX_TRY_VOID(stmt->bind(1, run_id_));
    ATX_TRY_VOID(stmt->bind(2, finished_at));
    ATX_TRY_VOID(stmt->bind(3, static_cast<atx::i64>(result_digest)));
    return step_done(*stmt, "commit");
  }

private:
  RunRecorder(atx::core::db::Database& db, std::string run_id)
      : db_{db}, run_id_{std::move(run_id)} {}

  [[nodiscard]] static atx::core::Status step_done(atx::core::db::Statement& stmt, const char* who) {
    ATX_TRY(const auto step, stmt.step());
    if (step != atx::core::db::Statement::Step::Done) {
      return atx::core::Err(atx::core::ErrorCode::Internal, who);
    }
    return atx::core::Ok();
  }

  atx::core::db::Database& db_;
  std::string run_id_;
};

}  // namespace atx::engine::store
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build build --target atx-engine-store-tests && ctest --test-dir build -R 'RunRecorder' --output-on-failure`
Expected: PASS — `FullWritePathCommits`, `ReplayOfCommittedFingerprintRejected`.

- [ ] **Step 5: Commit**

```bash
git add atx-engine/include/atx/engine/store/run_recorder.hpp atx-engine/tests/store/store_run_recorder_test.cpp
git commit -m "feat(store): RunRecorder write path with replay guard"
```

---

## Task 6: Event log (alpha_event + lifecycle dual-write)

**Files:**
- Create: `atx-engine/include/atx/engine/store/event_log.hpp`
- Create: `atx-engine/tests/store/store_event_log_test.cpp`

**Interfaces:**
- Consumes: `StoreDb` (Task 1). Reuses `LifecycleState` enum values 0..5 from `library/lifecycle.hpp` semantics (re-declared locally to avoid a cross-module dependency).
- Produces (`namespace atx::engine::store`):
  - `enum class LifecycleState : atx::u8 { Candidate=0, Admitted=1, Live=2, Decaying=3, Dead=4, Recycled=5 };`
  - `struct EventRow { atx::i64 ts; atx::u64 canon_hash; std::string event_type; std::string run_id; std::string actor; std::string payload; };`
  - `namespace event_log`:
    - `Status append(db::Database&, const EventRow&);`
    - `Status transition(db::Database&, atx::u64 canon_hash, LifecycleState from, LifecycleState to, atx::u64 as_of_period, std::string_view run_id, atx::i64 ts);` // dual-writes lifecycle_journal + alpha_event in one txn
    - `Result<LifecycleState> state_as_of(db::Database&, atx::u64 canon_hash, atx::u64 t);`
    - `Result<std::vector<EventRow>> history(db::Database&, atx::u64 canon_hash);`

- [ ] **Step 1: Write the failing test**

Create `atx-engine/tests/store/store_event_log_test.cpp`:
```cpp
// store_event_log_test.cpp — append-only event log + lifecycle projection (Task 6).
#include <gtest/gtest.h>
#include "atx/engine/store/db.hpp"
#include "atx/engine/store/event_log.hpp"

namespace atxtest_store_event_log_test {
using atx::engine::store::StoreDb;
using atx::engine::store::LifecycleState;
using atx::engine::store::EventRow;
namespace ev = atx::engine::store::event_log;

TEST(EventLog, TransitionDualWritesAndStateAsOf) {
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  auto& db = s->db();
  ASSERT_TRUE(ev::transition(db, 0xA1ull, LifecycleState::Candidate, LifecycleState::Admitted,
                             /*as_of*/100, "run1", /*ts*/10).has_value());
  auto st = ev::state_as_of(db, 0xA1ull, 150); ASSERT_TRUE(st.has_value());
  EXPECT_EQ(*st, LifecycleState::Admitted);
  // PIT: before its birth it is Candidate
  auto before = ev::state_as_of(db, 0xA1ull, 50); ASSERT_TRUE(before.has_value());
  EXPECT_EQ(*before, LifecycleState::Candidate);
  // the transition also wrote an alpha_event row
  auto h = ev::history(db, 0xA1ull); ASSERT_TRUE(h.has_value());
  ASSERT_EQ(h->size(), 1u);
  EXPECT_EQ((*h)[0].event_type, "lifecycle");
}

TEST(EventLog, HistoryIsOrderedAppendOnly) {
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  auto& db = s->db();
  ASSERT_TRUE(ev::append(db, EventRow{1, 0xB2ull, "created",  "run1", "system", "{}"}).has_value());
  ASSERT_TRUE(ev::append(db, EventRow{2, 0xB2ull, "evaluated","run1", "run",    "{\"sharpe\":1.2}"}).has_value());
  auto h = ev::history(db, 0xB2ull); ASSERT_TRUE(h.has_value());
  ASSERT_EQ(h->size(), 2u);
  EXPECT_EQ((*h)[0].event_type, "created");
  EXPECT_EQ((*h)[1].event_type, "evaluated");
}

}  // namespace atxtest_store_event_log_test
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target atx-engine-store-tests`
Expected: FAIL — `event_log.hpp: No such file`.

- [ ] **Step 3: Write `event_log.hpp`**

Create `atx-engine/include/atx/engine/store/event_log.hpp`:
```cpp
#pragma once
// atx::engine::store::event_log — the unified append-only timeline of every change to
// any alpha (alpha_event), plus a typed lifecycle projection (lifecycle_journal) for
// fast PIT state queries. transition() writes BOTH in one transaction so they never
// drift. INSERT-only — no UPDATE/DELETE. Ordering uses the AUTOINCREMENT PK as the
// deterministic tie-break, mirroring library/lifecycle.hpp.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::store {

// Values 0..5 are the on-disk contract — do NOT reorder (matches library::LifecycleState).
enum class LifecycleState : atx::u8 {
  Candidate = 0, Admitted = 1, Live = 2, Decaying = 3, Dead = 4, Recycled = 5,
};

struct EventRow {
  atx::i64 ts{0};
  atx::u64 canon_hash{0};
  std::string event_type;
  std::string run_id;
  std::string actor;
  std::string payload;
};

namespace event_log {

[[nodiscard]] inline atx::core::Status append(atx::core::db::Database& db, const EventRow& e) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "INSERT INTO alpha_event(ts, canon_hash, event_type, run_id, actor, payload)"
      " VALUES (?1,?2,?3,?4,?5,?6)"));
  ATX_TRY_VOID(stmt->bind(1, e.ts));
  ATX_TRY_VOID(stmt->bind(2, static_cast<atx::i64>(e.canon_hash)));
  ATX_TRY_VOID(stmt->bind(3, e.event_type));
  ATX_TRY_VOID(stmt->bind(4, e.run_id));
  ATX_TRY_VOID(stmt->bind(5, e.actor));
  ATX_TRY_VOID(stmt->bind(6, e.payload));
  ATX_TRY(const auto step, stmt->step());
  if (step != atx::core::db::Statement::Step::Done) {
    return atx::core::Err(atx::core::ErrorCode::Internal, "event_log::append: incomplete");
  }
  return atx::core::Ok();
}

// Dual-write a lifecycle move: a typed row into lifecycle_journal AND a unified
// alpha_event(event_type='lifecycle') row, atomically.
[[nodiscard]] inline atx::core::Status
transition(atx::core::db::Database& db, atx::u64 canon_hash, LifecycleState from,
           LifecycleState to, atx::u64 as_of_period, std::string_view run_id, atx::i64 ts) {
  ATX_TRY(auto txn, atx::core::db::Transaction::begin(db));
  {
    ATX_TRY(auto* j, db.prepare_cached(
        "INSERT INTO lifecycle_journal(canon_hash, from_state, to_state, as_of_period, run_id)"
        " VALUES (?1,?2,?3,?4,?5)"));
    ATX_TRY_VOID(j->bind(1, static_cast<atx::i64>(canon_hash)));
    ATX_TRY_VOID(j->bind(2, static_cast<atx::i64>(static_cast<atx::u8>(from))));
    ATX_TRY_VOID(j->bind(3, static_cast<atx::i64>(static_cast<atx::u8>(to))));
    ATX_TRY_VOID(j->bind(4, static_cast<atx::i64>(as_of_period)));
    ATX_TRY_VOID(j->bind(5, run_id));
    ATX_TRY(const auto step, j->step());
    if (step != atx::core::db::Statement::Step::Done) {
      return atx::core::Err(atx::core::ErrorCode::Internal, "event_log::transition: journal incomplete");
    }
  }
  ATX_TRY_VOID(append(db, EventRow{ts, canon_hash, "lifecycle", std::string(run_id), "system",
                                   "{\"to\":" + std::to_string(static_cast<int>(to)) + "}"}));
  ATX_TRY_VOID(txn.commit());
  return atx::core::Ok();
}

// PIT state: to_state of the latest journal row with as_of_period <= t (ties broken by
// seq DESC). No row at-or-before t ⇒ Candidate (the implicit pre-journal state).
[[nodiscard]] inline atx::core::Result<LifecycleState>
state_as_of(atx::core::db::Database& db, atx::u64 canon_hash, atx::u64 t) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "SELECT to_state FROM lifecycle_journal WHERE canon_hash = ?1 AND as_of_period <= ?2"
      " ORDER BY as_of_period DESC, seq DESC LIMIT 1"));
  ATX_TRY_VOID(stmt->bind(1, static_cast<atx::i64>(canon_hash)));
  ATX_TRY_VOID(stmt->bind(2, static_cast<atx::i64>(t)));
  ATX_TRY(const auto step, stmt->step());
  if (step == atx::core::db::Statement::Step::Done) {
    return atx::core::Ok(LifecycleState::Candidate);
  }
  return atx::core::Ok(static_cast<LifecycleState>(static_cast<atx::u8>(stmt->column_int(0))));
}

// Full ordered timeline for one alpha (created → evaluated → lifecycle → promoted …).
[[nodiscard]] inline atx::core::Result<std::vector<EventRow>>
history(atx::core::db::Database& db, atx::u64 canon_hash) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "SELECT ts, event_type, run_id, actor, payload FROM alpha_event"
      " WHERE canon_hash = ?1 ORDER BY ts, event_id"));
  ATX_TRY_VOID(stmt->bind(1, static_cast<atx::i64>(canon_hash)));
  std::vector<EventRow> out;
  for (;;) {
    ATX_TRY(const auto step, stmt->step());
    if (step == atx::core::db::Statement::Step::Done) break;
    EventRow e;
    e.ts = stmt->column_int(0);
    e.canon_hash = canon_hash;
    e.event_type = std::string(stmt->column_text(1));
    e.run_id = std::string(stmt->column_text(2));
    e.actor = std::string(stmt->column_text(3));
    e.payload = std::string(stmt->column_text(4));
    out.push_back(std::move(e));
  }
  return atx::core::Ok(std::move(out));
}

}  // namespace event_log
}  // namespace atx::engine::store
```
(Add `#include <string>` is already present; `std::to_string` needs `<string>` — present.)

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build build --target atx-engine-store-tests && ctest --test-dir build -R 'EventLog' --output-on-failure`
Expected: PASS — `TransitionDualWritesAndStateAsOf`, `HistoryIsOrderedAppendOnly`.

- [ ] **Step 5: Commit**

```bash
git add atx-engine/include/atx/engine/store/event_log.hpp atx-engine/tests/store/store_event_log_test.cpp
git commit -m "feat(store): append-only event log + lifecycle dual-write projection"
```

---

## Task 7: Segment index (locate PnL/weight arrays)

**Files:**
- Create: `atx-engine/include/atx/engine/store/segment_index.hpp`
- Create: `atx-engine/tests/store/store_segment_index_test.cpp`

**Interfaces:**
- Consumes: `StoreDb` (Task 1).
- Produces (`namespace atx::engine::store`):
  - `struct SegmentRow { std::string segment_id; std::string path; atx::u64 content_hash; atx::u64 base_alpha_id; atx::i64 n_alphas; atx::u64 crc; atx::i64 format_version; std::string created_by_run_id; };`
  - `struct SegmentLoc { std::string segment_id; atx::i64 dir_index; };`
  - `namespace segment_index`:
    - `Status register_segment(db::Database&, const SegmentRow&);`
    - `Status map_alpha(db::Database&, atx::u64 canon_hash, std::string_view segment_id, atx::i64 dir_index);`
    - `Result<SegmentLoc> locate(db::Database&, atx::u64 canon_hash);` // Err(NotFound) if unmapped

- [ ] **Step 1: Write the failing test**

Create `atx-engine/tests/store/store_segment_index_test.cpp`:
```cpp
// store_segment_index_test.cpp — segment registry + alpha->segment location (Task 7).
#include <gtest/gtest.h>
#include "atx/engine/store/db.hpp"
#include "atx/engine/store/segment_index.hpp"

namespace atxtest_store_segment_index_test {
using atx::engine::store::StoreDb;
using atx::engine::store::SegmentRow;
namespace si = atx::engine::store::segment_index;

TEST(SegmentIndex, RegisterMapLocate) {
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  auto& db = s->db();
  ASSERT_TRUE(si::register_segment(db, SegmentRow{"seg1", "/data/seg1.atxseg", 0xC0DEull,
                                                  /*base*/0, /*n*/3, /*crc*/0x55ull, /*fmt*/1, "run1"}).has_value());
  ASSERT_TRUE(si::map_alpha(db, 0xABCull, "seg1", /*dir_index*/2).has_value());
  auto loc = si::locate(db, 0xABCull); ASSERT_TRUE(loc.has_value());
  EXPECT_EQ(loc->segment_id, "seg1");
  EXPECT_EQ(loc->dir_index, 2);
  auto missing = si::locate(db, 0x999ull);
  EXPECT_FALSE(missing.has_value());  // unmapped -> Err(NotFound)
}

}  // namespace atxtest_store_segment_index_test
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target atx-engine-store-tests`
Expected: FAIL — `segment_index.hpp: No such file`.

- [ ] **Step 3: Write `segment_index.hpp`**

Create `atx-engine/include/atx/engine/store/segment_index.hpp`:
```cpp
#pragma once
// atx::engine::store::segment_index — the DB index over the binary mmap segments that
// hold heavy PnL/position arrays. The DB never stores the arrays; it maps a canon_hash
// to (segment_id, dir_index) so a reader can mmap the file and seek the alpha's slot.

#include <string>
#include <string_view>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::store {

struct SegmentRow {
  std::string segment_id;
  std::string path;
  atx::u64 content_hash{0};
  atx::u64 base_alpha_id{0};
  atx::i64 n_alphas{0};
  atx::u64 crc{0};
  atx::i64 format_version{0};
  std::string created_by_run_id;
};

struct SegmentLoc {
  std::string segment_id;
  atx::i64 dir_index{0};
};

namespace segment_index {

[[nodiscard]] inline atx::core::Status
register_segment(atx::core::db::Database& db, const SegmentRow& s) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "INSERT OR REPLACE INTO segment(segment_id, path, content_hash, base_alpha_id, n_alphas,"
      " crc, format_version, created_by_run_id) VALUES (?1,?2,?3,?4,?5,?6,?7,?8)"));
  ATX_TRY_VOID(stmt->bind(1, s.segment_id));
  ATX_TRY_VOID(stmt->bind(2, s.path));
  ATX_TRY_VOID(stmt->bind(3, static_cast<atx::i64>(s.content_hash)));
  ATX_TRY_VOID(stmt->bind(4, static_cast<atx::i64>(s.base_alpha_id)));
  ATX_TRY_VOID(stmt->bind(5, s.n_alphas));
  ATX_TRY_VOID(stmt->bind(6, static_cast<atx::i64>(s.crc)));
  ATX_TRY_VOID(stmt->bind(7, s.format_version));
  ATX_TRY_VOID(stmt->bind(8, s.created_by_run_id));
  ATX_TRY(const auto step, stmt->step());
  if (step != atx::core::db::Statement::Step::Done) {
    return atx::core::Err(atx::core::ErrorCode::Internal, "segment_index::register_segment: incomplete");
  }
  return atx::core::Ok();
}

[[nodiscard]] inline atx::core::Status
map_alpha(atx::core::db::Database& db, atx::u64 canon_hash, std::string_view segment_id,
          atx::i64 dir_index) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "INSERT OR REPLACE INTO segment_alpha(canon_hash, segment_id, dir_index) VALUES (?1,?2,?3)"));
  ATX_TRY_VOID(stmt->bind(1, static_cast<atx::i64>(canon_hash)));
  ATX_TRY_VOID(stmt->bind(2, segment_id));
  ATX_TRY_VOID(stmt->bind(3, dir_index));
  ATX_TRY(const auto step, stmt->step());
  if (step != atx::core::db::Statement::Step::Done) {
    return atx::core::Err(atx::core::ErrorCode::Internal, "segment_index::map_alpha: incomplete");
  }
  return atx::core::Ok();
}

[[nodiscard]] inline atx::core::Result<SegmentLoc>
locate(atx::core::db::Database& db, atx::u64 canon_hash) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "SELECT segment_id, dir_index FROM segment_alpha WHERE canon_hash = ?1 LIMIT 1"));
  ATX_TRY_VOID(stmt->bind(1, static_cast<atx::i64>(canon_hash)));
  ATX_TRY(const auto step, stmt->step());
  if (step == atx::core::db::Statement::Step::Done) {
    return atx::core::Err(atx::core::ErrorCode::NotFound, "segment_index::locate: alpha not mapped");
  }
  SegmentLoc loc;
  loc.segment_id = std::string(stmt->column_text(0));
  loc.dir_index = stmt->column_int(1);
  return atx::core::Ok(std::move(loc));
}

}  // namespace segment_index
}  // namespace atx::engine::store
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build build --target atx-engine-store-tests && ctest --test-dir build -R 'SegmentIndex' --output-on-failure`
Expected: PASS — `RegisterMapLocate`.

- [ ] **Step 5: Commit**

```bash
git add atx-engine/include/atx/engine/store/segment_index.hpp atx-engine/tests/store/store_segment_index_test.cpp
git commit -m "feat(store): segment index mapping alphas to binary array slots"
```

---

## Task 8: Env config + cross-env promotion (ATTACH)

**Files:**
- Create: `atx-engine/include/atx/engine/store/env_config.hpp`
- Create: `atx-engine/include/atx/engine/store/promotion.hpp`
- Create: `atx-engine/tests/store/store_promotion_test.cpp`

**Interfaces:**
- Consumes: `StoreDb` (Task 1), `alpha_catalog` (Task 2), `event_log` (Task 6).
- Produces (`namespace atx::engine::store`):
  - `namespace env_config`:
    - `Status set(db::Database&, std::string_view key, std::string_view value);`
    - `Result<std::string> get(db::Database&, std::string_view key);` // Err(NotFound) if unset
  - `namespace promotion`:
    - `struct PromotionRequest { atx::u64 canon_hash; std::string from_env; std::string to_env; std::string justifying_run_id; std::string approved_by; atx::i64 ts; std::string dest_path; };`
    - `Status promote(db::Database& src, const PromotionRequest&);` // ATTACH dest_path, copy alpha+lineage, write ledger+event in dest

- [ ] **Step 1: Write the failing test**

Create `atx-engine/tests/store/store_promotion_test.cpp`:
```cpp
// store_promotion_test.cpp — env_config kv + cross-env promotion via ATTACH (Task 8).
#include <filesystem>
#include <string>
#include <gtest/gtest.h>
#include "atx/engine/store/db.hpp"
#include "atx/engine/store/alpha_catalog.hpp"
#include "atx/engine/store/env_config.hpp"
#include "atx/engine/store/promotion.hpp"

namespace atxtest_store_promotion_test {
using atx::engine::store::StoreDb;
namespace cat = atx::engine::store::alpha_catalog;
namespace ec = atx::engine::store::env_config;
namespace pr = atx::engine::store::promotion;

[[nodiscard]] std::string tmpdir() {
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "atx_store_promo" /
      (std::string(info->test_suite_name()) + "_" + info->name());
  std::error_code e; std::filesystem::remove_all(dir, e); std::filesystem::create_directories(dir, e);
  return dir.string();
}

TEST(EnvConfig, SetGetRoundTrip) {
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  ASSERT_TRUE(ec::set(s->db(), "cost_model", "flat_5bps").has_value());
  auto v = ec::get(s->db(), "cost_model"); ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, "flat_5bps");
  EXPECT_FALSE(ec::get(s->db(), "missing").has_value());
}

TEST(Promotion, PromotesAlphaIdentityIntoDestEnv) {
  const std::string dir = tmpdir();
  const std::string dev = dir + "/atx_dev.sqlite";
  const std::string uat = dir + "/atx_uat.sqlite";
  // seed dev with an alpha + lineage
  { auto d = StoreDb::open(dev); ASSERT_TRUE(d.has_value());
    ASSERT_TRUE(cat::upsert(d->db(), 0xABCull, 1, "rank(close)", 10, "run1").has_value());
    ASSERT_TRUE(cat::add_lineage(d->db(), 0xABCull, 0x111ull, 7, 42).has_value()); }
  // create dest env so its schema exists
  { auto u = StoreDb::open(uat); ASSERT_TRUE(u.has_value()); }
  // promote dev -> uat
  auto d = StoreDb::open(dev); ASSERT_TRUE(d.has_value());
  pr::PromotionRequest req{0xABCull, "dev", "uat", "run1", "nathan", /*ts*/20, uat};
  ASSERT_TRUE(pr::promote(d->db(), req).has_value());
  // verify the alpha now exists in uat
  auto u = StoreDb::open(uat); ASSERT_TRUE(u.has_value());
  auto ex = cat::exists(u->db(), 0xABCull); ASSERT_TRUE(ex.has_value()); EXPECT_TRUE(*ex);
}

}  // namespace atxtest_store_promotion_test
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target atx-engine-store-tests`
Expected: FAIL — `env_config.hpp: No such file`.

- [ ] **Step 3: Write `env_config.hpp`**

Create `atx-engine/include/atx/engine/store/env_config.hpp`:
```cpp
#pragma once
// atx::engine::store::env_config — key/value config describing THIS environment
// (cost knobs, capacity curve, execution model, rebalance schedule). One row per key.

#include <string>
#include <string_view>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"

namespace atx::engine::store::env_config {

[[nodiscard]] inline atx::core::Status
set(atx::core::db::Database& db, std::string_view key, std::string_view value) {
  ATX_TRY(auto* stmt, db.prepare_cached(
      "INSERT OR REPLACE INTO env_config(key, value) VALUES (?1, ?2)"));
  ATX_TRY_VOID(stmt->bind(1, key));
  ATX_TRY_VOID(stmt->bind(2, value));
  ATX_TRY(const auto step, stmt->step());
  if (step != atx::core::db::Statement::Step::Done) {
    return atx::core::Err(atx::core::ErrorCode::Internal, "env_config::set: incomplete");
  }
  return atx::core::Ok();
}

[[nodiscard]] inline atx::core::Result<std::string>
get(atx::core::db::Database& db, std::string_view key) {
  ATX_TRY(auto* stmt, db.prepare_cached("SELECT value FROM env_config WHERE key = ?1 LIMIT 1"));
  ATX_TRY_VOID(stmt->bind(1, key));
  ATX_TRY(const auto step, stmt->step());
  if (step == atx::core::db::Statement::Step::Done) {
    return atx::core::Err(atx::core::ErrorCode::NotFound, "env_config::get: key unset");
  }
  return atx::core::Ok(std::string(stmt->column_text(0)));
}

}  // namespace atx::engine::store::env_config
```

- [ ] **Step 4: Write `promotion.hpp`**

Create `atx-engine/include/atx/engine/store/promotion.hpp`:
```cpp
#pragma once
// atx::engine::store::promotion — move an alpha's identity (single canon_hash) from one
// environment DB file to the next (Dev→UAT→PROD) by ATTACH-ing the destination file,
// copying alpha + alpha_lineage rows, and recording a promotion_ledger entry plus an
// alpha_event in the destination. canon_hash keeps the identity stable across files.

#include <string>
#include <string_view>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::store::promotion {

struct PromotionRequest {
  atx::u64 canon_hash{0};
  std::string from_env;
  std::string to_env;
  std::string justifying_run_id;
  std::string approved_by;
  atx::i64 ts{0};
  std::string dest_path; // file path of the destination env DB
};

// ATTACH the destination, copy identity + lineage for canon_hash into it, write the
// ledger + event, then DETACH. All within one transaction on the source connection.
[[nodiscard]] inline atx::core::Status
promote(atx::core::db::Database& src, const PromotionRequest& req) {
  // ATTACH cannot run inside a transaction; do it first, DETACH at the end.
  {
    ATX_TRY(auto* att, src.prepare_cached("ATTACH DATABASE ?1 AS dest"));
    ATX_TRY_VOID(att->bind(1, req.dest_path));
    ATX_TRY(const auto step, att->step());
    if (step != atx::core::db::Statement::Step::Done) {
      return atx::core::Err(atx::core::ErrorCode::Internal, "promotion::promote: ATTACH failed");
    }
  }
  const auto h = static_cast<atx::i64>(req.canon_hash);
  auto run_copy = [&]() -> atx::core::Status {
    ATX_TRY(auto txn, atx::core::db::Transaction::begin(src));
    {
      ATX_TRY(auto* c1, src.prepare_cached(
          "INSERT OR IGNORE INTO dest.alpha SELECT * FROM main.alpha WHERE canon_hash = ?1"));
      ATX_TRY_VOID(c1->bind(1, h));
      ATX_TRY(const auto s1, c1->step());
      if (s1 != atx::core::db::Statement::Step::Done)
        return atx::core::Err(atx::core::ErrorCode::Internal, "promote: copy alpha failed");
    }
    {
      ATX_TRY(auto* c2, src.prepare_cached(
          "INSERT OR IGNORE INTO dest.alpha_lineage SELECT * FROM main.alpha_lineage"
          " WHERE child_hash = ?1"));
      ATX_TRY_VOID(c2->bind(1, h));
      ATX_TRY(const auto s2, c2->step());
      if (s2 != atx::core::db::Statement::Step::Done)
        return atx::core::Err(atx::core::ErrorCode::Internal, "promote: copy lineage failed");
    }
    {
      ATX_TRY(auto* c3, src.prepare_cached(
          "INSERT INTO dest.promotion_ledger(canon_hash, from_env, to_env, justifying_run_id,"
          " approved_by, ts) VALUES (?1,?2,?3,?4,?5,?6)"));
      ATX_TRY_VOID(c3->bind(1, h));
      ATX_TRY_VOID(c3->bind(2, req.from_env));
      ATX_TRY_VOID(c3->bind(3, req.to_env));
      ATX_TRY_VOID(c3->bind(4, req.justifying_run_id));
      ATX_TRY_VOID(c3->bind(5, req.approved_by));
      ATX_TRY_VOID(c3->bind(6, req.ts));
      ATX_TRY(const auto s3, c3->step());
      if (s3 != atx::core::db::Statement::Step::Done)
        return atx::core::Err(atx::core::ErrorCode::Internal, "promote: ledger insert failed");
    }
    {
      ATX_TRY(auto* c4, src.prepare_cached(
          "INSERT INTO dest.alpha_event(ts, canon_hash, event_type, run_id, actor, payload)"
          " VALUES (?1,?2,'promoted',?3,'user',?4)"));
      ATX_TRY_VOID(c4->bind(1, req.ts));
      ATX_TRY_VOID(c4->bind(2, h));
      ATX_TRY_VOID(c4->bind(3, req.justifying_run_id));
      ATX_TRY_VOID(c4->bind(4, std::string("{\"to_env\":\"") + req.to_env + "\"}"));
      ATX_TRY(const auto s4, c4->step());
      if (s4 != atx::core::db::Statement::Step::Done)
        return atx::core::Err(atx::core::ErrorCode::Internal, "promote: event insert failed");
    }
    ATX_TRY_VOID(txn.commit());
    return atx::core::Ok();
  };
  const auto copy_status = run_copy();
  // Always DETACH, even on failure (best-effort; ignore detach error if copy already failed).
  ATX_TRY_VOID(src.exec("DETACH DATABASE dest"));
  return copy_status;
}

}  // namespace atx::engine::store::promotion
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build --target atx-engine-store-tests && ctest --test-dir build -R 'EnvConfig|Promotion' --output-on-failure`
Expected: PASS — `SetGetRoundTrip`, `PromotesAlphaIdentityIntoDestEnv`.

- [ ] **Step 6: Commit**

```bash
git add atx-engine/include/atx/engine/store/env_config.hpp atx-engine/include/atx/engine/store/promotion.hpp atx-engine/tests/store/store_promotion_test.cpp
git commit -m "feat(store): env config kv + cross-env promotion via ATTACH"
```

---

## Task 9: Integration test + golden schema snapshot

**Files:**
- Create: `atx-engine/tests/store/store_integration_test.cpp`
- Create: `atx-engine/tests/store/store_schema_golden_test.cpp`

**Interfaces:**
- Consumes: every unit above.
- Produces: no new production header — end-to-end proof + a schema-drift guard.

- [ ] **Step 1: Write the integration test (full lifecycle)**

Create `atx-engine/tests/store/store_integration_test.cpp`:
```cpp
// store_integration_test.cpp — end-to-end: mint alpha -> run -> metrics -> lifecycle ->
// segment -> query "how built" + "what changed". Proves the units compose (Task 9).
#include <gtest/gtest.h>
#include "atx/engine/store/db.hpp"
#include "atx/engine/store/alpha_catalog.hpp"
#include "atx/engine/store/universe_registry.hpp"
#include "atx/engine/store/fingerprint.hpp"
#include "atx/engine/store/run_recorder.hpp"
#include "atx/engine/store/event_log.hpp"
#include "atx/engine/store/segment_index.hpp"

namespace atxtest_store_integration_test {
using namespace atx::engine::store;
namespace cat = atx::engine::store::alpha_catalog;
namespace ur  = atx::engine::store::universe_registry;
namespace fp  = atx::engine::store::fingerprint;
namespace ev  = atx::engine::store::event_log;
namespace si  = atx::engine::store::segment_index;

TEST(StoreIntegration, FullAlphaLifecycle) {
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  auto& db = s->db();
  const atx::u64 H = 0xABCDEFull;

  // 1. universe + snapshot
  ASSERT_TRUE(ur::define(db, "u1", "SP500", 20260101, "top500", 0xFEEDull).has_value());
  ASSERT_TRUE(ur::record_snapshot(db, "snap1", "ORATS", 20260101, 0xBEEFull).has_value());

  // 2. mint alpha + lineage + created event
  ASSERT_TRUE(cat::upsert(db, H, 1, "rank(close - ts_mean(close,5))", 1, "run1").has_value());
  ASSERT_TRUE(cat::add_lineage(db, H, 0x1ull, 3, 99).has_value());
  ASSERT_TRUE(ev::append(db, EventRow{1, H, "created", "run1", "system", "{}"}).has_value());

  // 3. run with fingerprint
  RunInputs in{"sha", "cfg", 0xFEEDull, 0xBEEFull, 7, "gates"};
  RunRow r; r.run_id = "run1"; r.fingerprint = fp::compute(in); r.kind = "backtest";
  r.universe_id = "u1"; r.snapshot_id = "snap1"; r.fit_start = 0; r.fit_end = 100;
  r.bt_start = 100; r.bt_end = 200; r.position_mode = "book"; r.sector_neutral = true;
  r.rebalance_every = 5; r.cost_model = "flat"; r.started_at = 2;
  auto rec = RunRecorder::begin(db, r); ASSERT_TRUE(rec.has_value());
  ASSERT_TRUE(rec->link_alpha(H, "admitted").has_value());
  ASSERT_TRUE(rec->record_metrics(AlphaMetricsRow{H, 1.8, 0.25, 0.08, 0.4, 0.6, 2.5}).has_value());
  ASSERT_TRUE(rec->commit(50, 0xD16E57ull).has_value());

  // 4. lifecycle Candidate->Admitted (dual write)
  ASSERT_TRUE(ev::transition(db, H, LifecycleState::Candidate, LifecycleState::Admitted,
                             100, "run1", 3).has_value());

  // 5. segment mapping for heavy arrays
  ASSERT_TRUE(si::register_segment(db, SegmentRow{"seg1", "/d/seg1", 0xC0DEull, 0, 1, 0x55ull, 1, "run1"}).has_value());
  ASSERT_TRUE(si::map_alpha(db, H, "seg1", 0).has_value());

  // ---- queries the design promised ----
  // "how was it built": parents present
  auto ps = cat::parents(db, H); ASSERT_TRUE(ps.has_value()); EXPECT_EQ(ps->size(), 1u);
  // "what changed over time": created + lifecycle = 2 events
  auto h = ev::history(db, H); ASSERT_TRUE(h.has_value()); EXPECT_EQ(h->size(), 2u);
  // state as-of
  auto st = ev::state_as_of(db, H, 150); ASSERT_TRUE(st.has_value()); EXPECT_EQ(*st, LifecycleState::Admitted);
  // locate heavy arrays
  auto loc = si::locate(db, H); ASSERT_TRUE(loc.has_value()); EXPECT_EQ(loc->segment_id, "seg1");
}

}  // namespace atxtest_store_integration_test
```

- [ ] **Step 2: Write the golden schema snapshot test**

Create `atx-engine/tests/store/store_schema_golden_test.cpp`:
```cpp
// store_schema_golden_test.cpp — guard against accidental schema drift (Task 9).
// Asserts the exact set of table names created by schema::create_all. Adding/removing
// a table is a deliberate act that must update this golden set.
#include <set>
#include <string>
#include <gtest/gtest.h>
#include "atx/engine/store/db.hpp"

namespace atxtest_store_schema_golden_test {
using atx::engine::store::StoreDb;

TEST(StoreSchema, GoldenTableSet) {
  auto s = StoreDb::open_memory(); ASSERT_TRUE(s.has_value());
  auto& db = s->db();
  auto stmt = db.prepare("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name");
  ASSERT_TRUE(stmt.has_value());
  std::set<std::string> got;
  for (;;) {
    auto step = stmt->step(); ASSERT_TRUE(step.has_value());
    if (*step == atx::core::db::Statement::Step::Done) break;
    got.insert(std::string(stmt->column_text(0)));
  }
  const std::set<std::string> golden = {
    "alpha", "alpha_event", "alpha_lineage", "alpha_metrics", "conviction", "data_snapshot",
    "env_config", "eval_fold", "lifecycle_journal", "promotion_ledger", "run", "run_alpha",
    "run_param", "schema_meta", "segment", "segment_alpha", "universe", "universe_member",
  };
  EXPECT_EQ(got, golden);
}

}  // namespace atxtest_store_schema_golden_test
```

- [ ] **Step 3: Run the full store suite**

Run: `cmake --build build --target atx-engine-store-tests && ctest --test-dir build -R 'StoreIntegration|StoreSchema' --output-on-failure`
Expected: PASS — `FullAlphaLifecycle`, `GoldenTableSet`.

- [ ] **Step 4: Run the entire store group to confirm nothing regressed**

Run: `ctest --test-dir build -R 'StoreDb|AlphaCatalog|UniverseRegistry|Fingerprint|RunRecorder|EventLog|SegmentIndex|EnvConfig|Promotion|StoreIntegration|StoreSchema' --output-on-failure`
Expected: PASS — all store tests green.

- [ ] **Step 5: Commit**

```bash
git add atx-engine/tests/store/store_integration_test.cpp atx-engine/tests/store/store_schema_golden_test.cpp
git commit -m "test(store): end-to-end lifecycle integration + golden schema guard"
```

---

## Self-Review

**1. Spec coverage:**
- Identity & lineage → Task 2 (`alpha`, `alpha_lineage`). ✓
- Runs + inputs (universe, fitting window, snapshot, seed, cost model, position mode) → Task 5 `run` row + Task 3 universe/snapshot. ✓
- Run fingerprints + replay → Task 4 + Task 5 `begin` guard. ✓
- Alpha↔run link + per-run metrics → Task 5 `run_alpha` + `alpha_metrics` (`eval_fold`/`conviction` tables created in Task 1 schema; populated by future callers — noted below). ✓
- Event history ("changes over time") → Task 6 `alpha_event` + `history`. ✓
- Lifecycle PIT state → Task 6 `lifecycle_journal` + `state_as_of`. ✓
- Environments (per-env DB file) → Task 1 `StoreDb::open(path)`; env config → Task 8. ✓
- Single-identity promotion → Task 8 `promote` + `promotion_ledger`. ✓
- Heavy arrays stay in segments, DB indexes → Task 7 `segment`/`segment_alpha`. ✓
- Determinism/append-only/golden schema/replay-property → Task 6 + Task 9 tests. ✓

**Gap note (intentional, not a placeholder):** `eval_fold` and `conviction` tables are created in Task 1 and exercised by the golden schema test, but their typed writer functions are deliberately deferred — they follow the exact `record_metrics` pattern and are not needed to prove the architecture. A follow-up task can add `RunRecorder::record_fold` / `record_conviction` when a caller needs them. Flagged here so it is a known omission, not a silent one.

**2. Placeholder scan:** No "TBD"/"implement later"/"add error handling" — every step has full code and exact commands. ✓

**3. Type consistency:** `canon_hash` is `atx::u64` everywhere; bound via `static_cast<atx::i64>` and read via `static_cast<atx::u64>(column_int)` uniformly. `LifecycleState` enumerators 0..5 match `library/lifecycle.hpp`. Function names (`upsert`, `exists`, `add_lineage`, `parents`, `define`, `add_member`, `members`, `record_snapshot`, `compute`, `is_replay`, `begin`, `set_param`, `link_alpha`, `record_metrics`, `commit`, `append`, `transition`, `state_as_of`, `history`, `register_segment`, `map_alpha`, `locate`, `set`, `get`, `promote`) are referenced consistently across tasks and tests. ✓
