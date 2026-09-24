# Iteration 16: first alpha scorecard — cp14 configurations restricted to cp15 year-union cuts, 2013–2019

Status: design, pre-run, pre-registration. LEAN path (parent ruling R16-13). Binds R16-1 … R16-13 (`.superpowers/sdd/equity-platform-parent-goal/progress.md:114-125` plus the R16-13 amendment) and proposes R16-14 … R16-21 as PROVISIONAL. No design review follows. Frozen inputs: cp14 design `atx-engine/reviews/2026-09-20-iteration14-cross-section-ic-design.md` (SHA `888c726b123c02b39636876a667cfab890b4304794f6be42d46d35befbedbe25`, `stage_equity_ic.cpp:58-59`); cp15 design `…-iteration15-point-in-time-universe-design.md` (SHA `4810fda2…`); research `.superpowers/sdd/equity-platform-parent-goal/research-cp16-scorecard.md` (396 lines). Neither frozen design is edited. **`equity-ic` and the engine's IC unit are not modified at all** (R16-13a).

## 1. Purpose and honesty statement

**Purpose.** One page saying, for three pre-registered signals on two universe cuts across seven calendar years, whether an annualised net-of-cost long/short decile-spread Sharpe is positive with a bootstrap interval excluding zero and a sign that does not flip between years. The new statistic is computed in Python from the per-date `spread_net` column `ic.csv` already emits (`stage_equity_ic.cpp:634-636`; 10,140 rows in the cp14 run, research §4). No new statistic in C++.

**A "candidate"** is a configuration that survived a pre-registered evaluation on training-window data and therefore deserves the next unit of effort. It is **not** tradeable, deployable, sized or accepted alpha. Not established here: that the spread is achievable (a gross-2.0 paper book, cp14 §3.12 / AR-11 — no borrow availability, no capacity, no impact beyond a flat 5 bps, no fills); that the cost model is right (`trade_bps = 5.0`, `stage_equity_ic.cpp:84`; `annual_borrow_bps = 365.0`, `:87` — a deployed-config convention, not a measurement); any out-of-sample result. **2020-01-01 onward has never been read and is not read here; 2023–2025 remain sealed and are never read.** Validation begins at 2020-01-01, on data this checkpoint does not touch. **A synthetic fixture is never evidence about the market**, and no number in `scorecard.md` may come from one.

**The year-union bias, declared up front (R16-13a).** Because `equity-ic` is unmodified there is no as-of membership predicate. The cut is applied **once, at panel compaction, as the union of the year's members**, so a name that joined the cut in July is in the cross-section from January, admitted on those earlier dates by the panel screen alone. This uses end-of-year membership information to choose the cross-section: **a within-year selection look-ahead.** Its direction is not known a priori — it tilts toward names liquid enough to be ranked at some point in the year, which plausibly flatters breadth and may flatter or dampen momentum. It is **not corrected**; the machinery to correct it exists in `membership.bin` and is deferred (§13). **Every output file and every scorecard row is labelled `year-union, not as-of`.**

**Caveats printed beside every number in `scorecard.md`**, unabbreviated: (1) the year-union bias above; (2) the 19 corrupted pre-holiday sessions and which years they contaminate (§2.6); (3) `IncludeAuditedTerminalV1 ≡ DropMissingForward` outside 2013 (§2.5); (4) `_ex34` excludes 34 names chosen for their 2013 behaviour and is arbitrary elsewhere; (5) cp16 contexts are **not** the cp14 context (§2.3), so the 2013 cp16 row is not the cp14 number — the cp14 row is printed beside it; (6) the 2017 top-3000 cell was pre-declared NOT FIT before any number existed; (7) per-year intervals at `n_obs < 20` are wide and indicative; (8) *"Passing these bars makes a signal a candidate for further work. It does not make it tradeable."*

## 2. Pre-registration block (FROZEN)

Changing any value below is a new pre-registration: new design SHA, new ledger lines, versioned `--attempt`.

**2.0 The 30 configurations — by reference, unchanged.** cp14's: signals `momentum_252`, `momentum_126`, `blend_equal` (§4.1); `H = {1,5,10,21,63}` (§4.2); variants `DropMissingForward` and `IncludeAuditedTerminalV1` — both, always; restrictions `full` and `ex34` — both, always (§4.3, AR-7); `Q = 10`; `min_names_per_date = 2`; `B = 2000`; `kBootstrapSeed = 20260920`; `L_h = max(5, ceil(h/2))`; 5 bps trade + 365 bps annual borrow; `kDayBasis = 365`; seal `RejectSealedV1`. `N_14 = 3 × 5 × 2 = 30` (cp14 §4.4). The binary is unchanged in all of it.

**2.1 The 14 (year × cut) cells.** Cut = `top_n:band`, band `0.00` (`band_bp = 0`), from `C:/atx/data/equity_universe_pit_2013_2019_20260920/membership.bin`. Sizes are the research §3 `+1 prior` column.

| year | top-1000 | status | top-3000 | status |
| --- | --- | --- | --- | --- |
| 2013 | 1,228 | RUN | 3,574 | RUN |
| 2014 | 1,254 | RUN | 3,544 | RUN |
| 2015 | 1,273 | RUN | 3,608 | RUN — measure-first (R16-11) |
| 2016 | 1,251 | RUN (hole) | 3,587 | RUN (hole) |
| 2017 | 1,697 | RUN (hole) | **5,072** | **NOT FIT — 5,072 > 4,096, NOT RUN (R16-4)** |
| 2018 | 1,264 | RUN | 3,555 | RUN |
| 2019 | 1,254 | RUN | 3,550 | RUN |

**13 cells run.** The 2017 top-3000 cell is declared unrunnable **here, before any number exists**: `kMaxIcInstruments = 4096` (`cross_section_ic.hpp:96`) and the union is 5,072 — an artefact of the 2016-12-30 / 2017-01-31 rebalance pair (`PLATFORM_PROGRESS.md:1022-1045`), not real churn. The cap is not raised (RR-1, cp14 §11.5). A cell missing *after* the numbers exist is a selection; a cell declared missing *before* them is a disclosure.

**2.2 Allow-list rule (R16-3, amended by R16-13a).** `M = decode_membership_bin(...)` → `PitMembershipImage` (`point_in_time_universe.hpp:319-332`); `R = M.rebalances`; cut index `c = ti * |band_bp| + bi` (`ti` outer, `bi` inner, `:317`); `members(r) = R[r].cuts[c].security_ids`. For the evaluation year `Y`:

```text
E(Y)      = { r : year(R[r].effective_session_key) == Y }
p(Y)      = argmax { R[r].effective_session_key : effective < Y-01-01 }
A(Y,c)    = ( UNION_{r in E(Y)} members(r) )  UNION  members(p(Y))
A(2013,c) = UNION_{r in E(2013)} members(r)          # p(2013) does not exist
```

`p(2013)` does not exist: the first rebalance is ranked 2012-12-31, effective 2013-01-02 (research §3). `A(Y,c)`, sorted ascending and deduplicated, is the panel's `allow_ids`, applied **once at compaction for the whole context window** — warmup rows and evaluation rows alike. **There is no per-date membership test anywhere** (§1, R16-13a).

**2.3 Panel screen and window (R16-5, R16-10).** Every cp16 context: `--min-adv-usd 0 --min-price 1 --top-n-by-adv 0 --compact-universe true` plus `--universe-membership` / `--universe-cut` / `--universe-eval-start`. These are the PIT floors (`point_in_time_universe.hpp:53-56`); the ranking is membership's, not the panel's, because `panel` hard-codes `adv_window = 21` (`stage_panel.cpp:118`) against the PIT's 63-session ADV — the two cannot be made equal. **A cp16 context is therefore not a cp14 context** (the recipe differs), so the 2013 cp16 numbers are not the cp14 numbers; the cp14 2013 run stays the anchor. One `prepare` + `load` span **2012-03-26 … 2019-12-31** under `tickerhistory-qa-v1` into a new versioned directory; per-year dirs untouched (each has its own `_ingestion.manifest.json` bound to one year's zip, `stage_data_provenance.cpp:38,327-335`, and `--segs` is one directory, `config.cpp:78`, `stage_panel.cpp:145`). Per cell: `--end = (Y+1)-01-01`; `--start` = the 256th attached session before `Y`-01-01 (`kEquityBaselineWarmup = 256`, `equity_baseline_views.hpp:14`, enforced `:96-98`); `D ≈ 508`. Evaluation `[S, (Y+1)-01-01)` with `S = Y-01-01`, **except 2013 where `S = 2013-04-04`** (R16-21). Both stages clamp to `[2013-04-01, 2020-01-01)` (`stage_equity_ic.cpp:386-391`, `stage_equity_baseline.cpp:97-103`).

**2.4 The Sharpe recipe (R16-7) — exact algorithm.** Per `(signal, horizon h, variant, restriction, year Y, cut C)`, over that run's `ic.csv`:

1. **Series.** Rows of that `(signal, h, variant, restriction)` with `spread_emitted == 1`, ascending `date_index`; `S[j] = spread_net`. Rows with `spread_emitted == 0` carry a blank `spread_net` (`stage_equity_ic.cpp:600-603`) and are **excluded, never imputed**. `n_h` is read, not assumed.
2. **Sub-series.** `X_o = [S[o], S[o+h], S[o+2h], …]`. Headline is `o = 0`, anchored at the cell's first emitted date. `n_obs = |X_0| = ceil(n_h / h)`.
3. **Sharpe.** `mu = mean(X_0)`; `sd = sqrt( sum_j (X_0[j] - mu)^2 / (n_obs - 1) )` (ddof = 1); `sharpe_net = (mu / sd) * sqrt(252 / h)`. `sd == 0` or `n_obs < 2` ⇒ blank, `reportable = 0`, reason `zero-dispersion` (R16-16). **Annualisation:** `sqrt(252/h)` is pre-registered; the alternative `sqrt(365 / mean_days_forward)` uses the real calendar gap (`cross_section_ic.cpp:865-867`) and lands **≈ 2 % away** (research OQ-7, lines 392-393) — smaller than every interval printed here, and exactly the knob that becomes a trial if chosen after the fact. Fixed **now**, the only time it is free; `mean_days_forward` stays published and auditable.
4. **Reportability.** `reportable = 1` iff `n_obs >= 8` and `sd > 0` and `B >= 1`. **Weaker than cp14's engine rule** (`n >= 20 && floor(n/L_h) >= 10`, cp14 §3.10), deliberately: cp14's rule governs the overlapping per-date series, `X_0` is non-overlapping. Under cp14's rule **every** per-year `h = 21` cell would be unreportable (`n_obs = 11`). Cost: per-year intervals at `n_obs < 20` are wide, which caveat 7 and R16-20 require to be printed. Predicted `n_obs` from `n_h = T - h` (`T = 252` full year; `T = 189` for 2013):

   | `h` | 1 | 5 | 10 | 21 | 63 |
   | --- | --- | --- | --- | --- | --- |
   | `n_obs`, T=252 | 251 | 50 | 25 | 11 | **3** |
   | `n_obs`, 2013 | 188 | 37 | 18 | **8** | **2** |

   **Prediction: `h = 63` is pooled-only in every per-year cell; everything else is reportable.** Pooled `n_obs` at `h = 63`: `2 + 6×3 = 20` (top-1000), `2 + 5×3 = 17` (top-3000).
5. **CI.** Circular block bootstrap on `X_0`, block length **`L = 5`** (`kBlockLenFloor`, `stage_equity_ic.cpp:78`) — fixed at 5, not `max(5, ceil(h/2))`, because `X_0` is already non-overlapping and `L_h` prices an overlap that is gone. `b = ceil(n_obs/5)` blocks; each draw takes `b` uniform starts `s_j ∈ [0, n_obs)`, emits `X_0[(s_j + k) mod n_obs]` for `k = 0..4`, concatenates in draw order, truncates to `n_obs`; step 3 is recomputed on the resample. `B = 2000`; RNG `splitmix64` → `Xoshiro256pp` (`atx-core/include/atx/core/random.hpp:83`) with Lemire bounded draws exactly as cp14 §3.10, stream key per R16-17. **Interval = the 2.5 and 97.5 percentiles of the sorted draw statistics, nearest-rank round-half-up** (cp14 §3.10:605, §4.3; R16-7 instructs matching cp14 where it differs from its own 5–95 — R16-14). Blank when `reportable == 0`.
6. **Pooled.** `X_pool` = the **concatenation of the per-year offset-0 sub-series in ascending year order**, same `(signal, h, variant, restriction, cut)`. A year absent from the cut (2017 top-3000) contributes nothing and the pooled row says so. Steps 3–5 unchanged.
7. **Sign stability.** Over years whose per-year row is reportable: `n = |{Y : reportable}|`, `k = |{Y : reportable and sign(sharpe_net(Y)) == sign(sharpe_net(pooled))}|`; `sign_matches_pool = "k/n"`. `sign(0) = 0`, never matches. Years are **not** filtered by whether their interval excludes zero — that filter would itself be a selection (research OQ-8).
8. **Robustness only.** `offset_min_sharpe` / `offset_max_sharpe` = min and max of the step-3 Sharpe over `o = 0…h-1`. **Never a headline, never a bar.** At `h = 1` both equal `sharpe_net`.
9. **Implied turnover.** `1 - rho_rank` from `signal_autocorr.csv` (`stage_equity_ic.cpp:640`, cp14 §3.11, AR-5) — per `(signal, cut, year)` only, repeated across that signal's rows (R16-15).

**2.5 Headline cell (R16-6).** Headline row: `variant = IncludeAuditedTerminalV1`, `restriction = full`, `h = 21`, `year = pooled`, per signal, per cut. **Confirmed against cp14: cp14 declares no headline variant** — a search for "headline" returns two hits (lines 811, 2257), neither naming a variant; §3.8, §4.3 and AR-7 (`:430-441`, `:939-944`) instead require both variants and both restrictions always, "never selected between". Nothing in cp14 contradicts R16-6, so its designation stands as a cp16 **reporting label** over a full side-by-side emission: all 30 configurations still run, every row is still printed, and the label — fixed before the run — only names which row the §3 bars read. Pre-declared before any number: **for 2014–2019 `IncludeAuditedTerminalV1 ≡ DropMissingForward`** (the three evidenced terminal events are hard-coded 2013 events — HNZ 2013-06-07, DELL 2013-10-29, MOLX 2013-12-06, `stage_equity_ic.cpp:137-140`), so the variant axis is degenerate outside 2013 and the honest per-year configuration count is 15, not 30; and **`_ex34` is 2013-specific** (the 34 ids come from the frozen `equity_source_reconciliation_2013_20260919`, `:71`).

**2.6 Data-hole prediction and the material test (R16-9).** *Prediction, pre-registered:* the archive carries corrupted `open/high/low` on the session before every NYSE holiday from 2016-01-15 to 2018-02-16 — **19 sessions**, 9 in 2016, 8 in 2017, 2 in 2018 (`PLATFORM_PROGRESS.md:1030-1046`). About half of each such session fails `ohlc_order_violation` under qa-v1 (2016-12-30: 3,957 accepted of 8,102 raw); rejected rows never reach a segment, the cell lands NaN, no forward-fill exists (`stage_panel.cpp:56`), and one NaN close at `d` NaNs `ts_mean(delay(close,21)/delay(close,{252,126}) - 1, 5)` at `d+21…d+25` **and** `d+252…d+256` (`equity_baseline_views.hpp:15-17`) — up to 10 lost evaluation dates per corrupted session per affected name, reaching 2016→2017 and 2017→2018 at `momentum_252`. **Therefore 2016 and 2017 are contaminated for every signal, and 2018 additionally for `momentum_252` and `blend_equal`** (the published combo over both momentum columns, `stage_equity_ic.cpp:798`; R16-18). Attempt 1 runs **as-is on qa-v1**. *Labelling:* `hole_flag = 1` on every row with `year ∈ {2016, 2017}`; on `year = 2018` rows with `signal ∈ {momentum_252, blend_equal}`; and on **every pooled row**. `scorecard.md` names the affected rebalances **2016-12-30** and **2017-01-31**, with the fact that 2016-12-30 dropped 1,674 top-3000 incumbents as `drops_last_bar` and 2017-01-31 reversed it — which is what makes the 2017 top-3000 union 5,072. *The exact "material" test,* on the headline row of the same cut, per signal, with `[lo, hi] = [min(ci_lo(2015), ci_lo(2019)), max(ci_hi(2015), ci_hi(2019))]`:

```text
material(signal, cut) := exists Y in ({2016, 2017} intersect run-years of cut) :
                         sharpe_net(Y) < lo  or  sharpe_net(Y) > hi
```

Attempt 2 (qa-v2 re-ingest, or a hole-aware rank rule using the last full session ≤ the rank date — `PLATFORM_PROGRESS.md:1046-1054`) opens **only if `material` fires for at least one signal on at least one cut**, and is a fresh pre-registration under a versioned `--attempt`. If 2015 or 2019 is unreportable on that cut the test is inevaluable and the default is **no attempt 2**, printed (R16-19). On top-3000 only 2016 is testable, because 2017 is NOT FIT.

## 3. Acceptance bars (R16-8) — written before the run

Per signal, on the headline row at `h = 21`, `year = pooled`, on **both** cuts: (1) **`ci_lo > 0`** — the pooled interval excludes zero; per R16-14 this is the 2.5th percentile, **strictly stronger** than the 5th percentile R16-8 names. (2) **`sign_matches_pool == n/n`** — no reportable year carries the opposite sign. (3) **`implied_turnover` and `breadth_mean` are printed** for that signal and cut.

A signal is a **CANDIDATE** iff bars 1 and 2 hold on **both** cuts and bar 3 is satisfied. **Passing = candidate, not tradeable**; §1 applies in full and is printed beside the verdict. **Failing = not a candidate, and nothing is tuned**: no horizon, variant, cut, restriction, annualisation or offset is re-picked, no year is dropped; the failure is written to the ledger and to `scorecard.md`, and the next checkpoint starts from the failure, not from a knob. A bar may **never** read a row with `reportable == 0` (R16-20).

## 4. Code delta (R16-1 as amended by R16-13a) — three panel-side edits, nothing else

1. **`atx-engine/include/atx/engine/data/history_panel.hpp:47-57`** — add to `HistoryDataConfig`: `std::vector<atx::i64> allow_ids;` (ascending security ids; **empty ⇒ off**, the pre-cp16 path bit-for-bit).
2. **`atx-engine/src/data/history_panel.cpp:312`**, immediately before the `cfg.compact_to_universe` block (`:313-350`): when `allow_ids` is non-empty, zero `mask_out[t*N + i]` for every `t` and every column `i` whose parsed `securityID` is absent from `allow_ids` (`std::binary_search`; caller supplies it sorted, callee asserts sortedness). Compaction then retains exactly `allow_ids ∩ screen` — the cut and the memory fix in one place.
3. **`atx-impl` `panel` flags** — `--universe-membership <path>`, `--universe-cut <top_n>:<band>`, `--universe-eval-start <date>` (the evaluation year `Y`, given explicitly rather than inferred). Parsed in `config.cpp:78-135` in the `--trial-ledger` idiom (`:90-97`); the union `A(Y,c)` of §2.2 is computed in `stage_panel.cpp:143-146`, between the `HistoryDataConfig hc{...}` construction and `build_history_panel`, and assigned to `hc.allow_ids`. `panel` has no closed flag allow-list, so nothing else is touched.

| condition | error |
| --- | --- |
| not all three flags present together | `InvalidArgument`, naming the missing one |
| membership file absent | `NotFound`, path named |
| `--universe-cut` not `<int>:<decimal>`, or `--universe-eval-start` not a date | `InvalidArgument`, value echoed |
| `decode_membership_bin` failure | propagated unchanged (`point_in_time_universe.hpp:330-332`) |
| `top_n` ∉ `image.top_n`, or `round(band*10000)` ∉ `image.band_bp` | `InvalidArgument`, request and available sets named |

**Manifest — additive keys only:** `universe_membership_sha256`, `universe_cut`, `universe_eval_start`, `allow_list_size`. **The recipe `version` string is unchanged**, so `require_context_recipe` (`stage_equity_ic.cpp:407-425`) still accepts the context and `equity-ic` needs no edit. The compacted instrument count is already published in the panel shape.

**Flag-absent invariant.** With the three flags absent `allow_ids` is empty, the `history_panel.cpp:312` branch is not entered, and every path is byte-identical to cp14. Tests: (a) an engine test building a synthetic panel with `allow_ids` empty and asserting a byte-identical serialized payload and mask against the no-allow-list reference; (b) `atx-impl` config tests for each error row above; (c) cp14 §7.5's byte-identity obligation for every other subcommand, re-run unchanged. **No cp14 anchor re-run** (R16-2: it would declare 30 more trials for no information).

**Defaults for what the code leaves unclear.** Cut index `c = ti * |band_bp| + bi` is computed from the decoded vectors, never hard-coded. `"0.00"` → `band_bp = round(0.00 × 10000) = 0`, matched as an exact integer against `image.band_bp`; no nearest-value fallback. The stage asserts `instrument_namespace == spiderrock.securityID` (`stage_equity_ic.cpp:855-859` uses the same ids) before applying the allow-list and refuses otherwise. If `allow_ids ∩ screen` is empty or tiny, `panel` succeeds and `equity-ic` refuses at `min_names_per_date`; the manifest's `allow_list_size` and the panel shape make the cause legible.

## 5. Outputs

**Per cell — unchanged cp14 outputs**, byte-for-byte under the cp14 schema (§7.3): `request.json`, `seal.json`, `coverage.csv`, `ic.csv`, `ic_summary.json`, `ic_decay.csv`, `signal_autocorr.csv`, `quantile_spread.csv`, `manifest.json`. No column added, removed or reordered.

**NEW — `build-equity/audits/iteration16_equity_scorecard.py`**, reading the 13 run directories (and, read-only for the anchor row, `C:/atx/data/equity_ic_training_2013_20260920`). `scorecard.csv` carries one row per `(signal, horizon, variant, restriction, year|pooled, cut)` — `3 × 5 × 2 × 2 × 8 × 2 = 960` rows, the 2017 top-3000 rows emitted at `reportable = 0` with the NOT FIT note rather than omitted. Columns, in order:

```text
signal, horizon, variant, restriction, year, cut, n_obs, sharpe_net, ci_lo, ci_hi,
reportable, hole_flag, sign_matches_pool, rank_ic_mean, implied_turnover, breadth_mean,
offset_min_sharpe, offset_max_sharpe
```

`year` is `2013…2019` or `pooled`; `cut` is `top1000` / `top3000`; `sign_matches_pool` is the `k/n` string on pooled rows, blank on per-year rows; `rank_ic_mean` = mean of `rank_ic` over rows with `emitted == 1`, **asserted equal to that run's own `ic_summary.json` full-sample `rank_ic_mean` to 1e-9** (a free check that the script reads `ic.csv` the way the engine wrote it); `breadth_mean` = mean of `n_used` over the same rows; `n_obs` is over **emitted rows**, not calendar dates, so an interior `spread_emitted == 0` gap shortens the stride grid and the skipped dates stay derivable from `coverage.csv`. Unreportable reals are **blank**, never `0`, never `NaN` (cp14 §7.4). Deterministic: `"C"` locale, LF, fixed column order, rows sorted `(signal, horizon, variant, restriction, cut, year)` with `pooled` last. A header comment carries `year-union, not as-of`.

`scorecard.md` — one page; per `signal × cut`, in fixed order: (1) pooled net Sharpe at each horizon with CI and `n_obs`, `h = 21` marked headline, `h = 63` marked pooled-only; (2) per-year Sharpe row 2013…2019 with `n_obs`, `NOT FIT`, `hole_flag` and `sign_matches_pool`; (3) per-year `rank_ic_mean` row; (4) turnover, with the note that `1 - rho_rank` is a practitioner rule of thumb (cp14 §3.11), not an accounting identity, and not comparable to `decile_one_way_turnover`; (5) `breadth_mean` per year; (6) the §1 caveats 1–8, verbatim; (7) the three §3 bars, each PASS/FAIL with the number that decided it, and the CANDIDATE / NOT-A-CANDIDATE verdict per signal; (8) the NOT FIT cell and the hole flags with their rebalances; (9) the cp14 2013 anchor row, recomputed by this same script from the cp14 `ic.csv`, printed beside the 2013 cp16 rows with the §2.3 sentence.

## 6. Verification — one hand-checked cell (R16-13c)

No oracle and no fixture suite. Instead **one cell is checked by hand before the scorecard is published**: for `2015 top-1000, momentum_252, h = 21, IncludeAuditedTerminalV1, full`, the reviewer extracts the emitted `spread_net` rows from that run's `ic.csv`, takes every 21st from the first, and recomputes `mean`, `sd(ddof=1)` and `mean/sd × sqrt(252/21)` independently, then compares to `scorecard.csv` — **exact for `n_obs`, 1e-9 for the reals**. The extracted series, the hand figures and the comparison go into `build-equity/audits/iteration16-handcheck-2015-t1000.json`; a mismatch blocks publication. This checks the series extraction, the stride, the ddof and the annualisation — everything except the bootstrap, whose interval is therefore labelled *unverified-by-hand* in that audit file and in `scorecard.md`.

## 7. Memory, cost, measure-first (R16-11)

Panel preflight (research §2, verbatim from `iteration6_build_training_context.py:149-152` and `iteration6-context-preflight.json`): `max(137*D*U + M, 375*D*U + 290*D*K) + 512_000_000`, with `K` the **retained (post-compaction)** count. The driver's `K = U` shortcut must be replaced by the real allow-list size: at `D = 508`, `U ≈ 9,500` (`coverage_by_year.csv`), `K = U` gives `665*508*9500 + 512e6 = 3.72 GB` and is refused at 3 GB — a driver constant, not a real breach. With the true `K`: **2015 top-3000** `1,809,750,000 + 531,530,560 + 512e6 = 2,853,280,560 B ≈ 2.85 GB`; **2015 top-1000** `1,809,750,000 + 187,538,360 + 512e6 = 2,509,288,360 B ≈ 2.51 GB`. Both fit; top-3000 barely. The number to trust is the measured extrapolation: cp14's 2013 panel peaked at **1,026,482,176 B** over 666 samples (`iteration6-context-memory-samples.jsonl`) in 66.94 s; the source-cell ratio `508*9500/(445*7650) = 1.42` gives **≈ 1.46 GB**.

**3 GB refusal rule:** every invocation passes `--max-working-bytes 3000000000`; a preflight bound or measured peak above it means the run is **refused, not raised** — the stage returns `OutOfRange`, the runner stops, the failure is ledgered, and the fallback is a thin exporter, **not** the streaming builder (§13).

**Measure-first cell 2015 top-3000**, seven checks with expectations: (1) panel peak WS ≈ 1.46 GB, hard stop above 3 GB; (2) compacted count ≤ 4,096 and ≈ `3,608 ∩ screen` — an overshoot means the `+1 prior` rule is wrong; (3) `equity-ic` ≈ ×3.2 cells vs cp14's 28.30 s / 165,363,712 B (`iteration14-equity-ic-measurement.json`) ⇒ ≈ 90 s / ≈ 0.5 GB; (4) `ic.csv` data rows `(251+247+242+231+189) × 12 = 13,920`; (5) `spread_emitted` fraction ≈ 1.0 — a low fraction means the decile spread is unreportable and the Sharpe has no series to stand on; (6) `n_obs = 3` at `h = 63`, pooled-only, exactly as §2.4 predicts; (7) ledger = 2 lines in the cp16 sidecar with the canonical ledger byte-unchanged. Total cost (research §6): **≈ 1.5 h machine, ≈ 5.8 GB disk** (97 GB free).

## 8. Ledger (R16-2 as amended by R16-13b)

cp16 runs pass **`--trial-ledger atx-engine/reviews/trial-ledger-cp16-restrictions.jsonl`** — a **new, separate, hash-chained file** with its own sidecar, created at the first append (`trial_ledger.hpp:261`). **`atx-engine/reviews/trial-ledger.jsonl` is not opened and its 4 lines and sidecar are byte-unchanged**; the runner asserts its SHA before and after the run.

- **13 cells × 2 lines = 26 lines** in the sidecar file ("Two lines per run, never one", `trial_ledger.hpp:207-208`): one `pre-registered`, one `completed` or `failed`. The NOT FIT cell is never invoked and appends nothing.
- Because the binary is unmodified, each line carries `checkpoint 14`, purpose `training-only-forecast-evaluation`, `trial_count_declared 30` and `trial_id iteration14-cross-section-ic-NNNN` (`stage_equity_ic.cpp:88-89,783-784,820-823`). **That 30 is the unchanged binary's compile-time constant, not a declaration of 30 new trials.** `declared_trials_for_checkpoint` on the sidecar will therefore read `390` after 13 runs; **that number is an artefact of the constant and must never be used as an `N`.** It is recorded here so no later reader mistakes it for a trial count.
- **`N_14` stays 30.** The 13 cells are AR-7 restrictions of the same 30 cp14 configurations: reported side by side, unconditionally, never selected between (cp14 §4.4 `:939-944`, AR-7 `:430-441`). `N` for any future deflated Sharpe is **30**, read from the canonical ledger, which this checkpoint does not touch.
- The AR-7 statement, the year, the cut, the allow-list size and `year-union, not as-of` are written into the per-cell audit JSON (the binary's `notes` field is fixed and cannot carry them).
- A failed run appends its `pre-registered` line and then a `failed` line with the error in `result`; the pre-registered line is never rewritten (append mode only, `:256-258`). A retry is a new `trial_id`; the failed pair stays in the chain, which is what makes a later recount possible.

## 9. Runner — `build-equity/audits/iteration16_run_scorecard.py`

Parent-only, detached, serialized, one native process at a time. `--dry-run` mandatory first (prints every command line, output dir and preflight bound; touches nothing); `--only-year` / `--only-cut` for the R16-11 measure-first pass; `--attempt N` versions every output dir, and existing dirs are refused, never overwritten (`reserve_pipeline_output`, `stage_panel.cpp:141`). Preflight: membership SHA and FNV trailer verify, the cp14 design note hashes to `888c726b…`, the canonical `trial-ledger.jsonl` SHA is recorded. Per cell: `panel` → `equity-baseline` → `equity-ic` (a baseline per cell, since the IC stage requires the baseline window to equal the requested window, `stage_equity_ic.cpp:868-874`), peak WS sampled at 100 ms (`:63-65`); then the scorecard and the §6 hand-check. **Layout (R16-12):** baselines must sit directly under `C:/atx/data` because the required-marks audit is looked up at the baseline's parent under a frozen directory name (`:71`, AR-9; no flag exists) ⇒ `C:/atx/data/equity_scorecard16_{ctx|base|ic}_{Y}_t{CUT}_20260920`, all immutable.

## 10. Seal

Unchanged: `RejectSealedV1`, `apply_calendar_seal`, the CLI window guard. `require_context_recipe` refuses any context ending after 2020-01-01 (`stage_equity_ic.cpp:418-423`) and `stage_equity_baseline.cpp:97-103` clamps identically. **Every cp16 context ends ≤ 2020-01-01** (the largest `--end` is the 2019 cell's `2020-01-01`, exclusive; the prepared span ends 2019-12-31). 2020–2022 are not read; **2023–2025 are sealed and are never read.** No cp16 edit touches a seal constant, and §4 test (c) re-runs cp14's byte-identity obligation to prove it.

## 11. Rulings

**R16-1 (route), amended.** Panel-level allow-list (`HistoryDataConfig::allow_ids`, zeroed into `in_universe` before compaction at `history_panel.cpp:312`) + three `panel` flags + a Python scorecard over `ic.csv`. **No engine IC change, no `equity-ic` change, no cap change.** *Rationale:* `ic.csv` already carries `spread_net` per date (`:634-636`), so the statistic is a post-processor; only the *cut* needs code, because `panel`'s ADV window is 21 (`stage_panel.cpp:118`) against the PIT's 63, so `--top-n-by-adv` cannot reproduce cp15 membership. *Cost if wrong:* ≈ 40 lines reworked.

**R16-2 (ledger / `N`), amended by R16-13b.** The 13 cells are AR-7 restrictions of the 30 cp14 configurations, published side by side and never selected between ⇒ **`N_14` stays 30**; cp16 runs write to a separate hash-chained sidecar ledger, the canonical ledger is untouched, and the per-run `trial_count_declared 30` there is the unchanged binary's constant, not new trials (§8). *Rationale:* it keeps the canonical `N` correct without modifying a frozen, reviewed binary. *Cost if wrong:* if a later reader deems years and cuts a search, the 26 sidecar lines make an exact recount possible.

**R16-3 (allow-list), amended by R16-13a.** Union of members over rebalances effective in `Y` plus the last rebalance effective before `Y` (2013: effective-in-year only), applied **once at compaction** for the whole window; **no as-of predicate**. *Rationale:* including the prior rebalance stops a turn-of-year member vanishing on 01-02; twelve prior rebalances would inherit 2017's contaminated churn into 2018 (5,351 > 4,096, research §3). *Cost if wrong:* the within-year selection look-ahead of §1, declared and uncorrected, plus year-edge membership off by one rebalance.

**R16-4 (2017 top-3000).** 5,072 > 4,096 ⇒ pre-declared NOT FIT, not run, not cap-raised; printed as `NOT FIT (5,072 > 4,096; hole-contaminated)`. 13 cells run. *Rationale:* RR-1 stands (`cross_section_ic.hpp:84`) and the 5,072 is a data-hole artefact. *Cost if wrong:* one cell, disclosed in advance.

**R16-5 (panel screen).** PIT floors + membership; cp16 contexts are not cp14 contexts and the 2013 cp16 numbers are not the cp14 numbers; the cp14 2013 run stays the anchor, printed beside. *Rationale:* membership does the ranking, so the screen must be a floor — but the recipe changes, so the artefact changes, so the number changes. *Cost if wrong:* none; disclosure only.

**R16-6 (variants / restrictions).** All 30 configurations run and are emitted per cell; the design pre-declares the 2014–2019 variant degeneracy and `_ex34`'s 2013-specificity; the headline label is `IncludeAuditedTerminalV1` on the unrestricted sample; every other row is printed side by side, never chosen. **cp14 declares no headline variant** (§2.5), so nothing in cp14 is contradicted. *Cost if wrong:* the label only.

**R16-7 (Sharpe recipe).** §2.4 in full. *Rationale:* the sub-series is non-overlapping, so the annualisation is a clean `sqrt(252/h)` and the block length need not price a vanished overlap; every constant is a cp14 constant (`stage_equity_ic.cpp:77-79`). *Two deviations from its literal text, both authorised by the ruling itself:* CI percentiles 2.5/97.5 rather than 5–95 ("match cp14 if different" — it is; R16-14), and turnover read from `signal_autocorr.csv`, the file that actually carries the `rho_rank`-derived quantity (R16-15). *Cost if wrong:* recomputable from the same `ic.csv` under a versioned attempt 2; the native run need not repeat.

**R16-8 (acceptance bars).** §3 in full; passing = candidate, not tradeable; failing = not a candidate and nothing is tuned. *Rationale:* a bar written after the numbers is not a bar; two cuts and a sign count are the cheapest defences against a result that lives in one slice or one year. *Cost if wrong:* the bars are off by one decision, and every underlying number is printed anyway.

**R16-9 (data hole).** Attempt 1 runs as-is on qa-v1 with the §2.6 prediction pre-registered, `hole_flag` labelling, and remediation gated on the §2.6 material test. *Rationale:* a caveat written after the numbers is an excuse; written before them it is a prediction that can be wrong. *Cost if wrong:* one extra pre-registered attempt.

**R16-10 (data span).** One `prepare` + `load` span 2012-03-26 … 2019-12-31 under qa-v1 into a new versioned dir; per-year dirs untouched; parent-only, detached, `--dry-run` first. *Rationale:* `--segs` is one directory and the per-year dirs carry zip-bound ingestion manifests, so they cannot be merged. *Cost if wrong:* ≈ 35 min and ≈ 3.5 GB.

**R16-11 (measure first).** 2015 top-3000 measured natively before the 13-cell run, seven checks (§7); if panel peak WS > 3 GB: stop, ledger, fall back to a thin exporter — not the streaming builder. *Rationale:* 2015 is hole-free with the largest fitting allow-list; it is the cheapest cell that can falsify the memory model and the `+1 prior` rule at once. *Cost if wrong:* ≈ 4 minutes.

**R16-12 (layout).** Baselines directly under `C:/atx/data` ⇒ `C:/atx/data/equity_scorecard16_{ctx|base|ic}_{Y}_t{CUT}_20260920`, immutable, attempts versioned. *Rationale:* no flag exists for the audit directory (AR-9), so the location is derived and the name is frozen — the layout bends, not the code. *Cost if wrong:* every cell fails at the required-marks check with `NotFound`, immediately and loudly.

**R16-13 (lean path, parent).** `equity-ic` is not modified: no as-of predicate, no cp16 ledger mode, no `kNonTrialPurposes` growth; membership is a year-union allow-list at panel compaction only, labelled `year-union, not as-of` on every output; cp16 runs use a separate sidecar trial ledger; the oracle suite is replaced by one hand-checked cell. *Rationale:* it buys the scorecard without touching a frozen, reviewed binary or a frozen ledger, at the price of a declared within-year selection look-ahead. *Cost if wrong:* the year-union bias is larger than assumed and the per-year numbers need re-running under an as-of predicate — a new checkpoint, not a patch.

**Provisional (R16-14 … R16-21) — for the parent to confirm.** Research open questions 1–8 are answered by binding rulings: OQ-1/OQ-2 by R16-2, R16-4 and R16-13; OQ-3 by R16-9; OQ-4 by R16-4; OQ-5 by R16-5; OQ-6 by R16-6 (moot for `N`, and moot for the ledger since the declared count is the binary's constant); OQ-7/OQ-8 by R16-7.

- **R16-14 — CI percentiles.** `ci_lo`/`ci_hi` = the **2.5 / 97.5** percentiles, nearest-rank round-half-up (cp14 §3.10:605, §4.3), under R16-7's own "match cp14 if different"; R16-8's bar is therefore evaluated at 2.5 %, stronger than the 5 % it names. No second pair is computed, so no knob is added. *Cost if wrong:* one recomputation from the same draws.
- **R16-15 — turnover source.** `implied_turnover = 1 - rho_rank` from `signal_autocorr.csv` (`:640`, AR-5). R16-7 names `quantile_spread.csv`, which carries no `rho_rank`-derived quantity — its `decile_one_way_turnover` is a different object on scale `[0,2]` (cp14 §7.3). The operative phrase is "cp14 `rho_rank`-derived". *Cost if wrong:* one column re-read; both files are published.
- **R16-16 — zero dispersion.** `sd == 0` or `n_obs < 2` ⇒ `sharpe_net` blank, `reportable = 0`, reason `zero-dispersion`. R16-7 is silent and a division would emit an infinity a CSV cannot honestly encode. *Cost if wrong:* degenerate cells only.
- **R16-17 — bootstrap seeding (parent-amended).** The scorecard is stdlib Python, not the engine: for each (signal, horizon, variant, restriction, cut) group, in a fixed iteration order (signals in cp14 order, horizons ascending, variants then restrictions in ic.csv token order, cuts 1000 then 3000), one `random.Random(20260920)` instance is created and consumed by the per-year rows in year order and then by the pooled row; draws are Python's `randrange`, block starts uniform on [0, n_obs). The engine's splitmix64/Xoshiro stream key is NOT reproduced (different statistic, different runtime; reproducing it would add ~80 lines for no evidential gain). *Cost if wrong:* the CI is reproducible from the script alone; it is not bit-comparable to any engine CI, which is stated in the caveats.
- **R16-18 — `blend_equal` hole inheritance.** `hole_flag = 1` on 2018 rows for `blend_equal` as well as `momentum_252`, since `blend_equal` is the published combo over both momentum columns (`:798`) and inherits the 252-session reach. Every pooled row also carries it. *Cost if wrong:* one extra flag; flags never change a number.
- **R16-19 — materiality scope.** §2.6's test runs for each of the three signals on each cut; attempt 2 opens if it fires for any signal on any cut. If 2015 or 2019 is unreportable there, the test is inevaluable and the default is no attempt 2, printed. *Cost if wrong:* one attempt opened or not.
- **R16-20 — unreportable rows.** An unreportable row still prints its `sharpe_net` point estimate with blank `ci_lo`/`ci_hi`; **no §3 bar may read a row with `reportable == 0`**; per-year rows with `n_obs < 20` carry a printed "wide interval — indicative only" caveat. *Cost if wrong:* a reader over-reads a per-year interval, which the caveat exists to prevent.
- **R16-21 — evaluation start, warmup start (parent-amended).** The baseline/IC stages require the evaluation start to be an exact session key with at least 256 attached rows before it (`equity_baseline_views.cpp:96-98`, `<` test, so ≥ 256 rows). Evaluation start S(Y) = the first NYSE session on or after `Y`-01-01: 2013-04-04 (cp14's own 2013 start, the 256th session after the span start 2012-03-26), 2014-01-02, 2015-01-02, 2016-01-04, 2017-01-03, 2018-01-02, 2019-01-02. Panel `--start` = 2012-03-26 for 2013 and `(Y-1)`-01-01 minus 10 calendar days otherwise (≥ 256 sessions; surplus rows are harmless), `--end` = `(Y+1)`-01-01, `--universe-eval-start` = `Y`-01-01 (calendar, drives the union rule only). *Cost if wrong:* a stage refuses the window with an explicit error before anything is written.

## 12. Q&A / ambiguities

The points where the code left something unstated, each with its default, are written where they bind rather than in a separate list: the cut index, band rounding, namespace assertion and empty-intersection behaviour in §4; the emitted-row stride and the blank-vs-zero encoding in §5; the reading of R16-8 as per signal in §3; the no-cp14-re-run anchor in §4 and §8.

## 13. Not in scope

- **The as-of membership predicate** — deferred with the year-union bias declared (§1, R16-13a); a future checkpoint, not a patch to this one.
- **Hole remediation** — qa-v2 and the hole-aware rank rule are attempt 2, opened only by §2.6's test, and are a fresh pre-registration.
- **The streaming panel builder** — excluded by R16-11's fallback, which is a thin exporter.
- **Stage 3 and Stage 4** — allocation, replay, realized turnover, capacity, borrow availability. Nothing here is an executable book.
- **Raising `kMaxIcInstruments`** — RR-1 stands; the 2017 top-3000 cell is NOT FIT, not admitted.
- **Any tradeability claim** — §1 governs every sentence of `scorecard.md`.
- **Any new signal, horizon, variant, restriction, cut, band, cost parameter or knob.** The configuration set is cp14's, unchanged, and this checkpoint adds exactly one output.

Design SHA-256: DESIGN_SHA256_PLACEHOLDER
