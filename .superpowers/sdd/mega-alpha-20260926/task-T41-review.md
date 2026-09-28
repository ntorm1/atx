# Task T41 review: whole-branch adversarial review of the mega-alpha v5 sprint

**Reviewer:** Claude Opus 5.5, 2026-09-27. I made no edits, commits, builds, bounded runs or tool runs on real data.

**Branch:** pool-2 `feat/aes-codex-integration-20260925`, HEAD `ce04d7d5`.

**Scope:**
- **Primary diff:** `41fb5e39..ce04d7d5`, i.e. `review-v5-only.diff` (16 files, +4111/−115). I read all of it in five passes: studies, NAV C++, target C++, generator, and fitter plus tests.
- **Plan-scoped diff:** `d63a7058..ce04d7d5`, i.e. `review-v5-branch.diff`. I checked its file index for overlap between sprints. Every NAV and target-replay C++ change on the branch is v5 (291 and 192 changed lines, the same in both diffs). I skimmed the rest only for the named risks.
- **Result checks:** read-only Python over the `build-equity/` outputs. No tool was re-run on real data.

**Verdict:** ready to merge after one Important fix. It is a one-line C++ change plus a rebuild and the target tests; nothing needs re-measuring. The T40 verdict is correct: the objective was not met on TRAIN, so there is no freeze and no validation run. Every gate statistic I recomputed independently matches T40. No mechanics call depends on which gross-leverage definition is used.

**Counts:**
- Critical: 0.
- Important: 1, promoted from the deferred T36 Minor 1.
- New Minor: 7.
- Deferred minors triaged: 30. One is must-fix (the same item as I1), one is already resolved, and 28 are acceptable as-is.

---

## Spec Compliance (plan §2 D1–D7, §3, §11, Task T41)

✅ The branch is spec-compliant for code. ❌ The T40 report is incomplete on D4 (a document gap, M2).

- **D1 ✅** — met by the pre-registered L re-run, cell `ew t.05 d.1 fixed L1.279`:
  - Gross leverage is 1.00198 averaged over all 756 CSV rows, and 1.00326 over the 753 return rows (this equals the summary's `exposure.mean_gross_leverage`).
  - Mean net leverage is +.0148.
  - Held share is .9837.
  - The L = 1 reference cell fails at .78191. The extra cell follows the R5' rule and is disclosed as +2 trials.
- **D2 ✅** — recomputed. These three runs are identical in `recipe.json` `af058239`, `summary.json` `cfcc2d25`, and all 10 daily and events CSVs (not just S2):
  - `mega-nav-v4-train-b2-f.25`
  - `mega-nav-v5-baseline-check` (exe `b6b21d88`, v5-0)
  - `mega-nav-v5-1-baseline-check` (exe `fb2d3e94`, v5-1)

  The v5 reference cell is also identical under v5-0 and v5-1: recipe `5624e007` and S2 CSV `0b572a25`. So T36 left the fixed-rate path byte-stable on real data.
- **D3 ✅** — W_aim `54f823c1` has a `provenance.aim` block with θ, the lags, per-candidate ρ̄, g and g_unclipped, the half-sample gains, and the coverage-effective theme weights. I re-derived the EW-REFIT check: `composition_weights.json` and `admission.json` equal `9a9c949a` and `880a0a6a` once the script, context and admission SHAs are masked.
- **D4 ❌ (document)** — `task-T40-report.md` lacks two metrics D4 names: cost per unit GMV turnover, and signed mean net leverage (only mean |net| is shown). Both exist only in `build-equity/mega-nav-v5-t40-summ-n13.{txt,json}`. See M2.
- **D5 ✅** — the delisting lane is NO-GO (T33a: 59.9% < 80%) and nothing half-built landed (named risk f).
- **D6 ⚠️** — T42 is still pending. `docs/plans/2026-09-27-mega-alpha-parent-handoff-4.md` was untracked in pool-2 when I reviewed.
- **D7 ✅** — recomputed over 24 run directories (48 receipt files); see (b).
- **§11 review focus:**
  - Items 1, 2, 3 and 6 are pinned by fixtures:
    - `TargetReplayV5.AimPartialV5_ThetaOne_MatchesBaseline`
    - `NavV5.ThetaOneMatchesBaselineBooks`
    - `AimPartialV5_DustDoesNotBlockEntry`
    - `NavV5.PerNameRate_NoLiquidity_UsesMin`
    - `V1BytesUnchangedByAim`
  - Item 4 is pinned by `AimGainRules.test_aim_gain_degenerate_and_gappy`.
  - Item 5 (T32) does not apply because the lane is parked.
- **§3 global constraints:**
  - All 24 v5/v5.1 runs are bounded and completed with exit 0.
  - They read TRAIN only.
  - No v5 script passes `--band-multiple`.
  - The R1'–R3' library, data and admission pins are unchanged.
- **⚠️ Not verifiable from the diff:** the build and fixture pass counts (55/55, 18/18, 67/67, 81 pytest). I took them from the T37 ledger entry and did not re-run them, per the contract.

## Strengths

- **Byte stability is designed in, not bolted on.** Every v5 surface is gated on the rule:
  - `construction_on` (`strategy_target_replay.cpp:600`)
  - `construction_recipe` v5 keys (`:659`)
  - `nav_recipe` rate keys (`strategy_nav_replay.cpp:1138`)
  - `construction.v5` (`:1818-1824`)
  - The new `NavReplayDay` fields never reach the CSV.

  `validate_config` refuses v5 parameters under the other rules (`strategy_target_replay.cpp:104`). Real data confirms it across three binaries.
- **The θ=1 / dust=0 / L=1 path is bit-identical to baseline-v1** at weight level, target level and NAV level, including capped S2/S3 books at cadence 1 and 3. There is deliberately no `theta == 1` shortcut, so rounding matches.
- **The per-name rate fails closed.**
  - `per_name_rate_v1` can never return NaN (`strategy_nav_replay.cpp:1563`).
  - `check_rates` refuses a bad span before any weight moves.
  - The fixed-rate recipe refuses stray rate parameters (`:274`).
  - The shared liquidity cache is pinned to the per-book arithmetic by `PerNameRate_FixedEqualsTradeFraction`.
- **The aim fit reads second moments only.**
  - Ranks are masked outside TRAIN (`fit_composition_weights.py:845`).
  - Aim records are SHA-sealed and bound to the script, context and payload.
  - The v1 path never reads or writes aim records.
  - Byte stability is proven against the pre-T31 git blob.
- **`nav_summ` statistics are correct** and are covered by independent longhand fixtures: `NormalDist`, the Gaussian denominator, and the plan's 0.69/0.91/1.10 null anchors.
- **`v51_train.sh` is the stronger pipeline.** It checks pins between phases, verifies the weights → combined provenance, refuses VAL tokens, and has a DRY mode.

## Issues

### Critical (Must Fix)

None.

### Important (Should Fix)

**I1. The cache-coverage invariant is enforced only by a debug assert.** Location: `atx-impl/src/strategy_nav_replay.cpp:602` (`execute_orders`), together with `fill_liquidity` at `:419-432`. This is promoted from T36 review Minor 1.

- **What's wrong:** under rate per-name-v1, execution reads `cache.adv[i]` and `cache.sigma[i]`, which were formed before MARK. That is only correct while "MARK only cancels orders" holds.
- **What breaks:** if a later edit invalidates that invariant, a release build gets a NaN ADV. The cost model then treats the ADV as unusable and silently fills nothing: the order is counted in `blocked_liquidity` and no error is raised.
- **Why it matters:**
  - It violates house rule `.agents/cpp/agent.md:84`: "Fail loud in debug, fail safe in release … never let a violated invariant silently corrupt state."
  - The next planned edit of exactly this coupling is T32 (terminal returns in `carry_absent`/MARK). That lane is parked, not cancelled.
  - Merging makes this file the base that other lanes on main will build on.
- **Holds today:** both per-name cells show `blocked_liquidity` = 0 on every session in all five scenario books, and every value is finite. I checked this.
- **Fix:** keep the assert and add a release fallback:

  ```cpp
  const auto liquidity = cache.on() && !std::isnan(cache.adv[i])
      ? liquidity_row(c, WindowLiquidity{cache.adv[i], cache.sigma[i]})
      : liquidity_row(c, t, i);
  ```

  Both branches give the same values by construction, so no output bytes change.
- **Root steps after the fix:**
  - Build with a new `mega-build.ps1` tag, targeting `atx-equity-strategy-targets` and `atx-impl-strategy-target-tests`.
  - Run `--gtest_filter=TargetReplayV5.*:NavV5*:*BitIdentical*`.
  - Repeat one D2 check.
  - No grid re-run is needed and no trial is added.

### Minor (Nice to Have)

**M1. The ruled gross-leverage statistic for the gate has no committed producer.**
- **Where:** `studies/nav_summ.py:144` and the C++ summary's `exposure.mean_gross_leverage` both average the prior close over the 753 return rows.
- **The mismatch:** the ruled D1/R6' definition (ledger T30 ruling, used in T40) is the mean over all 756 CSV rows. Only `construction_audit.py` computes that, and it globs `build-equity/mega-nav-v[34]*`, which misses every v5 directory. So L = 1.279 and all 13 mechanics calls were computed ad hoc.
- **Impact:** no verdict flips. The two definitions differ by at most .0014 per cell. The nearest boundary margins are .016 (aim t.08 at .884 against .90) and .022 (aim L1.279 at 1.072 against 1.05).
- **Fix:** have `nav_summ` emit `mean_gross_leverage_all_rows` (and a signed all-rows net) and label which one the gate uses. T42 should cite the tool's number.

**M2. D4 is claimed as MET, but the T40 table is missing two of its metrics** (`task-T40-report.md:13-27`).
- The missing columns are cost per unit GMV turnover and the signed mean net leverage; only mean |net| is shown.
- `nav_summ` does produce cost/GMV-τ (.00080–.00142), but the metric mixes bases: see T31 Minor 3 at `nav_summ.py:152`, where the numerator includes the deployment row and the denominator excludes it.
- **Fix:** in T42 or a T40 addendum, add both columns once the deployment row is excluded from the numerator. Otherwise, state that D4 is met except for this metric.

**M3. The DSR benchmark is narrow, and the report does not say so.**
- **The code is correct:** `nav_summ.py:299-318` implements Bailey–López de Prado. I recomputed the values below.

  | quantity | value |
  |---|---|
  | V[SR_n] | 3.0831e-05 |
  | SR0 | .00946/session (.150 annual) |
  | DSR, reference cell | .839 |
  | DSR, L1.279 cell | .827 |
  | DSR, v5.1 cell | .846 |
  | DSR, per-name cell | .748 |

- **The problem is the benchmark:** V[SR_n] is the variance across 13 near-duplicate cells (paired ρ from .986 to .9998). That gives SR0 = .150 annual, against the plan §4.E null of .91 (N = 10, using the Lo sampling variance).
- **Under the Lo variance** at N = 13, the same cells score DSR .18–.35 (reference .342).
- **Impact:** no decision depends on it, because the freeze rule needs net ≥ 1.0. But "DSR .84" in an owner packet reads as strong evidence.
- **Fix:** T42 should quote both DSRs and say the DSR deflates only the 13-cell construction search, not the v3/v4/v4.2 family search.

**M4. The netting ratio mixes construction with netting.**
- NR = τ_book / Σ w_k τ_k compares a θ = .05 partial-adjustment book against full-rebalance (θ = 1) standalone turnovers from the fitter's `standalone_turnover`. The v4.1 value of .26 came from a banded, fraction-.25 book.
- So .835 does not mean "v5 trades nearly its full standalone turnover" (T40 §3).
- It is computed correctly as the lane contract defines it (`nav_summ.py:355`, with τ taken from the summary).
- **Fix:** disclose this in T42. A pure netting ratio would need the τ of the composite target at θ = 1.

**M5. V[SR_n] depends silently on which directories are listed** (`nav_summ.py:299-305`).
- It is independent of `--dsr-n`. Listing a check directory changes SR0 without any warning. For example, `mega-nav-v5-ref-v50-check` is byte-identical to the reference cell. Omitting a cell has the same effect.
- **Fix:** warn when the number of defined SRs differs from N, and refuse identical net series.

**M6. Per-name-v1 books differ by scenario.**
- `per_name_rates` (`strategy_nav_replay.cpp:722-730`) evaluates θ_i at each book's own `nav_pre`. So the S1, S2, S3, flat and engine-tiers books of a per-name cell hold different portfolios: the mean rate ranges from .0493 to .0508 by scenario. Fixed-rate books share one plan.
- Stress deltas inside per-name cells are therefore not construction-controlled.
- This is disclosed only in the recipe text. **Fix:** add one sentence to T42.

**M7. The gate outputs have thin provenance.**
- The `nav_summ` `--json` output (`build-equity/mega-nav-v5-t40-summ-n13.json`) records the directories, weights SHAs, draws/block/seed and N. It does not record the command line, `nav_summ`'s own SHA or the HEAD commit.
- NAV `recipe.json` and `summary.json` carry no executable SHA; only the bounded-runner `start.json` does.
- Reproduction works today through the ledger and the receipts.
- **Fix:** have `nav_summ` write argv, `sha256(nav_summ.py)` and `git HEAD` into its JSON.

## Named-risk checks (one focused check each)

### (a) Byte stability of baseline-v1 and band paths — PASS (code and real data)

**Code:**
- Construction columns, recipe keys and summary blocks switch on only for `aim_partial`: see `strategy_target_replay.cpp:600` and `:659`, and `strategy_nav_replay.cpp:1818-1824`.
- Rate keys appear only for `PerNameV1` (`strategy_nav_replay.cpp:1138`).
- `write_daily` is unchanged: it is not in the diff. `planned_held_names` and `decision_members` are struct-only.
- Other rules refuse `aim_leverage` and `dust_multiple` (`strategy_target_replay.cpp:104`).
- No key specific to v5 can reach a baseline recipe or summary.

**Outputs:** `recipe.json` `af058239`, `summary.json` `cfcc2d25` and all 10 CSVs are identical across the v4, v5-0 and v5-1 binaries.

**Only side effect:** `NavReplayDay` grows by 16 bytes, which raises every rule's admission reserve by at most 512 KiB (T30 Minor 5). No output changes.

### (b) No validation (2023–2024) or 2025+ read — PASS

**Receipts:**
- I scanned `command` and `bindings` in `start.json` and `receipt.json` for all 24 directories matching `build-equity/mega-*v5*-run*` and `mega-*v51*-run*`.
- They reference only `recent-fast-train-2020-2022-v2` (64 references) and `-fields-v6` (80).
- No 2023, 2024, 2025, `val` or `validation` token appears.
- All 24 runs completed with exit 0.

**Role manifest:**
- `score_start_ns` is 2020-01-01 and `score_end_ns` is 2023-01-01 (exclusive).
- It has 1,155 dates, with warmup from 2018-06-01.
- No 2023 row exists to be read.

**Scripts:**
- `v5_train.sh:25-27,149` and `v51_train.sh:39-41,300` hard-code TRAIN.
- `v51_train.sh:54-60` refuses val and 2023–2025 tokens; it does not refuse 2026+ (T34c Minor 5).
- `v5_train.sh` has no refusal, but it takes no path from the environment.
- Neither script references `v3_validation_once.sh` or `v4_validation_once.sh`.

**Fitter:** `train_mask` covers [`FIT_BEGIN_NS`, `TRAIN_END_NS`) (`fit_composition_weights.py:182-184`), and z is set to NaN outside it (`:845`).

**nav_summ:** it reads only the directories on its command line.

### (c) Per-name rate units and NaN safety — PASS (residual: I1)

**Units:**
- ADV = Σ raw_close × volume / w over [d−w, d) (`window_liquidity`, `strategy_nav_replay.cpp:368`). Volume is raw shares per the header contract, so ADV is in dollars.
- σ is the daily sample SD of adjusted simple returns.
- NAV is the book's `nav_pre` (ratified in T36).
- The formula matches plan §4.B: the fixtures give .0237, .0474 and .106.

**Real data** (S2, ew cell) is consistent with dollars. If ADV were in shares or dollars², the median would sit at a clip.

| statistic | value |
|---|---|
| p05 | .0100 |
| p50 | .0359 |
| p95 | .1500 |
| mean | .0496 |
| share at min | .081 |
| share at max | .067 |

T36 acceptance is met: p50 lies in [.02, .06] and share at min is below .25.

**NaN safety:**
- `per_name_rate_v1` returns `rate_min` for any non-finite or non-positive input and for a NaN quotient.
- A fallback σ is stored as NaN in the cache, so it maps to `rate_min`.
- `check_rates` refuses NaN or out-of-range spans.
- All 10 per-name CSVs are finite in `net_return`, `gross_return`, gross and net leverage, `applied_fraction` and `planned_gross`.

### (d) Aim gain clipping and normalization — PASS

**Code:**
- `aim_gain` (`fit_composition_weights.py:1011-1016`) interpolates lags 0..126 linearly, counts a NaN lag as 0, and returns `min(1, max(.05, g))`. The unclipped ceiling is 1 − .95^127 = .9985.
- `ew_theme_aim_weights` (`:1240`) sets weights to g/(T·n_theme), normalizes globally, and refuses non-finite or non-positive sums.

**Real W_aim `54f823c1`:**
- 31 members.
- g ranges from .358 to .985, and no gain sits at a clip.
- No member has an undefined ρ.
- The half-sample |Δg| is at most .05; none exceeds the .3 risk-register threshold.
- The weights re-derive exactly from the gains (maximum error 0.0).
- No nonmember carries weight.
- v5.1 W_aim `89b146f8` shows the same.

**Note, not a finding:** ρ̄ uses the day-standardized z and does not re-standardize over the names present on both days. That is the plan's own sketch; it attenuates ρ̄ slightly as names turn over.

### (e) Provenance completeness — PASS with gaps M1 and M7

**Chain verified on real outputs:**
- C_aim `00439b98` carries `composition_weights_sha256` `54f823c1`, library `daa9663e`, role `210fff96`, fields `32565c32`, and status `complete`.
- Every NAV recipe carries the combined, role and fields SHAs and the rule, θ, dust, L and rate (with parameters).
- Every NAV summary carries `composition_weights_sha256` and `recipe_sha256`.
- `nav_summ` matched every one of the 13 cells' weights by SHA.
- The executable SHA lives only in the receipts.

That is enough to reproduce every cell today.

### (f) PIT of terminal fields — PASS (nothing landed)

- The branch diff contains none of these: `terminal_kind`, `terminal_return`, `terminal_events`, `build_terminal`, `NavTerminal`, `writeoff_by_kind`, `write-off-terminal`, `terminal-adverse-100`.
- There is no terminal producer in `atx-engine/tools/`.
- No T32 or T33b code commit exists; only the T33a NO-GO document and ledger commits do.
- The sprint directory holds just `task-T32-brief.md` and `task-T33b-brief.md`.

### (g) Gate statistics in nav_summ — PASS (caveats M3, M5)

I recomputed the statistics independently from the S2 CSVs, not through `nav_summ`.

**Moments and DSR:**
- Per-session SR, skew −1.279 and kurtosis 14.532 (non-excess).
- V[SR_n] 3.0831e-05 and SR0 .00946.
- DSR: reference .839, L1.279 .827, per-name .748, v5.1 .846.
- All match T40.

**Standard errors:**
- Memmel SE: .013 (L1.279), .096 (per-name), .011 (v5.1).
- A separate Newey-West (lag 20) delta-method SE gives .012, .095 and .011, which equals the SE `nav_summ` uses for its Ledoit-Wolf studentization.

**Code:**
- The Memmel variance [2−2ρ+½(a²+b²−2abρ²)]/T is correct (`nav_summ.py:170`).
- The Ledoit-Wolf gradient of μ/√(γ−μ²) is right in all four components.
- The bootstrap Ψ* = Σ_j S_j S_j′/T uses the centered resample (Ledoit and Wolf 2008, §3.2.2).
- The CI uses a symmetric |t*| quantile, and the p-value is (#+1)/(M+1).
- The circular block bootstrap uses uniform starts.
- The DSR uses (γ₄−1)/4 with non-excess kurtosis and √(T−1).

## Deferred-minor triage (30 items)

"Acceptable" means acceptable as-is for merge. **DO NOT EDIT** marks files whose bytes are SHA-pinned or embedded in pinned artifacts: editing them re-keys caches or artifacts that the recorded results depend on.

| # | Source | Item | Decision | Reason |
|---|---|---|---|---|
| 1 | T30 m1 | v5 does not force cadence 1 | Acceptable | All 16 v5/v5.1 NAV recipes record `cadence` 1 (checked); the recipe records it. |
| 2 | T30 m2 | Silent fixed-θ fallback on a bad rate span | **Resolved** | T36 `check_rates` refuses at `update_weights` entry; fixture `PerNameRateSpanIsChecked`. |
| 3 | T30 m3 | Unrequested extras (`aim_partial` key, `v5.decisions`, target-replay v5 block) | Acceptable | Ratified; v5-only; baseline byte-stability verified. |
| 4 | T30 m4 | "Recipe minus rule name" not asserted | Acceptable | The behavior is proven bit-for-bit at NAV level; the test is optional follow-up. |
| 5 | T30 m5 | `NavReplayDay` +16 B raises the reserve | Acceptable | No output effect (D2 identical on real data). |
| 6 | T30 m6 | Entry at L > 1 checked only in aggregate | Acceptable | Real L1.279 cells hold .984 / .984 of members. |
| 7 | T31 m1 | `provenance.aim` has extra keys | Acceptable | Ratified; report-only. Fitter: DO NOT EDIT (`SCRIPT_SHA256`). |
| 8 | T31 m2 | Coverage-effective weight uses fractional coverage | Acceptable | Ratified; recorded in `coverage_definition`. |
| 9 | T31 m3 | `cost_per_gmv_turnover` mixes bases | Acceptable for merge | Fix in nav_summ before T42 cites it (see M2). |
| 10 | T31 m4 | No gross cross-check vs summary | Acceptable | nav_summ equals `summary.exposure.mean_gross_leverage` to 1.2e-15 on all 13 T40 cells (checked); the real gap is M1. |
| 11 | T31 m5 | `print_scenarios` crashes on a null tau | Acceptable | Fails loud, not silent; never null in v5 runs. |
| 12 | T31 m6 | A single unmatched `--weights` still gives a numeric NR | Acceptable | Flagged UNMATCHED; T40 matched all 13 by SHA (checked). |
| 13 | T31 m7 | `v5_train.sh` edges (silent `w` failure, C_aim pin from the same file, LEV string, REF spelling) | Acceptable | All v5 runs completed; I verified C_aim → W_aim provenance on disk. |
| 14 | T31 m8 | Third copy of the git-blob byte-stability harness; `aim_record_valid` repeats `record_valid` | Acceptable | Test-side refactor is free follow-up. Fitter side: DO NOT EDIT (any edit re-keys the work cache and the embedded script SHA). |
| 15 | T31 rr-m1 | Misleading "single cell" fallback label | Acceptable | Cosmetic. |
| 16 | T31 rr-m2 | One bad dir suppresses earlier output | Acceptable | Fails loud. |
| 17 | T34b m1 | Slot-limit provenance comment | Acceptable | DO NOT EDIT: `generate_fund_ic_v5.py` bytes feed `generation.generator_sha256` in the recipe, so any edit changes recipe `a26670b0`, which `v51_train.sh` and the fitter pin. |
| 18 | T34b m2 | Stale v4 trial counts in the v5 recipe JSON | Acceptable | DO NOT EDIT (SHA-pinned); disclose in T42 that the `v51_family` block governs. |
| 19 | T34b m3 | profitability_quality description changed | Acceptable | Ratified; the runner reads the id only. |
| 20 | T34b m4 | Test polish | Acceptable | Tests only. |
| 21 | T36 m1 | Cache coverage enforced only by debug assert | **MUST-FIX before merge** | Promoted to I1: house rule `agent.md:84`; T32 will edit MARK; one line, no output change. |
| 22 | T36 m2 | NAV_d = `nav_pre` | Acceptable | Ratified before measurement; effect bps^½. |
| 23 | T36 m3 | `per_name_rate_declaration` re-types T30 text | Acceptable | Frozen recipe strings; editing would change per-name recipe bytes against the measured cells. |
| 24 | T36 m4 | `NavRateOptions` duplicates the 5 rate fields | Acceptable | Refactor when a sixth parameter is added. |
| 25 | T36 m5 | `--rate-lambda` CLI and interior quantiles untested in the CLI fixture | Acceptable | Struct-level coverage exists; real per-name `rate_stats` are sane. |
| 26 | T34c m1 | u-pass cap of 3 | Acceptable | 1 pass was used. |
| 27 | T34c m2 | Directory naming | Acceptable | Collision-free; the ledger names the dirs. |
| 28 | T34c m3 | A failed nav run exits 0 | Acceptable for merge | The root reads receipts, and all runs completed. Make it fail-closed before any future use. |
| 29 | T34c m4 | `record` can overwrite or write empty pin files | Acceptable | Fails closed later (on `pin`). |
| 30 | T34c m5 | 2026+ not refused; THETA/DUST/LEV not numeric-checked; dead `else` branch | Acceptable | Operator-only; structural TRAIN whitelists. Add before any validation-era reuse. |

## Assessment

**Merge readiness:** ready after I1.

**Before merge:**
- Apply the one-line fallback at `strategy_nav_replay.cpp:602`.
- Root rebuilds and runs `TargetReplayV5.*:NavV5*:*BitIdentical*`.
- Repeat one D2 check.
- No real-data re-measurement and no trial is needed.

**Before T42 cites numbers:**
- M1: `nav_summ` emits the ruled all-rows gross.
- M2: add cost/GMV-τ (deployment excluded) and the signed net.
- M3: quote both DSRs.
- M4 and M6: disclosure.

**Also:**
- The actual merge is owner gate U5.
- main has diverged; I did not assess merge conflicts.
- The branch deletes 581 `studies/.mypy_cache` files that are still tracked on main (the intended cleanup in `c6896ec9`).

**Reasoning:** the v5 code does what the pre-registration says:
- Baseline paths are byte-stable on real data.
- The aim gains are clipped and normalized as R4' specifies.
- The per-name rate is in dollar units and NaN-safe.
- No TRAIN-external data is read.
- The gate statistics recompute exactly.

The one blocker is a release-build fail-safe that house rules require and that costs one line.
