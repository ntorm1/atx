# Task T10 review: swap-fin-v1 financing, borrow tiers, locate block (root commit ad6d7682)

Reviewed `review-T10.diff` (4 files, +1323/−138) in one pass. The package's trailing "Changes" section repeats the first hunks, so nothing was cut off. The source files at pool-2 HEAD have not changed since ad6d7682 (`git log ad6d7682^..HEAD` on the three files returns only ad6d7682). All line numbers below are current source lines. Root rulings override the brief.

### Spec Compliance

✅ **Spec compliant.**

**Requirement 1: financing spec.**
- `NavFinancingRule {FlatShortV0, TieredSwapV1}`, and `NavFinancing` has spreads, tier bps, `day_count` and `block_special_shorts` (hpp:59-68).
- `valid_financing` stops one spec from mixing parameters from both rules (cpp:185-199).
- The declared numbers are in `nav_financing_scenarios` (cpp:~1352):
  - swap-fin-v1: 40 / 20 / 30 / 100 / 500, D360, block on.
  - flat-300-v0: `NavFinancing{}`, 300 bps, D365.
  - engine-tiers-v1: 27.5 / 300 / 2750. The test asserts these equal `BorrowTierRecipe{}` × 1e4 (test:1113).

**Requirement 1: flat-300-v0 is bit-identical.** Desk-checked:
- `accrue_financing` fills every tier slot with the legacy factor `borrow_rate*days/365.0` (cpp:414). It sums `-h*factor` in name order, starting from 0.
- The long leg stays exactly +0.0 (cpp:424). So the extra `b.cash -= 0.0` (cpp:454), the `- 0.0` in `nav_pre` (cpp:456) and the `- 0.0` in the identity are all exact.
- Fixture (a) checks this bit for bit inside the 5-book matrix, with and without fields (test:1155).

**Requirement 2: scenario matrix.**
- With fields, `nav_scenario_matrix` returns S1/S2/S3 × swap-fin-v1, then S2 × flat-300-v0, then S2 × engine-tiers-v1. The primary index 1 is S2 × swap-fin-v1 (cpp:1381).
- Without fields it returns the legacy three books, and `primary_financing_available:false` goes to both recipe and summary.

**Requirement 3 and the root ruling: CLI and pins.**
- `--fields <manifest.json> --fields-sha256` must come together. The check is at dispatch and again in `run_nav_replay`.
- `pinned_document` checks the external SHA pin (cpp:1243).
- The manifest's `role.manifest_sha256` must equal `--role-sha256`. Its sessions, ids and member SHAs must equal the pinned role manifest's receipts (cpp:1318-1323).
- A used field whose `point_in_time` is false or missing is refused (cpp:1289).
- Only `shares_out` and `si_shares` are loaded, and all of this happens before the blend load or any directory is created (cpp:1683).
- The fields SHA, the role SHA, per-field clocks and each scenario's financing spec are recorded in recipe.json and summary.json.

**Requirement 4 and the root rulings: tiers.**
- Market cap is `shares_out × raw_close` only, and `mktcap_lagged` is never read.
- `shares_out` outside [1e5, 5e10] or NaN counts as missing. A non-finite or negative `si_shares` also counts as missing.
- SI ratio is `si_shares / shares_out`.
- IPO age runs from the first present role row, and a name present at row 0 is seasoned (cpp:523-541).
- A missing predictor maps to warm **before** the engine call and is counted (cpp:546-570). The engine is called once per decision and its result is shared by every book (cpp:716).

**Requirement 5: accrual.**
- Accrual runs on pre-mark `held`, before the realize loop, so it charges the positions actually held over (t−1, t].
- It uses calendar days / `day_count`, which is ACT/360 for tiered books.
- The fee comes from the tiers of decision t−1: `classify_borrow` runs after every book's MARK at t (cpp:716).
- The identity becomes `r = gross − cost − borrow − long_financing`. `borrow_*` stays the whole short leg, and `long_financing_*` is added.
- Fixture (b) hand-computes a weekend under ACT/360 (test:1194).

**Requirement 6 and the root ruling: locate.**
- `block_special_plan` (cpp:576) runs after `update_weights`, so after the rule, band and budget (cpp:615).
- It applies `planned = max(planned, min(current, 0))` and counts blocked dollars and names.
- A special short stays open and pays special at the next MARK (fixture (d), test:1314).

**Requirement 7: reporting.**
- Per scenario:
  - long financing dollars;
  - short financing dollars in total and by tier;
  - short-dollar share by tier (mean and p95);
  - blocked short dollars and names;
  - missing-predictor short name-days and member decisions;
  - net-exposure mean, mean |net| and max (cpp:1056-1076).
- The daily CSV gets the same columns, but only in the fields matrix, so legacy CSVs keep their exact bytes.

**Constraints and fixtures.**
- All six fixtures are present: (a) test:1155, (b) test:1194, (c) test:1261, (d) test:1314, (e) test:1380, (f) test:1427.
- The T4 daily GMV turnover code is untouched. It is still computed from fills (`traded_dollars / pretrade_gross_dollars`), so the locate block can only change it by changing real trades.
- No accounting identity is weakened; one is extended by an exact −0 term.

⚠️ **Cannot verify from the diff (controller decisions or root checks):**
1. **Planned turnover and v2 budget before the block.** `b.spent += plan.turnover` (cpp:613) runs before the block (cpp:615). Under monthly-budget-v2, turnover that the block refuses still uses up the monthly budget. That is conservative, and it has no effect under baseline at cadence 1. The implementer flagged it, and the root should confirm it as a ruling.
2. **Whether "pays special until exit" is sticky.** The implementation charges the tier of the latest decision. See Minor 1.
3. **Real-role memory admission.** The 5-book reserve plus 16 B/cell of fields on the real TRAIN and validation roles can only be verified by the root's run.

**Checks run, one per named risk:**
- **Engine contract** (`atx-engine/src/cost/borrow_tiers.cpp`).
  - It needs `available_at < decision`; 22 h < 23 h holds.
  - It returns InvalidArgument for non-positive cap or price, or a negative or non-finite SI or age. The domain filter at cpp:529-535 rules all of these out.
  - The seasoned age `DBL_MAX` is finite and non-negative, so the engine accepts it.
  - So the propagated engine error (cpp:~561) cannot fire on real data.
  - Engine defaults: gc 0.00275, warm 0.03, special 0.275, which is 27.5 / 300 / 2750 bps.
- **Producer schema** (`atx-engine/tools/prepare_research_fields.py:1155-1190`).
  - The key names match what `used_field` and `load_fields` read: `files[<name>.f64].{bytes, sha256}`, entries `name/file/dtype/layout/shape/sha256/point_in_time`, and `role.{manifest,sessions,ids,member}_sha256`.
  - `SHARES_OUT_DOMAIN = (1e5, 5e10)` (py:79) matches the replay's bounds.
- **SI look-ahead** (py:131-135). The `si_shares` clock is the latest row with `available_at < date(session)`, strict, where `available_at` is the official dissemination date, and it goes NaN after 45 days. So SI is visible only after dissemination.
  - `shares_out` is the A8 90-day lag, restated for splits (py:176). It is point in time as of the decision.
- **Role manifest keys** (`files.{sessions.i64, ids.u64, member.u8}.sha256`, `dates`, `instruments`) are the same keys the target-replay role loader reads (`strategy_target_replay.cpp:454-467`).
- **Empty quantile.** `TierShares::summarize` calls `sorted_quantile` on an empty vector when there are no tiers. The existing daily-GMV code (cpp:1561-1563) already relies on it returning NaN for an empty input.
- **Downstream consumers.** A search for `dsl-nav-replay`, `nav-replay-summary` and `daily_modeled-1bn` outside Markdown hits only this source file and its test. No other consumer depends on the removed `annual_borrow_bps` / `borrow_day_count` recipe keys or on the new `<trading>+<financing>` file labels.
- **Style.** No line in the three changed files exceeds 100 columns (house-style limit).

### Strengths

- **Bit-identity is exact, not approximate.**
  - The flat path keeps the legacy expression and summation order, and the only new arithmetic is an exact −0.0.
  - Fixture (a) proves it inside the full 5-book matrix under two target rules, including `max_return_identity_error`.
  - Fixture (f) proves it end to end: identical events SHA, and every daily line is the legacy line plus the appended columns.
- **Tier handling.** Tiers are computed once per decision, from row d only, and shared by every book as flags that do not depend on fees; each book applies its own fee (cpp:88-99, 716).
  - Fixture (e) is a strong as-of test: rewriting rows ≥ m leaves rows < m and row m's MARK bit-identical, and changes m+1.
- **Loader and pins fail closed.** They check the external SHA, the exact payload byte extent plus SHA, shape and dtype, that `file` is never a path, duplicate entries, missing `point_in_time`, and schema and status.
  - Every refusal is tested to leave no `out/` directory, and there is a positive control (test:1512-1538).
- **Tier-mapping boundaries are tested exactly** (test:1261): strict `<` 1e9 cap, `>=` 0.10 SI, both `shares_out` domain edges, a young name, missing at d only, absent at d, and NaN at d±1 to prove only row d is read.
- **Declarations are candid** about the tier priors, the no-locate modeling and the understated SI ratio (cpp:851-873, limitations).

### Issues

#### Critical (Must Fix)

None.

#### Important (Should Fix)

None.

#### Minor (Nice to Have)

1. **The declaration says more than the code does** (hpp:62, cpp:856). Both say "a short that migrates into special pays special until it exits".
   - The code re-tiers every name at every decision (cpp:567), so a held special short can pay less before it exits:
     - it drops back to warm or GC when it loses a flag (for example, price or cap near a threshold);
     - it drops to warm when a predictor goes missing (an absent day, or SI older than 45 days).
   - This matches brief §5 ("fee from the tier of the latest decision <= t-1"), but the recipe text reads as a sticky rule.
   - **Fix:** either reword the text to "charged its latest-decision tier; a short is never force-closed by the locate rule", or, if the root rules that special is sticky, keep a per-name held-short special latch that clears when the position goes flat. Fixture (d) only covers the non-reverting case.
2. **No fixture actually clamps in `clamp_kept_order`** (cpp:589-597, called at cpp:621-622).
   - The branch changes trades on non-rebalance days.
   - It is reachable at cadence > 1, and in the primary cadence-1 configuration whenever the price-risk-v1 guard skips a rebalance.
   - Fixture (d) runs at cadence 1, so this branch never fires there.
   - Desk-check says it is correct: clamp to `min(held, 0)`, then deactivate when the clamped order equals the holding.
   - **Fix:** add a fixture where a GC name's capped (S2) residual short order is still working when the name turns special before the next rebalance, and assert the clamp, the blocked dollars and the deactivation.
3. **Summary `blocked_short_dollars` is hard to read** (cpp:780, 1073).
   - It sums each decision's refusal, so a persistent block on one name is counted again every session at cadence 1.
   - It also adds two bases: decision-NAV dollars for plan blocks (cpp:582) and order dollars for kept-order clamps (cpp:592).
   - **Fix:** add a normalized statistic, such as the mean blocked dollars per decision divided by the planned short dollars (or by NAV), so the TRAIN and validation readout can be interpreted. The daily CSV is fine as it is.
4. **The IPO-age proxy can flag seasoned names as young** (cpp:538, fold at cpp:549-553).
   - Any name absent at role row 0, for example from a one-day data gap or a halt, is flagged young for its first 365 days in the role. That adds a flag (GC → warm, warm → special) and can trigger the locate block.
   - This follows the root ruling. The size is unmeasured.
   - **Fix:** report the count of member decisions whose young flag comes from a first-present row inside the first few role sessions, so the root can tell whether the proxy matters.
5. **The visibility clock is hard-coded** (cpp:52). `available_at = session + 22h` and `decision = +23h` are constants. The replay never checks them against the manifest's `visibility_mark` or the role's `clock_recipe`; it only records the per-field `clock` and `staleness` strings.
   - **Fix:** a string check against the producer's declared mark would turn a silent assumption into a refusal.

### Assessment

**Task quality:** Approved

**Reasoning:**
- The financing matrix, the tiers, the locate rule and the pinned loader match the brief and every root ruling.
- Flat-300-v0 is bit-identical by construction and by test, the tier inputs are point in time (the producer clocks were verified), and every refusal fails closed before any output.
- What remains is a declaration wording question that needs a ruling (sticky special), one untested clamp branch, and reporting polish.
