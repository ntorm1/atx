### Task 1: `Clock::from_surface_db` — SurfaceDb-backed backtest clock

The backtest must run *from a `SurfaceDb` root*. Partitions are bare ATXVSA files at `<root>/partitions/<KEY>.atxvsa` — the same format `MarketSnapshot::load` already opens — so the glue is a `Clock` factory that turns the db's partition index into ordered `SnapshotRef`s. `SnapshotCache`/prefetch stay path-keyed and work unchanged.

**Files:**
- Modify: `atx-vol/include/atx/vol/backtest.hpp` (Clock class, ~line 60-69)
- Modify: `atx-vol/src/backtest.cpp` (next to `Clock::from_manifest`, ~line 293-309)
- Create: `atx-vol/tests/surface_db_backtest_test.cpp`
- Modify: `atx-vol/tests/CMakeLists.txt` (add the new test source to the `add_executable(atx-vol-tests ...)` list, before the closing paren ~line 86)

**Interfaces:**
- Consumes: `SurfaceDb` public API (`surface_db.hpp`: `root()`, `partitions()` → `std::vector<DbPartitionInfo{key, surface_count, file_size, created_ts_ns}>`, constants `kSurfaceDbPartitionDir="partitions"`, `kSurfaceDbPartitionExt=".atxvsa"`), existing `SnapshotRef{std::string date; std::string archive_path;}` and private `Clock` ref storage.
- Produces:

```cpp
// backtest.hpp — inside class Clock, next to from_manifest. Forward-declare
// `class SurfaceDb;` in the atx::vol namespace at the top of backtest.hpp
// (do NOT include surface_db.hpp in the header; include it in backtest.cpp).
//
// Build a clock over a SurfaceDb: one SnapshotRef per partition, ordered by
// ascending partition key (keys are canonical uppercase; ISO dates like
// "2026-07-11" sort chronologically). ref.date = partition key, ref.archive_path
// = "<root>/partitions/<KEY>.atxvsa". InvalidArgument if the db has no
// partitions. The snapshot files are ordinary ATXVSA archives, so
// MarketSnapshot::load / SnapshotCache consume the refs unchanged.
static Result<Clock> from_surface_db(const SurfaceDb &db);
```

- [ ] **Step 1: Write the failing tests.** New file `atx-vol/tests/surface_db_backtest_test.cpp`. Copy the analytic-eSSVI `make_surface(S, now_ts, vol_bump, uid)` helper pattern VERBATIM from `atx-vol/tests/strategy_test.cpp:60-91` (7 slices, T∈{0.05,0.10,0.20,0.35,0.50,0.75,1.00}, `EssviParams`, `PricingContext{S, r=0.043, now_ts, AndersenLake, al_fast_opts(), uid}`) into the anonymous namespace — it is the binding fixture pattern. Give each symbol a distinct uid and vol_bump; each date a distinct `now_ts` advancing by `kDayNs = 86'400'000'000'000` and all symbols within one date share the same `now_ts` (MarketSnapshot requires ts agreement).

```cpp
TEST(SurfaceDbBacktest, ClockFromDb_OrderedRefsAndPathsLoad) {
  const auto root = test_root("clock_from_db");   // temp-dir helper, copy from surface_db_test.cpp:150-153
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  // 3 dates, deliberately written OUT of order; 2 symbols per date.
  const char *dates[] = {"2026-01-06", "2026-01-02", "2026-01-05"};
  std::int64_t base_ts = 1'700'000'000'000'000'000;
  const std::int64_t day_ts[] = {base_ts + 4 * kDayNs, base_ts, base_ts + 3 * kDayNs};
  for (int d = 0; d < 3; ++d) {
    const auto spy = make_surface(500.0, day_ts[d], 0.0, /*uid=*/1);
    const auto aapl = make_surface(200.0, day_ts[d], 0.05, /*uid=*/2);
    std::vector<SurfaceArchiveItem> items{{"SPY", &spy}, {"AAPL", &aapl}};
    ASSERT_TRUE(db->write_partition(dates[d], items).has_value());
  }
  auto clock = Clock::from_surface_db(*db);
  ASSERT_TRUE(clock.has_value());
  ASSERT_EQ(clock->size(), 3u);
  EXPECT_EQ(clock->refs()[0].date, "2026-01-02");
  EXPECT_EQ(clock->refs()[1].date, "2026-01-05");
  EXPECT_EQ(clock->refs()[2].date, "2026-01-06");
  // Every ref path loads as a MarketSnapshot with both symbols resolvable.
  for (const auto &ref : clock->refs()) {
    auto snap = MarketSnapshot::load(ref.archive_path);
    ASSERT_TRUE(snap.has_value()) << ref.archive_path;
    EXPECT_TRUE(snap->uid_of("SPY").has_value());
    EXPECT_TRUE(snap->uid_of("aapl").has_value());   // symbol lookup canonicalizes
  }
  std::filesystem::remove_all(root);
}

TEST(SurfaceDbBacktest, ClockFromDb_EmptyDbRejected) {
  const auto root = test_root("clock_from_db_empty");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  auto clock = Clock::from_surface_db(*db);
  ASSERT_FALSE(clock.has_value());
  EXPECT_EQ(clock.error().code(), ErrorCode::InvalidArgument);
  std::filesystem::remove_all(root);
}

TEST(SurfaceDbBacktest, DbDrivesRunBacktestEndToEnd) {
  // Acceptance gate: a db populated with a small synthetic multi-date corpus
  // drives run_backtest end-to-end — no loose archive paths, no CorpusManifest.
  const auto root = test_root("db_backtest_e2e");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  std::int64_t base_ts = 1'700'000'000'000'000'000;
  for (int d = 0; d < 6; ++d) {
    char date[11];
    std::snprintf(date, sizeof date, "2026-02-%02d", 2 + d);
    const double spot = 500.0 * (1.0 + 0.002 * d);   // gentle drift so pnl is nonzero
    const auto spy = make_surface(spot, base_ts + d * kDayNs, 0.0, /*uid=*/1);
    std::vector<SurfaceArchiveItem> items{{"SPY", &spy}};
    ASSERT_TRUE(db->write_partition(date, items).has_value());
  }
  auto clock = Clock::from_surface_db(*db);
  ASSERT_TRUE(clock.has_value());
  StrategySpec spec;   // simple daily-restrike short strangle, mirrors spy_strangle_backtest_test::make_spec
  spec.name = "db-e2e";
  LegSpec leg;
  leg.symbol = "SPY";
  leg.tenor.target_T = 0.5;
  leg.structure.kind = StructureSpec::Kind::Strangle;
  leg.structure.call_leg = {StrikeSelector::Kind::Delta, 0.40};
  leg.structure.put_leg = {StrikeSelector::Kind::Delta, 0.40};
  leg.size = {SizeSpec::Kind::FixedContracts, 10.0, -1.0};
  spec.legs.push_back(leg);
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::RollAtHorizon;
  spec.lifecycle.roll_at_T = 1.0;   // > tenor: daily restrike, single cohort
  DeclarativeStrategy strat(spec);
  auto r = run_backtest(*clock, strat, RunConfig{});
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->size(), 6u);
  EXPECT_EQ(r->date.front(), "2026-02-02");
  EXPECT_EQ(r->date.back(), "2026-02-07");
  for (std::size_t i = 0; i < r->size(); ++i) EXPECT_EQ(r->n_open_lots[i], 2u) << i;
  // Non-degenerate: something priced and moved.
  EXPECT_NE(r->nav.back(), 0.0);
  std::filesystem::remove_all(root);
}
```

- [ ] **Step 2: Build; verify the new tests fail** (missing `Clock::from_surface_db` symbol → compile failure of the test file). `& .\scripts\atx-build.ps1 build atx-vol-tests` — expect error naming `from_surface_db`.

- [ ] **Step 3: Implement.** In `backtest.hpp`: forward-declare `class SurfaceDb;` and add the static factory declaration to `Clock`. In `backtest.cpp` (include `atx/vol/surface_db.hpp` at the top):

```cpp
Result<Clock> Clock::from_surface_db(const SurfaceDb &db) {
  auto parts = db.partitions();
  if (parts.empty()) {
    return Err(ErrorCode::InvalidArgument, "surface db has no partitions");
  }
  std::sort(parts.begin(), parts.end(),
            [](const DbPartitionInfo &a, const DbPartitionInfo &b) { return a.key < b.key; });
  Clock c;
  c.refs_.reserve(parts.size());
  const std::filesystem::path dir = std::filesystem::path(db.root()) / kSurfaceDbPartitionDir;
  for (const auto &p : parts) {
    c.refs_.push_back(SnapshotRef{p.key, (dir / (p.key + kSurfaceDbPartitionExt)).string()});
  }
  return Ok(std::move(c));
}
```

Match `from_manifest`'s construction mechanics exactly (whatever private ctor/member access it uses, use the same — do not add new public mutators).

- [ ] **Step 4: Build + run targeted suites.** `& .\scripts\atx-build.ps1 build atx-vol-tests` then `& .\scripts\atx-build.ps1 -Ctest -R "SurfaceDbBacktest|SurfaceDb|SurfaceArchive|Backtest"` — ALL PASS.

- [ ] **Step 5: Commit.**

```bash
git add -A
git commit -m "feat(atx-vol): Clock::from_surface_db - SurfaceDb-backed backtest clock"
```

---

