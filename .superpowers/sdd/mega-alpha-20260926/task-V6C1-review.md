# Task V6-C1 review: order basis, exit rate, locate-in-aim, post-ramp L, fixed-rate liquidity cache

Reviewer: Claude Opus 5.5 (read-only), 2026-09-27. Scope: pool-2 commits ee574c4f and 709beb69 (review-V6C1.diff),
the brief, the report, v6-code-review-exec.md (F1, F2, F4, F5, F8) and v4-prereg.md "## v6 revision" (C1-C4). I did not
build or run anything. Root facts relayed by the controller: tag v6-0 compiles clean under /W4 /WX; the real-data default
cell is byte-identical to the v5.1 parent with the liquidity cache on; one new test fails
(`NavV6.RecipeSummaryKeysAndCliRefusals`, json type_error.304 "cannot use at() with number").

## Verdict 1: SPEC COMPLIANCE: PASS (notes N1-N3 below)

| item | status | evidence |
|---|---|---|
| C1 delta stored at DECIDE in decision-NAV dollars | OK | `plan_decision` nav.cpp:817-818: `order = planned*NAVpost`, `anchor = held_d` (`anchor_order` :783-787). `order - anchor` is exactly `planned*NAVpost - held_d`, the brief's delta. It is not computed as `(planned - current)*NAVpost`, which differs only by rounding, and this choice makes the no-drift case bit-identical to target. |
| EXECUTE fills delta minus filled-so-far | OK | :630 requests `order - anchor`. A partial fill adds `filled` to both `held` and `anchor` (:652-653), so the request is always delta minus the dollars filled so far. |
| Drift rides | OK | A complete fill sets `held = order + (held - anchor)`, i.e. the target plus the drift since the decision. |
| Carry-over of capped residuals | OK, documented (hpp:126-137, recipe `order_basis_rule`) | The residual keeps its remaining decision-dollar delta and is never drift-adjusted. It ends when filled, replaced by a plan change (the new delta is planned×NAV - held_now, so nothing is double-counted), or cancelled (unchanged plan on a rebalance, flat nonmember, write-off). At cadence 1 every residual is re-planned at the next decision, exactly as under target. |
| Zero plan and special tier stay target orders | OK | Needed so every exit ends flat and no special long is sold through zero. `clamp_kept_order` :757-761 converts a kept delta order to its target equivalent before the clamp; the signs are correct for shorts (hand-checked on a short growing under a price rise). |
| θ = 1 "reproduces target" | OK as interpreted (N1) | Two tests cover it. `DeltaWithoutDriftIsTargetBitForBit` (which includes θ 1) shows the bases are bit-identical without drift. `DeltaThetaOne…` shows both bases plan the aim. With drift the bases differ by design. |
| C2 exit rate: geometric decay, snap inside the dust band, non-live names only | OK | tr.cpp:243-246. The branch is reached only when `!live && decaying && present`. N_d = `members_at(in,d)`, the same N_d as the live band; +inf when N_d = 0. |
| C2 no-price / stale path untouched | OK (N2) | An absent nonmember keeps `next = 0`. `carry_absent` / write-off are untouched. |
| C3 locate-in-aim | OK | The tier is classified at :930 before `form_desired` at :938, and the mask is filled at :933-934. `form_desired` zeroes negative member aims before `neutralize_price_risk` (tr.cpp:345-348). `block_special_plan` is kept. Refused without fields or with `--neutralize none`. |
| C4 nav_summ | OK | `[RAMP_ROWS:]` with `RAMP_ROWS = 63` drops exactly 63 rows (no off-by-one); ≤ 63 rows gives null. The JSON is written with `sort_keys=True`, so key placement does not matter. The byte test against blob 73fbb749 covers every existing key and line. |
| F8 | OK, opt-in and bit-identical by construction | `fill_liquidity` stores `window_liquidity(ctxs.front(),t,i)`. It reads only shared input and the base config's window and pair minimum, so it is the same value `execute_orders` computes per book. It runs before MARK, which only cancels orders. Present-cell volume is validated finite (:322-328), so the debug assert cannot fire. The root's real-data identity result confirms this. |
| v6_train.sh | OK | Derived from `v51_train.sh`'s nav phase with identical flags plus `$OB $XF $LF $CF`. Knobs `ORDER_BASIS`, `EXIT_RATE`, `LOCATE_AIM`, `LCACHE`. Directory names exactly as the brief. Any result other than "0 completed", or a missing receipt or `summary.json`, exits 1. The REF byte check exits 3 on a mismatch. `REF` (mega-nav-v51-ew-t.05-d.1-fixed, 16:52) predates only a1c4aec5, which leaves the fixed path unchanged, so the check is meaningful. |

## Verdict 2: CODE QUALITY: FIX REQUIRED (one Important finding, in a test)

**Default path traced line by line; unchanged.**
- `NameState`, `Construction` and `LiquidityCache` allocate nothing new by default. The `anchor`, `delta` and `no_short` vectors are empty, and the cache is off on the fixed path.
- `execute_orders` runs the same expressions: `order - held`, and `held = order` on a complete fill.
- `plan_decision` guards per-name rates with `rate == PerNameV1`. That is equivalent to the old `cache.on()`, because the cache used to be on only under per-name-v1.
- `anchor_order` and the clamp conversion are unreachable by default.
- In `aim_partial_weights` the new `else if` is unreachable at r = 1, and `members_at` is not called.
- The validation, recipe, summary and budget additions are all gated on non-default values.
- `aim_partial_summary` builds the same object.
- The per-name path is unchanged: same cache sizing and same fill arguments, with `rate_decision = decision`.
- Root evidence agrees: the v5.1 parent is byte-identical.

**Accounting checks.**
- Filled-so-far accounting is correct across capped fills.
- A stale write-off sets `order` and `active` to 0. The stale `delta` flag and `anchor` are never read, because every read is gated on `active`, and `anchor_order` rewrites both at the next placement.
- No `/W4` hazards found. The root's clean build confirms this.
- No added C++ line exceeds 100 columns.

### I1 (Important, test bug; not a code bug): dangling range-for temporary
- **Where:** `atx-impl/tests/strategy_nav_replay_test.cpp:2517`
  `for (const auto& s : read_json(dir.path / "exit" / "summary.json").at("scenarios"))`
- **Cause:** `read_json` returns a temporary `Json`, and `.at()` returns a reference into it. C++20 does not extend a temporary's lifetime through a member call in the range-init (that changes only with C++23 P2718). The temporary is destroyed before the loop body runs, so the loop iterates freed memory, which is undefined behaviour. The observed "type_error.304 cannot use at() with number" is that garbage.
- **Why the code is not implicated:** test (a) makes the same `construction.v5` check on a *named* summary object and passes. `aim_partial_summary` writes `exit_rate` into an object (tr.cpp:1604 area).
- **Fix:** `const auto exit_summary = read_json(dir.path / "exit" / "summary.json");` then iterate over `exit_summary.at("scenarios")`. Re-run `*V6*`.

### Minor (optional)
- **M1 (C3 disclosure).**
  - After zeroing, the special name stays a regression row. Its neutralized weight is `-fitted_i × scale`, which can have either sign.
  - A positive value is a long in a special-tier name that the signal ranks short, and `block_special_plan` does not stop longs.
  - The rescale also sets gross to the zeroed entry gross (1 - special share), not 1.
  - Say both in `locate_in_aim_rule`. Consider a count of zeroed names that end long.
- **M2.** `zeroed_special_short_aims` also counts decisions whose rebalance was then skipped by the neutralization guard (nav.cpp:1971-1972; the test's zeroed == 6 relies on this). Document it, or count applied rebalances only.
- **M3 (key naming).** The recipe pairs `order_basis`/`order_basis_rule` and `locate_in_aim`/`locate_in_aim_rule`, but `exit_rate` pairs with `exit_rule`. The `aim_partial` and per-name declaration strings still say "nonmembers exit to 0". `exit_rule` overrides them, but the recipe text is inconsistent.
- **M4 (N2).** An absent nonmember gets an immediate exit order. If it reappears present before that order fills, the next decision replaces the order with a decay. This holds exit-prone names longer. It is consistent with the per-decision rule; disclose it with the S3 watch.
- **M5 (`v6_train.sh`).**
  - With `REF` absent, the byte check prints "skipped" and exits 0. Consider exit 3 unless `REF_CHECK=0`.
  - Without `pipefail`, a `nav_summ` failure is masked. Only the nav run is required to fail loudly, so this is optional.
- **M6.** The conditional-lvalue assignment `(cond ? a : b) = true` (nav.cpp CLI) is clever where house style prefers explicit code.
- **M7.** `validate_nav_config` returns one message for two unrelated refusals.

## Recomputed test expectations
- **`TargetReplayV6` snap at decision 59: correct.**
  - Name 0's weight is 0.095 and decays by 0.95 per decision. The band is .1/19 from decision 4 on.
  - k ≥ ln(.0554017)/ln(.95) = 56.40, so k = 57 and d = 3 + 56 = 59. Margins: -3.0% at k = 57, +2.1% at k = 56.
  - `keep = 1.0 - 0.05` is the same double as the test's `1 - 0.05`, and forced turnover sums in the same order, so the bit checks hold.
- **NAV snap at decision 54: correct.**
  - Start .5/1.8 = .27778, decay .95, band .1/5 = .02 (0.025 at decision 3).
  - k ≥ ln(.072)/ln(.95) = 51.30, so k = 52 and d = 54. Margin +1.5% at k = 51.
  - `ri->days[3].planned_forced` = .8/1.8 and `days[4]` = 0: correct (name 4's exit fills at session 4 before decision 4).
- **`DeltaThetaOne`: all values correct.**
  - 210 + 50 / 150 + 50; -450/450 vs -510/450; NAV 840; 30 + 30 / 90 + 30.
  - `pretrade_nav` is bit-equal, because the pnl terms are exactly 0 at constant prices.
- **`DeltaCapped`: correct.**
  - Caps: 2000, 2000, then 2050, since ADV = (3×2e5 + 2.2e5)/4.
  - Target fills 2000, 2000, 380 (= 5000 - 2420 - 2200). Delta fills 2000, 2000, 1000 and ends on 5620.
- **`DeltaSpecialTier`: correct.**
  - Weights 1/3 → 1/6 (ranks .5 and .25 over gross 1.5). Opened = .6·h2 - nav2/6 ≈ 33,340 > 30,000.
  - h2 = 1e6/3 to within ulps, because nav_post1 = 1e6 exactly (no financing before deployment). The ±1 $ tolerance holds.
  - `member_tiers` {4,0,1}: the default fields are GC; name 0 (cap 1e8, SI .5) is special.
- **zeroed == 6: correct.**
  - 9 sessions give 7 decisions (t = 0..6). Name 0 is the lowest-ranked member at all of them except t = 4 (absent).
  - The 3-name panel is below `min_names`, so the neutralizer returns Unavailable, a skip rather than an error, and the count is taken before that.
- **Locate thresholds: not computed exactly; plausible with a wide margin.**
  - Post-block |net| is about the blocked neutralized weight of name 6 (~.15-.18 > .05).
  - The aim book's net is bounded by `|fitted_6|` ≈ intercept .0127 ± a small slope term, because name 6's exposures sit near the cross-section mean (loading, idio and volume indices = 6 of 0..11). That is < .5×.
  - Not verifiable without running.
- **F8 and `RecipeSummary` key checks:** structurally right. `recipe_sha256` round-trips.

## Not checked
- Exact regression values in the locate test.
- The `test_nav_summ.py` run; the implementer reports 20/20.
- The v6_train.sh DRY runs.
- Merge interaction with V6-C2: not in pool-2 `src` yet; conflicts reported as textual.

## N-notes (spec interpretation, no action)
- **N1.** "θ = 1 reproduces target" is read as: bit-identical without drift, and the same planned weights with drift. Literal equality under drift is impossible by design. The tests pin both readings.
- **N2.** See M4.
- **N3.** The root has already ruled on the dust re-tune {.05, .2} and on locate-in-aim applying to every book (progress.md ad31e817).

**FIX REQUIRED:** the implementer must fix I1, the dangling temporary at `strategy_nav_replay_test.cpp:2517` (bind the summary
to a named local before the range-for), then re-run `atx-impl-strategy-target-tests
--gtest_filter=TargetReplayV5.*:NavV5*:*BitIdentical*:*V6*` green. No production-code change is required. M1-M7 are optional.
