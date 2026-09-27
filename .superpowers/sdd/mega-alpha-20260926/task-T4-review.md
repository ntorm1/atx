# Task T4 review: construction options (price-risk-v1, no-trade band), lockstep NAV, daily GMV turnover

Reviewed `review-T4.diff` (root commit ef089af8, 7 files, +1260/-125), read in one pass: production code first, then tests. No hunk was cut off. Line numbers refer to the current source in `C:/atx-wt/pool-2`:
- `tr.cpp` = `atx-impl/src/strategy_target_replay.cpp`
- `nav.cpp` = `atx-impl/src/strategy_nav_replay.cpp`
- `ttest` / `ntest` = the two test TUs

Root evidence was not re-run: build 19.1 s, `atx-impl-strategy-target-tests` 32/32.

## Spec Compliance

- ✅ **Spec compliant.** Every requirement has a matching hunk:
  - **Req 1, neutralize.**
    - `TargetNeutralize{None, PriceRiskV1}` plus the config fields (`strategy_target_replay.hpp:1689-1711`), validated at `tr.cpp:67-83`.
    - `form_desired` (`tr.cpp:202-229`) calls `desired_target` and then T3 `neutralize_price_risk` once per decision.
    - Guard: amplification = `stats.gross/stats.residual_gross` > 5; excluded share = `excluded_gross/gross` > 0.5; or a T3 `Unavailable`. Any of these returns `false`, which makes it a non-rebalance: members are kept and forced exits still run in `update_weights`.
    - Skips are counted with a reason histogram, and used-names min/median are recorded (`tr.cpp:551-598`).
    - `--role` is required in targets mode (`tr.cpp:719-721`, test `ttest:536`).
  - **Req 1, band.**
    - `band = band_multiple / N_d` on effective rebalance decisions only (`tr.cpp:161-167`).
    - A banded member keeps `current` and is left out of the v2 `distance` (`tr.cpp:171-174`, `:182-184`).
    - `banded_names` is recorded per decision.
  - **Req 2, CLI.** `--neutralize` and `--band-multiple` are in both dispatchers (`tr.cpp` dispatch, `nav.cpp:1205-1213`). They are recorded in `recipe.json` and in the summary `construction` block. The target recipe hash is unchanged at defaults (`construction_on` gate, `tr.cpp` `recipe()`).
  - **Req 3, rule id.** `<rule>[+neutral-price-risk-v1][+band-<X>]` in both recipes, both summaries and `construction.rule_id`.
  - **Req 4, NAV uses the single extension point.** `form_desired_target` (`nav.cpp:414`) forwards to `detail::form_desired`. The band lives in the shared `update_weights`.
  - **Req 5, cadence 1.** Covered by `ntest:873`: the decision runs every session and the band acts every session.
  - **Req 6, GMV turnover.**
    - `pretrade_gross_dollars` = Σ|held| after MARK and before EXECUTE (`nav.cpp:515`).
    - `one_way_turnover_gmv = traded/pretrade gross` on execution rows, NaN at zero gross.
    - The column is appended after the 48 T2 columns.
    - Mean/median/p95/max are taken over executed sessions with positive gross, excluding `deployment_index` (`nav.cpp:1009-1020`, `:1040-1070`).
    - Ceilings come from `--daily-turnover-mean-max`/`--daily-turnover-p95-max`, default .20/.30, and are recorded in the recipe. The flags use `<=`, and NaN never meets.
    - The monthly fields are unchanged and labelled legacy in `limitations` (`nav.cpp:645`) and in `monthly_turnover_target_status`.
  - **Fixtures (a)–(f).** All present: `ntest:820`, `ttest:370`, `ttest:452`, `ttest:498`, `ntest:873`, `ntest:956`. I re-derived the hand values:
    - (d): band .15; turnovers .75 and .625; v2 fraction 2/3.
    - (f): {0,.5,0,.5,2} gives mean .6, median .5, p95 = .5+.8·1.5 = 1.7. The forced-exit case {0,.5,0} gives mean 1/6 and p95 .45.
  - **The brief lists `tools/equity_strategy_targets.cpp` but the diff does not touch it.** It needs no change: `main` only routes argv to `dispatch_nav_replay`/`dispatch_target_replay` (`equity_strategy_targets.cpp:9-11`), and all flag parsing lives in those functions.
- **Root additions**
  - **Executed fills only.** `traded_dollars` is Σ|filled| from `execute_orders` (`nav.cpp:393,400`). Forced exits count; planned turnover and write-offs are not in the numerator.
  - **Amplification/excluded-share caps.** Present, with skips counted.
  - **Exposures once per decision (T3 M7).** One `neutralize_price_risk` call per decision (`tr.cpp:210`). In the NAV that one call is shared by all lockstep books (`nav.cpp:525-533`).
  - **Monthly 30% fields labelled legacy.** Yes.
  - **Band 0 + no neutralize reproduces the old replay exactly.** `band = -1` makes `gap <= band` impossible. So `distance += gap` and `next = … rebalance && !banded ? …` reduce to the pre-T4 expressions token for token (`tr.cpp:171-184`). `form_desired` with `None` is `desired_target` returning `true`.
- **Global constraints**
  - **tau definition.** Matches: pre-trade gross is taken after the mark. Written-off names are already zeroed (`carry_absent`, `nav.cpp:308`); stale names are kept at their stale mark.
  - **Causality.**
    - T3 windows end at d (T3 review §1 #3).
    - `ntest:873` asserts that every NAV decision's construction record equals the target replay's at the same t. `ttest:370` asserts that `form_desired` at d=60 equals `compute_price_exposures(d=60)` + `neutralize_target` bit-for-bit.
    - Together these pin the decision index handed to T3 on both paths.
  - **Lockstep.**
    - Books share only `Construction{row, desired, price}`. `desired` is written once, before any book plans, and read through `const` (`nav.cpp:423`, `update_weights` takes `const std::vector<f64>&`).
    - Every book runs its own MARK→EXECUTE→DECIDE→close.
    - `expect_same_result` (`ntest` helper) compares days, construction, GMV fields, events, deployment and participation bitwise, for defaults × 2 rules and for neutralize+band.
  - **Accounting identities.** `mark_session`/`execute_orders`/`close_day` are unchanged.
- ⚠️ **Cannot verify from the diff (for the controller)**
  1. **Validation-role warmup.** price-risk-v1 needs 126 beta pairs, so each role must carry at least about 127 sessions of price history before `score_begin`. Otherwise the first scored decisions skip as too-few-names and deployment is delayed. The report says TRAIN has a 399-session warmup; check the 2023-2024 role manifest.
  2. **Default-run artifact hashes change.**
     - The NAV `recipe.json` gains the required ceilings plus `daily_turnover_definition` and `monthly_turnover_target_status`.
     - The NAV daily CSVs gain the two GMV columns.
     - Compare the TRAIN smoke reference on `cut -d, -f1-48` and on the pre-existing summary keys, not on SHAs.
  3. **Runtime at cadence 1 with neutralization.** T3 recomputes the full 252-interval block per decision, about 2.8M `log` × 754 decisions. The NAV pays this once for 3 books. Time the first TRAIN run against the 180 s cap.
  4. **S2/S3 capped deployment ramp.** Per the literal "deployment session excluded", ramp sessions 2..k count toward mean/p95, and their tau is large because pre-trade GMV is still small. The code does what the constraint says and documents it (`nav.cpp:645`). The owner should confirm this is intended before the flags are read.
  5. **T2 review cross-task item (pre-trade GMV not exposed).** Resolved by `pretrade_gross_dollars`.

## Strengths

- **The default path is provably the old arithmetic.** The `band = -1` sentinel (`tr.cpp:161`) needs no branch, and both loops compute the same `gap` from the same pre-assignment `current[i]`. The target CSV, recipe, rule and summary are gated by `construction_on`, and `ttest:536` pins the exact default header and the absence of every construction key.
- **One construction seam for both replays.** `detail::form_desired` is called by `replay_targets` and by the NAV extension point, so the NAV cannot drift from the target replay. `ntest:873` asserts record equality at every decision.
- **T3's contracts are used correctly.**
  - The skip classification reads `stats.used`/`excluded_gross`, which T3 populates even on a refusal (`strategy_price_exposures.hpp:108-109`, `.cpp:395-406`).
  - The strong guarantee means a skipped decision never trades a half-neutralized target.
  - The T3 M2 case (all gross on excluded rows) lands in `skipped-excluded-share` rather than being mislabelled.
- **Contract and allocation errors abort rather than silently skip** (`tr.cpp:213`). The price-risk config is validated once, up front.
- **The lockstep refactor is clean.** Per-book state was split from the shared construction (`nav.cpp:105-123`). `replay_nav` became the 1-scenario case, so all 10 pre-existing NAV fixtures now exercise the lockstep engine. The admission charges k books plus the shared scratch (`nav.cpp:166-183`, `:910-916`).
- **The GMV fixture is exact, not approximate.** It uses equality at NAV 1000 with constant prices and no costs. It includes a forced-exit-only session and a flag flip at ±1e-9 on both sides. The quantile convention is written into the summary JSON.

## Issues

#### Critical (Must Fix)
None.

#### Important (Should Fix)
None.

#### Minor (Nice to Have)

1. **A skipped decision in the NAV does not "keep current weights" when capped orders are still working.**
   - Where: `nav.cpp:432-438`. With `rebalance=false`, members whose `planned == current` are not cancelled (`else if (rebalance || !member)`). So under S2/S3, a capped residual order from the previous decision keeps filling toward that decision's target.
   - This matches the T2 non-cadence-day semantics and is defensible. But the recipe text `on_skip: "current weights kept"` (`tr.cpp:540`) describes only the target replay.
   - Fix: say explicitly in the NAV recipe/limitations that a skipped decision leaves prior working orders active. Alternatively, cancel member orders on a skipped cadence decision. Either way, decide it deliberately.
2. **Guard branches have no fixture.**
   - Untested: `SkippedExcludedShare` (the root-mandated 0.5 cap), `SkippedRefused` (constant, ill-conditioned or spanned), the classification order (`tr.cpp:219-228`), and Decision #1 (non-`Unavailable` aborts rather than skips).
   - Only the too-few-names and amplification paths are exercised (`ttest:452`).
   - Fix: add a `ttest:452`-style case where more than half the gross sits on names lacking beta history, with `min_names` small enough to pass. Assert `skipped-excluded-share`, unchanged weights and the count. Add a constant-volume panel with a flat log-ADV column and assert `skipped-refused`.
3. **The target-replay aggregate admission omits the price-risk scratch.**
   - `admit_saved`'s "aggregate input/workspace budget" (`tr.cpp:408-416`) charges 36 B/cell plus the per-name workspace, but not `price_risk_scratch_bytes`. That scratch is charged only in the separate workspace check (`tr.cpp:102-106`).
   - So a neutralized target replay can exceed the declared `max_working_bytes` by the scratch: about 12 MB at N=5,600 and about 43 MB at `max_names`.
   - The NAV path is correct: `nav_reserve_bytes` includes it (`nav.cpp:910-916`).
   - Fix: add `budget.add(1, price_risk_scratch_bytes(cfg.target, n))` in `admit_saved` when neutralizing.
4. **`price_risk_valid` (`tr.cpp:61-66`) re-implements T3's `validate_config` contract.**
   - If T3's ranges change, T4 either refuses a valid recipe or lets an invalid one reach every decision (the latter aborts, so the failure is loud).
   - Fix: expose T3's config validator (e.g. `validate_price_exposure_config`) and call it. Otherwise, add a parity test over boundary configs.
5. **The `replay_nav_scenarios` contract is stated too strongly.** The header says "results[k] is bit-identical to replay_nav(...)" (`strategy_nav_replay.hpp:1004-1009`). That holds only when every book succeeds and the k-book admission passes:
   - One scenario's hard error (e.g. nonpositive NAV under S3) now fails all books.
   - The k-book budget can refuse where k single runs pass.
   - Single-scenario admission also moved slightly at defaults (`shared_name_bytes`, larger `NavReplayDay`).

   None of this changes a published value. Fix: qualify the comment.
6. **`summarize_nav` is now about 97 lines** (`nav.cpp:978-1075`), against the ~60-line guideline; T2 M7 already flagged it at 68.
   - The ceiling validation is duplicated verbatim at `nav.cpp:983-985` and `:1089-1091`.
   - Fix: extract a `daily_gmv_stats(days, deployment_index, limits, s)` helper and a `valid_limits(limits)` predicate.
7. **GMV-denominator edge cases are unpinned.**
   - No fixture asserts that a held stale name contributes at its stale mark, or that a name written off at t contributes 0 to `pretrade_gross_dollars`.
   - No fixture runs a capped multi-session deployment to show that the ramp sessions are counted (`ntest:956` is uncapped).
   - Fix: extend `ntest:956` with a K=5 gap name, and add one S2-capped panel.
8. **House-style nits.**
   - A new 101-column line at `tr.cpp:213`; the limit is 100.
   - `noexcept` is missing on new non-throwing leaf helpers: `gross_dollars` (`nav.cpp:450`), `quantile` (`tr.cpp:515`), `neutralizing`, `price_risk_valid`, `construction_on`, `outcome_label`.

## Assessment

**Task quality:** Approved

**Reasoning:**
- **Defaults.** The default path is the pre-T4 arithmetic token for token.
- **Construction.** Neutralization and the band are formed once, by one shared seam, and the NAV is pinned to the target replay at every decision.
- **Lockstep.** Books couple only through read-only shared construction.
- **GMV tau.** It uses executed fills over pre-trade gross, excludes the deployment session, and is hand-checked exactly.
- **Remaining work.** It is test coverage of the excluded-share/refused guard branches, a skip-semantics note for capped NAV orders, and small admission and style fixes.
