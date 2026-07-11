# MAG7 vs SPY Dispersion-Strangle Backtest Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the MAG7-vs-SPY dispersion-strangle strategy backtestable from a small example file, running entirely off a `SurfaceDb` on real YTD 2026 data, emitting machine-readable run outputs that a Python helper renders into one self-contained HTML report.

**Architecture:** Four library seams close the gaps: (1) `Clock::from_surface_db` bridges `SurfaceDb` date-keyed partitions into the existing path-based backtest loader; (2) a `LifecycleSpec::Holding::CloseAtHorizon` mode plus a missing-name policy extend `DeclarativeStrategy` to overlapping daily cohorts closed at target DTE; (3) `make_dispersion_strangle_spec` composes the existing DSL (per-leg symbols, 40Δ strangles, `TargetTheta`, `FlatVega`) into the strategy; (4) `run_report` emitters + `populate_surface_db` produce the data files and the populated db. The example only composes these. Python renders.

**Tech Stack:** C++23 (clang-cl, /WX), GoogleTest, existing atx-vol surface/backtest stack, databento-cpp (pull reuse only), Python pandas+matplotlib (report).

## Global Constraints

Copied binding requirements from the goal spec (`atx-vol/sprints/2026-07-11-atx-vol-mag7-dispersion-strangle-backtest-goal.md`):

- Universe: MAG7 = AAPL, MSFT, GOOGL, AMZN, NVDA, META, TSLA; index = SPY.
- Strategy defaults (document in report header): per-name theta budget **$10/calendar-day per name per cohort**, option multiplier **100**, target |Δ| **0.40** both legs, tenor **90 calendar days** (expiry = entry ts + 90d, so DTE decays naturally), close each cohort at **10 days to expiry**, entry **every trading day**, frictions **off by default** (flag to enable).
- **Projection path only**: strikes/expiries synthetic, resolved and marked off fitted `PricedSurface`s. No listed-contract snapping, no OPRA quote marks for P&L.
- All new functionality in `atx-vol` library; the example only composes library calls, target ≤ ~300 lines.
- **No HTML/SVG generation in C++** — atx-vol emits data (`# key=value` metadata-header CSV/TSV convention); `atx-vol/tools/mag7_dispersion_report.py` renders. Python deps limited to what `tools/tearsheet.py` uses (pandas/matplotlib or stdlib).
- Determinism: same db + same config ⇒ bit-identical `BacktestResult` across runs and worker counts (respect `ATX_VOL_FIT_WORKERS`).
- `/WX` clean (warnings are errors). No regression in `SurfaceDb|SurfaceArchive|Backtest|Dispersion|Strategy` targeted suites. Full `-L atx_vol` gate is DEFERRED TO THE OPERATOR (do not run it; run targeted suites only).
- Don't rewrite `DispersionStrategy`/`build_dispersion_book` — extend or parameterize. Don't touch `tools/tearsheet.py` (stays as-is for PNG workflows).
- Missing-name handling: `MissingNamePolicy` semantics (document per-name failures, never silently drop without recording).
- Data guardrails (Task 8 only): always cost-estimate (`MetadataGetCost` / `--dry-run`) and log before any paid request; if total YTD estimate > ~$150 STOP and ask the operator; pull once, cache parquet hive under `data/`, never re-pull existing files; API key via `DATABENTO_API_KEY` env var only — never print or commit the key.
- Build: Windows PowerShell 5.1. Worktree is already configured. Build with `& .\scripts\atx-build.ps1 build atx-vol-tests`; test with `& .\scripts\atx-build.ps1 -Ctest -R <regex>` (both from the worktree root — the script derives repo root from its own location).
- Commit style: `feat(atx-vol): ...` / `test(atx-vol): ...` / `fix(atx-vol): ...`; commit per green task.
- Multiplier is hardcoded `kMult=100` in `resolve_spec` (strategy.cpp) — do not add a knob (YAGNI); the default is the spec.

**Ground-truth corrections to the goal doc** (recon-verified; the goal doc text yields to these):
- A bulk OPRA pull tool ALREADY EXISTS: `atx-core/examples/databento_bulk_opra.cpp` (cost-gated `--cap`/`--dry-run`, idempotent skip of existing files, writes `<out>/{symbol}/{date}.parquet`). It produced the existing 123-day SPY hive `data/spy_ytd/opra/SPY/`. Task 8 REUSES it; no new pull tool is built.
- `BacktestResult`'s `gross_delta/gamma/vega/theta` columns are signed net book greeks (position-scaled sums from `PortfolioPricer::price_totals`), despite the name. Net-vega evidence comes straight from that series.

---

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

### Task 2: Lifecycle `CloseAtHorizon` + missing-name policy for `DeclarativeStrategy`

Two DSL gaps block the strategy. (1) `LifecycleSpec::Holding` today is `HoldToExpiry` (overlapping cohorts, engine settles at expiry) XOR `RollAtHorizon` (close at DTE but single cohort). The strategy needs BOTH: overlapping daily cohorts, each independently closed at 10 DTE. (2) `DeclarativeStrategy` hard-fails (`NotFound`) if any leg's symbol is missing from a snapshot; real multi-name data will have gaps, and `MissingNamePolicy` semantics must apply.

**Files:**
- Modify: `atx-vol/include/atx/vol/strategy.hpp` (LifecycleSpec ~line 105-115, StrategySpec ~line 127-133, new free function near `resolve_spec` ~line 184)
- Modify: `atx-vol/src/strategy.cpp` (`lifecycle_decide` ~line 386-408, `DeclarativeStrategy::on_step`/`open_cohort` ~line 410-465, `resolve_spec` ~line 292-382)
- Modify: `atx-vol/tests/strategy_test.cpp`

**Interfaces:**
- Consumes: existing `LifecycleSpec`, `StrategySpec`, `resolve_spec`, `lifecycle_decide`, `Lot{... expiry_ts_ns, cohort ...}`, `MissingNameSpec{MissingNamePolicy policy{Error}; std::size_t min_names{2}}` and `MissingNamePolicy{Error, DropRenormalize}` from `dispersion.hpp` (already included by strategy.hpp for `DispersionStrategy`).
- Produces:

```cpp
// strategy.hpp — LifecycleSpec::Holding gains a third mode (append, keep values):
enum class Holding : std::uint8_t {
  HoldToExpiry = 0,
  RollAtHorizon = 1,
  // Overlapping cohorts (one per entry tick, like HoldToExpiry), but each lot
  // is closed by the strategy when its residual maturity falls below
  // roll_at_T: close when (lot.expiry_ts_ns - base_ts) < roll_at_T * kNsPerYear.
  // The engine books the close at current marks (roll-close diff), never
  // settlement. lifecycle_decide never returns clear=true in this mode.
  CloseAtHorizon = 2,
};

// strategy.hpp — StrategySpec gains a missing-name policy (default preserves
// today's hard-fail behavior exactly):
struct StrategySpec {
  // ... existing fields unchanged ...
  MissingNameSpec missing{};   // {Error, min_names=2}
};

// strategy.hpp — policy-aware resolution, near resolve_spec. With policy Error
// this is EXACTLY resolve_spec (same errors). With DropRenormalize:
//  - a leg whose expansion or sizing fails is DROPPED and recorded in *dropped
//    (symbol + error detail), UNLESS the leg's group equals
//    spec.constraint.group_b (the scaled hedge group) — a missing hedge leg
//    makes the whole entry unbuildable: return Err(Unavailable, ...).
//  - if the count of surviving legs whose group == spec.constraint.group_a
//    (all legs when constraint.kind == None) is < spec.missing.min_names,
//    return Err(Unavailable, ...).
//  - sizing + the cross-leg constraint then run on the survivors; FlatVega's
//    scale = gross_a/gross_b is computed from surviving legs' actual vegas, so
//    the hedge renormalizes automatically.
struct ResolveDrop {
  std::string symbol;
  std::string detail;
};
[[nodiscard]] Result<std::vector<SizedLeg>>
resolve_spec_with_policy(const MarketSnapshot &snap, const StrategySpec &spec,
                         std::vector<ResolveDrop> *dropped = nullptr);
```

Behavioral contract for `DeclarativeStrategy` (consumed by Tasks 3/6):
- `on_step`, when `spec.lifecycle.holding == CloseAtHorizon`: FIRST erase from the book every lot with `(lot.expiry_ts_ns - base.ts_ns()) < roll_at_T * kNsPerYear` (close pass — the engine's before/after diff books the closes at today's marks); THEN if `lifecycle_decide(...).open` is true, open a new cohort. `lifecycle_decide` for `CloseAtHorizon` returns `open` on every entry tick (same rule as `HoldToExpiry`: `EveryStep`, or `step_index % entry_every_n == 0`) and `clear=false` always.
- `open_cohort` uses `resolve_spec_with_policy(base, spec_, &last_dropped_)`. An `Err` with code `Unavailable` under `DropRenormalize` is a NO-TRADE step: the book is left untouched and `on_step` returns Ok (mirror `DispersionStrategy`'s no-trade contract, dispersion_strategy.cpp). Any other error code is fatal (propagate).
- New accessor on `DeclarativeStrategy`: `[[nodiscard]] std::span<const ResolveDrop> dropped_on_last_entry() const;` (cleared at each entry attempt) — this is the "document per-name failures" hook.

- [ ] **Step 1: Write the failing tests.** Append to `atx-vol/tests/strategy_test.cpp` (reuse its existing `make_surface`/`write_archive`/`make_manifest` helpers and corpus-building pattern — see `Strategy.OverlappingClips` for the multi-date fixture idiom):

```cpp
TEST(Strategy, CloseAtHorizonOverlappingCohorts) {
  // 10 consecutive daily snapshots. Tenor 6 calendar days, close below 2.5
  // days residual (half-day margin keeps the comparison off exact-boundary
  // floating point). A cohort opened on day d has expiry d+6; residual at age
  // 3 is 3d (alive), at age 4 is 2d (< 2.5 -> close), so each cohort lives
  // ages 0..3 = 4 days. Steady state: 4 live cohorts x 2 strangle lots = 8.
  auto corpus = make_corpus(/*n_dates=*/10);   // per-file helper pattern; daily ts spacing
  auto clock = Clock::from_manifest(corpus.manifest);
  ASSERT_TRUE(clock.has_value());
  StrategySpec spec;
  spec.name = "close-at-horizon";
  LegSpec leg;
  leg.symbol = "SPY";
  leg.tenor.target_T = 6.0 / 365.25;
  leg.structure.kind = StructureSpec::Kind::Strangle;
  leg.structure.call_leg = {StrikeSelector::Kind::Delta, 0.40};
  leg.structure.put_leg = {StrikeSelector::Kind::Delta, 0.40};
  leg.size = {SizeSpec::Kind::FixedContracts, 1.0, +1.0};
  spec.legs.push_back(leg);
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::CloseAtHorizon;
  spec.lifecycle.roll_at_T = 2.5 / 365.25;
  DeclarativeStrategy strat(spec);
  auto r = run_backtest(*clock, strat, RunConfig{});
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->size(), 10u);
  // Ramp 2,4,6,8 then plateau at 8 (close of oldest exactly offsets the new entry).
  const unsigned expect[] = {2, 4, 6, 8, 8, 8, 8, 8, 8, 8};
  for (std::size_t i = 0; i < 10; ++i) EXPECT_EQ(r->n_open_lots[i], expect[i]) << i;
  // Closes are roll-closes at marks, never engine settlement.
  for (std::size_t i = 0; i < 10; ++i) EXPECT_EQ(r->pnl_settlement[i], 0.0) << i;
}

TEST(Strategy, MissingNameDropRenormalize) {
  // Snapshot holds SPY + XOM only; spec asks for SPY-index vs {XOM, FAKE} basket.
  auto spy = make_surface(500.0, kNowTs, 0.0, /*uid=*/1);
  auto xom = make_surface(110.0, kNowTs, 0.05, /*uid=*/2);
  auto snap = snapshot_of({{"SPY", &spy}, {"XOM", &xom}});   // write_archive + MarketSnapshot::load helper
  StrategySpec spec;
  auto name_leg = [&](std::string sym) {
    LegSpec l;
    l.symbol = std::move(sym);
    l.tenor.target_T = 0.25;
    l.structure.kind = StructureSpec::Kind::Strangle;
    l.structure.call_leg = {StrikeSelector::Kind::Delta, 0.40};
    l.structure.put_leg = {StrikeSelector::Kind::Delta, 0.40};
    l.size = {SizeSpec::Kind::TargetTheta, 10.0, +1.0};
    l.group = "basket";
    return l;
  };
  spec.legs.push_back(name_leg("XOM"));
  spec.legs.push_back(name_leg("FAKE"));
  LegSpec idx = name_leg("SPY");
  idx.size = {SizeSpec::Kind::TargetVega, 10000.0, -1.0};
  idx.group = "index";
  spec.legs.push_back(idx);
  spec.constraint = {CrossLegConstraint::Kind::FlatVega, "basket", "index"};
  spec.missing = {MissingNamePolicy::DropRenormalize, /*min_names=*/1};

  std::vector<ResolveDrop> dropped;
  auto legs = resolve_spec_with_policy(*snap, spec, &dropped);
  ASSERT_TRUE(legs.has_value());
  ASSERT_EQ(dropped.size(), 1u);
  EXPECT_EQ(dropped[0].symbol, "FAKE");
  // Survivors: XOM strangle (2) + SPY strangle (2); constraint held on survivors.
  ASSERT_EQ(legs->size(), 4u);
  double net_vega = 0.0, gross_vega = 0.0;
  for (const auto &sl : *legs) {
    net_vega += sl.qty * sl.leg.vega * sl.multiplier;
    gross_vega += std::abs(sl.qty * sl.leg.vega * sl.multiplier);
  }
  EXPECT_LE(std::abs(net_vega), 1e-9 * gross_vega);

  // min_names floor: requiring 2 surviving basket names -> Unavailable.
  spec.missing.min_names = 2;
  auto floored = resolve_spec_with_policy(*snap, spec, nullptr);
  ASSERT_FALSE(floored.has_value());
  EXPECT_EQ(floored.error().code(), ErrorCode::Unavailable);

  // Missing HEDGE leg (group_b) is never droppable.
  spec.missing.min_names = 1;
  spec.legs[2].symbol = "NOPE";
  auto no_hedge = resolve_spec_with_policy(*snap, spec, nullptr);
  ASSERT_FALSE(no_hedge.has_value());
  EXPECT_EQ(no_hedge.error().code(), ErrorCode::Unavailable);

  // Error policy preserves today's hard fail.
  spec.legs[2].symbol = "SPY";
  spec.missing = {MissingNamePolicy::Error, 2};
  auto hard = resolve_spec_with_policy(*snap, spec, nullptr);
  ASSERT_FALSE(hard.has_value());
  EXPECT_EQ(hard.error().code(), ErrorCode::NotFound);
}

TEST(Strategy, CloseAtHorizonNoTradeOnMissingEntry) {
  // Under DropRenormalize with an unbuildable entry (hedge symbol absent from
  // EVERY snapshot), DeclarativeStrategy no-trades instead of erroring, and
  // the run completes with an empty book throughout.
  auto corpus = make_corpus(/*n_dates=*/4);   // archives contain SPY only
  auto clock = Clock::from_manifest(corpus.manifest);
  ASSERT_TRUE(clock.has_value());
  StrategySpec spec;
  LegSpec l;
  l.symbol = "SPY";
  l.tenor.target_T = 0.25;
  l.structure.kind = StructureSpec::Kind::Strangle;
  l.structure.call_leg = {StrikeSelector::Kind::Delta, 0.40};
  l.structure.put_leg = {StrikeSelector::Kind::Delta, 0.40};
  l.size = {SizeSpec::Kind::TargetTheta, 10.0, +1.0};
  l.group = "basket";
  spec.legs.push_back(l);
  LegSpec idx = l;
  idx.symbol = "MISSING_INDEX";
  idx.group = "index";
  idx.size = {SizeSpec::Kind::TargetVega, 10000.0, -1.0};
  spec.legs.push_back(idx);
  spec.constraint = {CrossLegConstraint::Kind::FlatVega, "basket", "index"};
  spec.missing = {MissingNamePolicy::DropRenormalize, 1};
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::CloseAtHorizon;
  spec.lifecycle.roll_at_T = 2.0 / 365.25;
  DeclarativeStrategy strat(spec);
  auto r = run_backtest(*clock, strat, RunConfig{});
  ASSERT_TRUE(r.has_value());
  for (std::size_t i = 0; i < r->size(); ++i) EXPECT_EQ(r->n_open_lots[i], 0u) << i;
}
```

Adjust helper names to what strategy_test.cpp actually provides (`make_corpus`, `snapshot_of`, `kNowTs` are descriptive here — reuse/extend the file's real fixtures rather than inventing parallel ones).

- [ ] **Step 2: Build; verify failure** (missing enum value / `resolve_spec_with_policy` symbol / `missing` member).

- [ ] **Step 3: Implement.**
  - `lifecycle_decide`: `CloseAtHorizon` takes the `HoldToExpiry` open rule; `clear` stays false. Keep the existing two modes byte-for-byte identical in behavior.
  - `resolve_spec_with_policy`: implement per the contract above. Refactor `resolve_spec`'s body so both share one implementation (e.g. `resolve_spec` delegates with policy `Error`) — do NOT duplicate the sizing/constraint block.
  - `DeclarativeStrategy`: close pass before entry (per contract); `open_cohort` switches to `resolve_spec_with_policy`; add `dropped_on_last_entry()`; `Unavailable`+`DropRenormalize` → no-trade.
- [ ] **Step 4: Build + run targeted suites.** `& .\scripts\atx-build.ps1 -Ctest -R "Strategy|Backtest|Dispersion"` — ALL PASS (existing `Strategy.*` suites must be untouched-green).
- [ ] **Step 5: Commit.**

```bash
git add -A
git commit -m "feat(atx-vol): CloseAtHorizon lifecycle + missing-name policy for DeclarativeStrategy"
```

---

### Task 3: `make_dispersion_strangle_spec` — the strategy in one config struct

The example must stay small, so the leg/constraint/lifecycle assembly lives in the library: a validated builder from a plain config to a `StrategySpec`. Tests pin the acceptance math: 40Δ strikes reprice, per-name theta equal, cohort net vega ≈ 0 at entry.

**Files:**
- Create: `atx-vol/include/atx/vol/dispersion_strangle.hpp`
- Create: `atx-vol/src/dispersion_strangle.cpp`
- Create: `atx-vol/tests/dispersion_strangle_test.cpp`
- Modify: `atx-vol/CMakeLists.txt` (add `src/dispersion_strangle.cpp` to the `add_library(atx-vol ...)` source list, near `src/dispersion.cpp`)
- Modify: `atx-vol/tests/CMakeLists.txt` (add the test source)

**Interfaces:**
- Consumes: Task 2's `Holding::CloseAtHorizon`, `StrategySpec::missing`, plus existing `LegSpec`, `StructureSpec::Strangle`, `StrikeSelector::Delta`, `SizeSpec::{TargetTheta,TargetVega}`, `CrossLegConstraint::FlatVega`, `MissingNameSpec`.
- Produces:

```cpp
// dispersion_strangle.hpp
namespace atx::vol {

// Long equal-theta single-name strangles vs a short vega-flat index strangle,
// one cohort per entry tick, each cohort closed at close_dte_days to expiry.
// Pricing is projection-path only (synthetic strikes/expiries off the fitted
// surfaces); expiry = entry ts + tenor_days calendar days.
struct DispersionStrangleConfig {
  std::vector<std::string> names;              // long single names (>= 1)
  std::string index_symbol{"SPY"};             // short hedge leg
  double target_abs_delta{0.40};               // both strangle legs, in (0,1)
  double tenor_days{90.0};                     // calendar days to synthetic expiry
  double close_dte_days{10.0};                 // close cohort below this residual
  unsigned entry_every_n_days{1};              // 1 = every trading day (EveryStep)
  double theta_per_name_daily{10.0};           // $/calendar-day theta per name
  double index_base_vega{10000.0};             // pre-constraint index sizing seed
  MissingNameSpec missing{MissingNamePolicy::DropRenormalize, 4};
  HedgeSpec hedge{};                           // default: no delta hedge
};

// Validated assembly into the declarative DSL:
//  - one LegSpec per name: Strangle{Delta d call, Delta d put}, tenor
//    tenor_days/365.25, SizeSpec{TargetTheta, theta_per_name_daily, +1},
//    group "basket";
//  - one index LegSpec: same structure/tenor, SizeSpec{TargetVega,
//    index_base_vega, -1}, group "index";
//  - constraint FlatVega{group_a="basket", group_b="index"} (scales the index
//    leg so gross index vega == gross basket vega; opposite signs net ~0);
//  - lifecycle: EveryStep when entry_every_n_days==1 else EveryNDays with
//    entry_every_n, Holding::CloseAtHorizon, roll_at_T = close_dte_days/365.25;
//  - spec.missing = cfg.missing, spec.hedge = cfg.hedge,
//    spec.name = "mag7_dispersion_strangle" (or names.size()-agnostic label).
// InvalidArgument when: names empty; index_symbol empty or contained in
// names; target_abs_delta outside (0,1); tenor_days <= close_dte_days;
// close_dte_days < 0; theta_per_name_daily <= 0; index_base_vega <= 0;
// entry_every_n_days == 0; missing.min_names > names.size().
[[nodiscard]] Result<StrategySpec>
make_dispersion_strangle_spec(const DispersionStrangleConfig &cfg);

}  // namespace atx::vol
```

- [ ] **Step 1: Write the failing tests.** New `atx-vol/tests/dispersion_strangle_test.cpp`. Fixture: copy the `make_surface` analytic-eSSVI pattern from strategy_test.cpp; build ONE archive holding 4 surfaces — 3 "names" (`AAA` uid 1 vol_bump 0.00, `BBB` uid 2 bump 0.06, `CCC` uid 3 bump 0.12, spots 100/150/200) + index `SPX` (uid 9, spot 500, bump 0.02) — and `MarketSnapshot::load` it.

```cpp
TEST(DispersionStrangle, SpecShape) {
  DispersionStrangleConfig cfg;
  cfg.names = {"AAA", "BBB", "CCC"};
  cfg.index_symbol = "SPX";
  cfg.missing = {MissingNamePolicy::DropRenormalize, 2};
  auto spec = make_dispersion_strangle_spec(cfg);
  ASSERT_TRUE(spec.has_value());
  ASSERT_EQ(spec->legs.size(), 4u);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(spec->legs[i].group, "basket");
    EXPECT_EQ(spec->legs[i].size.kind, SizeSpec::Kind::TargetTheta);
    EXPECT_DOUBLE_EQ(spec->legs[i].size.sign, +1.0);
    EXPECT_EQ(spec->legs[i].structure.kind, StructureSpec::Kind::Strangle);
    EXPECT_DOUBLE_EQ(spec->legs[i].tenor.target_T, 90.0 / 365.25);
  }
  EXPECT_EQ(spec->legs[3].symbol, "SPX");
  EXPECT_EQ(spec->legs[3].group, "index");
  EXPECT_DOUBLE_EQ(spec->legs[3].size.sign, -1.0);
  EXPECT_EQ(spec->constraint.kind, CrossLegConstraint::Kind::FlatVega);
  EXPECT_EQ(spec->constraint.group_a, "basket");
  EXPECT_EQ(spec->constraint.group_b, "index");
  EXPECT_EQ(spec->lifecycle.holding, LifecycleSpec::Holding::CloseAtHorizon);
  EXPECT_DOUBLE_EQ(spec->lifecycle.roll_at_T, 10.0 / 365.25);
  EXPECT_EQ(spec->lifecycle.entry, LifecycleSpec::Entry::EveryStep);
}

TEST(DispersionStrangle, RejectsBadConfig) {
  DispersionStrangleConfig ok;
  ok.names = {"AAA"};
  ok.missing.min_names = 1;
  ASSERT_TRUE(make_dispersion_strangle_spec(ok).has_value());
  auto expect_reject = [&](auto mutate) {
    DispersionStrangleConfig c = ok;
    mutate(c);
    auto r = make_dispersion_strangle_spec(c);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
  };
  expect_reject([](auto &c) { c.names.clear(); });
  expect_reject([](auto &c) { c.index_symbol = "AAA"; });
  expect_reject([](auto &c) { c.target_abs_delta = 1.0; });
  expect_reject([](auto &c) { c.tenor_days = 10.0; c.close_dte_days = 10.0; });
  expect_reject([](auto &c) { c.theta_per_name_daily = 0.0; });
  expect_reject([](auto &c) { c.entry_every_n_days = 0; });
  expect_reject([](auto &c) { c.missing.min_names = 5; });
}

TEST(DispersionStrangle, EntryMath_EqualTheta_VegaFlat_FortyDelta) {
  auto snap = load_fixture_snapshot();   // the 4-surface archive above
  DispersionStrangleConfig cfg;
  cfg.names = {"AAA", "BBB", "CCC"};
  cfg.index_symbol = "SPX";
  cfg.tenor_days = 90.0;
  cfg.theta_per_name_daily = 10.0;
  cfg.missing = {MissingNamePolicy::DropRenormalize, 2};
  auto spec = make_dispersion_strangle_spec(cfg);
  ASSERT_TRUE(spec.has_value());
  auto legs = resolve_spec_with_policy(*snap, *spec, nullptr);
  ASSERT_TRUE(legs.has_value());
  ASSERT_EQ(legs->size(), 8u);   // 4 symbols x {call, put}

  // 40-delta strike correctness: every resolved leg reprices to |delta| ~ 0.40
  // (mirror spy_strangle_backtest_test::FortyDeltaEntry: reprice via
  // surf->delta(K, T, side), tolerance 1e-3; call K above forward, put below).
  for (const auto &sl : *legs) {
    const PricedSurface *surf = snap->find(sl.leg.uid);
    ASSERT_NE(surf, nullptr);
    auto d = surf->delta(sl.leg.K, sl.leg.T, sl.leg.side);
    ASSERT_TRUE(d.has_value());
    EXPECT_NEAR(std::abs(*d), 0.40, 1e-3);
    const double F = surf->forward_at(sl.leg.T);
    if (sl.leg.side == Side::Call) EXPECT_GT(sl.leg.K, F); else EXPECT_LT(sl.leg.K, F);
  }

  // Equal theta: each name's |sum(qty*theta*mult)| == 10 $/day * 365.25, all
  // names equal within 1e-6 relative.
  const double want_theta = 10.0 * 365.25;
  std::map<std::uint32_t, double> theta_by_uid;
  double net_vega = 0.0, gross_vega = 0.0;
  for (const auto &sl : *legs) {
    if (sl.leg.group == "basket") theta_by_uid[sl.leg.uid] += sl.qty * sl.leg.theta * sl.multiplier;
    net_vega += sl.qty * sl.leg.vega * sl.multiplier;
    gross_vega += std::abs(sl.qty * sl.leg.vega * sl.multiplier);
  }
  ASSERT_EQ(theta_by_uid.size(), 3u);
  for (const auto &[uid, th] : theta_by_uid) {
    EXPECT_NEAR(std::abs(th), want_theta, 1e-6 * want_theta) << uid;
  }
  // Vega-flat at entry: net cohort vega ~ 0 (FlatVega scale is exact in fp).
  EXPECT_LE(std::abs(net_vega), 1e-9 * gross_vega);
  // Short index: negative qty on index legs.
  for (const auto &sl : *legs) {
    if (sl.leg.group == "index") EXPECT_LT(sl.qty, 0.0);
  }
}
```

(If `ResolvedLeg` lacks a `group` member for the theta grouping, group by uid using the fixture's known uids — the assertions above stand.)

- [ ] **Step 2: Build; verify failure** (missing header/symbols).
- [ ] **Step 3: Implement** `make_dispersion_strangle_spec` exactly per the doc-comment contract (pure assembly + validation, no pricing).
- [ ] **Step 4: Build + run.** `& .\scripts\atx-build.ps1 -Ctest -R "DispersionStrangle|Strategy|Dispersion"` — ALL PASS.
- [ ] **Step 5: Commit.**

```bash
git add -A
git commit -m "feat(atx-vol): dispersion-strangle strategy spec builder (equal-theta basket vs vega-flat index)"
```

---

### Task 4: `run_report` emitters — machine-readable run outputs

atx-vol emits data only; the Python renderer consumes these files. Library code with unit tests. The `# key=value` metadata-header convention comes from `spy_strangle_backtest.cpp:398-437`; the deterministic-column discipline from `tearsheet.hpp::write_backtest_tsv`.

**Files:**
- Create: `atx-vol/include/atx/vol/run_report.hpp`
- Create: `atx-vol/src/run_report.cpp`
- Create: `atx-vol/tests/run_report_test.cpp`
- Modify: `atx-vol/CMakeLists.txt` (library source list), `atx-vol/tests/CMakeLists.txt` (test source)

**Interfaces:**
- Consumes: `BacktestResult`, `TearSheet` + `tearsheet()` (tearsheet.hpp), `SnapshotCacheStats{loads,hits,prefetches}`, `SurfaceDb` (`root()`, `generation()`, `symbols()`, `partitions()`).
- Produces (all writers: `\n` line endings, `\t`-free CSV with `,` separators, doubles `%.17g` for series and `%.10g` for metric values, `IoError` on failure, deterministic output):

```cpp
// run_report.hpp
namespace atx::vol {

using MetaKv = std::vector<std::pair<std::string, std::string>>;

// File shape shared by every writer below:
//   # key=value          (one line per meta entry, in given order)
//   <header row>
//   <data rows>
//
// write_backtest_series_csv columns, exactly this order:
//   date,ts_ns,pnl_total,nav,pnl_delta,pnl_gamma,pnl_vega,pnl_vanna,pnl_volga,
//   pnl_theta,pnl_rho,pnl_charm,pnl_unexplained,pnl_settlement,pnl_shares,
//   financing,cost,cash,gross_delta,gross_gamma,gross_vega,gross_theta,
//   turnover_notional,turnover_vega,n_open_lots,n_unpriced_lots,
//   n_unpriced_greeks
// then one extra column per entry of r.signals, in order, named by the signal.
[[nodiscard]] Status write_backtest_series_csv(const BacktestResult &r,
                                               const MetaKv &meta,
                                               std::string_view path);

// Generic two-column metrics table: header "metric,value"; one row per entry.
[[nodiscard]] Status write_metrics_csv(const MetaKv &meta, const MetaKv &metrics,
                                       std::string_view path);

// TearSheet -> metrics rows (keys exactly): total_return, ann_return, ann_vol,
// sharpe, max_drawdown, hit_rate, avg_turnover, total_cost, total_financing,
// attr_delta, attr_gamma, attr_vega, attr_vanna, attr_volga, attr_theta,
// attr_rho, attr_charm, attr_unexplained, return_on_gross_vega,
// vega_adj_sharpe, pnl_per_vega_traded, avg_gross_vega, avg_gross_gamma.
[[nodiscard]] MetaKv strategy_metrics(const TearSheet &ts);

// BacktestResult -> summary rows (keys exactly): total_pnl (nav.back()),
// avg_daily_pnl (mean pnl_total over rows 1..n-1), avg_net_vega
// (mean gross_vega over rows with n_open_lots > 0), avg_net_theta (same over
// gross_theta), avg_open_lots, peak_open_lots, total_unpriced_lots,
// total_unpriced_greeks, n_steps.
[[nodiscard]] MetaKv result_summary_metrics(const BacktestResult &r);

// Engine performance -> metrics rows (keys exactly): wall_clock_ms,
// steps_per_s, n_steps, cache_loads, cache_hits, cache_prefetches.
struct EngineRunStats {
  double wall_clock_ms{0.0};
  std::uint64_t n_steps{0};
  SnapshotCacheStats cache{};
};
[[nodiscard]] MetaKv engine_metrics(const EngineRunStats &s);   // steps_per_s derived

// SurfaceDb inventory. Meta gets (in addition to caller meta, appended):
// db_root, generation, n_symbols, n_partitions, total_file_size.
// Header: "key,surface_count,file_size,created_ts_ns"; one row per partition,
// ascending key order.
[[nodiscard]] Status write_surface_db_stats_csv(const SurfaceDb &db,
                                                const MetaKv &meta,
                                                std::string_view path);

}  // namespace atx::vol
```

- [ ] **Step 1: Write the failing tests.** `atx-vol/tests/run_report_test.cpp`:
  - `RunReport.SeriesCsvRoundTrips`: build a tiny `BacktestResult` by hand (3 rows, one signal series, one double chosen to need full precision e.g. `0.1 + 0.2`), write, re-read the file as text; assert: every meta line starts `# ` and contains `=`; header EXACTLY the pinned column string + `,sig_name`; 3 data rows; the full-precision double round-trips via `std::stod` to bit-equal (`%.17g` discipline).
  - `RunReport.MetricsCsv`: `write_metrics_csv({{"a","b"}}, {{"sharpe","1.25"}}, path)`; assert file == `"# a=b\nmetric,value\nsharpe,1.25\n"`.
  - `RunReport.StrategyAndSummaryMetrics`: feed a hand-built `TearSheet`/`BacktestResult`, assert exact key set and spot-check values (`total_pnl == nav.back()`, `peak_open_lots` max, `avg_net_vega` skips zero-lot rows).
  - `RunReport.EngineMetrics`: `EngineRunStats{2000.0, 10, {5,4,3}}` → `steps_per_s == 5`, all six keys present.
  - `RunReport.DbStatsCsv`: create a temp `SurfaceDb`, write 2 partitions (reuse `make_essvi`-style fixture from surface_db_test.cpp or a minimal 1-surface archive), write stats; assert meta contains `generation=`, `n_partitions=2`, rows sorted by key.
- [ ] **Step 2: Build; verify failure.**
- [ ] **Step 3: Implement** in `src/run_report.cpp`. Single internal helper writes the meta+header+rows shape; all public writers go through it. No iostream formatting state leaks (`snprintf` into a buffer for doubles, like tearsheet.cpp).
- [ ] **Step 4: Build + run.** `& .\scripts\atx-build.ps1 -Ctest -R "RunReport"` and `-R "TearSheet"` — ALL PASS.
- [ ] **Step 5: Commit.**

```bash
git add -A
git commit -m "feat(atx-vol): run_report emitters - metadata-header CSV outputs for backtest runs"
```

---

### Task 5: `populate_surface_db` — fit boards into a SurfaceDb, honoring per-symbol configs

The "fit + store" pipeline stage: boards (from the OPRA hive) are fitted with each symbol's `SymbolFitConfig` from the db manifest applied via `apply_symbol_config`, and stored one partition per date. Records per-symbol fit outcomes (the surface-stats source for the report). Plus the thin CLI example that runs it for real data.

**Files:**
- Create: `atx-vol/include/atx/vol/surface_db_populate.hpp`
- Create: `atx-vol/src/surface_db_populate.cpp`
- Create: `atx-vol/tests/surface_db_populate_test.cpp`
- Create: `atx-vol/examples/mag7_surfdb_populate.cpp`
- Modify: `atx-vol/CMakeLists.txt` (library source; example registration in the `ATX_BUILD_EXAMPLES` block:
  `add_executable(mag7_surfdb_populate examples/mag7_surfdb_populate.cpp)` + `target_link_libraries(... PRIVATE atx::vol atx::core atx_warnings)` with a comment naming the gate test `SurfaceDbPopulate`)
- Modify: `atx-vol/tests/CMakeLists.txt` (test source)

**Interfaces:**
- Consumes: `SurfaceDb` (symbol_config/upsert_symbol/write_partition/partitions), `apply_symbol_config`, `symbol_config_from_preset`, `CorpusBoard` (corpus.hpp:61-76), the board→`PricedSurface` fitting path inside `src/corpus.cpp` (the per-board fit that `build_corpus` runs), `load_opra_daterange`/`corpus_board_from_opra` (opra_batch.hpp) in the example, Task 4's `MetaKv`/`write_metrics_csv` shape for the stats file.
- Produces:

```cpp
// surface_db_populate.hpp
namespace atx::vol {

struct SurfaceDbPopulateConfig {
  // Base fit inputs (preset etc). Per-symbol SymbolFitConfig from the db
  // manifest is overlaid via apply_symbol_config; a symbol absent from the
  // manifest uses `fallback` unchanged.
  SymbolFitConfig fallback{};
  unsigned n_threads{0};          // 0 = serial; determinism must hold regardless
  bool skip_existing{true};       // date key already in db.partitions() -> skip whole date
};

struct PopulateSymbolStats {
  std::string symbol;
  std::uint32_t n_attempted{0};
  std::uint32_t n_ok{0};
  std::uint32_t n_failed{0};
  std::uint32_t n_disabled{0};    // skipped because manifest enabled=false
  // Mean fit-quality score over successful fits, when the shared corpus fit
  // path yields one (oos_in_band from curve selection; see corpus.cpp's
  // CorpusEntry.oos_in_band recording). NaN when unavailable (e.g. the
  // pinned-curve path has no OOS score — mirrors corpus.cpp).
  double mean_oos_in_band{std::numeric_limits<double>::quiet_NaN()};
};

struct SurfaceDbPopulateStats {
  std::uint32_t n_boards{0};
  std::uint32_t n_ok{0};
  std::uint32_t n_failed{0};
  std::uint32_t n_dates_written{0};
  std::uint32_t n_dates_skipped_existing{0};
  std::vector<PopulateSymbolStats> per_symbol;   // sorted by symbol
};

// Fit every board and store one partition per distinct board date (key =
// date). Boards are grouped by date; within a date, boards fit in symbol
// order (deterministic). A board whose symbol's manifest config has
// enabled=false is skipped (n_disabled). A board whose fit fails records
// n_failed and does NOT abort the date (document per-name failures, don't
// silently drop). A date with zero successful fits writes NO partition.
// Partition write uses SurfaceArchiveItem{symbol, &surface} with owning
// symbol-string storage kept alive across the call.
// Top-level Err only on: empty boards span, db write errors, or a date key
// the db rejects.
[[nodiscard]] Result<SurfaceDbPopulateStats>
populate_surface_db(SurfaceDb &db, std::span<const CorpusBoard> boards,
                    const SurfaceDbPopulateConfig &cfg = {});

// Stats file for the report: meta (caller's, plus n_boards/n_ok/n_failed/
// n_dates_written appended), header
// "symbol,n_attempted,n_ok,n_failed,n_disabled,success_rate,mean_oos_in_band",
// one row per symbol (success_rate = n_ok / max(1, n_attempted - n_disabled),
// %.10g; mean_oos_in_band prints "nan" when NaN).
[[nodiscard]] Status write_populate_stats_csv(const SurfaceDbPopulateStats &s,
                                              const MetaKv &meta,
                                              std::string_view path);

}  // namespace atx::vol
```

**Implementation constraint:** the board→surface fit must REUSE `build_corpus`'s per-board pipeline (SessionInputs from board env/frame → preset/curve application → fitter → `PricedSurface`, including uid assignment). Read `src/corpus.cpp` first; if that logic is file-local, extract a shared internal function (e.g. into a `src/`-private header) that both `build_corpus` and `populate_surface_db` call — verbatim duplication of the fit block is a review-rejectable defect. The ONLY config difference: after building the board's `SessionInputs`, call `apply_symbol_config(cfg_for_symbol, inputs)` where `cfg_for_symbol` is `db.symbol_config(board.symbol)` if present else `cfg.fallback`.

**Example CLI** (`examples/mag7_surfdb_populate.cpp`, header comment naming gate test `SurfaceDbPopulate`):

```
mag7_surfdb_populate --opra-root DIR --db DIR --symbols A,B,... --start YYYY-MM-DD --end YYYY-MM-DD
                     [--r 0.043] [--preset fast] [--fit-workers N] [--stats FILE]
```

Flow: parse args → `SurfaceDb::open(db)` else `SurfaceDb::create(db)` → for each symbol absent from the manifest, upsert a config mirroring the SPY YTD corpus fit policy (`spy_ytd_corpus.cpp:94-101`): `auto c = symbol_config_from_preset(FitPreset::Fast); c.pin_curve = true; c.curve = CurveConfig{};` (default pinned ConvexDense) → `upsert_symbol(sym, c)` → `load_opra_daterange(OpraBatchSpec{symbols, start, end, root, default template, snapshot_suffix "T19:55:00Z", r})` → `corpus_board_from_opra` per loaded entry (skip `!entry.panel`, count missing) → `populate_surface_db` → `write_populate_stats_csv(stats, meta, --stats else <db>/populate_stats.csv)` → print summary. Honor `ATX_VOL_FIT_WORKERS` env for default `--fit-workers` if the corpus config does (check `corpus.hpp` — mirror whatever `spy_dispersion_backtest.cpp build-corpus` does with `fit_workers`).

- [ ] **Step 1: Write the failing tests.** `atx-vol/tests/surface_db_populate_test.cpp`. Fixture: build genuinely fittable boards with the `make_board_spec`/`fit_board` synthetic-panel pattern from `dispersion_test.cpp:76-120` (`SynthPanelSpec` → `make_synthetic_american_panel` → `OptionChain::from_frame` → boards). 2 symbols ("AAA","BBB") × 2 dates ("2026-03-02","2026-03-03").
  - `SurfaceDbPopulate.FitsAndStoresPartitionsPerDate`: fresh db; populate; assert `n_dates_written==2`, `db.partitions().size()==2`, `db.load_surface("2026-03-02","AAA")` serves, stats per_symbol has both symbols `n_ok==2`.
  - `SurfaceDbPopulate.HonorsDisabledSymbol`: upsert `BBB` with `enabled=false`; populate; partitions contain only AAA surfaces (`open_partition(...)->map_symbol("BBB")` NotFound), stats `n_disabled==2` for BBB.
  - `SurfaceDbPopulate.SkipExistingResumes`: populate once; populate again same boards; second stats `n_dates_skipped_existing==2`, `n_dates_written==0`, db generation advanced only by the first run's writes.
  - `SurfaceDbPopulate.FailedFitRecordedNotFatal`: corrupt one board (e.g. empty frame) → populate succeeds, that (symbol,date) counted in `n_failed`, the date's partition still written with the other symbol.
  - `SurfaceDbPopulate.StatsCsvShape`: write stats file; assert header exact and success_rate arithmetic.
  - `SurfaceDbPopulate.PinnedConfigHonored`: upsert `AAA` with `pin_curve=true` + a distinctive `CurveConfig` (e.g. ConvexDense node_cap 48 — copy the pinned-config pattern from `surface_db_test.cpp`'s `ConfigureStoreReloadServe`); populate; `load_surface(date,"AAA")` reports that curve kind (`kind_at(0)`), proving `apply_symbol_config` reached the fit.
- [ ] **Step 2: Build; verify failure.**
- [ ] **Step 3: Implement** library + example per contracts above.
- [ ] **Step 4: Build + run.** `& .\scripts\atx-build.ps1 -Ctest -R "SurfaceDbPopulate|SurfaceDb|Corpus"` — ALL PASS. Also build the example: `& .\scripts\atx-build.ps1 build mag7_surfdb_populate`.
- [ ] **Step 5: Commit.**

```bash
git add -A
git commit -m "feat(atx-vol): populate_surface_db - fit OPRA boards into SurfaceDb with per-symbol configs"
```

---

### Task 6: `examples/mag7_dispersion_backtest.cpp` + gate test

The acceptance example: open db → configure strategy → run → emit data files. Small (≤ ~300 lines) because Tasks 1-5 carry the machinery.

**Files:**
- Create: `atx-vol/examples/mag7_dispersion_backtest.cpp`
- Create: `atx-vol/tests/mag7_dispersion_backtest_test.cpp`
- Modify: `atx-vol/CMakeLists.txt` (examples block: `add_executable(mag7_dispersion_backtest examples/mag7_dispersion_backtest.cpp)`, link `PRIVATE atx::vol atx::core atx_warnings`, comment naming gate test `Mag7DispersionBacktest`)
- Modify: `atx-vol/tests/CMakeLists.txt` (test source)

**Interfaces:**
- Consumes: `Clock::from_surface_db` (T1), `make_dispersion_strangle_spec` + `DeclarativeStrategy` (T2/T3), `run_backtest`, `tearsheet`, all T4 emitters, `SnapshotCache`.
- Produces: the run-output file contract the Python renderer (T7) reads. Output dir layout (pinned):

```
<out>/series.csv             write_backtest_series_csv
<out>/strategy_metrics.csv   write_metrics_csv(meta, strategy_metrics(ts) + result_summary_metrics(r))
<out>/engine_metrics.csv     write_metrics_csv(meta, engine_metrics(stats))
<out>/db_stats.csv           write_surface_db_stats_csv
<out>/populate_stats.csv     copied from <db>/populate_stats.csv when present (byte copy)
```

Shared meta block written into EVERY file (keys exactly): `strategy=mag7_dispersion_strangle`, `names` (comma-joined), `index_symbol`, `data_source=surface_db`, `db_root`, `db_generation`, `window_start`, `window_end` (first/last clock date), `n_steps`, `delta_target`, `tenor_days`, `close_dte_days`, `theta_per_name_daily`, `entry_every_n_days`, `multiplier=100`, `frictions` (`on`/`off`), `missing_policy` (`error`/`drop_renormalize`), `min_names`.

**CLI:**

```
mag7_dispersion_backtest --db DIR [--out DIR] [--names AAPL,MSFT,GOOGL,AMZN,NVDA,META,TSLA]
                         [--index SPY] [--theta-per-name 10.0] [--delta 0.40]
                         [--tenor-days 90] [--close-dte 10] [--min-names 4]
                         [--frictions] [--threads N]
```

Defaults are the Global Constraints' pinned strategy defaults. `--out` default `<tmp>/atx-mag7-dispersion/`. `--frictions` toggles a simple nonzero `FrictionModel` (copy the existing example's B2 defaults if `spy_strangle_tradeable` has one; otherwise spread_frac-style single knob — keep trivial). Flow: open db → `Clock::from_surface_db` → `make_dispersion_strangle_spec` → `DeclarativeStrategy` → `RunConfig` with a `SnapshotCache` (`std::make_shared`) and `UnpricedLotPolicy::ExcludeAndReport` → time `run_backtest` with `std::chrono::steady_clock` → `tearsheet(r)` → emit the five files → print a short console summary. Exit codes: 2 bad args, 1 runtime error.

- [ ] **Step 1: Write the failing gate test.** `atx-vol/tests/mag7_dispersion_backtest_test.cpp`, suite `Mag7DispersionBacktest`. Fixture: synthetic `SurfaceDb` with 8 symbols (7 fake MAG7 + "SPY"), 12 daily partitions, per-symbol vol bumps/spots, `make_surface` pattern (as Task 1's test); TEST-scale config: `tenor_days=6`, `close_dte_days=2.5`, `theta_per_name_daily=10`, `min_names=4`.
  - `EndToEnd_DbToEmittedFiles`: run the full library pipeline the example composes (db → clock → spec → strategy → `run_backtest` → all emitters into a temp out dir); assert every output file exists, meta lines parse, `series.csv` row count == 12.
  - `FortyDeltaOnDbSurfaces`: resolve the spec against the FIRST db snapshot (`MarketSnapshot::load` of `clock.refs()[0]`); every leg reprices to |Δ|≈0.40 within 1e-3 (mirror T3's assertion, now through db-loaded surfaces).
  - `CohortMechanics`: `n_open_lots` ramps by 16/day (8 symbols × 2 legs). Same residual arithmetic as T2's `CloseAtHorizonOverlappingCohorts`: with tenor 6d and close below 2.5d, a cohort lives ages 0..3, so 4 live cohorts at steady state → plateau 64 lots. Expected series over 12 steps: `{16, 32, 48, 64, 64, 64, 64, 64, 64, 64, 64, 64}`. Assert the exact vector, and `pnl_settlement` all zero.
  - `VegaFlatAtEntry`: at each step's entry, net book vega change from the new cohort ≈ 0 → assert `|gross_vega|` (signed net, see Global Constraints note) stays ≤ 1e-6 × cumulative absolute leg vega proxy; simplest robust check: resolve the spec directly on each snapshot and assert per-entry net vega ≤ 1e-9 × gross (as T3) for all 12 dates.
  - `DeterminismAcrossThreads`: run twice with `RunConfig.price.n_threads` 1 vs 4 → bit-identical result (copy `expect_result_bit_identical` from `spy_strangle_backtest_test.cpp:297`'s pattern).
- [ ] **Step 2: Build; verify failure** (helpers/example glue not yet present — the test drives library code only, so failures should be assertion-level once T1-T5 exist; if all pass immediately, the test must be strengthened until it exercises something new — at minimum the emit-files integration).
- [ ] **Step 3: Implement the example** (thin CLI over the tested pipeline). Line count target ≤ ~300; if it exceeds, move reusable glue INTO the library, not the reverse.
- [ ] **Step 4: Build + run.** `& .\scripts\atx-build.ps1 build atx-vol-tests mag7_dispersion_backtest` then `-Ctest -R "Mag7DispersionBacktest|DispersionStrangle|SurfaceDbBacktest"` — ALL PASS.
- [ ] **Step 5: Commit.**

```bash
git add -A
git commit -m "feat(atx-vol): mag7_dispersion_backtest example + gate test"
```

---

### Task 7: `tools/mag7_dispersion_report.py` — HTML/SVG renderer

Python renders; C++ emitted. One self-contained HTML: inline SVG chart(s), inline CSS, no JS, no external assets. Follows `tools/tearsheet.py` precedent (pandas + matplotlib Agg; meta parser `read_run` at tearsheet.py:39-50).

**Files:**
- Create: `atx-vol/tools/mag7_dispersion_report.py`
- Create: `atx-vol/tests/mag7_dispersion_report_test.py`
- Modify: `atx-vol/tests/CMakeLists.txt` (register the python test with `add_test` + `LABELS atx_vol`, mirroring the existing Python3-guarded block at ~lines 117-134)

**Interfaces:**
- Consumes: the T6 output-dir contract (five CSV files, `# key=value` meta + pinned columns).
- Produces: `mag7_dispersion_report.html` (default: `<run-dir>/mag7_dispersion_report.html`).

**CLI:** `python mag7_dispersion_report.py <run-dir> [out.html]`

**Structure (pinned):**
- `read_meta_csv(path) -> (meta: dict, df: DataFrame)` — copy tearsheet.py's parse (`# k=v` lines, then `pd.read_csv(comment="#")`).
- SVG: matplotlib figure(s) saved to an `io.StringIO` with `format="svg"`, strip the XML declaration/DOCTYPE, embed the `<svg>...</svg>` inline. Chart 1 (required): YTD cumulative P&L / NAV equity curve with a drawdown panel beneath (shared x, `fill_between` shading). Chart 2 (welcome, cheap): cumulative attribution lines (theta/vega/gamma/unexplained cumsums) — include it.
- Tables (HTML `<table>`, built by a small helper, values formatted sensibly):
  1. **Strategy metrics** from `strategy_metrics.csv` (all rows) — includes total P&L, sharpe, max drawdown, hit rate, avg daily P&L, turnover, avg/peak open lots, attribution totals, avg net vega/theta after entry.
  2. **Backtest engine metrics** from `engine_metrics.csv` (wall clock, steps/sec, cache stats) + unpriced counts (from strategy_metrics rows `total_unpriced_*`).
  3. **Surface/db statistics**: from `db_stats.csv` meta+rows (dates covered = first/last partition key + count, partition sizes, generation) and, when `populate_stats.csv` exists, the per-symbol fit success table.
- Header block: strategy name, universe, window, and ALL pinned defaults from the shared meta (delta, tenor, theta budget, multiplier, frictions, missing policy) — "document them in the report header" is an acceptance requirement.
- Self-containment: single `<html>` string with one inline `<style>` block; assert-no-`http`/`src=` discipline.
- Dependencies: `pandas`, `matplotlib` (Agg), stdlib only. No new deps.

- [ ] **Step 1: Write the failing test.** `atx-vol/tests/mag7_dispersion_report_test.py` (pytest style consistent with the existing `tools/*_test.py` files — read one to copy conventions): a fixture function writes a minimal synthetic run dir (5 files, 3-row series, tiny metrics) into `tmp_path`; run the script via `subprocess` (or import + call `main`) → assert exit 0, output HTML exists, contains `<svg`, contains the three section headings (`Strategy metrics`, `Engine metrics`, `Surface/db statistics` — pin exact heading strings in the script), contains a pinned-default string (e.g. `theta_per_name_daily`), and contains NO `http://`/`https://`/`src=` substrings (self-containment).
- [ ] **Step 2: Run the test; verify it fails** (script missing): `& .\scripts\atx-build.ps1 -Ctest -R "Mag7DispersionReport"` (after CMake registration) or `python -m pytest atx-vol/tests/mag7_dispersion_report_test.py` directly.
- [ ] **Step 3: Implement the script.**
- [ ] **Step 4: Re-run; PASS.** Also re-run `-R "TearSheet"` C++ suite untouched-green (no C++ changes expected in this task; the check is cheap).
- [ ] **Step 5: Commit.**

```bash
git add -A
git commit -m "feat(atx-vol): mag7_dispersion_report.py - self-contained HTML/SVG report renderer"
```

---

### Task 8: Real-data run — pull, populate, backtest, report (OPERATOR-GATED, controller-led)

Operational task, run by the controller directly (no fresh implementer): paid data pulls need the cost gate and operator escalation path. Every guardrail in Global Constraints applies.

**Prereqs:** Tasks 1-7 merged into the branch and green. `ATX_BUILD_EXAMPLES=ON` configured (the worktree preset builds examples; verify `mag7_surfdb_populate`/`mag7_dispersion_backtest`/`databento_bulk_opra` targets exist, else re-configure with `-DATX_BUILD_EXAMPLES=ON`).

**Data layout decision (pinned):** raw hive at `C:/atx/data/mag7_ytd/opra/{SYMBOL}/{YYYY-MM-DD}.parquet`; SurfaceDb at `C:/atx/data/surfdb/mag7_ytd/`. Use ABSOLUTE paths into the MAIN checkout's `data/` dir (untracked, operator-owned; do not duplicate pulls per worktree).

- [ ] **Step 1: Seed SPY from the existing hive (zero cost).** `robocopy C:\atx\data\spy_ytd\opra\SPY C:\atx\data\mag7_ytd\opra\SPY *.parquet` (123 files, 2026-01-02..2026-07-01). The bulk puller skips existing files, so SPY only pulls the tail.
- [ ] **Step 2: Dry-run cost estimate (free), log it.** Window: `--start 2026-01-02 --end <last complete trading day>`. `databento_bulk_opra --symbols AAPL,MSFT,GOOGL,AMZN,NVDA,META,TSLA,SPY --start 2026-01-02 --end YYYY-MM-DD --out C:/atx/data/mag7_ytd/opra --dry-run` (key from `DATABENTO_API_KEY` env; never echo it). Record the printed total estimate in the ledger.
- [ ] **Step 3: GATE.** If estimate > $150: STOP, ask the operator with the number and cheaper alternatives (shorter window, fewer names). Else proceed.
- [ ] **Step 4: Pull.** Same command without `--dry-run`, with `--cap <ceil(estimate * 1.25)>`. Idempotent; on interruption re-run (existing files skip).
- [ ] **Step 5: Populate.** `mag7_surfdb_populate --opra-root C:/atx/data/mag7_ytd/opra --db C:/atx/data/surfdb/mag7_ytd --symbols AAPL,MSFT,GOOGL,AMZN,NVDA,META,TSLA,SPY --start 2026-01-02 --end YYYY-MM-DD`. Review `populate_stats.csv`: a name with success rate < ~80% is an escalation per the goal doc (document, ask operator) — do not silently proceed with a hollow universe.
- [ ] **Step 6: Backtest.** `mag7_dispersion_backtest --db C:/atx/data/surfdb/mag7_ytd --out C:/atx/data/mag7_ytd/run` (defaults = pinned strategy). Sanity-check console tearsheet (finite numbers, n_steps == trading days, unpriced counts ~0).
- [ ] **Step 7: Report.** `python atx-vol/tools/mag7_dispersion_report.py C:/atx/data/mag7_ytd/run C:/atx/atx-vol/tools/mag7_dispersion_report.html`. Artifact stays untracked in `atx-vol/tools/` (the `spy_strangle_real_tearsheet.png` precedent). Open/inspect: SVG present, three tables populated, defaults documented in header.
- [ ] **Step 8: Ledger.** Record: estimate, actual spend, window covered, per-symbol fit rates, headline strategy metrics.

---

## Final Verification (controller, after all tasks)

- [ ] Targeted suites all green: `& .\scripts\atx-build.ps1 -Ctest -R "SurfaceDb|SurfaceArchive|Backtest|Dispersion|Strategy|RunReport|Mag7"` — zero failures. (Full `-L atx_vol` gate deferred to operator.)
- [ ] Dispatch the final whole-branch code review (superpowers:requesting-code-review) with the merge-base review package.
- [ ] superpowers:finishing-a-development-branch.
