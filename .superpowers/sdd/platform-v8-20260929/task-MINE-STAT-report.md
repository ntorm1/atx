# Task MINE-STAT report: overlap factor at the campaign's level, MINE-14, MINE-15

Lane MINE-STAT (wave AG), worktree `C:/atx-wt/pool-9`, branch `feat/platform-v8-minestat-20261001` from `20e7bd19`.
Nothing was built in C++ (lane rules): every C++ change is written for clang-cl `/W4 /WX` against the owning files
read in full, not compiled. Python ran on synthetic null draws with fixed seeds only. No real data, nothing dated
2024-01-01 or later opened, no subagents, no pushes. `atx-engine/` is untouched (`git diff 20e7bd19 -- atx-engine`
is empty), so the determinism golden `0x889874a3b9b29c55` cannot move.

| task | commit | what |
|---|---|---|
| 1 overlap factor by budget (lifts PM4-13) | `781b6bea` | discover factor table by budget band, confirm factor table by confirm reads, `kMinedMaxBudget` 1,000 -> 10,000 |
| 2 MINE-14 (PM5-9), MINE-15 (PM5-8) | `f6e0b985` | rho step over the whole shortlist then the cap, memory-bounded; an undefined rho pair fails |
| 3 golden | no commit | neither task touches `atx-engine`; nothing stopped |
| 4 report | this commit | |

## 1. The overlap factors

### What each factor must make nominal

- **Discover hurdle.** The shortlist reads f2 / F >= z(N) = -norm_ppf(.05 / (2 N)). With zero regressors f2 = |t|,
  so the per-trial rejection rate is P(|t| >= F z(N)) and the Bonferroni level is .05 / N. So
  F(N) = Q(t, 1 - .05 / (2 N)) / z(N): one-sided levels 2.5e-4, 2.5e-5 and 2.5e-6 at N = 100, 1,000 and 10,000. A
  budget band (previous top, top] reads the factor of its top: the ratio grows into the tail, so the top covers the
  band. With regressors, f2 = sign(raw IC) x marginal t <= |marginal t|, which is conservative.
- **Confirm read.** The plan says "HAC t 2.0 with BY p <= .10". BY at .10 compares the k-th smallest p with
  .10 k / (m H(m)), where H is the harmonic number and m the number of reads. Its proof needs P(p <= that level) <=
  that level at every k, and k = 1 is the deepest level. So Fc(m) = max(gate ratio at Phi(-2), ratio at
  .10 / (m H(m))) on the 200-row confirm floor. The band of m reads uses the factor of its top.
- **Answer to the brief: yes, the confirm needs its own factor.** The discover F is the wrong number for the
  confirm, for two reasons:
  - the window: 200 rows against 504, so heavier tails;
  - the levels: the confirm levels are set by m and not by N.

  At the default cap (16 reads) BY's first level is 1.85e-3, where the 200-row ratio is 1.76. That is the 1.70 the
  brief names, read one level deeper. With 1.55 the BY p's were anti-conservative. Fc is now `kMinedConfirmBands`,
  keyed on m = the number of reads that reach the confirm (PM5-9). Fc is known from m alone, and `mined_confirm`
  takes it from the size of its input.

### The null and the estimator (unchanged from MINE-FIX)

- **Model.** 30 names, i.i.d. N(0, 1) returns. The label of row t is the sum of returns t+2 .. t+22. The signal is
  fully persistent, the worst case.
- **Daily statistic.** The daily IC is `marginal_rank_ic_day` with zero regressors, that is Pearson(centred signal
  rank, label rank).
- **t.** t = `summarize_rank_ic(daily, 21)`: Bartlett lag 21 with the small-sample factor. The estimator pins
  (2.0969..., -0.3360...) are kept in both the pytest and the C++ test.
- **Signal ranks fixed.** The names are exchangeable, so the signal ranks are fixed at 0..29; the distribution of t
  is the same as with MINE-FIX's random signal. Fixing them makes the daily IC a single gather plus a dot product
  (pytest `test_the_daily_statistic_is_the_rank_correlation`).

### Tail estimation (why so few draws suffice)

Plain draws cannot reach 2.5e-6: about 50 tail events need about 20 million draws, roughly 4.5 hours at 0.8 ms per
draw. Each cell therefore draws from an importance-sampling proposal and weights every draw by its exact likelihood
ratio. The simulated model is still the exact null above: ranks, label overlap and the verb's t. Only the proposal
changes.

The proposal is the dominating point of the linearised statistic:

- to first order the daily IC is a moving sum, over the label window, of y(s) = the session's returns projected on
  the signal's ranks;
- so t ~ kappa Z / sqrt(sum_k lam_k z_k^2), with:
  - Z the projection of y on the mean's direction;
  - z_k the projections on the eigenvectors of the Bartlett form (the HAC variance), lam_k the eigenvalues
    normalised by the trace;
  - kappa about 1.25: the lag-21 under-correction;
- for a target c, the proposal shifts Z by theta and shrinks each z_k to sd 1 / sqrt(1 + (c / kappa)^2 lam_k). That
  is the likeliest way the event happens: a large mean together with a small HAC variance.

Each cell runs:

1. a pilot of 1,000 draws to place c;
2. the main draws;
3. a bootstrap standard error of the quantile (200 resamples).

The factor is ceil(ratio + 2 se, 2 decimals): an upper confidence bound, rounded up.

Seeds: `SEED = 20261001`. Cell k uses `default_rng([SEED, k, 0])` for the pilot, `[SEED, k, 1]` for the draws and
`[SEED, k, 2]` for the bootstrap.

Draws: 20,000 per cell at 200 and 504 rows, 10,000 at 984. The reason for those counts:

- the relative standard error of the tail probability is 1.2% to 3.9% (the `tail_rel_se` column);
- every ratio standard error is at most .003 on the discover floor and at most .0122 on the confirm floor (m 256;
  the others at most .0068), so the 2-se margin adds at most .006 to a discover factor (under one rounding step of
  .01) and at most .024 to a confirm factor;
- plain draws would need 0.3 million to 539 million draws for the same precision (the `plain_equivalent` column).

Cost:

- a full run takes 3 to 5 minutes (288 s and 176 s measured);
- peak simulation memory is about 300 MB (tracemalloc, every window length);
- `python atx-impl/tools/mine_overlap_factor.py` exits 0: a second full run reproduced the pinned tables and record.

### The table (`strategy_mine_rule.hpp`; pinned in `mine_overlap_factor.py` `OVERLAP_BANDS`, `CONFIRM_BANDS`, `DERIVED`)

Discover, on the 504-row floor (`kMinedOverlapBands`), 20,000 draws per cell plus a 1,000-draw pilot:

| budget band | one-sided level | z(N) | quantile of t +- se | ratio +- se | ratio + 2 se | **F** | tail prob. rel. se | plain draws for same se |
|---|---|---|---|---|---|---|---|---|
| 1 - 100 | 2.5e-4 | 3.4808 | 5.0743 +- .0103 | 1.4578 +- .0030 | 1.4637 | **1.47** | 1.8% | 12.4 M |
| 101 - 1,000 | 2.5e-5 | 4.0556 | 6.2083 +- .0110 | 1.5308 +- .0027 | 1.5362 | **1.54** | 2.1% | 93 M |
| 1,001 - 10,000 | 2.5e-6 | 4.5648 | 7.4081 +- .0134 | 1.6229 +- .0029 | 1.6287 | **1.63** | 2.7% | 539 M |

**New budget ceiling: `kMinedMaxBudget` = 10,000**, the table's last top (`static_assert`). `MINED_MAX_BUDGET` in
`backtest_integrity.py` is 10,000 too, under the same name.

Confirm, on the 200-row floor (`kMinedConfirmBands`), 20,000 draws per cell:

| reads m | level | z | ratio +- se | ratio + 2 se | **Fc** = ceil(max(gate, BY)) |
|---|---|---|---|---|---|
| gate t / Fc >= 2 | Phi(-2) = 2.275e-2 | 2.0000 | 1.5347 +- .0050 | 1.5447 | (binds below m = 3) |
| 1 - 16 | .10 / (16 H16) = 1.849e-3 | 2.9029 | 1.7563 +- .0068 | 1.7700 | **1.77** |
| 17 - 64 | 3.294e-4 | 3.4062 | 1.9402 +- .0050 | 1.9501 | **1.96** |
| 65 - 256 | 6.378e-5 | 3.8311 | 2.1219 +- .0122 | 2.1462 | **2.15** |

`--max-promotions` is bounded to 256 by `check_config`, so m <= 256. Above the table Fc is NaN and nothing confirms
(fail-safe).

Reference, the whole of TRAIN (984 label rows, 10,000 draws per cell): 1.3272 +- .0032, 1.3621 +- .0031 and
1.4020 +- .0030 at N = 100, 1,000 and 10,000. Every value is below the floor's, so the floor's table holds for
every admitted (longer) discover window.

Checks:
- IS against plain draws:
  - pytest, confirm gate, 200 rows: 20,000 plain draws against 2,000 IS draws agree within 3 combined se, the IS
    se is the smaller, and the mean weight is 1.
  - scratch, 504 rows at the 1% two-sided level: 60,000 plain draws give 3.530 +- .035, 8,000 IS draws give
    3.502 +- .011.
- Agreement with MINE-FIX:
  - its 200,000-draw plain one-off at 504 rows read ratio 1.461 at 99.95% (= the N = 100 level); here
    1.4578 +- .0030;
  - its confirm-gate value 1.5422 (20,000 plain draws); here 1.5347 +- .0050.
- Robustness to the cross-section (one-off, not committed): 100 names instead of 30, 5,000 draws per cell, read
  1.4524 / 1.5272 / 1.6091 (se about .005). These are at or below the 30-name values, so the 30-name table is the
  conservative one for real cross-sections.

### What changed in the rule (`strategy_mine_rule.{hpp,cpp}`)

- `kMinedOverlapFactor` is gone. In its place:
  - `MinedFactorBand {top, factor}`, `kMinedOverlapBands` and `kMinedConfirmBands`;
  - `mined_overlap_factor(budget)` and `mined_confirm_factor(reads)`: NaN for 0 or above the table;
  - `mined_overlap_corrected(t, factor)`.
- `mined_shortlist(reads, hurdle, factor)` reads f2 / F(budget).
- `mined_confirm` uses Fc(m), m = its input size. `MinedConfirm.factor` records it.
- The header text states both derivations, the floors and the lifted ceiling.
- Verb (cross-lane, `strategy_mine.cpp`):
  - `check_config` refuses budgets above 10,000. The message is "--budget N is above kMinedMaxBudget 10000 (Ruling
    PM4-13): the mined-v1 overlap table kMinedOverlapBands is validated to that budget only; ...".
  - `PromotionContext.overlap_factor = mined_overlap_factor(--budget)`.
  - The recipe carries `overlap_bands`, `confirm_bands` and `max_budget`.
  - `campaign.json` `hurdle.overlap_factor` is the F used. Each promotion carries `confirm_factor` (Fc) and
    `f2_corrected` on F.
  - `hurdle.t` is unchanged.
  - The usage text says 10000.

## 2. MINE-14 (PM5-9) and MINE-15 (PM5-8)

**MINE-15**: `mined_rho_select(rho, fixed_rows, candidates, min_dates, cap)`.
- A candidate fails when any pair it is checked against has fewer than max(--min-dates, 1) defined dates
  (`PairwiseRowCorrelation::dates`). A date counts when at least --min-names names are joint and the correlation is
  defined.
- Every member is checked. `MinedRho.undefined` and campaign.json `rho_undefined_row` name the first such row.
- Interpretation: the ruling names members. The greedy step checks earlier kept candidates the same way (they
  become members together), so an undefined pair against one of them also fails. This is pinned in
  `UndefinedRhoFailsTheCandidate`. It is stricter than the ruling's words; on one role and one mask such a pair
  needs sparse signals.

**MINE-14**: `mined_shortlist` no longer caps.
- `promote` builds the shortlist as every above-hurdle trial in f2 order, then hash.
- `rho_step` runs the greedy step over the whole list. The first `--max-promotions` that pass reach the confirm;
  BY's m is their count, as before.
- Memory (the PM's note): the list is walked in the rule's order in batches of the free slots, batch =
  max_promotions - kept. So the kept signals plus the batch never exceed `max_promotions`.
  - With one free slot the batch is a single candidate: the PM's one-at-a-time walk.
  - Each batch is checked against the pool members and every kept candidate. Failing signals are freed at the end
    of the batch. The step stops once the cap is filled.
- The result is exactly the greedy rule's (same order, same ties):
  - a pair's rho reads only its own two rank rows on each date, so the values are bit-identical to one pass over
    the whole list;
  - a list that fits the cap is one batch, which is the old pass.
  - A scratch replica (not committed) checked batched == whole-list-greedy-then-cap over 20,000 random cases,
    undefined pairs and row numbering included, with peak <= cap.
- Rows the step never reaches carry `rho_read` false.
- Cost note: each batch re-ranks the member and kept rows over the discover dates. At worst (one failure per
  batch) there are as many batches as failures.

### Memory model term for PM5-9 (for lane MINE-MEM, `mine_working_bytes`)

- **Peak signal panels held in `promote`: `max_promotions`** (kept + batch <= cap; never cap + 1). The candidate in
  evaluation is part of the batch.
- The model's existing term is therefore exact, and PM5-9 adds no term:
  - `shortlist x C x 8` with shortlist = `--max-promotions`, C = dates x names;
  - the rank rows `(members + shortlist) x names x 8`: one discover date at a time.
- Also present, as before PM5-9:
  - one promotion `al::Engine` at a time (already in the `(W + 1) x C` engine term);
  - the `PairwiseRowCorrelation` sums and counts, `(members + max_promotions)^2 x 16` B: at most 1.6 MB, inside the
    64 MiB metadata term unless MINE-MEM wants it explicit;
  - the `Promotion` vector, now one entry per above-hurdle trial (a few hundred bytes each, bounded by T and inside the
    per-trial 16 KiB allowance).

## Tests

C++ (`atx-impl/tests/strategy_mine_test.cpp`; not compiled):
- **Rule section.**
  - Updated: `ShortlistIsByF2ThenHash` (renamed from `...AndCapped`: no cap now; a NaN factor shortlists nothing),
    `OverlapFactorIsRegisteredAndItsEstimatorIsTheVerbs`, `RhoIsGreedyAgainstMembersAndKeptCandidates` (new
    signature, `read`, `undefined`) and `ConfirmIsOneSidedWithBenjaminiYekutieli` (4 reads read on Fc 1.77).
  - Appended at the end of the section: `FactorsAreReadFromTheirBands`, `ConfirmFactorFollowsTheReadsThatReachIt`,
    `RhoStepRunsOverTheWholeShortlistThenCaps` (cap 2 keeps c and d; the old cap-first rule kept one) and
    `UndefinedRhoFailsTheCandidate` (sparse member, a date floor, min_dates 0, an earlier kept candidate).
- **Fixture campaigns** (placed after `RulePinsOnTheTemplates`, away from MINE-MEM's memory and rung-failure tests).
  Both fail under the old behaviour.
  - `RhoStepReadsTheWholeShortlistBeforeTheCap`:
    - a first stage-1 campaign on {p1, p2} names the template that leads the shortlist;
    - the second campaign makes that field a pool member and sets `--max-promotions 1`;
    - the lead is blocked (|rho| 1), the first passing row further down gets the one confirm read and is admitted,
      and nothing after it is read;
    - the old rule's shortlist was the blocked lead alone, with 0 admitted.
  - `UndefinedRhoAgainstAMemberFails`: two pools whose second member is m2 on 5 names (under --min-names 10) or on
    100 discover rows (under --min-dates 128). Every shortlisted row fails on that member and 0 are admitted; the
    old rule admitted the planted fields.
- **Campaign tests edited** for the new API (cross-lane in this file):
  - `PromotesThePlantedSignalsOnlyInFiveSeeds`: F of the budget's band, and the recipe tables.
  - `RulePinsOnTheTemplates`: now `--budget 1000` explicitly. At the 10,000 ceiling the hurdle 4.56 x 1.63 is
    above the replica's rank(swap). Also the tables, F, and Fc for the swap's raw-IC pin.
  - `RefusesABudgetAboveTheCeilingBeforeAnyPayload`: 10,001 is refused; the ledger accepts 10,000.

pytest (passed here, 20 in 45 to 73 s):
`"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_mine_overlap_factor.py atx-impl/tools/test_trial_ledger_rules.py scripts/tests/test_research_ledger.py`.
`test_mine_overlap_factor.py` covers:
- the header tables against the script;
- the tables following from the pinned record (2-se ceiling, monotone, confirm >= gate, four-year < floor);
- the rule's levels;
- IS against plain draws;
- three cells re-derived at their committed seeds (the gate, m = 16 and N = 1,000) equal to the record within
  1e-5;
- the estimator pins and the daily-IC identity.

## How root verifies

- **check** (single-TU):
  - `atx-impl\src\strategy_mine_rule.cpp`, `strategy_mine_promote.cpp`, `strategy_mine.cpp`;
  - `strategy_mine_trials.cpp` and `strategy_mine_ledger.cpp` (include the changed headers);
  - `atx-impl\tests\strategy_mine_test.cpp`.
- **build**: `atx-impl-strategy-mine-tests`.
- **gtest**: `--gtest_filter=StrategyMineRule.*:StrategyMine.*:StrategyMineCampaign.*`. The new campaign tests run
  4 small stage-1 campaigns (22 templates each).
- **Golden**: `atx-engine` is untouched, so `0x889874a3b9b29c55` cannot move. As a sanity check after the merge:
  `atx-engine-factory-tests --gtest_filter=SignalFitnessDefaults.*:NsgaSearch.ScalarRaw_ReproducesGoldenDigest`.
  `StrategyMineCampaign.SameSeedSameChainHeadAtOneAndFourWorkers` compares the mine fixture's registry heads across
  runs and pins no value.
- **What changes in the mine fixture's outputs.** The trial recipe changes (new keys: tables, ceiling), so the mine
  fixture's registry records and heads differ from 20e7bd19's. No test pins them, and no accepted v8 output comes
  from the mine verb (no campaign has run; OD-7). In the fixture's rule results:
  - F at budgets 128 and 1,000 is 1.54 (was 1.55);
  - Fc is 1.77 or 1.96 (was 1.55);
  - with the cap not binding and every pair defined, the rho values are bit-identical.
- Identity with flags absent: no flag was added. These are mined-v1 rule changes, ruled by PM5-8 and PM5-9 and by the
  brief for the factor, binding only under OD-7. Nothing outside the mining verb and its ledger line reads them.

## Deviations from the brief

- **The recipe carries the tables, not F(budget).** The brief says "the recipe and campaign.json record the F used".
  Putting F of the budget's band in the recipe would make two campaigns on the same windows with budgets in
  different bands two identities. A second confirm read on the same expressions would then pass `registered_trials`
  and the ledger's `check_campaign`, re-opening MINE-3. The recipe therefore records the tables F and Fc are read
  from (budget-independent), and `campaign.json` records the F used (`hurdle.overlap_factor`) and the Fc used (each
  promotion's `confirm_factor`).
- **Fc is keyed on m, not on the budget.** The brief has the table "read by the hurdle and by the confirm's BY p".
  The confirm's levels depend on m and its window floor is 200 rows, so it has its own table, as the brief allows.
- **Rounding policy.** Each factor is the 2-se upper bound, rounded up. MINE-FIX rounded the point estimate up.

## Cross-lane edits

- `atx-impl/src/strategy_mine.cpp` (MINE-MEM):
  - `check_config` ceiling message;
  - `bands_json` plus recipe keys `overlap_bands`, `confirm_bands` (replacing `overlap_factor`);
  - `context.overlap_factor`, `hurdle.overlap_factor` = F used, the `promotions_json` call;
  - usage text.
- `atx-impl/src/strategy_mine_detail.hpp` (MINE-MEM): `PromotionContext::overlap_factor`, `promotions_json(...,
  f64 overlap_factor)`, and the `promote` contract comment. Likely to conflict with MINE-MEM edits to
  `PromotionContext`; the resolution is to keep both.
- `atx-impl/src/strategy_mine.hpp` (MINE-MEM): comments only (budget paragraph, label-overlap paragraph,
  `max_promotions`).
- `atx-impl/tests/strategy_mine_test.cpp` campaign tests, as listed under Tests.
- `atx-impl/tools/backtest_integrity.py`: `MINED_MAX_BUDGET` 1000 -> 10000, same name. `scripts/research_mine.py
  max_budget()` (MINE-RUN) will read 10000.
- `atx-impl/tools/test_trial_ledger_rules.py`: refusal text "at most 10000", one comment.
  `scripts/tests/test_research_ledger.py`: the recipe fixture follows the new layout; the over-ceiling case is
  10,001.

## Open risks

1. C++ is not compiled. The fixture expectations rest on MINE-FIX's numpy replica:
   - `RhoStepReadsTheWholeShortlistBeforeTheCap` needs rank(p1) and rank(p2) to lead the {p1, p2} shortlist ahead of
     every delta or ts_mean template. The replica gives about 5.1 and 6.4 against at most .7 x 6.4.
   - The confirm margins under Fc 1.77 / 1.96: the replica's raw confirm t's are at least about 5.6 for flip's
     rejection and about 6.7 for p1's admission.
2. `evaluate_signals` now runs once per batch, on a fresh promotion engine. This assumes `al::Engine::evaluate` is
   value-pure across engine instances. That is how the search uses it, but it is not re-checked here.
3. PM5-8 is applied to earlier kept candidates too (an interpretation; see MINE-15 above).
4. The 30-name, fully-persistent null is the model behind every factor. The 100-name check reads lower. A
   real-data dependence the null does not have (cross-sectional return correlation, non-Gaussian returns) is outside
   the derivation.
