### Task 4: Partition store — `write_partition` / `open_partition` / `load_surface` / `drop_partition`

Partitioned surface storage: each partition key maps to one ATXVSA archive file under `partitions/`; the manifest's partition index is the database's "table of contents".

**Files:**
- Modify: `atx-vol/src/surface_db.cpp` (implement the four methods declared in Task 3)
- Modify: `atx-vol/tests/surface_db_test.cpp`

**Interfaces:**
- Consumes: `write_surface_archive_file` (surface_archive.hpp:275), `SurfaceArchive::open_file` (surface_archive.hpp:292), `SurfaceArchive::map_symbol`, Task 3 `persist_locked`.
- Produces: the four `SurfaceDb` partition methods exactly as declared in Task 3.

**Semantics (bind the implementation to these):**
- `write_partition(key, items, opts)`: validate/canonicalize key (InvalidArgument on bad key; empty `items` is InvalidArgument — delegated to the archive writer which already rejects it). Write the archive to `partitions/<KEY>.atxvsa` via `write_surface_archive_file` (which is itself atomic tmp+rename). On success stat the file size, then update the manifest partition index under the writer lock: upsert `DbPartitionInfo{key, count(items), file_size, now}` — **overwriting an existing key is allowed** (a partition rewrite replaces it; generation bumps). Symbols in `items` do NOT need to be in the manifest symbol table (partitions store surfaces; the symbol table stores fit config — orthogonal namespaces; document this in the header comment).
- `open_partition(key)`: canonicalize; look up the CURRENT snapshot; NotFound if the key is not in the index; `SurfaceArchive::open_file` the path (its ParseError/IoError propagate).
- `load_surface(key, symbol)`: `open_partition` then `map_symbol(symbol)` — one symbol, no full-archive scan (archive guarantees O(1) probe + single blob parse).
- `drop_partition(key)`: NotFound if absent from index; remove from the manifest FIRST (persist, generation++), then `std::filesystem::remove` the file (an orphaned file after a crash between the two steps is harmless garbage — never the reverse order which would leave a dangling index entry; put this reasoning in a comment).

**Steps:**

- [ ] **Step 1: Write failing tests.** Synthesize `PricedSurface`s exactly the way surface_archive_test.cpp does — copy its `make_essvi(uid, n_slices)`, `make_convex(uid, n_slices, n_nodes)`, and `make_linear(...)` helpers (top of that file) into surface_db_test.cpp's anonymous namespace (or a tiny shared local helper; keep it self-contained). **ConvexDense and LinearVariance are first-class citizens of this task's coverage — the mixed-kind test below is mandatory, not optional.**

```cpp
TEST(SurfaceDbPartition, WriteOpenLoad_TheoBitIdentical) {
  const auto root = test_root("part_roundtrip");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());

  const auto s1 = make_essvi(/*uid=*/1, /*n_slices=*/3);
  const auto s2 = make_essvi(/*uid=*/2, /*n_slices=*/2);
  const std::vector<SurfaceArchiveItem> items{{"AAPL", &s1}, {"MSFT", &s2}};
  ASSERT_TRUE(db->write_partition("2026-07-10", items).has_value());
  EXPECT_EQ(db->partitions().size(), 1u);
  EXPECT_EQ(db->partitions()[0].key, "2026-07-10");
  EXPECT_EQ(db->partitions()[0].surface_count, 2u);

  auto loaded = db->load_surface("2026-07-10", "aapl");
  ASSERT_TRUE(loaded.has_value());
  // Bit-identical theo assertion: copy VERBATIM the probe + `bits_equal`
  // comparison block from surface_archive_test.cpp's
  // RoundTrip_Essvi_TheoBitIdentical (compare `loaded` against `s1` exactly
  // the way that test compares its mapped surface against its source —
  // same probe points, same bits_equal oracle, no tolerance comparisons).

  // reopen db cold and load through the fresh instance:
  auto db2 = SurfaceDb::open(root.string());
  ASSERT_TRUE(db2.has_value());
  auto arch = db2->open_partition("2026-07-10");
  ASSERT_TRUE(arch.has_value());
  EXPECT_EQ(arch->count(), 2u);
  ASSERT_TRUE(arch->map_symbol("MSFT").has_value());
  std::filesystem::remove_all(root);
}
```

(The implementer copies the exact theo-probe + `bits_equal` assertions from `RoundTrip_Essvi_TheoBitIdentical` — the plan intentionally defers to that file as the bit-identity oracle rather than restating it; it is the binding pattern.)

```cpp
TEST(SurfaceDbPartition, MixedKinds_ConvexDenseAndLinearVariance_RoundTripBitIdentical) {
  // Explicit requirement: ConvexDense + LinearVariance surfaces fully
  // supported through the db's binary path. One partition holding all three
  // kinds; each loads back with the SAME assertions the archive suite uses:
  //  - ConvexDense: theo bit-identical AND node arrays byte-equal (copy the
  //    assertion block from RoundTrip_ConvexDense_TheoBitIdentical_
  //    AndNodesByteEqual in surface_archive_test.cpp);
  //  - LinearVariance: theo + nodes bit-identical (copy from RoundTrip_
  //    LinearVariance_TheoAndNodesBitIdentical);
  //  - Essvi: theo bit-identical.
  const auto root = test_root("part_mixed_kinds");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  const auto sc = make_convex(/*uid=*/11, /*n_slices=*/2, /*n_nodes=*/40);
  const auto sl = make_linear(/*uid=*/12, /*n_slices=*/2);
  const auto se = make_essvi(/*uid=*/13, /*n_slices=*/2);
  const std::vector<SurfaceArchiveItem> items{
      {"CVX", &sc}, {"LIN", &sl}, {"ESS", &se}};
  ASSERT_TRUE(db->write_partition("2026-07-10", items).has_value());
  auto db2 = SurfaceDb::open(root.string());
  ASSERT_TRUE(db2.has_value());
  auto c = db2->load_surface("2026-07-10", "CVX");
  ASSERT_TRUE(c.has_value());
  // <ConvexDense assertions here — theo bit-identity + byte-equal nodes>
  auto l = db2->load_surface("2026-07-10", "LIN");
  ASSERT_TRUE(l.has_value());
  // <LinearVariance assertions here — theo + nodes bit-identical>
  auto e = db2->load_surface("2026-07-10", "ESS");
  ASSERT_TRUE(e.has_value());
  // <Essvi theo bit-identity assertion here>
  std::filesystem::remove_all(root);
}

TEST(SurfaceDbPartition, RewriteReplaces_DropRemoves) {
  const auto root = test_root("part_lifecycle");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  const auto s1 = make_essvi(1, 2);
  const auto s2 = make_essvi(2, 2);
  const std::vector<SurfaceArchiveItem> one{{"AAPL", &s1}};
  const std::vector<SurfaceArchiveItem> two{{"AAPL", &s1}, {"MSFT", &s2}};
  ASSERT_TRUE(db->write_partition("2026-07-10", one).has_value());
  ASSERT_TRUE(db->write_partition("2026-07-10", two).has_value());  // rewrite
  EXPECT_EQ(db->partitions().size(), 1u);
  EXPECT_EQ(db->partitions()[0].surface_count, 2u);

  ASSERT_TRUE(db->drop_partition("2026-07-10").has_value());
  EXPECT_TRUE(db->partitions().empty());
  EXPECT_EQ(db->open_partition("2026-07-10").error().code(), ErrorCode::NotFound);
  EXPECT_EQ(db->drop_partition("2026-07-10").error().code(), ErrorCode::NotFound);
  // file physically gone:
  EXPECT_FALSE(std::filesystem::exists(
      std::filesystem::path(root) / "partitions" / "2026-07-10.atxvsa"));
  std::filesystem::remove_all(root);
}

TEST(SurfaceDbPartition, ManySymbols_ManyPartitions_SingleSurfaceLookup) {
  const auto root = test_root("part_scale");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  std::vector<PricedSurface> pool;
  pool.reserve(64);
  for (int i = 0; i < 64; ++i) pool.push_back(make_essvi(100 + i, 2));
  for (int p = 0; p < 4; ++p) {
    std::vector<SurfaceArchiveItem> items;
    for (int i = 0; i < 64; ++i) {
      items.push_back({std::string("SYM") + std::to_string(i), &pool[i]});
    }
    // NOTE: symbol strings must outlive the call — build a std::vector<std::string>
    // holder first, then string_views into it.
    ASSERT_TRUE(db->write_partition("2026-07-1" + std::to_string(p), items).has_value());
  }
  EXPECT_EQ(db->partitions().size(), 4u);
  auto s = db->load_surface("2026-07-12", "SYM42");
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ(db->load_surface("2026-07-12", "NOPE").error().code(), ErrorCode::NotFound);
  EXPECT_EQ(db->load_surface("2026-99-99", "SYM1").error().code(), ErrorCode::NotFound);
  std::filesystem::remove_all(root);
}

TEST(SurfaceDbPartition, BadKey_Rejected) {
  const auto root = test_root("part_badkey");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  const auto s1 = make_essvi(1, 2);
  const std::vector<SurfaceArchiveItem> items{{"AAPL", &s1}};
  for (const char* bad : {"", "a/b", "a\\b", "..", "x..y",
                          "0123456789012345678901234567890123"}) {
    EXPECT_EQ(db->write_partition(bad, items).error().code(),
              ErrorCode::InvalidArgument) << bad;
  }
  std::filesystem::remove_all(root);
}
```

- [ ] **Step 2: Build; verify new tests fail.** (Methods stubbed as `Err(ErrorCode::NotImplemented, ...)` from Task 3 or missing → compile/behavioral failure.)

- [ ] **Step 3: Implement** per the Semantics block above.

- [ ] **Step 4: Build + run all db tests + archive regression.** `& .\scripts\atx-build.ps1 build atx-vol-tests` then `-Ctest -R "SurfaceDb|SurfaceArchive"` — expect ALL PASS.

- [ ] **Step 5: Commit.**

```bash
git add -A
git commit -m "feat(atx-vol): SurfaceDb partition store over ATXVSA archives"
```

---

