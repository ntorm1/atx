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

