### Task T30: `aim-partial-v5` target rule (C++)

**Lane:** A · **Pool:** pool-3 (`git checkout -B feat/mega-alpha-v5-construction-20260927 <pool-2 HEAD>`) · **Model:** Opus 5.5 · **Depends on:** T29 · **Tokens:** NAVRULE, NAVSIM · **Peak:** 0.3 GiB

**Files:**
- Modify: `atx-impl/src/strategy_target_replay.hpp:12-36` (enum, config fields)
- Modify: `atx-impl/src/strategy_target_replay.cpp:155-195` (update_weights), `:55-99` (validate_config), `:372-374` (rule_name), recipe writer near `:529-540`
- Modify: `atx-impl/src/strategy_target_replay_detail.hpp` (signature)
- Modify: `atx-impl/src/strategy_nav_replay.cpp:1745-1765` (CLI), `:603-634` (plan_decision call), recipe/summary writers
- Test: `atx-impl/tests/strategy_target_replay_test.cpp` (append), `atx-impl/tests/strategy_nav_replay_test.cpp` (append)

**Interfaces:**
- Consumes: `desired_target` / `form_desired` unchanged; `ConstructionDay.banded_names` reused for dust-banded count.
- Produces: `TargetReplayRule::AimPartialV5 = 3`; config fields `aim_leverage`, `dust_multiple`; `update_weights(..., std::span<const f64> per_name_rate = {})`;
  CLI `--rule aim-partial-v5 --trade-fraction <theta> --dust-multiple <d> --aim-leverage <L>`; recipe `rule = "aim-partial-v5"`, keys `theta`, `dust_multiple`, `aim_leverage`, `rate = "fixed"`;
  summary `construction.v5 = {theta, dust_multiple, aim_leverage, rate, mean_gross, mean_net, mean_held_share}`. T36 fills `per_name_rate`.

- [ ] **Step 1:** Header changes (`strategy_target_replay.hpp`):

```cpp
enum class TargetReplayRule : atx::u8 { BaselineTargetV1 = 1, MonthlyTargetBudgetV2 = 2, AimPartialV5 = 3 };
// ... inside TargetReplayConfig, after band_multiple:
  // aim-partial-v5 (pre-registered R5'): on every rebalance decision each member moves
  //   next_i = current_i + theta_i * (aim_leverage * desired_i - current_i)
  // unless |aim_leverage * desired_i - current_i| <= dust_multiple / N_d (dust band; 0 = off).
  // theta_i = trade_fraction, or the per-name rate span when one is supplied (T36).
  // Under this rule band_multiple must be 0 and monthly_budget is ignored. Non-members
  // are still forced to 0. The other rules are byte-for-byte unchanged.
  atx::f64 aim_leverage{1.0}, dust_multiple{};
```

- [ ] **Step 2:** `validate_config` (`strategy_target_replay.cpp:55-99`): add

```cpp
  if (cfg.rule == TargetReplayRule::AimPartialV5) {
    if (!(cfg.trade_fraction > 0 && cfg.trade_fraction <= 1)) return invalid("aim-partial-v5 needs trade_fraction in (0,1]");
    if (cfg.band_multiple != 0) return invalid("aim-partial-v5 refuses band_multiple (use dust_multiple)");
    if (!(cfg.dust_multiple >= 0 && cfg.dust_multiple <= 0.5)) return invalid("dust_multiple must be in [0, 0.5]");
    if (!(cfg.aim_leverage >= 1.0 && cfg.aim_leverage <= 2.0)) return invalid("aim_leverage must be in [1, 2]");
  } else if (cfg.aim_leverage != 1.0 || cfg.dust_multiple != 0) return invalid("aim_leverage/dust_multiple are aim-partial-v5 only");
```
(Use the file's existing error-return idiom in place of `invalid(...)`.)

- [ ] **Step 3:** `update_weights`: add the trailing parameter `std::span<const f64> per_name_rate = {}` in `strategy_target_replay_detail.hpp` and the definition, and branch at the top of the existing function:

```cpp
  if (cfg.rule == TargetReplayRule::AimPartialV5) {
    const f64 dust = rebalance && cfg.dust_multiple > 0 && members_at(in, d) > 0
                     ? cfg.dust_multiple / static_cast<f64>(members_at(in, d)) : -1;
    out.applied_fraction = rebalance ? cfg.trade_fraction : 0;
    f64 squared = 0;
    for (usize i = 0; i < in.instruments; ++i) {
      const bool live = in.member[offset + i] != 0;
      f64 next = 0;
      if (live) {
        const f64 aim = cfg.aim_leverage * desired[i];
        const f64 gap = aim - current[i];
        const bool dusted = rebalance && std::abs(gap) <= dust;
        if (dusted) ++out.construction.banded_names;
        const f64 theta = !rebalance ? 0 : per_name_rate.empty() ? cfg.trade_fraction : per_name_rate[i];
        next = dusted ? current[i] : current[i] + theta * gap;
      }
      const f64 trade = std::abs(next - current[i]);
      out.turnover += trade;
      if (!live) out.forced_turnover += trade; else out.discretionary_turnover += trade;
      current[i] = next;
      out.gross += std::abs(next); out.net += next;
      out.long_weight += std::max(0.0, next); out.short_weight += std::max(0.0, -next);
      out.max_abs_weight = std::max(out.max_abs_weight, std::abs(next));
      out.held_names += next != 0 ? 1U : 0U; squared += next * next;
    }
    out.effective_names = squared > 0 ? out.gross * out.gross / squared : 0;
    return;
  }
```
`members_at` is a tiny helper counting `in.member[offset+i]` (the existing loop at `:163-165` does the same; factor it).

- [ ] **Step 4:** `rule_name` → `"aim-partial-v5"`; recipe writer adds `{"theta", cfg.trade_fraction}, {"dust_multiple", …}, {"aim_leverage", …}, {"rate", "fixed"}` **only when rule == AimPartialV5** (keeps other recipes byte-identical). NAV CLI: `--rule aim-partial-v5`, `--dust-multiple`, `--aim-leverage`. NAV summary: `construction.v5` block (v5 only) with `mean_gross`, `mean_net`, `mean_held_share` = mean over decisions of held/members.
- [ ] **Step 5:** Fixtures (append to `strategy_target_replay_test.cpp`, reuse the file's `Fixture f` and `replay_targets` helpers as at `:495-530`):

```cpp
TEST(TargetReplayV5, AimPartialV5_ThetaOne_MatchesBaseline) {
  Fixture f; TargetReplayConfig base; base.rule = TargetReplayRule::BaselineTargetV1; base.cadence = 1; base.trade_fraction = 1;
  TargetReplayConfig v5 = base; v5.rule = TargetReplayRule::AimPartialV5; v5.dust_multiple = 0; v5.aim_leverage = 1;
  const auto a = replay_targets(f.input(), base), b = replay_targets(f.input(), v5);
  ASSERT_TRUE(a && b);
  for (usize d = 0; d < a->days.size(); ++d) {
    EXPECT_DOUBLE_EQ(a->days[d].turnover, b->days[d].turnover);
    EXPECT_DOUBLE_EQ(a->days[d].gross, b->days[d].gross);
    EXPECT_EQ(a->days[d].held_names, b->days[d].held_names);
  }
}
TEST(TargetReplayV5, AimPartialV5_DustDoesNotBlockEntry) {
  Fixture f; TargetReplayConfig v5; v5.rule = TargetReplayRule::AimPartialV5; v5.cadence = 1; v5.trade_fraction = 0.05; v5.dust_multiple = 0.1;
  const auto r = replay_targets(f.input(), v5); ASSERT_TRUE(r);
  // first rebalance: every member with |desired| > 0.1/N must be held (rank targets are spread over [-2/N, 2/N])
  EXPECT_GE(r->days[0].held_names, f.members_at(0) * 9 / 10);
  EXPECT_NEAR(r->days[0].gross, 0.05, 1e-12);  // theta * gross-1 aim on the first step
}
TEST(TargetReplayV5, AimPartialV5_ConvergesToAimGross) {
  Fixture f; TargetReplayConfig v5; v5.rule = TargetReplayRule::AimPartialV5; v5.cadence = 1; v5.trade_fraction = 0.25; v5.dust_multiple = 0;
  const auto r = replay_targets(f.constant_signal_input(), v5); ASSERT_TRUE(r);
  EXPECT_NEAR(r->days.back().gross, 1.0, 1e-6);   // constant aim: (1-(1-theta)^T) -> 1
}
TEST(TargetReplayV5, RefusesBandUnderV5) {
  TargetReplayConfig bad; bad.rule = TargetReplayRule::AimPartialV5; bad.band_multiple = 1; EXPECT_FALSE(validate_config(bad));
}
```
`f.members_at(d)` and `f.constant_signal_input()` (a role whose signal never changes, so the aim is constant) are helpers to add to the existing `Fixture` in this test file if absent; keep them test-local.
Add a NAV fixture `NavV5_RecipeAndSummaryKeys` asserting the recipe has `theta/dust_multiple/aim_leverage/rate` under v5 and **does not** have them under baseline, and that a baseline run's `recipe.json` bytes equal the pre-change fixture bytes (store the expected SHA-256 in the test).

- [ ] **Step 6:** Report `task-T30-report.md`: root targets `atx-impl-strategy-target-tests`, `atx-equity-strategy-targets`; test filter `TargetReplayV5.*:NavV5*`; CMake diff (none expected: no new TUs).
- [ ] **Step 7:** Commit: `feat(nav): aim-partial-v5 construction rule with dust band and aim leverage (T30)`.

**Acceptance (root, in T37/T38):** fixtures pass; re-run of `mega-nav-v4-train-b2-f.25` recipe (baseline band 2) yields identical `recipe.json` and daily CSV SHA-256; reference v5 cell mean gross ∈ [0.90, 1.05] on TRAIN.

---

