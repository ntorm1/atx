# T16 — v3 post-mortem: verify the math and measure the construction's overfit (analysis script)

Context: frozen daily v3 book (FREEZE v3-daily-2026-09-27) scored TRAIN in-sample S2 x swap-fin-v1 net
SR 1.807 (gross 2.741) but VALIDATION net -1.271 (gross +0.161). Root already confirmed: NAV headline
stats reproduce from the daily CSVs; NAV chain pre[t+1]/pre[t]-1 == net[t+1] to 1e-16; costs stable
(TRAIN tc 1.97%/yr, VAL 1.67%/yr) — the gross alpha collapsed (6.99%/yr -> 0.23%/yr). Root also found:
across the 121 v3 candidates, oriented FIT(2020-21) Sharpe vs HOLD(2022) Sharpe Spearman = 0.04 (p .66);
only 55.4% kept their sign in 2022 (coin flip = 50%). Admission kept 23 (54 unstable, 44 redundant).
Weights mv-shrink-0.9-nonneg-v1 were fit on ALL of 2020-22 (including the HOLD year used by the screen),
and the band was picked on the same data.

You write ONE bounded analysis script + synthetic fixtures. The ROOT runs it on real data. You never run
it on real data and never read validation data yourself.

## Deliverable

`.superpowers/sdd/mega-alpha-20260926/studies/postmortem_v3.py` (commit it with `git add -f` if the dir is
ignored) and `studies/test_postmortem_v3.py` (pytest, synthetic only). CLI:
`python postmortem_v3.py --output DIR [--sections A,B,C,D,E,F] [--reps N] [--seed S]`, all paths via
arguments with the defaults below, writes `DIR/postmortem.json` + `DIR/postmortem.md` (compact tables).
Must fit the root guard: <= 180 s wall, <= 1536 MiB RSS per invocation; sections must be runnable
separately (`--sections`) so the root can split work across invocations; print per-section timings.

Inputs (read-only, relative to C:/atx-wt/pool-2; make each an argument):
- fitter + its per-candidate work cache: `atx-impl/tools/fit_composition_weights.py`,
  `build-equity/mega-fit-work/185eae59c2da7ef6/` (TRAIN f_k daily factor returns + tau_k). IMPORT and
  reuse the fitter's own functions (orientation, tau, greedy redundancy, v3-admit-v1 screen,
  mv-shrink-0.9-nonneg-v1) — do not re-implement the pipeline; where a function hard-codes the window,
  add a minimal window parameter to the fitter (keeping default behaviour byte-identical; run its
  existing tests `atx-impl/tools/test_fit_composition_weights.py`, which are pure Python).
- frozen outputs: `build-equity/mega-weights-v3-plain/{admission.csv,admission.json,composition_weights.json}`
- library `atx-impl/strategies/pv_fields_ic121_v3.json` (for families / any declared prior sign).
- TRAIN combined `build-equity/mega-v3w-plain-train-1/train_combined.*` (+ `.json` layout metadata:
  date-major little-endian f64, finite/member u8 masks, ids u64, sessions i64) and TRAIN role
  `build-equity/recent-fast-train-2020-2022-v2/manifest.json`.
- VALIDATION (book-level only, root runs): `build-equity/mega-v3-VAL-1/validation_combined.*`,
  role `build-equity/recent-fast-validation-2023-2024-v1/manifest.json`.
- NAV daily CSVs: `build-equity/mega-nav-v3-plain-b1/`, `build-equity/mega-nav-v3-VAL/`
  (`daily_modeled-1bn-stale5-v1+swap-fin-v1.csv`; `return_observation==1` rows; columns
  `gross_return`, `net_return`, `session_ns`).
- Reuse loaders from `studies/nav_recon.py` (role manifest -> close/volume arrays, timing d->d+1->d+2).

## Sections

A. Reproduce (TRAIN): run the fitter pipeline from the work cache on the full window; assert weights ==
   frozen `composition_weights.json` (report max |dw|) and admitted set == admission.csv. Report
   in-sample combined factor series F_t = sum_k w_k s_k f_k,t: annualised Sharpe (sqrt 252), by year, and
   each year's share of total P&L.
B. Walk-forward process estimate (TRAIN only). Re-run the pipeline fit on a sub-window and score the
   combined F on the following untouched window: B1 fit 2020-21 -> score 2022 (screen WITHOUT the
   HOLD-sign test, since 2022 is the target); B2 fit 2020 -> score 2021; B3 fit 2021 -> score 2022;
   B4 fit 2021-22 -> score 2020 (backward). Report IS and OOS Sharpe per split.
C. Construction alternatives under the same splits (B1 and fit-2020 -> score 2021-22): C1 equal weight all
   121 with FIT-oriented signs; C2 equal weight all with the library's declared prior sign where one
   exists (skip + say so if the library has none); C3 equal weight of admitted; C4 inverse-vol admitted;
   C5 mv-shrink (frozen rule); C6 family-level: within-family mean then equal across families
   (FIT-oriented). Table: rule x split -> IS SR, OOS SR.
D. Null (selection-bias) simulation on TRAIN: demean each f_k over the full window, circular block
   bootstrap of DATES (block 21, same blocks for all candidates -> keeps cross-correlation and
   autocorrelation), run the full frozen protocol (FIT orientation, tau, HOLD-sign screen, greedy
   redundancy, mv-shrink weights on full window), record in-sample combined Sharpe, admitted count,
   FIT-vs-HOLD Spearman, frac sign-kept. `--reps` default 200 (vectorise; make reps split-able across
   invocations with `--seed`). Report the observed value's percentile in the null for each statistic.
E. Paper-book math check (book-level): from the saved combined arrays + role closes build the
   runner-equivalent paper book per decision d (tied-rank centered, gross 1 over member&finite names;
   match the fitter's `factor` semantics string in admission.json `rules.factor` as closely as the
   combined allows) with forward return r[d+2] (same timing as NAV/fitter). Report for TRAIN and VAL:
   paper gross Sharpe, correlation of the paper daily series with the NAV `gross_return` series aligned
   by session, and a lag test (r[d+3]: one extra day of delay). Also check metadata only: that the
   validation run used the pinned signs and weights (compare the validation `summary.json` /
   `validation_candidates.jsonl` sign/weight fields to `composition_weights.json`). HARD RULE: output
   NO per-candidate validation performance (no per-candidate IC/Sharpe/returns for 2023-24) — only
   book-level numbers and metadata equality.
F. TRAIN attribution: per-family and per-candidate contribution to in-sample F P&L (w_k s_k sum f_k) and
   by year; concentration (top-5 share).

Fixtures (synthetic): tiny f_k matrices where (1) known weights reproduce, (2) a pure-noise panel yields
walk-forward OOS ~ 0 while IS > 0, (3) the bootstrap keeps cross-correlation, (4) the paper book on a
3-name toy with known returns gives the exact hand-computed series, (5) the no-validation-per-candidate
rule: section E output keys contain no candidate ids for the VAL side.

Report `task-T16-report.md` (same dir): what each section computes, exact commands for the root (with
section splits sized for 180 s), any fitter change (with its test result), open questions.
