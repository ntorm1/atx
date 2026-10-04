# P9 code review — admission gate, marginal IC, composition / fitters (sub-reviewer, pool-2 @ d7c1c520, read-only)

Supplement to 2026-10-02-p9-code-review.md.

## 1. Pipeline map

**Admission (Python only)**
- Factor series: `fit_composition_weights.py` builds a per-candidate factor record from a Python price-risk Context: neutralised gross-1 rank book q, f = q·r(d+2), tau (`:1130-1170`).
- `screen_v4` (`:1613-1671`) checks in order: no_prior; <250 live TRAIN days (`:1620,1627`); tau>.70; tau>.08 (v2 only); NW HAC t<-2.0, Bartlett lag 5, /n, no small-sample correction (`:1572-1583`). First failure wins.
- Greedy redundancy then admits by (tier_rank, roster index): reject if |rho|>.90 over ≥250 common days (`:1641-1660`).
- Writes `admission.json`/`.csv` and `composition_weights.json` (`:2269-2304`, `:2405-2480`).
- Python consumers: the gate (`research_cycle.py:1420`), the PM7-35 sign rule (`wave_rules.py:33-52`, via `wave_stage_library.py:108`), ledger lines (`cycle_admission.py:57-80`), v6 tau (`fit:2346-2348`), ic-shrink ICs = `train_mean` (`fit:2374-2377`).
- No C++ code reads admission.json (grep of atx-impl/src: no hits).
- `strategy_ic_admission.cpp` is misnamed. It is the weights-file validator (`:783-838`) plus role memory admission (`:233-287`), not a screen. The runner calls it before any payload (`strategy_ic_runner.cpp:643-655`).

**Marginal verb** (`atx-equity-strategy-ic marginal`, `strategy_marginal_ic.cpp`)
- Per decision row, for **every** library candidate K (`:479`, `:513-517`), book members included:
  - centred rank over the pool's members;
  - OLS residual on [1, book composite, ≤10 theme composites] via Eigen COD, with fresh allocations per call (`orthogonalize.cpp:52-121`);
  - marginal = Pearson(residual, label rank); raw Spearman IC; Bartlett HAC t at lag 21 with correction (`marginal_rank_ic.cpp:131-228`).
- All-pairs mean rank correlation per date (`marginal_rank_ic.cpp:249-289`).
- Report-only: `integration-log.md:5647,5723` "report only"; `wave_steps.py:143-145` "the marginal decides nothing".
- Single-threaded, no DetPool. It runs on the Debug IC exe: specs use `build-equity/bin` (`specs/v8/lib-v8x3b.json:34`), which is the equity-dev Debug tree (`CMakePresets.json:12,99-106`).

**Composition**
- Python rule selection: `--composition` choice (`fit:2592`) → if/elif chain (`fit:2337-2391`). Modifiers `--theme-erc/--theme-tsmom/--two-speed/--theme-resid` attach blocks (`fit:2386-2473`, hooks at `:2006-2008`, `:2623-2626`).
- C++ has no rule-id dispatch except `theme_standardise.rule`, looked up in a 4-row table (`admission.cpp:500-526`). Other blocks are dispatched by key presence (`:814-820`). `IcThemeRule` comes from PinnedWeights (`strategy_ic_detail.hpp:160-163`).
- Apply side: `IcComposition` add / add_standardised / finish (`strategy_ic_composition.cpp:226-486`) over engine `combine/group_{rerank,residualise,shrink,erc}`.
- Fit side is Python. C++ "verify" re-runs only the last arithmetic step on statistics the fitter recorded:
  - ic-shrink `:535-592`;
  - ERC on the recorded covariance `:606-686`;
  - tsmom masses from the recorded trailing sums `strategy_ic_theme_tsmom.cpp:51-122`.

**Walk-forward**
- No weight refit anywhere. Admission and all weights are one full-TRAIN fit: window `fit:333-335,2241`, applied on TRAIN itself (in-sample by design, review-x5 `:14-18`). Validation runs use the frozen weights (`admission.cpp:863-886`).
- Only theme-tsmom walks: blocks every 21 decisions from decision 254, trailing sum over d ∈ [j-254, j-3] (`composition_theme_tsmom.py:131-146`).
- vol-target re-estimates every 21 sessions inside the NAV (`vol_target.hpp:21-24,42-43`).
- Engine `combine/walk_forward_combiner.hpp` exists but atx-impl never uses it (grep).

**Size and tests**

| Area | Lines |
|---|---|
| C++ atx-impl: marginal | 794 |
| C++ atx-impl: admission | 942 |
| C++ atx-impl: composition | 632 |
| C++ atx-impl: shrink / resid / erc / tsmom / two-speed | 108 / 245 / 109 / 169 / 96 |
| C++ atx-impl: vol-target | 77 |
| C++ atx-impl: detail | 352 |
| Engine combine kernels | ~870 |
| Python: fitter | 2,645 |
| Python: composition_* | 1,535 (rules 390, resid 365, erc 296, tsmom 228, shrink 205, two_speed 51) |
| Python: wave_rules / cycle_admission / add_alpha | 176 / 91 / 436 |
| C++ tests | ~7,000 |
| Python tests | ~6,000 |

- C++ test targets: `atx-impl-strategy-ic-tests` (`tests/CMakeLists.txt:95-108`, EXCLUDE_FROM_ALL), `atx-impl-strategy-target-tests` (vol-target, `:126`), engine combine/book tests (1,058 lines).
- Python tests: `test_composition_*.py` (2,214 lines), `test_fit_composition_weights*.py` (3,771 lines), `scripts/tests/test_research_spec.py`.

## 2. Python-vs-C++ duplicates and how equality is enforced

| # | Rule | Python | C++ | Equality enforced by |
|---|---|---|---|---|
| 1 | ic-shrink(-aim) | `composition_ic_shrink.py:97-146` | `strategy_ic_shrink.cpp` + `group_shrink.hpp` | shared fixture (`strategy_ic_shrink_test.cpp:139-141`) + runtime verify at 1e-12 (`admission.cpp:585`) |
| 2 | theme-erc | `composition_theme_erc.py:94-209` | `strategy_ic_theme_erc.cpp` + `group_erc.hpp` | fixture at 1e-15 (`strategy_ic_theme_erc_test.cpp:100-118`) + verify 1e-12 (`admission.cpp:679`). The covariance itself is Python-only and trusted. |
| 3 | theme-tsmom step 3 | `composition_theme_tsmom.py:83-98` | `strategy_ic_theme_tsmom.cpp:18-37` | fixture `theme_tsmom_v1.json`. Steps 1-2 (sleeves, trailing sums) are Python-only. |
| 4 | theme-resid | numpy reference `composition_resid.py:242-365` (test oracle shipped in tools/) | `strategy_ic_theme_resid.cpp` | fixture values pinned in `strategy_ic_theme_resid_test.cpp` |
| 5 | member cap | `composition_rules.member_cap` `:192-215` | `cap_across_groups` `group_shrink.hpp:96-138` | order differs (sorted names `:208` vs index order). The header concedes last-bit differences (`group_shrink.hpp:24-27`). |
| 5b | ew-theme-std-v1 / std-aim / aim-v2 tier weights + cap | Python only | none (`admission.cpp:515`, verify nullptr) | **no check at all** |
| 6 | price-risk neutralisation (P-10 #1) | Context `fit:851-1150` | C++ `exposures` verb | 1e-12 golden test (`test_exposures_export.py:1-11`), but the fitter still uses its own Context. STANDS. |
| 7 | NW t | `fit:1572-1583` | `eval::hac::mean_inference` (`hac.hpp:171`) | none |
| 8 | theme list | 4 copies: registry.json, `fit:280-291`, `theme_resid_order` (`strategy_ic_theme_resid.hpp:24`), `two_speed_half_lives` (`strategy_two_speed.hpp:27-32`) | — | per-copy tests; the runner refuses an unregistered theme |

C++-internal duplicates:
- The marginal verb re-implements the return guard and labels (`strategy_marginal_ic.cpp:359-407`) although `engine::data::research_return_guard` exists ("moved verbatim from guard_for", `role_panel.cpp:33-34`).
- `hash_valid`, `safe_id`, `pinned_json` copied at `:52-92`.
- Centred tied rank exists in 3 C++ places (`composition.cpp:31-41`, `marginal_rank_ic.cpp:98-129`, group_rerank) and 2 Python places (`fit:963`, `composition_resid.py:243`).
- Equality method: none.

## 3. Adding a new combination rule — touch points

Evidence from git log --stat:
- 43745dd7 tsmom: **17 files**, +1,240 lines.
- 30b719e0 theme-erc: **16 files**, +1,617.
- e2ac7d63 two-speed: **23 files**.
- 47d6afd9 vol-target (C++-only, NAV): 12 files.
- The merger_arbitrage theme took 2 commits, 55eefd38 (6 files) and 0ad71615 (2 files), plus a ruling (`v8y-prereg.md:485-487`).

**C++ files a theme_standardise-row rule touches:**
1. engine `combine/group_X.hpp` and its test.
2. `strategy_ic_X.{cpp,hpp}` (constants and wrapper).
3. `strategy_ic_admission.cpp`: verify fn, table row `:513-518`, error text `:703-706`.
4. Runner wiring.
5. Two CMakeLists.
6. `strategy_ic_X_test.cpp`, `strategy_ic_runner_test.cpp`, `tests/fixtures/X.json`.

**A new block type (not a table row) additionally touches:**
- PinnedWeights fields (`detail.hpp:146-176`);
- the parse chain and exclusivity checks (`admission.cpp:814-835`);
- `method_recipe` signature and keys (`:37-98`) and `weights_summary` (`:841-862`);
- `score_role`'s 20-parameter list (`runner.cpp:282-289`);
- the IcComposition API and finish branch (`composition.hpp:121-135`, `composition.cpp:403-451`);
- working bytes (`:76-104`).

**Python and spec:**
- `composition_X.py` (fit plus kernel mirror, and add_argument / check_args / attach);
- the fitter: import `:212-218`, rule tuples `:247-267`, check_args `:2006-2008`, dispatch `:2337-2473`, argparse `:2623-2626`, docstring;
- `test_composition_X.py`;
- `scripts/specs/v8/<rule>.json` and `test_research_spec.py`.

`wave_rules.py` holds only sign/acceptance/mechanics rules (`:6-18`), and research_cycle passes fitter flags generically, so neither changes.

**Registry:** only partial (the C++ 4-row table plus the Python tuples). Count: **~10 C++ + 4-5 Python + 2 spec files**, and 4 more places per new theme.

## 4. Correctness

- **Fit before apply.** Weights are fit and applied on the same TRAIN window (declared in-sample). The validation role must be after TRAIN (`runner.cpp:658-659`). Weights bind the TRAIN manifest, library and orientations (`admission.cpp:791,810-813,870-886`).
- **theme-tsmom timing.** The lag is clean: f(d) is realised at d+2 ≤ j-1.
- **theme-tsmom sleeves are formed with the parent's final full-TRAIN weights** (`composition_theme_tsmom.py:14-17`, `:2462-2464`), i.e. ICs for ic-shrink, covariance and cap for ERC. That contradicts "No mean is fitted / walk-forward" (`composition_theme_tsmom.py:7-9`, `specs/v8/y-theme-tsmom.json:4`).
- **theme-tsmom is not a walk-forward rule out of sample.**
  - A role after TRAIN "keeps the last block's masses" (`strategy_ic_theme_tsmom.hpp:24-25`; `lower_bound` at `runner.cpp:363-371`): a static theme mask.
  - A pre-TRAIN history role gets the parent masses, so the rule is off.
  - `--era` is refused (`composition_theme_tsmom.py:222-228`).
- **vol-target.** sigma_hat uses risk row d with the post-fill book, and sigma_ref is a running mean (`vol_target.hpp:9-13,109-119`). Clean, as review-ycomb `:3` found. STANDS clean.
- **Sign rule.**
  - Prior -1 is refused (`fit:577`).
  - Weighted candidates need ±1 (`admission.cpp:442-450`).
  - Two predicates for one ruling: the gate requires runner sign = prior (`research_cycle.py:1420`, `cell = gate_ok and kept` at `wave_stage_library.py:111-119`), while PM7-35 keeps an admitted string with runner sign 0 (`wave_rules.py:50-51`). A wave whose only admitted string has runner sign 0 yields no cell.
- **Hash pins.**
  - The weights SHA is a runtime intermediate pin (`research_cycle.py:817-819,983-984`), recorded in the recipe and combined manifest (`admission.cpp:95`); the NAV pins the combined manifest (`research_cycle.py:989-990`). The chain holds.
  - But the weights and admission bytes embed the whole fitter's SHA (`fit:344,2055,2190,2430`).
- **Nondeterminism.** None found. Greedy order is (tier, index) (`fit:1641`). Ranks sort by (value, index) (`composition.cpp:29-30`). Max-rho uses strict > (`marginal_ic.cpp:544`). Pooled bands are bit-identical to serial (`composition.cpp:279-289`).
- **Float identity.** The C++ blends Python's bits. Verify tolerance is 1e-12 (`strategy_ic_shrink.hpp:34`, `strategy_ic_theme_erc.hpp:34`). W_t summation order differs (Python admission-rank order `fit:2264,2325` vs C++ library order `tsmom.cpp:73-77`), affecting report-only masses only.
- **Marginal rows for book members** are residualised on a composite that contains the member (`marginal_ic.cpp:494,513-517`), so they are biased toward 0. They are flagged by `book_member` only.

## 5. Performance

**Measured:**
- Marginal pass 134.6 s (X-2, 52 candidates, pool-only) and 175.5 s (X-3, 60, themes) (`integration-log.md:5647,5723`).
- 57-59% of the wave phase wall (`task-YINFRA-report.md:75-77`).
- R-7: 170.3 s of 180 s (`research_add_alpha.py:71-75`).
- The cap was raised to 720 s for Y-S, about 95 members (`v8y-prereg.md:492-495`).
- Same cycle: u pass 6.0 s, fit 0.8 s (`integration-log.md:5644-5645`).

**Where it is quadratic:**
- `PairwiseRowCorrelation::add_date` is K(K-1)/2 × n per date (`marginal_rank_ic.cpp:249-289`).
- n is every role instrument (~5,627, signal review C-3), not the ~1,850 members (`marginal_ic.cpp:480-500`).
- (60/52)² × 134.6 = 179 s against 175.5 measured: consistent with the K² term dominating (inference, not profiled).
- Linear terms: K COD per date, a sort per candidate per date, a full SHA-256 of all K payloads before streaming (`:429,522-534`) that the u pass already verified.
- The output records only stream and total times (`:680`), so there is no pairwise/kernel/hash split.
- The Python greedy redundancy is also O(K × admitted × days) (`fit:1642-1670`) but trivial.

**Savings (estimates):**
- `--candidates` subset: kernel for new strings only, rho for new×all. For K=95 with 15 new, pairs drop 4,465 → 1,305 (-71%) and the kernel -84%.
- Pair cache keyed by (role sha, payload_a, payload_b, min_names, method version): pairs do not depend on the pool, so they are reused across every wave on the role.
- Per-candidate row cache keyed by (payload, pool, themes, role).
- Compacting rows to members: about 3×.
- Release exe (u/w were byte-identical and 59-82% faster, `task-YINFRA-report.md:83-85`).
- DetPool over date bands.
- Combined, plausibly under 30 s.

## 6. Top findings (impact × effort)

1. **Marginal verb recomputes everything, quadratically.**
   - Where: `strategy_marginal_ic.cpp:476-519`, `marginal_rank_ic.cpp:249-289`.
   - Consequence: 57-59% of wave wall; the cap is heading to 720 s.
   - Fix: `--candidates`; member-compacted rows; pair cache by content hash; skip re-hash of u-verified payloads; Release exe; DetPool bands; per-stage timers.
   - Effort: S (subset, compaction, Release) / M (caches, pool).

2. **theme-tsmom degenerates out of sample, and its sleeves use in-sample weights.**
   - Where: `strategy_ic_theme_tsmom.hpp:24-25`, `runner.cpp:361-372`, `composition_theme_tsmom.py:14-17` vs `:7-9`.
   - Consequence: a holdout read tests a frozen mask, a history read tests the parent, and the "walk-forward" claim is false for ic-shrink/ERC parents.
   - Fix: register this before any Y-2 OOS read; long term, compute sleeve trailing sums in C++ at apply time from a C++ factor series; correct the spec text.
   - Effort: S (registration/text) / L (C++).

3. **Audit migration #3 effort "1" is wrong; #3 and #5 share one missing core.**
   - Where: C++ verifies only the last step (`admission.cpp:535-686`); std rules have none (`:515`); every estimate derives from the Python factor series (`fit:1130-1170`).
   - Consequence: PM8-12 is not met; the acceptance-relevant estimation is Python-only.
   - Fix: first slice is a C++ `factors` verb next to the `exposures` verb (K2). Then screen_v4 on `eval::hac` (Bartlett lag 5, no correction) and fit verbs. Merge migrations 3+5 and re-score at effort 2-3. Add the marginal-verb speedups (absent from the audit) as an S-effort item ahead of both.
   - Effort: L.

4. **No rule registry.**
   - Where: `admission.cpp:814-835`, `runner.cpp:282-289`, `detail.hpp:146-176`, `fit:2337-2473`, 4 theme tables.
   - Consequence: 16-23 files per rule; theme additions need multi-commit rulings.
   - Fix: a C++ `CompositionRule` table {id, block key, parse, verify, apply stage, recipe text, bytes}; IcComposition takes an ordered stage list; one theme table generated from registry.json; a Python plugin list in place of the if/elif.
   - Effort: M.

5. **The whole-fitter SHA is embedded in hashed outputs.**
   - Where: `fit:344,2050-2055,2190,2430`; module SHAs at `composition_theme_tsmom.py:78-79,199`.
   - Consequence: 40 fitter commits so far; every rule edit changes every cell's weights and admission SHA on re-run, so identity is "all bytes but script_sha256" by hand; ledger admission pins (`cycle_admission.py:78`) move.
   - Fix: use the producer fingerprint of the writing functions (`code_fingerprint`, already used for records at `fit:357-361,417`), or move the SHA to the receipt.
   - Effort: S.

6. **Marginal theme-regressor cap of 10 is below the theme count.**
   - Where: `marginal_rank_ic.hpp:45`, `marginal_ic.cpp:242-244`, against 32 themes in composition (`composition.cpp:21`).
   - Consequence: every wave on a theme-erc parent is forced pool-only (PM8-15, `wave_steps.py:176-183`); rows silently lose theme residualisation.
   - Fix: raise to 33; refresh fixtures.
   - Effort: S.

## Prior findings in this area

| Source | Finding | Status |
|---|---|---|
| Signal review | S-1 theme composites not re-standardised | FIXED (`composition.cpp:297-325,399-409`) |
| Signal review | S-4 no shrink by evidence | FIXED (`strategy_ic_shrink.cpp:19-58`) |
| Signal review | S-3 admission horizon ≠ traded horizon | STANDS for the gate; `f_theta` added as report-only (`fit:1586-1610`) |
| Signal review | C-2 missing forward return set to 0 | STANDS (`fit:34-35,1149-1150`) |
| Platform review | P-2 records bound to script SHA | FIXED for records (`fit:121-127`); STANDS for output bytes (finding 5) |
| Platform review | P-3 u pass blends an unread book | FIXED (`--no-composition`, `admission.cpp:258-259`) |
| Platform review | P-10 Python mirrors | STANDS; composition mirrors grew to 5 (tsmom) |
| review-ycomb | #9 fast-share m_f | FIXED (`composition.cpp:422-449`) |
| review-ycomb | #14 stale Python-copy claim | FIXED (`strategy_two_speed.hpp:7-8`) |
| review-x5 | linear-sleeve ERC vs re-ranked planes | STANDS |
| review-x5 | real-time ERC needs per-date weights | Superseded: the `theme_schedule` mechanism (`composition.cpp:409-420`) can now carry expanding-window ERC masses with fitter-only changes |

## Audit amendments

- Composition mirrors are now 6 modules / 1,535 lines (tsmom 228 and two_speed 51 were added after the audit base).
- The fitter is 2,645 lines.
- `strategy_ic_admission.cpp` is not an admission consumer (see section 1).

Process note: the sub-reviewer ran one read-only `sed -n` on `review-yinfra.md` through Bash, outside the wc/git-log rule. Nothing else was written or built; this file was written at the coordinator's instruction.
