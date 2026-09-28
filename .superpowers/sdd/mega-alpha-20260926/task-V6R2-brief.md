# Task V6-R2: report iteration 2 -- strategy pitch to a quant PM (narrative + glossary + feature analysis + correlations)

**Pool:** C:/atx-wt/pool-4. First `git status` clean (discard nothing that is tracked; the untracked leftovers of the stopped
V6-R lane may be deleted), then `git checkout -B feat/mega-alpha-v6-report2-20260928 <pool-2 HEAD>` (`git -C C:/atx-wt/pool-2
rev-parse HEAD`; it contains atx-impl/tools/mega_report and docs/plans/mega-alpha-v6-report.config.json = iteration 1).
Read C:/atx-wt/pool-2/build-equity/** by absolute path (TRAIN outputs only; never anything named validation/VAL/2023/2024/2025;
never a per-candidate VAL statistic). Never build C++, never run the pipeline, never spawn subagents, never touch C:/atx.
Pure-Python analysis over existing outputs is allowed and expected. Commit trailer: `Co-Authored-By: Claude Opus 5.5
<noreply@anthropic.com>`. Report: C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/task-V6R2-report.md. Reply < 12 lines.

**Owner's words (binding):** "Do a second iteration of the report generation, treat it as a strategy pitch to a quant PM.
Explain symbology used in the report + the underlying process used to generate the alphas and the backtest. Include
figures, feature analysis, correlation matrices, everything you would need to explain the strategy in depth to pitch it as
a production strategy." Prose IS now wanted, written for a quantitative PM: precise, sceptical, no marketing. Every claim
must be backed by a number in the report or a cited file; honest caveats are part of the pitch (TRAIN only, sign-only
acceptance inside one SE, DSR gate .911 < .95, 2020 contributes little, cost model assumptions, borrow realism).

## Sources to read first
- `docs/plans/2026-09-28-mega-alpha-v6-handoff-5.md` (process, rulings, disclosures D1-D15), `docs/plans/2026-09-28-mega-alpha-
  scorecard-v6.md`, `.superpowers/sdd/mega-alpha-20260926/v4-prereg.md` (all "## v6 revision" / "## v6.1" text: the protocol),
  the top ~250 lines of `progress.md` (ledger), `v6-literature.md` (citations per theme), `v6-code-review-exec.md` §3 (cost
  model components: spread/commission 6 bps, sqrt impact, 1% ADV participation cap, stale-5 handling, swap financing long 40 /
  short GC 30 / warm 100 / special 500 bps ACT/360, terminal-adverse), `task-V6L-report.md` §1-4 (library v6 definitions and
  citations), `task-V61-report.md` (sv_flow), `task-V6-branch-review.md` (disclosures).
- Code you may cite for mechanics: atx-impl/src/strategy_nav_replay.cpp (aim-partial-v5, delta orders, exit rate, locate-in-aim,
  neutralisation price-risk-v1 via strategy_price_exposures.cpp, cost model), atx-impl/tools/fit_composition_weights.py
  (admission v4-prior-v1: prior sign, redundancy |rho| > .90, tau limit; composition ew-theme-v1), atx-engine/tools/
  prepare_recent_research.py (universe research-prior63-usd-adv-topn-v1 top-3000 by 63-day dollar ADV, linked-operating-v1),
  prepare_research_fields.py (PIT field construction, 22:00 UTC visibility, lags, staleness).
- Data for feature analysis (all TRAIN): `build-equity/mega-weights-v61-ew/admission.json` (per candidate hac_t, tau, train_sharpe,
  train_mean, max_abs_rho, status, s_k), the u pass dir of library v6.1 on lo1 (find it: `build-equity/mega-v61*u*` -> the one
  with train_daily_ic.csv, train_candidates.jsonl (per-candidate horizons: coverage, pearson/rank IC mean, SE, HAC lag),
  train_planned_targets.csv), `build-equity/mega-candidate-cache-v61/*` (per-candidate signal payloads: inspect format;
  if usable, compute the cross-sectional signal correlation matrix per day averaged over TRAIN), the final cell dir
  `build-equity/mega-nav-v61u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247` (daily_*.csv: long/short dollars, fills, capped fills,
  unfilled, blocked, planned turnover, cost components; events_*.csv), summary.json (financing tiers, per-year), and the
  same for the v6 final and the v5 deployable cell for comparison. Fields manifest `build-equity/recent-fast-train-2020-2022-
  v2-lo1-fields-v7/manifest.json` (coverage per field per year; source checks).

## Deliverables (extend the iteration-1 package; keep every existing component and the config-driven design)
1. `atx-impl/tools/mega_report/`: add `narrative.py` (prose blocks keyed by section id, with {placeholders} filled from the
   data so numbers never go stale), new components `corr_matrix` (symmetric heatmap with ordered labels, diverging scale,
   values in cells when n <= 45), `ic_panel` (per-candidate IC mean with SE whiskers by horizon), `exposure_chart`
   (stacked long/short dollars and net over time), `fill_panel` (capped/unfilled/blocked fills over time), `glossary`
   (two-column definition list), `flow_diagram` (SVG boxes-and-arrows pipeline: data -> fields -> DSL alphas -> admission ->
   composition -> construction -> costs -> gates; drawn with simple rects/paths, not hand-written long path data),
   `callout` (a boxed caveat). Analyses in `analysis.py`: candidate IC-series correlation matrix (from train_daily_ic.csv),
   candidate signal correlation matrix (from the cache payloads, if the format is readable; else state not available),
   theme-level correlation (average within/between), per-year IC by theme, turnover vs |IC| scatter, alpha decay proxy
   (IC by horizon from train_candidates.jsonl), cost decomposition (linear vs impact vs financing per year), capacity
   curve proxy (cost/$ vs L from the L 1 and L 1.247 cells; state the two-point limitation), drawdown table (top 5
   drawdowns with dates and recovery), monthly return distribution stats (skew, kurtosis, worst 5 days), gross/net
   attribution by year and by scenario.
2. `docs/plans/mega-alpha-v6-pitch.config.json` (new config; iteration-1 config untouched) with a `narrative` section and
   this layout, each section opening with 1-3 short paragraphs of PM-grade prose and a "what to check" list:
   0 Executive summary (thesis in 5 lines; KPI strip; gate panel; the honest caveats callout);
   1 Symbology and glossary (every symbol/abbreviation used anywhere in the report: S1/S2/S3/flat-300/engine-tiers, tau,
     gross/net leverage all rows vs post-ramp, L, theta, dust, exit rate, delta orders, locate-in-aim, R6', dSR, Memmel SE,
     CBB, LW, DSR cross-cell vs Lo, HAC t, IC, ew-theme-v1, v4-prior-v1, aim-partial-v5, price-risk-v1, lo1 / role v2,
     fields-v7, PIT, TRAIN/VAL/holdout, trial, Appendix A, cell naming convention `mega-nav-<lib><role>-<comp>-t<theta>-
     d<dust>-<rate>-ob<basis>-x<exit>[-loc][-L<lev>]`);
   2 Data and universe (sources, PIT rules, field coverage table by year, universe definition, linked-operating restriction
     with dropped-share breakdown, survivorship notes; flow diagram);
   3 Alpha library (themes, each member: definition in words + DSL + citation + prior sign + TRAIN HAC t / tau / IC by
     horizon; IC panel; alpha-decay figure; turnover vs |IC| scatter; what changed v5.1 -> v6 -> v6.1 and why, citing the
     literature review);
   4 Admission and composition (v4-prior-v1 rules, why prior-signed and equal-weight instead of fitted weights (DeMiguel
     et al. 2009 logic), redundancy rule, correlation matrices (IC-series and signal), theme weights, what was rejected
     (ew-theme-v6) and why the sign rule rejected it);
   5 Portfolio construction (aim-partial-v5 mechanics as equations in words, delta orders, exit rate, locate-in-aim,
     neutralisation, leverage calibration; lever ladder waterfall; grid table; exposures chart; fill panel; turnover
     distribution);
   6 Cost model and capacity (S1/S2/S3 definitions with the actual parameters, financing tiers, cost decomposition by
     year, cost/$ vs turnover scatter, capacity proxy, what the model does not include: borrow fees by name, short-sale
     constraints, market-on-close realism);
   7 Backtest protocol and statistics (pre-registration, sign-only acceptance, paired dSR with Memmel SE / CBB / LW,
     deflated Sharpe both benchmarks with the N used, trial accounting, what "one SE" means for a 3-year window, why
     validation #3 is unspent);
   8 Results (equity curves, drawdowns table, monthly heatmap, rolling Sharpe, per-year attribution, stress table,
     the 29-cell table);
   9 Risks, open items and path to production (regime concentration 2021-22, 2020 near zero, DSR gate, borrow realism,
     parked findings from the reviews, the U1 decision, monitoring plan: what to track live vs the TRAIN distribution);
   Appendix: reproducibility pins, trial accounting, input file SHAs.
3. Generated `docs/plans/2026-09-28-mega-alpha-v6-pitch.html` (self-contained; same design system as iteration 1: Source
   Serif 4 / IBM Plex Sans / IBM Plex Mono, cool neutrals, navy accent, both themes, academic figure/table numbering,
   running text <= 70 characters wide, no cards/gradients/emoji). Iteration-1 report must still regenerate unchanged
   (byte-identical except the generated-at stamp) from its own config: prove it.
4. Tests: component self-tests for the new components; analysis functions tested on synthetic inputs; a regeneration test.

Report: files + sizes, the regenerate command for both reports, which analyses could not be computed and why, and a list
of every prose claim that you could NOT back with a number (so the controller can cut it).
