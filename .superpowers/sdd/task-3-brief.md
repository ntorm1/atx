### Task 3: `SurfaceDb` class — create/open, atomic manifest persistence, symbol CRUD, `refresh()`

The database object: a root directory, a manifest held as an immutable snapshot, serialized mutations with atomic rewrite + generation bump, and cheap re-sync for the real-time-adjustment story.

**Files:**
- Modify: `atx-vol/include/atx/vol/surface_db.hpp` (add `SurfaceDb`)
- Modify: `atx-vol/src/surface_db.cpp`
- Modify: `atx-vol/tests/surface_db_test.cpp` (add tests)

**Interfaces:**
- Consumes: Task 2 manifest writer/parser.
- Produces (used by Tasks 4-5):

```cpp
// Append inside namespace atx::vol in surface_db.hpp:

struct SurfaceDbCreateOpts {
  std::int64_t created_ts_ns{0};      // 0 => system clock
};

// An opened surface database. Const queries are thread-safe (they read an
// immutable manifest snapshot swapped under a mutex); mutating calls are
// serialized internally. Cross-process: single writer, many readers; every
// mutation is an atomic manifest rewrite (tmp+rename) with generation++ so a
// reader process picks it up via refresh().
class SurfaceDb {
 public:
  // Create <root>/ (and partitions/) and write an empty manifest
  // (generation 1). Errors: AlreadyExists if a manifest already exists at
  // root; IoError on filesystem failure.
  [[nodiscard]] static Result<SurfaceDb> create(std::string_view root,
                                                const SurfaceDbCreateOpts& opts = {});

  // Open an existing database. Errors: NotFound (no manifest), ParseError,
  // IoError.
  [[nodiscard]] static Result<SurfaceDb> open(std::string_view root);

  [[nodiscard]] const std::string& root() const noexcept { return root_; }

  // ── Manifest snapshot queries (thread-safe) ──
  [[nodiscard]] std::shared_ptr<const DbManifest> manifest() const;
  [[nodiscard]] std::uint64_t generation() const;
  [[nodiscard]] std::vector<std::string> symbols() const;   // canonical, sorted
  [[nodiscard]] Result<SymbolFitConfig> symbol_config(std::string_view symbol) const;
  [[nodiscard]] std::vector<DbPartitionInfo> partitions() const;

  // ── Manifest mutation (serialized; atomic rewrite; generation++) ──
  [[nodiscard]] Status upsert_symbol(std::string_view symbol, const SymbolFitConfig& cfg);
  [[nodiscard]] Status remove_symbol(std::string_view symbol);  // NotFound if absent

  // Re-read the manifest from disk iff its generation advanced past the
  // in-memory snapshot (external writer). Ok and no-op when current.
  [[nodiscard]] Status refresh();

  // ── Partition IO (Task 4) ──
  [[nodiscard]] Status write_partition(std::string_view key,
                                       std::span<const SurfaceArchiveItem> items,
                                       const SurfaceArchiveWriteOpts& opts = {});
  [[nodiscard]] Result<SurfaceArchive> open_partition(std::string_view key) const;
  [[nodiscard]] Result<PricedSurface> load_surface(std::string_view key,
                                                   std::string_view symbol) const;
  [[nodiscard]] Status drop_partition(std::string_view key);

 private:
  SurfaceDb() = default;
  [[nodiscard]] Status persist_locked(std::vector<DbSymbolEntry> symbols,
                                      std::vector<DbPartitionInfo> partitions);
  [[nodiscard]] std::string manifest_path() const;
  [[nodiscard]] std::string partition_path(std::string_view canonical_key) const;

  std::string root_{};
  mutable std::mutex mu_{};                       // guards snapshot_ swap + writes
  std::shared_ptr<const DbManifest> snapshot_{};
};
```

(`<mutex>` joins the header includes. `SurfaceDb` is movable, non-copyable — `std::mutex` member means: implement move ctor/assign manually by locking the source, or hold `mu_` in a `std::unique_ptr<std::mutex>`; the unique_ptr route is simpler and fine here.)

**Steps:**

- [ ] **Step 1: Write failing tests** (append to surface_db_test.cpp). Use a per-test temp dir: `std::filesystem::temp_directory_path() / "atx_surface_db_test" / <unique test-name suffix>`; `std::filesystem::remove_all` it at test start AND end (self-cleaning even after a prior crashed run).

```cpp
TEST(SurfaceDb, CreateOpenUpsertReopen_ConfigPersists) {
  const auto root = test_root("create_open");     // helper: fresh temp dir
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  EXPECT_EQ(db->generation(), 1u);
  EXPECT_TRUE(db->symbols().empty());

  const auto cfg = make_full_config();
  ASSERT_TRUE(db->upsert_symbol("aapl", cfg).has_value());
  EXPECT_EQ(db->generation(), 2u);
  ASSERT_TRUE(db->upsert_symbol("SPY", SymbolFitConfig{}).has_value());
  EXPECT_EQ(db->generation(), 3u);

  auto db2 = SurfaceDb::open(root.string());      // fresh process simulation
  ASSERT_TRUE(db2.has_value());
  EXPECT_EQ(db2->generation(), 3u);
  EXPECT_EQ(db2->symbols(), (std::vector<std::string>{"AAPL", "SPY"}));
  auto got = db2->symbol_config("AAPL");
  ASSERT_TRUE(got.has_value());
  expect_config_eq(*got, cfg);

  ASSERT_TRUE(db2->remove_symbol("aapl").has_value());
  EXPECT_EQ(db2->symbol_config("AAPL").error().code(), ErrorCode::NotFound);
  EXPECT_EQ(db2->remove_symbol("AAPL").error().code(), ErrorCode::NotFound);
  std::filesystem::remove_all(root);
}

TEST(SurfaceDb, Create_RejectsExisting_Open_RejectsMissing) {
  const auto root = test_root("create_guard");
  ASSERT_TRUE(SurfaceDb::create(root.string()).has_value());
  EXPECT_EQ(SurfaceDb::create(root.string()).error().code(), ErrorCode::AlreadyExists);
  const auto missing = test_root("no_such_db");
  EXPECT_EQ(SurfaceDb::open(missing.string()).error().code(), ErrorCode::NotFound);
  std::filesystem::remove_all(root);
}

TEST(SurfaceDb, Refresh_SeesExternalWriterUpdate) {
  const auto root = test_root("refresh");
  auto writer = SurfaceDb::create(root.string());
  ASSERT_TRUE(writer.has_value());
  auto reader = SurfaceDb::open(root.string());
  ASSERT_TRUE(reader.has_value());
  EXPECT_EQ(reader->generation(), 1u);

  ASSERT_TRUE(writer->upsert_symbol("QQQ", SymbolFitConfig{}).has_value());
  // Reader still on its old snapshot until refresh:
  EXPECT_EQ(reader->generation(), 1u);
  ASSERT_TRUE(reader->refresh().has_value());
  EXPECT_EQ(reader->generation(), 2u);
  EXPECT_TRUE(reader->symbol_config("QQQ").has_value());
  // Idempotent when current:
  ASSERT_TRUE(reader->refresh().has_value());
  EXPECT_EQ(reader->generation(), 2u);
  std::filesystem::remove_all(root);
}

TEST(SurfaceDb, ConcurrentReaders_DuringUpserts_AreSafe) {
  const auto root = test_root("concurrent");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  ASSERT_TRUE(db->upsert_symbol("SPY", SymbolFitConfig{}).has_value());
  std::atomic<bool> stop{false};
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        auto snap = db->manifest();
        auto cfg = db->symbol_config("SPY");
        ASSERT_TRUE(cfg.has_value());
        (void)snap;
      }
    });
  }
  for (int i = 0; i < 50; ++i) {
    SymbolFitConfig c; c.band_k = 1.0 + 0.01 * i;
    ASSERT_TRUE(db->upsert_symbol("SPY", c).has_value());
  }
  stop.store(true);
  for (auto& th : readers) th.join();
  auto final_cfg = db->symbol_config("SPY");
  ASSERT_TRUE(final_cfg.has_value());
  EXPECT_DOUBLE_EQ(final_cfg->band_k, 1.0 + 0.01 * 49);
  std::filesystem::remove_all(root);
}
```

`test_root(name)` helper: `std::filesystem::temp_directory_path() / ("atx_surface_db_" + std::string(name))`, `remove_all` then return; add `<filesystem>`, `<thread>`, `<atomic>` includes.

- [ ] **Step 2: Build; verify the new tests fail** (missing SurfaceDb symbols): `& .\scripts\atx-build.ps1 build atx-vol-tests` — expect failure mentioning `SurfaceDb`.

- [ ] **Step 3: Implement.** Notes:
  - `create`: `std::filesystem::create_directories(root / "partitions")`; if `root/manifest.atxdb` exists → AlreadyExists; write empty manifest via `write_db_manifest({}, {}, {.generation = 1, .created_ts_ns = opts.created_ts_ns})` and the atomic file write helper below; then delegate to `open`.
  - Atomic write helper (private, reuse for every manifest persist): serialize → `manifest_path() + ".tmp"` → `std::ofstream` binary write → `std::filesystem::rename` (copy the archive's `write_surface_archive_file` error handling, incl. tmp cleanup on failure).
  - `open`: read file fully (NotFound if `!exists`, IoError otherwise on stream failure) → `DbManifest::open` → store `snapshot_ = make_shared<const DbManifest>(std::move(m))`.
  - Mutations (`upsert_symbol`, `remove_symbol`, and Task 4's partition bookkeeping): lock `mu_`; rebuild `std::vector<DbSymbolEntry>` + `std::vector<DbPartitionInfo>` from the current snapshot (decode each record); apply the change (upsert = replace by canonical match or append; remove = erase or NotFound); call `persist_locked` which writes with `generation = old + 1`, `created_ts_ns` preserved from header, `updated_ts_ns = 0 (now)`, re-opens the bytes via `DbManifest::open` (cheap; guarantees the in-memory snapshot is exactly what a fresh reader parses), swaps `snapshot_`.
  - `refresh()`: read the manifest file's first `sizeof(DbManifestHeader)` bytes; if `generation <= snapshot->generation()` → Ok no-op; else full re-read + parse + swap under lock. (Read the header via `std::ifstream` with `read()`; a short read → ParseError.)
  - `manifest()` / queries: lock, copy `shared_ptr`, unlock, then operate on the snapshot.

- [ ] **Step 4: Build + run.** `& .\scripts\atx-build.ps1 build atx-vol-tests && & .\scripts\atx-build.ps1 -Ctest -R SurfaceDb` — expect ALL SurfaceDb* PASS.

- [ ] **Step 5: Commit.**

```bash
git add -A
git commit -m "feat(atx-vol): SurfaceDb - atomic manifest persistence, symbol CRUD, generation refresh"
```

---

