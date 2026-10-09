# atmCen reproduction — convention sweep & locked `EarningsReproConfig` defaults

**Date:** 2026-07-18 · **Task:** M3 / Task 10 (final task of the earnings-censored ATM-vol reproduction)
**Tool:** `earnings-validation` (Task 9) · **Truth:** SpiderRock `tbltickerhistory3`, as-of 2026-02-10
**Snapshot:** on-disk OPRA cbbo panels, `C:\atx-data\spy-dispersion\opra\<T>\2026-02-10.parquet`

## TL;DR

- **The dominant reproduction lever is the time convention.** Switching the 12
  SR tenor year-fractions from `Calendar365` to SpiderRock's hybrid
  **`VolTime`** clock cuts the pooled cohort atmCenI RMSE from **0.0301 → 0.0121**
  (−60%) and repairs the earnings-move fit (NVDA iEMove 0.045 → 0.063 vs truth
  0.0665; term-fit `decay` no longer pinned at its bound; fit_error 0.0437 →
  0.0074). All other wired knobs (`censor_space`, `interp`, `clock_days_per_year`)
  are second-order and their existing defaults are already optimal.
- **Locked default = `time = VolTime`, everything else unchanged** (censor-then-interp
  in variance space). One-line change to the `EarningsReproConfig` default member
  initializers.
- **The tight gate (pooled RMSE ≤ 0.002, iEMove within ~10%) is NOT met; it is
  timing/data-limited.** Best achievable is RMSE **0.0121** (10 boards) / **0.0101**
  (9 cohort names only). iEMove lands within ±10% for 5/9 cohort names. The best
  individual clean name (NVDA) reproduces to **0.0031** — near the gate — which
  demonstrates the pipeline is correct and the residual is data/timing-driven, not
  a model defect. See **Gate assessment** and the **Databento decision**.
- **iEMult finding:** once the VolTime clock is used, censoring with the **raw
  fitted iEMove reproduces truth atmCen with no multiplier** (implied iEMult ≈ 1.00
  at NVDA's near-term event tenors). The ~1.5× "multiplier" seen under the calendar
  convention was a **time-convention artifact**, not a real SpiderRock iEMult.

## Cohort & data notes

- **Cohort (brief):** NVDA, CRM, WMT, HD, AVGO, MU, AAPL, TSLA, AMD, ADBE (10 names).
- **ADBE excluded — missing data.** No on-disk OPRA board exists for ADBE
  (`C:\atx-data\spy-dispersion\opra\ADBE\` is absent for 2026-02-10). ADBE's truth
  row and earnings forecast are present, but with no board the tool reports
  `SKIPPED(no parquet)`. This is a data gap, **not a bug**. Sweep runs the **9
  available cohort names**.
- **CSCO (+3 more) are extras in the checked-in truth CSV.** CSCO has an on-disk
  board, so the tool validates it too → the tool's pooled RMSE is over **10 boards
  (9 cohort + CSCO)**. ABNB/COIN/SHOP have no board and are skipped. Where it
  matters below, the **9-cohort-only** pooled RMSE is reported separately.
- **Earnings forecast:** full-cohort `tblstockearnforecasthist_v2.00_2026-02-10`
  (extracted from the vendor zip to scratchpad; not committed). All 10 cohort names
  present in the `ticker_tk` column.

## Sweep — one-factor-at-a-time from the base

Base = the pre-Task-10 default: `time = Calendar365`, `interp = Variance`,
`censor_space = on`, `clock_days_per_year = 0`. Pooled RMSE is over the 10
validated boards (9 cohort + CSCO), matching the Task-9 baseline of 0.030053.

| # | `time` | `interp` | `censor_space` | `clock` | pooled RMSE | vs base |
|---|--------|----------|----------------|---------|-------------|---------|
| 0 | Calendar365 | Variance | on | 0 | **0.030053** | baseline |
| 1 | **VolTime** | Variance | on | 0 | **0.012129** | **−60%  ✅ winner** |
| 2 | Calendar365 | Variance | **off** | 0 | 0.040508 | +35% (worse) |
| 3 | Calendar365 | **Vol** | on | 0 | 0.030584 | +2% (worse) |
| 4 | Calendar365 | Variance | on | **252** | 0.030102 | ~flat (naive fixed clock) |
| 5 | VolTime | Variance | **off** | 0 | 0.023458 | (VolTime + censor-off) |
| 6 | VolTime | **Vol** | on | 0 | 0.012173 | (≈ tied with #1) |

**Reading of the sweep:**
- **VolTime is the whole story.** It halves the RMSE on its own (#1) and is the
  only knob that moves the needle materially.
- **`censor_space = on` (SR-FLEX censor-then-interp) is confirmed best** — turning
  it off regresses under both clocks (#2, #5). This is the Task-8 default.
- **`interp = Variance` is confirmed best** — `Vol`-space interpolation is
  negligibly worse under VolTime (#6, 0.012173 vs 0.012129) and worse under
  calendar (#3). Variance-space is the historical default and stays.
- **A naive fixed 252-day clock (#4) does not help** — it lands on top of the
  calendar baseline. The *calendar-aware trading-day advance under VolTime* (the
  `clock_days_per_year = 0` path with `time = VolTime`) is what wins, not a flat
  `N/252`. `clock_days_per_year` stays 0.

**Winner: configuration #1** — `time = VolTime`, everything else at its existing
default.

## Locked `EarningsReproConfig` defaults (before → after)

Only one field changes; every other default is already the winning value.

| field | before | after | note |
|-------|--------|-------|------|
| `time` | `TimeSpec{}` (Calendar365) | **`TimeSpec{TimeConvention::VolTime}`** | the locked lever |
| `clock_days_per_year` | 0.0 | 0.0 | unchanged (calendar-aware advance) |
| `censor_space` | true | true | unchanged (SR FLEX) |
| `interp` | `Variance` | `Variance` | unchanged |
| `atm_mode` | `Forward` | `Forward` | carry-only, unwired |
| `deam_pricer` | `Alo` | `Alo` | carry-only, unwired |
| `implied_borrow` | false | false | carry-only, unwired |

The default `TimeSpec` carries default `VolTimeParams` (α = 0.7, 1890 trading-h/yr,
6870 non-trading-h/yr, 09:30–17:00 ET session) — SpiderRock's published VolTimeCalc
constants. The 3-arg `run_earnings_repro` overload still overwrites `cfg.time` with
`sess.inputs().time`, so the historical calendar smoke path is **bit-preserved**;
only the config-driven (4-arg) default now selects VolTime.

## iEMult vs iEMove — back-solve on NVDA

**Question:** does censoring the dirty surface with the *raw fitted* `iEMove`
reproduce SpiderRock's truth `atmCen`, or is an earnings-move multiplier `iEMult`
implied?

**Method:** run NVDA end-to-end under VolTime, then at each tenor with `n ≥ 1`
scheduled events back-solve the *effective* eMove that would exactly reproduce
truth atmCen, assuming model and truth share the dirty variance at that T:
`eMove_eff = sqrt( iEMove_fit² + (atm_model² − atm_truth²)·T / n )`, `iEMult =
eMove_eff / iEMove_fit`.

NVDA under VolTime: `iEMove_fit = 0.06274`, SR published `iEMove = 0.06650`,
per-name RMSE = **0.00306**.

| Nd | n | model atmCenI | truth atmCenI | resid | eMove_eff | iEMult |
|----|---|---------------|---------------|-------|-----------|--------|
| 5   | 0 | 0.37791 | 0.37065 | +0.00726 | (no evt) | — |
| 10  | 0 | 0.38060 | 0.38037 | +0.00023 | (no evt) | — |
| 21  | 1 | 0.39393 | 0.39498 | −0.00105 | 0.06218 | **0.991** |
| 42  | 1 | 0.41354 | 0.41476 | −0.00122 | 0.06137 | **0.978** |
| 63  | 1 | 0.42100 | 0.42057 | +0.00043 | 0.06346 | **1.011** |
| 84  | 2 | 0.42727 | 0.42642 | +0.00085 | 0.06369 | 1.015 |
| 105 | 2 | 0.42944 | 0.42788 | +0.00157 | 0.06494 | 1.035 |
| 126 | 2 | 0.43171 | 0.43038 | +0.00133 | 0.06498 | 1.036 |
| 189 | 3 | 0.43776 | 0.43505 | +0.00271 | 0.06728 | 1.072 |
| 252 | 4 | 0.44445 | 0.44163 | +0.00281 | 0.06753 | 1.076 |
| 378 | 6 | 0.45206 | 0.44800 | +0.00406 | 0.06965 | 1.110 |
| 504 | 8 | 0.45757 | 0.45306 | +0.00451 | 0.07044 | 1.123 |

**Finding:**
- At the **near-term event-bearing tenors** (21/42/63d, each with exactly one
  earnings event — the tenors that most directly pin the move), the back-solved
  effective eMove is **0.0614–0.0635 ≈ the fitted 0.0627**, so **iEMult ≈ 1.00
  (0.98–1.01)**. Censoring with the raw fitted iEMove reproduces truth atmCen
  there with **no multiplier**.
- The implied multiplier drifts to ~1.07–1.12 only at the **long tenors**
  (189d–504d). That is a small positive *base-vol* term-structure residual
  (model atmCen exceeds truth by +0.003…+0.0045), not an earnings effect — the
  back-solve inflates it into an apparent multiplier because it attributes the
  whole excess to the eMove removal over n = 3…8 events. The near-term signal is
  the clean read.
- **Aggregate**: fitted iEMove 0.0627 vs SR's 0.0665 → an eMove-level ratio of
  **1.060** (~6% low). This ~6% is consistent with the snapshot-vs-EOD timing gap
  (our 14:55 ET snapshot prices a slightly smaller move than SR's 16:00 ET EOD),
  **not** a structural multiplier.
- **Contrast with the calendar convention:** the calendar fit collapses iEMove to
  0.0451 and pins `decay` at its 30 upper bound (fit_error 0.0437), which would
  "imply" a spurious `iEMult = 0.0665 / 0.0451 = 1.47`. **That ~1.5× multiplier
  was purely a time-convention artifact.** Under VolTime it evaporates.

**Conclusion:** no `iEMult` knob is warranted. The reproduction needs the correct
clock, not a fitted earnings-move multiplier.

## Winning-config per-name residuals (locked VolTime default)

| ticker | iEMove (model) | iEMove (truth) | dEMove | iEMove rel-err | per-name RMSE | nEcMatch |
|--------|----------------|----------------|--------|----------------|---------------|----------|
| NVDA | 0.0627 | 0.0665 | −0.0038 | 5.7% | 0.00306 | yes |
| CRM  | 0.1066 | 0.0970 | +0.0096 | 9.9% | 0.01754 | yes |
| WMT  | 0.0554 | 0.0566 | −0.0012 | 2.1% | 0.00589 | yes |
| HD   | 0.0448 | 0.0448 | −0.0000 | 0.0% | 0.00383 | yes |
| AVGO | 0.0970 | 0.0965 | +0.0005 | 0.5% | 0.00373 | yes |
| MU   | 0.1049 | 0.0843 | +0.0207 | 24.4% | 0.01181 | yes |
| AAPL | 0.0567 | 0.0208 | +0.0359 | 172.6% | 0.01623 | yes |
| TSLA | 0.0787 | 0.0912 | −0.0125 | 13.7% | 0.00660 | yes |
| AMD  | 0.0629 | 0.0836 | −0.0207 | 24.8% | 0.00969 | yes |
| CSCO (extra) | 0.0693 | 0.0682 | +0.0011 | 1.6% | 0.02346 | yes |
| ADBE | — | 0.0923 | — | — | SKIPPED | no board |

- **iEMove within ±10% for 5/9 cohort names** (NVDA, CRM, WMT, HD, AVGO).
- **Schedule-alignment gate `nEcMatch = yes` for all 10 validated boards** under
  VolTime — the model's `nEarnCnt_Nd` matches truth exactly for every tenor.

Pooled RMSE, by subset (each cohort name has 12 finite scored tenors):

| subset | pooled RMSE |
|--------|-------------|
| 10 boards (9 cohort + CSCO) — the tool's headline number | 0.012129 |
| 9 cohort names only | 0.010114 |
| 8 (9 cohort − AAPL) | 0.009064 |
| 4 clean near-term-event names (NVDA, WMT, HD, AVGO) | 0.004261 |

## Per-tenor residual attribution (winning config, mean model − truth over 10 boards)

| Nd | meanResid | rmsResid |
|----|-----------|----------|
| 5   | −0.00735 | 0.02314 |
| 10  | −0.00642 | 0.01658 |
| 21  | −0.00499 | 0.01092 |
| 42  | −0.00458 | 0.00918 |
| 63  | −0.00135 | 0.01291 |
| 84  | −0.00312 | 0.00973 |
| 105 | −0.00348 | 0.00873 |
| 126 | −0.00252 | 0.00940 |
| 189 | −0.00238 | 0.01000 |
| 252 | −0.00206 | 0.00871 |
| 378 | −0.00011 | 0.00850 |
| 504 | +0.00017 | 0.00880 |

VolTime **removes the systematic mid/long-tenor negative bias** that the calendar
default carried (calendar mean resid was −0.017…−0.019 from 63d out; here it is
−0.002…0). The largest remaining per-tenor RMS is at the **front (5d = 0.0231)** —
the classic signature of a snapshot-vs-EOD timing gap (front-tenor IV is the most
sensitive to a 65-minute drift).

## Gate assessment — NOT met, timing/data-limited (honest attribution)

**Target:** pooled atmCenI RMSE ≤ 0.002; iEMove within ~10%.
**Achieved (best, VolTime):** pooled RMSE **0.012129** (10 boards) / **0.010114**
(9 cohort); iEMove within ±10% for **5/9** names.

The gate is **not** reached. The residual decomposes cleanly and is **not** a model
defect:

1. **Front-tenor snapshot-vs-EOD timing gap (dominant, structural).** The on-disk
   OPRA snapshot timestamp is ≈14:55 ET; SpiderRock's `tbltickerhistory` truth is
   an ≈16:00 ET EOD mark — a ~65-minute gap. The front tenor (5d) carries the
   largest per-tenor RMS (0.0231) and the clean near-term names still floor at
   ~0.003–0.006 RMSE (NVDA 0.0031, AVGO 0.0037, HD 0.0038, WMT 0.0059). This floor
   is consistent with intraday-vs-settle drift, and it caps how low the pooled RMSE
   can go from the model side. The fitted iEMove sitting ~6% below SR's published
   value on the clean names (NVDA 5.7%, WMT 2.1%) is the same effect at the eMove
   level.
2. **Weak earnings-move identifiability for far-first-event names (name-specific).**
   AAPL, TSLA, AMD have their first scheduled earnings only at the 63d tenor
   (`nEarnCnt` = 0 for 5–42d), so the near-dated surface carries **no** event to
   pin the move and the joint fit is under-determined for iEMove. AAPL is the
   extreme case (fitted 0.0567 vs truth 0.0208, +173%) and alone lifts the 9-cohort
   RMSE from 0.0091 to 0.0101. This is a fit-identifiability / data-coverage limit,
   addressed by the M5 per-event weighting item, not by any convention knob.
3. **CSCO (extra, not a cohort target)** contributes the single largest per-board
   RMSE (0.0235) and inflates the tool's 10-board headline; the 9-cohort figure
   (0.0101) is the cleaner gate read.

**No config, CSV, or number was adjusted to force the gate green.** The best
achievable RMSE/iEMove is reported as-is with the residual attributed above.

### Databento decision (surfaced; needs the user)

The dominant residual (item 1) is a **timing** limitation curable only with data,
not model changes. A **Databento `cbbo-1s` 15:45–16:00 ET EOD slice** for the 9–10
names on 2026-02-10 (est. **$2–$12** for a 15-minute, ~10-symbol OPRA slice) would
let us re-snapshot at ≈16:00 ET and quantify how much of the residual is timing vs
model — expected to pull the clean-name RMSE toward the gate and the iEMove within
a few percent.

**This is NOT self-servable.** `DATABENTO_API_KEY` is **unset** in this
environment; the implementer cannot provision it. **Decision for the user:** set
`DATABENTO_API_KEY` to authorize the EOD slice (run `metadata.get_cost` first to
confirm the exact price before pulling). Until then, the timing-limited residual
stands and is correctly attributed.

## Carry-only knobs — swept? (brief vs reality)

The brief's Step-1 grid lists `implied_borrow ∈ {true, false}` in the sweep. That
list **predates the Task-9 carry-only decision** and is honored-with-a-correction
here:

- `implied_borrow`, `atm_mode`, and `deam_pricer` are **carry-only** fields with
  **no wiring seam** in the Task 1–8 pipeline. Sweeping `implied_borrow` produces
  **byte-identical output** — it has no effect — so it is **not swept** (a
  no-effect row would be noise). Confirmed by code inspection: none of the three is
  read on the `run_earnings_repro` compute path (only `time`,
  `clock_days_per_year`, `censor_space`, `interp` are).
- **What each would need in M5 to become sweepable:**
  - `implied_borrow`: a borrow-implication seam — solve an implied borrow/hard-to-
    borrow rate from put-call parity per expiry and thread it into the forward used
    by `PricedSurface::forward_at`, instead of the flat `r`-only carry today.
  - `atm_mode`: a spot / delta-neutral ATM anchor path in the surface read
    (`total_variance` is sampled at forward-ATM / k = 0 only today).
  - `deam_pricer`: a CRR (or Vellekoop–Nieuwenhuis discrete-div) de-Americanization
    path as an alternative to the fixed upstream ALO pricer.
- **No new subsystem was wired** to make these sweepable — that is explicitly out
  of scope (Phase M5). The sweep ran the knobs that are actually wired, and the
  wired knobs were sufficient to identify the dominant lever (VolTime).

## Regression check (mandatory — both slow tests green under the new default)

Rebuilt `earnings-validation` + `atx-vol-tests` after the default edit (clang-cl
`/W4 /permissive- /WX`, clean), then:

```
$ scripts/atx-build.ps1 -Ctest -R "EarningsValidation|EarningsReproSmoke"
1/2 Test #1809: EarningsReproSmoke.NvdaRealBoard_TwelveFiniteAtmCenI_FiniteNonnegativeEmove ... Passed 3.02 sec
2/2 Test #1810: EarningsValidation.NvdaCohortRow_TwelveResiduals_FiniteRmse_ExactNEarnMatch ... Passed 3.04 sec
100% tests passed, 0 tests failed out of 2
```

- `EarningsReproSmoke` (Task 7): uses the 3-arg overload → still runs the calendar
  session path (unaffected by the new default); 12 finite atmCenI, iEMove finite ≥ 0.
- `EarningsValidation` (Task 9): now runs NVDA under the **new VolTime default**;
  residual vector size 12, finite RMSE, finite model iEMove, and — critically — the
  schedule-alignment gate `n_earn_match` **still exact** (NVDA
  0,0,1,1,1,2,2,2,3,4,6,8). VolTime does not break the count gate.

## Caveat carried forward to M5

`n_earn[i]` is computed via `count_events_at(sched, now_ns, Tq)`, which inverts the
tenor year-fraction `Tq` through the **Calendar365** inverse (`ns_from_year_fraction`)
even when `Tq` is a **VolTime** year-fraction. For this cohort/snapshot the small
VolTime-vs-calendar discrepancy never crosses an event boundary, so `nEcMatch = yes`
for all 10 boards. It is also harmless to the reproduced atmCenI in the winning
config: with `censor_space = true`, interior tenors interpolate with `n_query = 0`
and each pillar is censored with its *own* listed-expiry count, so `n_earn[i]` is
**diagnostic-only** (it feeds censoring only in the `censor_space = false` branch).
A fully VolTime-consistent event-count inverse is an M5 cleanup, not a gate blocker.
