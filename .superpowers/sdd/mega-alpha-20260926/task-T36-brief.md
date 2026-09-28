### Task T36: per-name trading rate `rate per-name-v1` (C++)

**Lane:** A (same pool-3, after T30) · **Model:** Opus 5.5 · **Depends on:** T30 · **Tokens:** NAVRULE, NAVSIM · **Peak:** 0.3 GiB

**Files:**
- Modify: `atx-impl/src/strategy_nav_replay.hpp` (config: `enum class NavRateRule : u8 { Fixed = 0, PerNameV1 = 1 }; f64 rate_rra{10.0}, rate_min{0.01}, rate_max{0.15}, rate_lambda{0.2};`)
- Modify: `atx-impl/src/strategy_nav_replay.cpp:291-313` (`liquidity_row` reuse), `:603-634` (`plan_decision`: build the rate span), CLI, recipe, summary
- Test: `atx-impl/tests/strategy_nav_replay_test.cpp`

**Interfaces:**
- Consumes: `update_weights(..., per_name_rate)` from T30; `liquidity_row(i, t)` → `{adv_dollars, daily_vol, half_spread_bps}` (existing).
- Produces: CLI `--rate per-name-v1 --rate-rra 10 --rate-min .01 --rate-max .15`; recipe `rate = "per-name-v1"` + params; summary `construction.v5.rate_stats = {mean, p05, p50, p95, share_at_min, share_at_max}`.

- [ ] **Step 1:** In `plan_decision`, when `cfg.rate == PerNameV1` and rule is v5, fill `std::vector<f64> rate(n, cfg.rate_min)` for members:

```cpp
// theta_i = clip( sqrt( RRA * sigma_i^2 * ADV_i / (lambda * NAV_decision) ), rate_min, rate_max )   [JKMP: Lambda_i = lambda / ADV_i, lambda = 0.2]
for (usize i = 0; i < n; ++i) {
  if (!member[i]) continue;
  const auto L = liquidity_row(i, d);           // window [d-63, d); ADV in raw dollars; sigma daily; fallback flagged
  if (!(L.adv_dollars > 0) || !(L.daily_vol > 0) || L.fallback) { rate[i] = cfg.rate_min; ++stats.at_min; continue; }
  const f64 theta = std::sqrt(cfg.rate_rra * L.daily_vol * L.daily_vol * L.adv_dollars / (cfg.rate_lambda * nav_decision));
  rate[i] = std::clamp(theta, cfg.rate_min, cfg.rate_max);
}
```
Compute liquidity rows once per decision for all members (today they are computed only for names with active orders; under v5 every member has one anyway).

- [ ] **Step 2:** Expose the rate formula as a free function so it can be tested without a replay:
  `f64 per_name_rate_v1(f64 rra, f64 lambda, f64 nav, f64 daily_vol, f64 adv_dollars, f64 rate_min, f64 rate_max)` in `strategy_nav_replay.hpp`.
  Fixtures (append to `strategy_nav_replay_test.cpp`; reuse the file's synthetic role builder used by the existing K=5 write-off fixture):

```cpp
TEST(NavV5, PerNameRate_Formula) {
  EXPECT_NEAR(per_name_rate_v1(10, 0.2, 1e9, 0.015, 5e7, 0.01, 0.15), 0.0237, 2e-4);   // sigma 1.5%/day, ADV $50m
  EXPECT_NEAR(per_name_rate_v1(10, 0.2, 1e9, 0.015, 1e9, 0.01, 0.15), 0.106, 1e-3);    // ADV $1bn
  EXPECT_DOUBLE_EQ(per_name_rate_v1(10, 0.2, 1e9, 0.015, 1e6, 0.01, 0.15), 0.01);       // $1m ADV clips at rate_min
  EXPECT_DOUBLE_EQ(per_name_rate_v1(10, 0.2, 1e9, 0.05, 5e10, 0.01, 0.15), 0.15);       // clips at rate_max
}
TEST(NavV5, PerNameRate_NoLiquidity_UsesMin) {
  // synthetic role: name A has volume 0 for the whole window, name B is absent for the 63 sessions before d0; both are members at d0.
  auto in = SyntheticRole::three_names_one_zero_volume_one_absent_window();
  NavReplayConfig cfg = v5_config(/*theta*/0.05, /*dust*/0.1); cfg.rate = NavRateRule::PerNameV1;
  const auto r = replay_nav(in.view(), cfg); ASSERT_TRUE(r);
  EXPECT_DOUBLE_EQ(r->construction.rate_stats.min, cfg.rate_min);
  EXPECT_EQ(r->construction.rate_stats.share_at_min_count, 2U);           // A and B
  for (const auto& day : r->days) EXPECT_TRUE(std::isfinite(day.gross_leverage));
}
TEST(NavV5, PerNameRate_FixedEqualsTradeFraction) {
  auto in = SyntheticRole::default_role();
  NavReplayConfig a = v5_config(0.05, 0.1); a.rate = NavRateRule::Fixed;
  NavReplayConfig b = a; b.rate = NavRateRule::PerNameV1; b.rate_min = b.rate_max = 0.05;   // per-name clipped to a constant == fixed
  const auto ra = replay_nav(in.view(), a), rb = replay_nav(in.view(), b); ASSERT_TRUE(ra && rb);
  for (usize d = 0; d < ra->days.size(); ++d) EXPECT_DOUBLE_EQ(ra->days[d].net_return, rb->days[d].net_return);
}
```
`SyntheticRole::*` and `v5_config` are small test-local helpers the implementer adds next to the existing NAV fixture builders (the file already constructs synthetic saved blends for the K=5 write-off and budget-refusal fixtures; extend those builders rather than writing new loaders).

- [ ] **Step 3:** Report and commit: `feat(nav): per-name trading rate per-name-v1 for aim-partial-v5 (T36)`.

**Acceptance (root, T38):** the two per-name cells run < 60 s / < 400 MiB; `rate_stats.p50` ∈ [0.02, 0.06] on TRAIN; `share_at_min` < 0.25.

---

