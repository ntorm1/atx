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

