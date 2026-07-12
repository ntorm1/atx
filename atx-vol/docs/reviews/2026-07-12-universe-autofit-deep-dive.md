# Whole-Universe Auto-Fit Deep Dive — OPRA 2026-07-01 10:00 ET, Russell-3000 proxy

**Claim under test** (vola.dynamic): "Super-fast and robust. Fit the whole US options
universe on one box!" This exercise points atx-vol's auto-select fitting pipeline at a
real full-universe OPRA snapshot and documents exactly where it breaks, crawls, or
mis-routes — the input for the follow-up hardening sprint.

## Method

- **Universe**: Russell-3000 proxy = top 3000 US common stocks by 21-session average
  dollar volume (EQUS.SUMMARY hive, window 2026-05-07..2026-06-05), ETF/fund names and
  unit/warrant symbols excluded (`atx-vol/tools/build_r3000_proxy_universe.py`;
  `data/universe/r3000_proxy_symbols.txt` + `_meta.csv`). Not official FTSE membership;
  adequate for a fitting stress test.
- **Snapshot**: OPRA.PILLAR `cbbo-1m`, single minute [14:00, 14:01) UTC = 10:00 ET,
  2026-07-01, parent symbology (`SYM.OPT`), pulled via
  `atx-vol/tools/pull_opra_universe_snapshot.py` into the `{symbol}/{date}.parquet`
  hive `load_opra_daterange` reads (px int64 1e-9, unset INT64_MIN; UNDEF→INT64_MIN
  conversion applied). Cost preflights returned $0.00 on this account's OPRA license;
  raw DBN chunks cached under `_dbn/` (content-addressed, idempotent re-split).
- **Harness**: `atx-vol/examples/universe_autofit.cpp` (new). Per symbol:
  `corpus_board_from_opra` → `OptionChain::from_frame(frame, env)` → `PricerFitter`
  with `PricerConfig::curve` UNSET (unified fit policy + CurveSelector auto-routing),
  `context = board.fit_context`, per-board serial fit, board-level `parallel_for`
  fan-out. Records per-symbol status/error, routing decision, chosen family, selector
  scores, `SessionDiagnostics`, valuation NaN rates, per-stage wall times → CSV.
  Analyzer: `atx-vol/tools/analyze_universe_autofit.py`.
- Flat r = 0.043 (no term structure supplied — itself a universe-scale gap: no
  per-date yield pillars, no per-name borrow/dividends fed in).

## Pull-side findings (data plumbing)

1. **Databento gateway fragility**: historical gateway 504s on parent requests ≥~25
   parents at this hour; 150-parent chunks failed 4/4 retries. Pull tooling needed
   content-addressed DBN caching, per-chunk skip-and-continue, exponential backoff,
   chunk size 10. Any productionized universe snapshotter needs the same discipline.
2. **`metadata.get_record_count` over-reports ~10×** vs. actual `get_range` records
   for parent OPRA cbbo-1m (MU: 108,677 reported vs 10,922 actual). Cost preflight
   unaffected ($0 flat-rate), but record-count-based capacity planning would be wrong.
3. **RTK/shell quirks** (local): `head`/`ls | wc` outputs are proxy-rewritten; file
   manipulation for universe lists must go through python. (Operational note only.)

## Smoke-scale findings (top-10 boards, release build)

Boards: MU NVDA TSLA SNDK AMD MSFT INTC AAPL GOOGL AVGO — the *heaviest* single-name
chains in the universe (2k–12.6k quote rows each; MU post-earnings chain 10.4k rows).

| finding | evidence |
|---|---|
| Fit latency far from "universe on one box" | robust preset: mean 3.25 s/board, MU 9.2 s, SNDK 8.4 s. fast preset: mean 1.13 s, MU 2.1 s. 10 mega boards alone = 32.5 s serial CPU (robust). |
| Profile misclassification at universe scale | MU, SNDK, AVGO, INTC classified `IndexEtfUltraLiquid` (they are single names). TickerPrior table covers only 21 tickers (7 index ETF, 8 megacap, 6 liquid names) — everything else falls to board-feature voting, which reads penny-dense huge single-name boards as index ETFs. |
| CrossValidation path = CPU sink | The 4 misrouted boards all went `CrossValidation` (5 families × ≤8 expiries, full fit + holdout score per family per expiry) → 3.4–9.2 s fits. Direct-routed megacaps (TickerPrior) fit in 0.7–1.6 s. |
| `calendar_arb_free = false` on 9/10 boards | Flag is a POST-repair check over the FULL k-grid (|k| ≤ 3, 25 pts), but Robust's `MonotoneFit` repair only enforces near-money (|k| ≤ 0.7). Wing violations survive by design → flag ~always false for essvi/svi routes. Semantics mismatch: the "robust" preset cannot report a clean surface even when near-money is repaired. Only by-construction kinds (linear-variance, INTC) report true. |
| Robust preset can be WORSE than fast | MU: robust in-band 0.726 / χ² 3.75 / 9.2 s vs fast in-band 0.941 / χ² 0.37 / 2.1 s. Same chosen family (essvi). Calendar-floored sequential slice fitting degraded the dense board fit. |
| Preset changes the *selected family* | AVGO: robust → svi, fast → linear-variance (selection instability across latency profiles). |
| Event boards fit poorly | SNDK χ² ≈ 31 (both presets), TSLA 10.5, NVDA 8.2; worst_in_band ≤ 0.11 on MU/NVDA/TSLA/SNDK. Post-earnings/HBM-squeeze term structures (negative ATM curvature, inverted terms) not captured by essvi/svi; C8 never won selection on these boards. |
| Valuation cost heavy | value_chain (Prices+Bands, serial): 0.5–0.7 ms/option → SNDK 7.9 s for 12.9k options. Cause: 3 independent American IV inversions per option (bid/ask/mid), no per-(K,T) dedup, plus fair_value per option. |
| bid_iv NaN 25–41% on megacap boards | Mostly structurally-empty bids (unset px → NaN by design), but no diagnostic distinguishes "no bid quoted" from "inversion failed". |

## Pipeline mechanics (from source, file:line in worklog)

- **Auto-select routing**: `PricerConfig.curve` unset → `select_fit_policy` (profile
  classify via 21-ticker prior table else board-feature voting) → direct route
  (essvi/linear-variance/C8 by profile) or CrossValidation (`select_curve`: 5
  candidate families × ≤8 expiries, leave-every-other-strike-out).
- **Fit cost split (dense direct route)**: `build_observations_european` (per-quote
  American de-Am inversion, cold path) ≈ 60–65%; sequential per-slice fit (QP +
  calendar floor rows) ≈ 30–40%; `resolve_chain_forward` 5–15%.
- **Spot inference**: PCP on earliest usable expiry (T ≥ 3/365), ATM strike = min
  |call_mid − put_mid|; fails → error. Bad spot poisons `n_atm` moneyness voting →
  misclassification cascade.
- **Selector scoring**: per candidate per expiry: fit on even strikes, score odd
  strikes; butterfly violations disqualify; chi²-closest-to-1 tie-break inside
  parsimony margin.

## 100-name results (top-100 ADV, release, 12 workers, robust preset)

- 96/100 ok, wall 62 s (12 workers), **serial fit CPU 433 s → mean 4.5 s/board,
  p50 4.5 s, p90 7.6 s, max 11.4 s (LITE)**. Extrapolated naïvely: 3000 boards ≈
  3.6 CPU-hours per snapshot minute.
- **CrossValidation routing = 82/96 boards = 97% of fit CPU.** TickerPrior caught 13,
  BoardFeatures direct exactly 1. At universe scale the "high-confidence direct
  route" almost never fires — the policy's cheap path is the exception, not the
  rule. Fit CPU: CV boards median 4.8 s vs TickerPrior-direct 0.94 s.
- Family histogram (ok boards): essvi 48, c8-event 33, svi 10, linear-variance 5.
  `calendar_arb_free`: essvi 0%, svi 30%, c8 45%, linear-variance 100%.
- Classifier inversion, both directions: CRWD/IBM/LLY/WDC/LITE/... (single names)
  → `IndexEtfUltraLiquid` (42 boards!), while actual sector ETFs XLV/XLE →
  `OrdinarySingleName`. The 21-ticker prior table is the only thing standing
  between megacaps and the wrong bucket.
- Hard failures (4): CBRS/COHR/CRDO `select_curve: no candidate produced a
  scorable fit` (healthy 1.7–2.1k-row boards — pinning essvi fits them fine, so
  the SELECTOR kills boards the fitters handle); BRK.B `underlying carries no
  chains` (hive underlying "BRK.B" vs OSI root "BRKB" key mismatch — every
  dotted class-share symbol will do this).
- Timing has no simple size story: KO (782 rows) 8.0 s, XLV (1 k rows) 7.7 s vs
  MU (10.4 k rows) 9.1 s — CV expiry subsampling + per-slice LM iterations, not
  row count, dominate.

### Pinned-family controls (same 100 boards)

| run | ok | fit p50 | fit CPU sum | in-band med | χ² med |
|---|---|---|---|---|---|
| auto (robust) | 96 | 4511 ms | 432.9 s | 0.950 | 0.37 |
| pinned essvi | 99 | 2903 ms | 313.5 s | **0.980** | 0.22 |
| pinned linear-variance | 90 | 2017 ms | 179.1 s | 0.911 | 0.68 |

- CV overhead on essvi-chosen boards: median **+2.7 s/board** (5.06 s auto vs
  2.95 s pinned).
- **Pinned essvi beats auto on BOTH availability (99 vs 96) and quality (0.980
  vs 0.950 in-band)** — on this population the selector subtracts value.
- Even pinned linear-variance (the "HFT" path) costs 2.0 s/board: the shared
  de-Americanization observation build (cold per-quote American IV inversion;
  `use_deam_cache_for_fit` false everywhere) is the universal floor — ≈60% of
  direct-route fit time. Curve solving is not the bottleneck; de-Am is.
- Pinned LV fails 9 boards ("no expiry produced a usable slice") — stricter
  slice admission than the parametric kinds.

## Full-universe results (3000 names, release, 16 workers)

Snapshot coverage: 2734/3000 symbols had OPRA quotes at the minute (266 listed
no options), 1.11M quote rows total, **p50 board = 192 rows** — 81% of the
universe sits below `sparse_validation_floor` (600). The universe is a sparse-
tail problem wearing a megacap hat.

### Headline: auto-select survives 62% of the boards it loads

| run | ok | fit_error | load_error | fit CPU | wall (16w) |
|---|---|---|---|---|---|
| auto (robust) | 1698 | 930 | 106 | 1929 s | 104 s load + 1049 s fit |
| auto (fast) | 1698 | 930 | 106 | **456 s** | 5 s load + 189 s fit |
| pinned essvi | **2267** | 361 | 106 | 2923 s | 37 s load + 483 s fit |

Fast vs robust at scale: **identical success/failure sets** (all 930+106
failures are structural, preset-independent) and fast's quality medians are
marginally *better* on the 1698 common boards (in-band 0.974 vs 0.967, χ²
0.284 vs 0.303, calendar-free 37.6% vs 37.3%) at 4.2× less CPU. 21 boards
(1.2%) flip family across presets. At universe scale the robust preset buys
nothing measurable — it spends its extra CPU on CV boards whose quality it
does not improve (the MU smoke finding generalizes).

Error taxonomy (auto, 2734 data boards):

- **874 × `fit_curve_surface: no expiry produced a usable slice`** — the sparse
  tail dies wholesale. Failed boards: p50 56 rows, max 592. Root cause chain:
  slice admission needs `kMinUsableObs = 5` de-Am'd OTM legs per expiry
  (curve_fit.cpp:33) and the leg gate requires a two-sided quote
  (`bid>0 && ask>0`, curve_fit.cpp:59-64). Sparse names at 10am are heavily
  one-sided → legs discarded → no expiry reaches 5 obs. One-sided quotes carry
  usable information (an ask bounds the surface) and are thrown away today.
- **106 × `no well-conditioned co-terminal expiry to imply spot`** — PCP spot
  inference fails; p50 27 rows. The `spot_override` knob exists but no universe
  entry point feeds it (an equity NBBO/close feed would fix all 106).
- **50 × `select_curve: no candidate produced a scorable fit`** — mid-liquidity
  boards (p50 758 rows, all 600–2400), i.e. exactly the band that clears the
  sparse floor and enters CV, where the selector's own scorability gates
  (T-floor, ≥8 eu-obs, holdout coverage) zero out every candidate. **Pinned
  essvi fits all 50.** Selector NotFound has no fallback ladder on the auto
  path (pricer_fitter.cpp:159 vs :183-196).
- 6 × `underlying carries no chains` — dotted class-share symbols (BRK.B, BF.B,
  …): hive keys "BRK.B" vs OSI root "BRKB".

### Routing at scale (auto, 1698 ok)

- `SparseGuard` 1320 boards (78%) → forced IlliquidSmallCap → direct svi,
  median 231 ms. This is why svi is 80% of chosen families (1359/1698).
- `CrossValidation` 318 boards (19%) → **82% of all fit CPU** (1591 s of
  1929 s, mean 5.0 s/board). The direct/cheap path the policy was designed
  around fires for TickerPrior(14) + BoardFeatures(46) = 3.5% of boards.
- Family histogram: svi 1359, essvi 183, c8-event 132, LV 22, convex-dense 2.
- Quality flags at scale: `calendar_arb_free` false on 62.7%; χ²>5 on 10.6%;
  worst_in_band<0.1 on 12.9%; bid_iv NaN 41% of 981k valued options (one-sided
  sparse quotes indistinguishable from inversion failures).

### Pinned essvi strictly dominates auto-select (full universe)

- ok-set is a **superset**: 2267 ⊇ 1698, auto-only = 0.
- Fixes all 50 selector-NotFound boards and **519/874** sparse no-usable-slice
  boards (the pinned path's slice admission differs from the auto path's —
  the 874 is not a data floor, it's an admission-policy artifact for most
  boards). Residual pinned failures: 355.
- Quality on the 1698 common boards: in-band 1.0 vs 0.967, χ² 0.076 vs 0.303
  (medians). The 569 recovered boards fit well: median in-band 1.0, χ² 0.067,
  median 84 rows.
- Cost: essvi 1074 ms vs sparse-svi 220 ms median on common boards — auto's
  only win is speed on the boards it doesn't kill.

### CPU profile (auto full run)

Stage shares of per-board serial CPU: fit 71.1% (1929 s), value 18.2% (494 s,
0.50 ms/option over 981k), board build 10.6%, chain 0.1%. Up-front hive load:
104 s serial for 2734 parquets (no load-phase parallelism on main; P4a fan-out
exists only on the feature branch). Within fit CPU, CV boards take 82%; the
100-name deep profile (fit_timing_probe) showed the direct-route split is
de-Am observation build ≈60-65% / slice fitting 30-40%, and `use_deam_cache_for_fit`
is off in every preset.

Tail pathologies (same boards in every run): BLK 17.5-38.5 s on 2.2k rows,
ABT 30 s on 871 rows, XLU 21.8 s on 770 rows — essvi-routed CV boards where
per-slice LM iteration count, not row count, drives cost. Board row count is
a poor cost predictor overall (fit_ms/row p50 1.16, p90 4.69).

Observability note: the harness needed a progress/ETA patch mid-exercise —
the first full-universe attempt ran 4 minutes with zero output (stdout fully
buffered under redirection, no per-board progress anywhere in the library or
harness). Any credible universe fitter needs library-level progress + per-stage
timing hooks (`ATX_VOL_PROFILE` regions exist for the backtest loop only).

## Weakness inventory → sprint candidates

1. **Routing/classification**: ticker-prior table is 21 names; board-feature voter
   misreads dense single names as index ETFs; no market-cap/ADV prior input; no
   confidence-driven cheap-path fallback (misroute → most expensive path).
2. **CPU**: CrossValidation 5×8 full fits; de-Am inversion per quote without
   correction-cache reuse at fit time; value_chain triple inversion no dedup;
   no per-board time budget; no global scheduler (heavy boards serialize a wall).
3. **Calendar semantics**: repair domain (|k|≤0.7) ≠ report domain (|k|≤3);
   robust preset degrades dense fits (MU); no per-kind repair strategy.
4. **Curve family gaps**: event/inverted term structures (SNDK/NVDA/TSLA χ²≫1) —
   C8 exists but never selected on these; no dedicated sparse-board kind for the
   sub-600-quote tail (to be quantified in full run).
5. **Error handling/logging**: no structured per-board error taxonomy; empty-bid vs
   failed-inversion indistinguishable in valuation; loader errors are strings;
   no progress/ETA or per-stage timing exposed by library (harness had to measure
   from outside).
6. **Config surface**: SelectorConfig (candidates, oos_max_expiries,
   parsimony_margin), FitPolicyConfig (mode, min_direct_confidence,
   validate_ambiguous, sparse_validation_floor, dense_node_cap), PricerConfig
   per-fit overrides (enforce_calendar_floor, max_obs_per_slice, correction cache
   toggles) — none reachable from the populate/universe entry points.

## Verdict on the claim

One box, 16 threads, release: the full universe *runs* in ~19 minutes wall
(robust) or ~3.5 minutes (fast), so "fit the whole US options universe on one
box" is mechanically reachable — but only 62% of boards survive auto-select,
the selector picks a worse family than a hard-coded essvi pin on every
population we measured, 63% of fitted surfaces can't attest calendar
cleanliness, and the errors that kill the other 38% are policy artifacts
(slice admission, scorability gates, missing spot feed), not data limits.
"Super-fast and robust" it is not — yet. The sprint plan below targets
exactly the gaps this run exposed.
