# Task T40 report — v5 TRAIN gate (R6'), DSR, paired ΔSR, trial accounting

Controller: Claude Opus 5.5 (SDD), 2026-09-27. Branch `feat/aes-codex-integration-20260925` (pool-2). TRAIN 2020-2022 only.
Inputs: 13 NAV dirs (10 pre-registered grid cells, 2 L re-run cells, 1 v5.1 reference cell), NAV exe `fb2d3e94` (build v5-1),
primary scenario S2 `modeled-1bn-stale5-v1+swap-fin-v1`, $1bn, cadence 1, rule `aim-partial-v5`, neutralization `price-risk-v1`.
Producer: `nav_summ.py --weights W_ew 9a9c949a --weights W_aim 54f823c1 --weights W_ew51 198375f9 --reference
build-equity/mega-nav-v5-ew-t.05-d.1-fixed --dsr-n 13` -> `build-equity/mega-nav-v5-t40-summ-n13.{txt,json}`; gross (all rows) is
the ruled D1/R6' definition (mean of the daily `gross_leverage` column over every CSV row); step-5 N = 10 table:
`build-equity/mega-nav-v5-grid-summ-n10.{txt,json}` (ledger section "T38 steps 4-5").

## 1. Gate table (all 13 TRAIN cells)

| cell | net SR | gross SR | HAC t | gross (all rows) | gross (nav_summ) | mean abs net | tau mean/p95 | NR | dSR vs REF (Memmel SE, t) | CBB 95% | LW 95% (p) | DSR N=13 | mechanics |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| v5-ew-t.05-d.1-fixed | +0.742 | 1.177 | 1.46 | 0.7819 | 0.7829 | 0.0110 | 0.0433/0.0602 | 0.835 | - | - | - | 0.839 | FAIL (gross) |
| v5-ew-t.03-d.1-fixed | +0.692 | 1.053 | 1.35 | 0.7088 | 0.7097 | 0.0083 | 0.0379/0.0580 | 0.731 | -0.050 (0.080, -0.62) | [-0.219, +0.139] | [-0.237, +0.137] (0.587) | 0.818 | FAIL (gross) |
| v5-ew-t.08-d.1-fixed | +0.681 | 1.193 | 1.34 | 0.8439 | 0.8450 | 0.0141 | 0.0490/0.0618 | 0.944 | -0.061 (0.076, -0.80) | [-0.238, +0.096] | [-0.237, +0.116] (0.485) | 0.814 | FAIL (gross) |
| v5-ew-t.05-d0-fixed | +0.724 | 1.178 | 1.43 | 0.7814 | 0.7824 | 0.0108 | 0.0464/0.0640 | 0.894 | -0.018 (0.027, -0.65) | [-0.066, +0.040] | [-0.078, +0.043] (0.520) | 0.832 | FAIL (gross) |
| v5-ew-t.05-d.1-per-name | +0.546 | 0.990 | 1.06 | 0.7391 | 0.7401 | 0.0093 | 0.0430/0.0666 | 0.828 | -0.195 (0.096, -2.03) | [-0.386, -0.007] | [-0.394, +0.003] (0.053) | 0.748 | FAIL (gross) |
| v5-ew-t.05-d.1-fixed-L1.279 | +0.712 | 1.167 | 1.40 | 1.0020 | 1.0033 | 0.0150 | 0.0438/0.0609 | 0.845 | -0.030 (0.013, -2.34) | [-0.054, -0.007] | [-0.055, -0.005] (0.018) | 0.827 | PASS |
| v5-aim-t.05-d.1-fixed | +0.616 | 1.012 | 1.20 | 0.8363 | 0.8374 | 0.0119 | 0.0364/0.0552 | 0.856 | -0.126 (0.112, -1.12) | [-0.346, +0.110] | [-0.353, +0.102] (0.259) | 0.783 | FAIL (gross) |
| v5-aim-t.03-d.1-fixed | +0.643 | 0.983 | 1.24 | 0.7748 | 0.7758 | 0.0092 | 0.0330/0.0543 | 0.776 | -0.099 (0.141, -0.71) | [-0.385, +0.197] | [-0.398, +0.199] (0.506) | 0.795 | FAIL (gross) |
| v5-aim-t.08-d.1-fixed | +0.572 | 1.026 | 1.12 | 0.8841 | 0.8852 | 0.0149 | 0.0401/0.0549 | 0.943 | -0.170 (0.119, -1.43) | [-0.392, +0.075] | [-0.406, +0.065] (0.161) | 0.761 | FAIL (gross) |
| v5-aim-t.05-d0-fixed | +0.618 | 1.035 | 1.21 | 0.8358 | 0.8369 | 0.0116 | 0.0401/0.0599 | 0.942 | -0.124 (0.121, -1.03) | [-0.357, +0.114] | [-0.357, +0.109] (0.289) | 0.784 | FAIL (gross) |
| v5-aim-t.05-d.1-per-name | +0.449 | 0.864 | 0.87 | 0.7936 | 0.7946 | 0.0091 | 0.0363/0.0588 | 0.854 | -0.293 (0.131, -2.23) | [-0.562, -0.004] | [-0.561, -0.026] (0.032) | 0.693 | FAIL (gross) |
| v5-aim-t.05-d.1-fixed-L1.279 | +0.607 | 1.022 | 1.18 | 1.0718 | 1.0732 | 0.0161 | 0.0371/0.0569 | 0.872 | -0.135 (0.112, -1.20) | [-0.356, +0.097] | [-0.359, +0.089] (0.220) | 0.778 | FAIL (gross) |
| v51-ew-t.05-d.1-fixed | +0.759 | 1.193 | 1.49 | 0.7812 | 0.7822 | 0.0110 | 0.0434/0.0603 | 0.838 | +0.017 (0.011, +1.54) | [-0.005, +0.038] | [-0.006, +0.040] (0.137) | 0.846 | FAIL (gross) |

DSR inputs: N = 13, V[SR_n] = 3.0831e-05 (per session; cross-cell sample variance of 13 per-session SRs), SR0 = 0.00946/session (0.150 ann); REF skew -1.279 kurtosis 14.532, T 754.

Stresses (S2 x engine-tiers, S2 x flat-300, S1, S3 terminal-adverse) are in the `.txt` files; S3 (K = 1 adverse write-off) is
negative for every cell (reference -0.102; L cell -0.136), as for v4.1.

## 2. R6' verdict

- **Mechanics** (gross ∈ [0.90, 1.05], |mean net| ≤ 0.02, τ mean ≤ .20 / p95 ≤ .30): **1 of 13 passes** — `ew t.05 d.1 fixed L1.279`
  (gross 1.002, |net| .015, τ .044/.061). Every L = 1 cell under-deploys (gross .71-.88): the partial-adjustment rule at θ ≤ .08 never
  reaches the aim because desired weights keep moving (the aim is chased, not held). `aim L1.279` overshoots (gross 1.072 > 1.05)
  because the same L was set from the ew reference, per R5'.
- **D1** (reference deploys the book): met by the pre-registered L re-run (gross 1.002, held share .984 ≥ .90, |net| .015), not by
  the L = 1 reference (gross .782).
- **Statistics** (paired ΔSR(net) vs the reference, Memmel SE, CBB-21 and Ledoit-Wolf studentized bootstrap, 2000 draws):
  no cell beats the reference significantly. Best ΔSR: v5.1 `opex_at` +0.017 (SE .011, LW 95% [-.006, +.040], p .137, n.s.).
  Among mechanics-passing cells, the only one (`ew L1.279`) is ΔSR -0.030 (SE .013, LW p .018): deploying at gross 1 costs a
  little Sharpe (S2 cost is convex in $ traded) while raising mu from 2.5% to 3.1%/yr.
  Significantly worse than reference: per-name rate (ew -0.195, LW p .053; aim -0.293, LW p .032).
  Aim composition trails ew in all 5 paired cells (-0.10 to -0.29); θ .03/.08 and dust 0 are indistinguishable from θ .05 dust .1.
- **DSR** (N = 13, V[SR_n] = 3.083e-05/session across the 13 cells, SR0 .150 ann): max .846 (v5.1 ew), reference .839, L cell .827.
  No cell reaches .95.
- **Freeze rule:** no cell has TRAIN net SR ≥ 1.0 (max +0.759, v5.1 ew; best mechanics-passing +0.712). **Objective not met on TRAIN;
  no validation; no freeze proposed.** Validation trial #3 stays unspent (needs U1 in any case).

## 3. What v5 changed (vs the frozen v4.1 book)

v4.1 TRAIN (b2, f.25): net +0.687 at mean gross 0.256 (a $256m book; R-1). v5 at gross 1.00 (`ew L1.279`): net +0.712, gross SR
1.167, mu 3.11%/yr, vol 4.37%, τ .044 — the construction now deploys $1bn at essentially unchanged net Sharpe, i.e. ~4x the dollar
P&L of v4.1 at the same risk-adjusted return. The binding constraint is unchanged: gross alpha (~1.17-1.19 SR) against the S2
$1bn cost hurdle (~0.45 SR). Netting ratio .835-.845 (v4.1 .26 — the band suppressed trading; v5 trades nearly its full
standalone turnover).

## 4. Definition-of-done status

| D | status |
|---|---|
| D1 | MET via the pre-registered L re-run (gross 1.002, |net| .015, held .984); L = 1 reference gross .782 |
| D2 | MET: `baseline-v1` band-2 recipe `af058239` / S2 CSV `3f846525` identical with the v5-0 and v5-1 binaries; fixtures green (55/55) |
| D3 | MET: W_aim `54f823c1` with provenance; EW-REFIT identical modulo SHA fields; `ew-theme-v1` byte-stability fixture green |
| D4 | MET: 10 grid cells under the bounded runner; every metric above; DSR N = 10 in the ledger and N = 13 here |
| D5 | MET as "feasibility settled: NO-GO" (T33a 59.9% < 80%); lane parked (R-6); U3 raised |
| D6 | T42 |
| D7 | MET: 26 v5 receipts' commands and bindings reference only `recent-fast-train-2020-2022-v2` (35) and `-fields-v6` (44); no VAL/2023+ token |

## 5. Appendix A — trial accounting

```
Trial accounting (TRAIN 2020-2022 only):
  v3 era: admission 48 + 121; composition 4 + 7; construction 14.
  since run #1: libraries v4 (37), v4.2 (40), v5.1 (38); compositions v4, v4.2, ew-theme-aim-v1, v5.1 x2 (ew-theme-v1, ew-theme-aim-v1);
  construction v4 1 + v4.1 grid 5 + v4.2 2 + v5 grid 10 + 2 L re-run + 1 v5.1 (delisting re-run not run: T33a NO-GO);
  studies T26 5 paper books, T16, T28 audit; stat-arb cluster study (separate family, negative).
  Not trials (reproductions): D2 checks (v5-0, v5-1), v5-0 reference byte-check (same config as grid cell 1).
  validation: #1 (v3, book level), #2 (v4.1, 24%-gross book). Per-candidate VAL statistics: never read.
  DSR inputs: N = 13, V[SR_n] = 3.083e-05 per session (cross-cell), skew = -1.279, kurtosis = 14.532 (reference cell), T = 754.
```

## 6. Addendum after the T41 whole-branch review (2026-09-27; no new trial, no verdict change)

Tool-produced gate numbers (T41 M1/M2): `nav_summ.py` at the T41-fix commit now emits `mean_gross_leverage_all_rows`,
`mean_net_leverage_all_rows` (the ruled R6' basis, labelled `leverage_gate_basis`) and a cost-per-GMV-τ whose numerator covers
the same sessions as τ. Re-run over the same 13 dirs: `build-equity/mega-nav-v5-t40-summ-n13-v2.{txt,json}`. Every other field
is identical to the T40 JSON; only `cost_per_gmv_turnover` changed (and 5 keys were added). The all-rows gross values equal the
§1 "gross (all rows)" column to 4 dp, so all 13 mechanics calls stand.

| cell | gross (all rows) | signed mean net (all rows) | cost per unit GMV τ |
|---|---|---|---|
| v5-ew-t.05-d.1-fixed (REF) | 0.7819 | +0.0106 | 0.00095 |
| v5-ew-t.03-d.1-fixed | 0.7088 | +0.0065 | 0.00080 |
| v5-ew-t.08-d.1-fixed | 0.8439 | +0.0140 | 0.00109 |
| v5-ew-t.05-d0-fixed | 0.7814 | +0.0104 | 0.00095 |
| v5-ew-t.05-d.1-per-name | 0.7391 | +0.0079 | 0.00098 |
| v5-ew-t.05-d.1-fixed-L1.279 | 1.0020 | +0.0148 | 0.00128 |
| v5-aim-t.05-d.1-fixed | 0.8363 | +0.0116 | 0.00105 |
| v5-aim-t.03-d.1-fixed | 0.7748 | +0.0080 | 0.00089 |
| v5-aim-t.08-d.1-fixed | 0.8841 | +0.0148 | 0.00119 |
| v5-aim-t.05-d0-fixed | 0.8358 | +0.0112 | 0.00104 |
| v5-aim-t.05-d.1-per-name | 0.7936 | +0.0078 | 0.00110 |
| v5-aim-t.05-d.1-fixed-L1.279 | 1.0718 | +0.0160 | 0.00141 |
| v51-ew-t.05-d.1-fixed | 0.7812 | +0.0107 | 0.00095 |

With these two columns D4 is fully met.

Disclosures (T41 M3, M4, M6):
- **DSR benchmark (M3).** The §2 DSR (~.84) uses V[SR_n] across 13 near-duplicate cells (paired ρ .97-1.00), which gives
  SR0 = .150 ann. Under the plan §4.E Lo sampling-variance null at N = 13 (SR0 .985 ann), the same cells score REF .342,
  L1.279 .324, v5.1 .353 and aim per-name .183 (range .18-.35). Both DSRs deflate only this 13-cell construction search, not
  the v3/v4/v4.2 family search. No decision depends on the DSR (the freeze rule needs net ≥ 1.0).
- **Netting ratio (M4).** NR = τ_book / Σ w_k τ_k compares a θ .05 partial-adjustment book with full-rebalance (θ = 1)
  standalone turnovers, so it mixes construction with netting. The §3 phrase "v5 trades nearly its full standalone turnover"
  is withdrawn: NR .835 is not a pure netting measure, and v4.1's .26 came from a banded f.25 book.
- **Per-name books differ by scenario (M6).** `per-name-v1` evaluates θ_i at each scenario book's own pre-trade NAV, so the
  S1/S2/S3/flat/engine-tiers books of a per-name cell hold different portfolios (mean rate .0493-.0508). Stress deltas inside
  per-name cells are therefore not construction-controlled. Fixed-rate books share one plan.
