# Stress-day fit-fidelity sprint — Task 5 validation notes (2026-08-06)

Verdict: **BLOCKED — Task 3's calendar-floor containment gate over-fires on
healthy dates.** The headline backtest pairs DO collapse to listed-market
magnitude and the two dossier dates audit dramatically better, but the same
gate that fixes them refuses 10-60% of the *interior* slices on every rebuilt
date — healthy neighbours and census dates included — leaving surfaces that
interpolate across wide tenor holes and audit far below the pre-fix baseline.
No sanctioned recalibration (Task 1 Step 12 band/gap; Task 2 floor toward
0.30) touches the gate that fires. All r2 partitions were restored
byte-exactly to their as-found state; nothing degraded was left behind.

Binary under test: worktree `worktree-strangle-backtest` @ `8e40487`.
Rebuild flow (matches production backfill + prior drills): `--preset populate
--r <month rate from data/rates/us_3m_monthly.csv> --fit-workers 2
--snapshot-suffix T19:55:00Z` (EDT dates) / `T20:55:00Z` (2022-12-19, EST).
Audits: `atx-vol-surface-db band-audit --r 0.0` per date (delta-comparable).
All raw artifacts (16 archived BEFORE partitions + SHA256 manifest, 16
as-found partitions, 32 band-audit TSVs, 7 build-report CSVs, 4 backtest
tearsheet dirs, bisect reports): session scratchpad `validation\` dir.

## 1. BEFORE state and sanity pin

r2 = Aug-2 file-copy of production (built by the main-repo Aug-2 binary,
per `C:/atx-data/logs/spy-backfill/build_*.log`). The Aug-5 starvation-drill
had overwritten 2020-03-18 / 2025-04-10; those two were restored from the
drill's own backups before measuring, so BEFORE = the canonical pre-fix
bytes on every acceptance date (verified: BEFORE backtest tearsheets are
bit-identical to the session's pre-sprint `fix_carry`/`fix_extrap` runs).

Sanity pin PASSED: `band_before` 2020-03-18 shows T=1.7496/1.8454/1.9986/
2.7462 BELOWFLOOR with `frac_above_ask` 0.88-0.96; 2025-04-10 shows the
plateau serving above ask across the board (13 BELOWFLOOR rows, avg signed
half-spread errors in the hundreds).

## 2. Rebuild gate activity (per date)

`FailedCell`: **0 everywhere** (every cell published; Task 2's floor never
fired). `PrepUncovered` (Task 1) and total `slice_drop` rows (dominated by
`FitFailed` = Task 3 calendar-unsupported refusals; see §3):

| date | PrepUncovered | other drops | note |
|---|---|---|---|
| 2020-03-16 | 1 (T=2.004) | 28 FitFailed, 2 CarryFailed | healthy-window neighbour |
| 2020-03-17 | 1 (T=2.001) | 29 FitFailed, 1 CarryFailed | healthy-window neighbour |
| 2020-03-18 | 4 (T=0.537, 1.750, 1.845, 2.746) | 25 FitFailed, 1 PrepStarved, 1 CarryFailed | stress date; publishes only to T=0.789 |
| 2020-03-19 | 0 | 22 FitFailed | HEALTHY — pre-sprint binary drops 3 |
| 2020-03-20 | 1 (T=1.993) | 26 FitFailed, 1 CarryFailed | named healthy date |
| 2025-04-08 | 0 | 26 FitFailed, 1 CarryFailed | |
| 2025-04-09 | 3 (T=0.033/0.036/0.038) | 21 FitFailed, 1 CarryFailed | brief predicted the 04-22/23/24 expiries |
| 2025-04-10 | 1 (T=0.038 — the seed row) | 24 FitFailed, 1 CarryFailed | brief's predicted refusal |
| 2025-04-11 | 0 | 25 FitFailed, 1 CarryFailed | HEALTHY — keeps only 7 of ~33 slices |
| 2024-07-15 | 1 (T=1.084) | ~20 FitFailed | |
| 2024-07-16 / 08-05 / 08-06 | 0 | ~15-25 FitFailed each | |
| 2019-10-11 | 0 | drops present | census |
| 2022-10-04 | 1 (T=2.289) | drops present | census |
| 2022-12-19 | 0 | drops present | census |

`n_slice_calendar_unsupported` is not surfaced in the build-report CSV
(gap: the counter exists on `CurveSurfaceReport` but the populate report
never prints it — worth wiring before the recalibration rerun).

## 3. Byte-identity on healthy dates + commit-level attribution

All three healthy dates DIFFER from the Aug-2 BEFORE bytes. Controlled
rebuilds of 2020-03-19 (throwaway db copies, identical flow) attribute it:

| binary | slice drops | partition SHA256 (16) |
|---|---|---|
| Aug-2 production bytes (BEFORE) | — | `DFE0BAB83B893D43` |
| `ea69cca` pre-sprint | 3 | `2336AF302D6A80BE` |
| `3189408` Task 1+2 | 3 | `2336AF302D6A80BE` (bit-identical to pre-sprint) |
| `10c0e77` Task 3 | **22** | `2E92C04381BE8399` (bit-identical to HEAD) |
| `8e40487` HEAD | 22 | `2E92C04381BE8399` |

- Tasks 1, 2 and 4 are calm-day byte-clean (T4 never touches populate).
- **Task 3 (`10c0e77`) alone introduces the 22-slice massacre**, refusing
  interior slices T=0.016..0.674 on a healthy date via the
  calendar-floor out-of-support refusal (`kCalendarFloorUnsupportedMsg`,
  FitFailed lane). First refused slice: 2020-03-19 T=0.016437.
- Separately, pre-sprint numeric drift exists (`2336AF` != `DFE0BA`; the
  Aug-2 binary predates the Aug-5 QP ratio-test fix `0406ed8`), so strict
  byte-identity to the Aug-2 archive was unachievable from HEAD regardless.
  That drift is *materially benign* (3 minor drops, full tenor span); the
  Task-3 drift is not.

Mechanism (consistent with all observations): at small T the previous
slice's data k-range is narrow, so the next slice's calendar scan finds
"violations" beyond `prev_data_k_range ± 0.10` — crossings the pre-fix
code silently ratcheted in the far wings, harmless to in-band fidelity.
Task 3 turns each into a whole-slice refusal; the cell still publishes
(so no FailedCell) but serves interpolation across the holes.

## 4. Band-audit BELOWFLOOR, before -> after (rebuild state)

| date | before | after | verdict |
|---|---|---|---|
| 2020-03-18 | 5 | 4 | count drops, long-end poison shrinks (1.75: above-ask 0.88->0.06) — but mid rows collapse (0.12: 0.97->0.29 in-band; 0.16: 0.76->0.23) — **violates "no row's frac_in_band decreases"** |
| 2020-03-19 | 0 | 6 | HEALTHY — **regression** |
| 2020-03-20 | 1 | 0 | healthy; rows NOT numerically identical (fits changed) |
| 2020-03-16 / 17 | 2 / 7 | 9 / 2 | mixed |
| 2025-04-10 | 13 | 0 | **the dossier defect is fixed**: seed row two-sided-sane (1.0000 in-band), plateau gone, long-end above-ask -> ~0 |
| 2025-04-09 | 12 | 1 | large improvement |
| 2025-04-08 | 4 | 9 | regression |
| 2025-04-11 | 0 | 7 | HEALTHY — **regression** (kept 7 of ~33 slices) |
| 2024-07-15 / 16 | 1 / 0 | 15 / 14 | **regression** |
| 2024-08-05 / 06 | 0 / 0 | 8 / 7 | **regression** |
| 2019-10-11 | 0 | 3 | census — **fails "must not increase"** |
| 2022-10-04 | 0 | 13 | census — **fails** |
| 2022-12-19 | 1 | 6 | census — **fails** |

## 5. Backtest pairs (SPY LEAPS strangle, both arms, r2 rebuilt state)

Pre-fix (both arms identical on these dates): 2020-03-18/19 = **+3726 /
-3408** pnl_total (+3812/-3451 pnl_vega); 2025-04-10/11 = **+2430 / -2369**
(+2435/-2400 pnl_vega). Post-fix:

| pair | carry | extrapolate | acceptance (<= 1/3 of pre-fix) |
|---|---|---|---|
| 2020-03-18/19 | -274 / +1712 (pnl_vega 0/0 — marks carried, spike vanishes) | +342 / +1091 (same-sign; vega-pnl ratio ~0.11/0.31 of pre-fix, consistent with well under ~5 vol pt) | **PASS both arms** |
| 2025-04-10/11 | -1529 / -75 (same-sign; vega carried on 04-10) | +1087 / -75 (reversal min 75 vs pre-fix 2369; vega-pnl 1049 vs 2435 ~ under ~2 vol pt) | **PASS both arms** |

No-new-artifacts scan (top-10 opposite-sign adjacent |pnl| pairs): largest
post-fix pair = 3719 (carry, 04-09/10) and 2724 (extrap, 07-16/17), both
below the pre-fix third-largest (4405) — **letter of the criterion PASSES**,
but the new top pairs sit exactly on the degraded rebuilt windows, i.e. the
pnl improvement is partly bought with the §4 fit damage. Carried marks rise
36->52 (carry arm), extrapolated marks 74->100.

## 6. Disposition

- r2 restored byte-exactly to the as-found state (all 16 partitions,
  SHA256-verified; the Aug-2 baseline for 03-18/04-10 additionally
  preserved in the validation archive).
- Sprint verdict: T1/T2/T4 behave as designed (correct refusals on the
  starved expiries, floor never spuriously fires, audit tool caught both
  the defect and the regression). T3 needs a redesign or recalibration of
  its own (out-of-scope here: the sanctioned knobs only cover T1's band/gap
  and T2's floor). Options to evaluate: scope the refusal to slices whose
  *in-support* fit actually moved (served-vs-bare delta), skip-not-refuse
  out-of-support scan hits, or a wider/T-scaled support margin.
- Carried Task-3 residuals (unchanged, still open): warm-refit path has no
  containment (`refit_slice_curve` range-blind, unreachable today);
  `per_slice_linear_fallback` bypasses the gate and un-counts refusals
  (dormant, populate never enables it).
