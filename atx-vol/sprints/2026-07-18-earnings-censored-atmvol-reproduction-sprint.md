# Sprint / Design Spec — Reproduce SpiderRock ATM earnings-censored vol term structure

**Date:** 2026-07-18  **Branch:** `feat/earnings-censored-vol-repro`  **Worktree:** `C:\atx-wt\earn-vol`
**Author:** main-thread orchestrator (subagent-driven dev)  **Standard:** `.agents/cpp/agent.md`
**Research:** `atx-vol/research/earnings/2026-07-18-{spiderrock-atmcen-conventions,event-variance-modeling-sota}.md`

## 0. Goal & acceptance
Reproduce SpiderRock `tbltickerhistoryv3` per-(symbol, tradingDate) **ATM earnings-censored
implied vol term structure** (`atmCenI_{5d,10d,21d,42d,63d,84d,105d,126d,189d,252d,378d,504d}`,
`atmCenI_st/lt/decay`) and **implied per-event move** (`iEMove`) from an OPRA option-chain
snapshot + estimated earnings dates, matching to a tight bar.

**Acceptance (user-chosen, tight):** cohort atmCenI per-tenor RMSE ≤ **0.002 vol (0.2 vol pt)**;
iEMove within ~10%. High convention-match effort authorized. Report residual attribution
honestly where a knob (snapshot instant, exact pricer) caps achievable match.

## 1. Model (SpiderRock, confirmed verbatim from vendor dictionaries)
    w_total(T) = σ_T²·T = σ_E²·n + σ_C²·T
- σ_E = eMove (per-event move, decimal fraction); σ_E² = per-event variance.
- n = scheduled earnings events before expiry T; σ_C = censored (event-free) vol.
- Censor: σ_C(T) = sqrt((σ_T²·T − n·σ_E²)/T). Re-add: σ_T² = (σ_C²·T + n·σ_E²)/T.
- **iEMove objective** (vendor): jointly minimize deviation of censored ATM expiry points from a
  **smooth term-curve model** over all fittable expiries → this IS the st/lt/decay fit.
- **st/lt/decay** parametric censored term curve (form inferred, standard):
      atmCen_model(T) = lt + (st − lt)·exp(−decay·T)
  st = 5d anchor, lt = 504d anchor, decay = model decay param.
- atmCenI_{Nd} = censored ATM vol at fixed CALENDAR-day tenor N (raw-interp vs parametric read = knob).
- ATM = **forward-ATM** (strike where call≈put) with **implied sdiv/borrow**; American IV
  (vendor CRR + Vellekoop-Nieuwenhuis discrete-div splicing), European-equivalent lognormal report.
- Clock: SpiderRock VolTimeCalc, α=0.7, 1890 trading h/yr, 6870 non-trading; earnings get NO
  extra vol-time weight (handled entirely as σ_E²·n).

## 2. What already exists (REUSE — do not rebuild)
| Stage | Status | Anchor |
|---|---|---|
| OPRA cbbo snapshot → QuoteFrame | EXISTS | `load_opra_cbbo_parquet` (opra_panel.hpp:195) |
| QuoteFrame → Chain/Underlying | EXISTS | `data_install` (data.cpp:381) |
| American→European de-Am IV | EXISTS | `build_observations_european`, `american_implied_vol` (Andersen-Lake) |
| Forward/div/borrow (implied) | EXISTS | `hybrid_forward`, `resolve_chain_forward`, `imply_borrow_european_pcp` |
| eSSVI surface fit (θ = ATM total var, w(0)=θ) | EXISTS | `run_surface_parity`, `essvi_fit_slice` |
| ATM vol at T (forward-ATM, k=0) | EXISTS | `atmf_vol(ps,T)` (analytics_primitives.cpp:48) |
| Cross-expiry interp in TOTAL VARIANCE, flat short/long extrap | EXISTS | `CurveSurface::w` (vol_curve.cpp:232) |
| Fixed-tenor ATM term structure (calendar grid) | EXISTS | `TenorGrid::standard`, `compute_surface_analytics` |
| Censoring math, two-expiry implied_emove, event_aware_w | EXISTS | `event_vol.{hpp,cpp}` |
| VolTimeCalc hybrid clock (α=0.7) | EXISTS | `vol_time.{hpp,cpp}` (standalone, routable via QuoteFrame::time) |
| Free OPRA fixtures 2026-02-10 (51 names) + 02-09/11/12/13/17 | EXISTS on disk | `C:\atx-data\spy-dispersion\opra\<SYM>\<date>.parquet` |

## 3. The gap (BUILD) — seams from the code map
- **S1 — stamp `expiry_ns` on fitted eSSVI slices** (field exists, left 0). Root-cause fix that
  unblocks VolTime end-to-end AND makes event bucketing exact (kills the Calendar365-inverse
  synthesis in `count_events_at`/`solve_implied_emove`). Highest-leverage seam.
- **S2 — unify the censoring path.** Term-structure output currently censors PLAIN-interpolated
  total variance (analytics path β) while the SpiderRock-correct censored-SPACE interpolation
  (`event_aware_w`, path α) is bypassed because `to_priced_surface()` drops the EventSchedule.
  Route the term-structure aggregator through censored-space interpolation. #1 correctness fix.
- **S3 — SpiderRock trading/calendar-day tenor grid** (5d,10d,21d,42d,63d,84d,105d,126d,189d,252d,
  378d,504d) + **short-end flat extrapolation** for tenors before the first listed expiry
  (analytics currently gates out T<Tmin; SpiderRock publishes 5d anyway).
- **S4 — earnings → EventSchedule adapter.** Read `tblstockearnforecasthist` (UTC earn instants +
  session) directly → `vector<int64_t>` epoch-ns → `EventSchedule`. (ref/atx_earnings gives
  dates+session but we have the vendor file directly with instants.)
- **S6 — global joint {eMove, st, lt, decay} censoring term-fit** (net-new, the CORE module).
  Replaces two-expiry `implied_emove` for the reproduction path. Objective:
      min over {eMove, st, lt, decay}  Σ_i weight_i · ( σ_C_obs(T_i; eMove) − model(T_i; st,lt,decay) )²
      σ_C_obs(T_i; eMove) = sqrt( max((w_dirty(T_i) − n_i·eMove²)/T_i, floor) )
      model(T) = lt + (st − lt)·exp(−decay·T)
  Hard constraint: censored forward variance monotone (no negative). Then
  atmCenI_{Nd} = model(N/clockdays) [or raw interp — knob]; iEMove = eMove.

## 4. New code (small, single-purpose units; cpp guidelines)
- `include/atx/vol/earnings_term_fit.hpp` + `src/earnings_term_fit.cpp` — the S6 joint fitter:
  `EarningsTermFit fit_earnings_term(span<CensorObsInput>, EarningsFitConfig) -> Result<EarningsTermFit>`
  where `EarningsTermFit{ eMove, st, lt, decay, per_tenor atmCenI[grid], fit_error, fit_code, expiry_count }`.
  Pure function over (per-expiry {T, w_dirty, n}) + config. Reuses `censored_total_variance`.
- `include/atx/vol/sr_tenor_grid.hpp` — SpiderRock fixed-tenor grid + short/long extrapolation policy (S3).
- earnings→EventSchedule adapter: `src/earnings_forecast_loader.cpp` (S4) — parse the vendor TXT
  slice for a (ticker, tradingDate) → sorted epoch-ns event instants.
- Wire S1/S2 into `run_surface_parity` (stamp expiry_ns) + `compute_surface_analytics` (censored-space).
- Convention config struct `EarningsReproConfig` — ATM def, TimeConvention (Calendar365|VolTime),
  interp space (variance|vol), tenor day-basis (calendar|vol-time), snapshot instant, de-Am pricer
  (ALO best-accuracy | CRR vendor-compat), sdiv/borrow (implied|textbook). Single carrier for the
  convention sweep.
- `examples/earnings_repro.cpp` (target `earnings-repro`) — drive one (name,date) end-to-end:
  OPRA parquet → session fit → S6 term-fit → print atmCenI_*/iEMove + parity vs tickerhistory row.
- Validation harness: batch over 51 names on 2026-02-10, RMSE table + residual attribution;
  test `tests/earnings_term_fit_test.cpp` (fast unit) + a real-data validation suite (atx_vol_slow).
- Fixtures under `tests/support/`: small checked-in tickerhistory 2026-02-10 subset (target rows)
  + earnings-forecast subset for the validation names.

## 5. Validation methodology
- Truth: tickerhistory 2026-02-10 slice (streamed once from the 11GB zip, subset saved).
- Per (name, tenor): resid = atmCenI_ours − atmCenI_SR. Report per-tenor + cohort RMSE.
- iEMove: rel err vs SR `iEMove`. nEarnCnt: EXACT-match sanity (validates earnings-schedule alignment).
- **Convention sweep** (residual attribution): toggle each knob (time convention, interp space,
  ATM/forward, de-Am pricer, sdiv implied vs textbook, snapshot instant) and measure cohort-RMSE
  sensitivity → LOCK the matching config. Back-solve the iEMult-vs-iEMove question on one name.
- Gate: cohort atmCenI RMSE ≤ 0.002 (stretch). If a residual is dominated by the 14:55 ET vs SR-EOD
  snapshot instant, spend the $100 Databento reserve on a ~15:59 ET slice for the cohort and re-test.

## 6. Data / budget
- Primary fixtures FREE on disk (51 names × 2026-02-10 + neighbors). $100 Databento = **reserve**,
  spent only if validation shows a timing/coverage residual an EOD slice would close. Any spend
  logged with the exact command + cost before executing.

## 7. Build / test (cpp guidelines)
- Configure via `scripts\atx-build.ps1 configure` (preset `dev`) in the worktree; build target
  `atx-vol-tests` + `earnings-repro`. New `*_test.cpp` appended to the explicit list in
  `tests/CMakeLists.txt`; fast unit tests labelled `atx_vol_fast`, real-data validation `atx_vol_slow`.
- TDD: failing unit tests first (censor round-trip; joint-fit recovers planted {eMove,st,lt,decay};
  monotone constraint; grid sampling; earnings adapter boundary at BMO/AMC/expiry-day). Then wire.
- `/W4 -Werror`, ASan/UBSan, no UB, `Result<T,E>` for expected failures, no dynamic alloc on hot path.
- Subagent-driven: one bounded task per subagent, review gate per task.

## 8. Milestones
- **M0** recon + design + spec (this doc) — DONE at commit.
- **M1** S6 joint fitter + SR tenor grid, unit-tested on synthetic planted params (no data).
- **M2** S1+S4 (expiry_ns stamp + earnings adapter) + end-to-end `earnings-repro` on NVDA 2026-02-10
  vs tickerhistory truth (single-name parity).
- **M3** S2 censoring unify + convention sweep → lock the matching `EarningsReproConfig`.
- **M4** batch-validate 51 names, cohort RMSE gate; residual attribution report; optional $100 EOD slice.
- **M5** SOTA upgrades (highest-leverage): non-spanning σ_C identification + monotone-θ hard
  constraint; per-event/next-print weighting; vendor-compat CRR de-Am mode; perf (branch-light IV).
- **M6** requesting-code-review + finishing-a-development-branch (PR/merge decision).

## 9. Open items to confirm (data-recon agent, in flight)
- Unit sanity (decimal vol/move) on known tickers; iEMult-vs-iEMove definition (back-solve one row);
  concrete atmCenI/iEMove/nEarnCnt target values for the cohort; final name list; databento cost (moot if free).
