# Sprint note — L3p-shootout: parameterisation family x shape class on the W2b binary (2026-10-20)

Lane `L3p-shootout`, branch `feat/vol-L3p-shootout-20261020`, base `be379ad4` (W2b root), pool-35, RunId `vol-20261020r3-L3p`.
Tool-only measurement lane: no build, no source edit, no new policy key. Binary
`C:\atx-cache\quote-benchmark\integrate-W2b-20261020-parity\bin\atx-vol-quote-bench-27eba264.exe`
(sha256 `e86e0b98d979dd03fa64795f8a47b0f58c80670fafdfecbc46937f90e88206b8`, verified before every stage). Evidence folder
`C:\atx-cache\quote-benchmark\L3p-shootout-20261020\` (`RESULTS.md`, `sha256.txt`). Brief: workspace `lane-L3p-shootout-brief.md`.

## 1. What a policy can select (be379ad4)

- The graded block (`tools/quote_bench_policy.hpp:772-777`) has no family key. The family is a PER-PROFILE key:
  `profiles[].curve` = `convex-dense | essvi | svi | linear-variance | c8-event | spline-vol` (point method, `parse_curve_kind`,
  `vol_curve.hpp:136-149`) or `knot-spline` (channel knot spline, served as spline-vol) — `quote_bench_policy.hpp:243-254`; and
  `profiles[].channel.family` = the family that solves the channel objective (`:263-277`; linear-variance refused).
- `graded-v1` (`d7f89d13`) is a regime MIXTURE: dense -> convex-dense point, ordinary / sparse -> essvi point, with the graded
  channel knot rung serving `spline-vol` rows where the point fit fails certification (policy echo + `per_profile_counts`).
- The fit-policy validator (`src/fitting/quote_fit_policy.cpp:128-176`) requires `min_quotes >=` the family floor: Svi 6, C8 10,
  SplineVol max(6, spline.min_obs) = 6; the base `sparse` profile (4) / `ordinary` (6) refuse plain `curve` selection of svi /
  c8-event / spline-vol ("invalid quote quality or model point count"). Lifting `min_quotes` (existing key) to the floor on the
  refusing profiles makes them run. LinearVariance is refused as a point family ("not a risk-fit family", `:153-155`) and as a
  channel family — a GAP in both modes.

## 2. Arms, runs, hashes

Arms (every W2b speed key OFF; `ATX_VOL_FIT_WORKERS = 2`; `--dividends funding-pv`; `--timing-detail`): graded-v1 control
(`d7f89d13`); point-convex-dense (`86355dcf`); point-essvi (`b3ce8e17`); point-svi-mq6 (`76ccb562`); point-c8-event-mq10 (`c0262beb`);
point-spline-vol-mq6 (`c0220159`); knotspline-v1 (`579e7720`); plus the W0 keys-ON ladder point / strict receipts (`61ec4c1a` /
`574f994b`) reused by sha256. Control: cohort5 graded-v1 at cap 2 -> `171fa2d2` MATCH; dev200 / dev200b graded-v1 = parity CSVs
`fc277413` / `46ad6c26` reused. Prediction CSVs (cohort5): convex-dense `a6e5991e`, essvi `83ad18db`, svi `05adacfb`, c8 `d30b1c82`,
spline-vol `49bf4b5a`, knot `05317167`; (dev200b) `e1d420de` / `bdbf1714` / `4f2024a0` / `4ba4bc09` / `95fe3272`, knot `2f635356`
(bench-v2, `final2` binary); (dev200) convex-dense `32dfbee3` only. Matrix STOPPED 2026-10-21T00:59:22Z by PM directive (no compute
20+ min): dev200 essvi / svi / c8 / spline-vol / knot not run. 22 class-stratified scores (`--split all --class-manifest ad323ccb`),
all exit 0. Timings unqualified (shared host): cohort5 fit+reprice 31 s (essvi) - 92 s (knot) vs graded-v1 57 s; de-Am solves 120k
(essvi) - 371k (knot) vs 249k.

## 3. The table and the verdicts

Served violations 0/0/0 in all 22 arms x 11 classes. all_rows (coverage / MAE/spread / containment / vol median bp):
- cohort5: graded-v1 38.4 % / 0.210 / 91.6 % / 24.8; essvi 58.6 % / **3.12** / 75.5 % / 70.5; svi 54.7 % / 1.83 / 78.2 %; c8 41.4 % /
  1.84 / 81.3 %; spline-vol 33.2 % / 0.097 / 97.0 % / 17.4; knot 30.5 % / 0.103 / 96.7 % / 16.3; ladder-strict 44.3 % / **0.070** /
  **98.9 %** / 16.6.
- dev200b: graded 87.2 % / 0.116 / 98.8 % / 175; essvi ~= graded; convex 75.6 % / 0.114 / 99.4 %; svi 84.3 % / 0.114 / 98.8 %; c8
  83.8 % / 0.128 / 98.2 %; spline-vol 81.3 % / 0.105 / 99.7 % / **143.7**; knot 82.7 % / 0.107 / 99.7 %; ladder-strict 89.5 % /
  **0.101** / **99.8 %** / 149.
- dev200 (4 arms): graded 78.7 % / 0.109 / 98.8 % / 177; convex 75.4 % / 0.105 / 99.2 %; ladder-strict 85.6 % / **0.096** / **99.75 %** / 144.6.
Verdicts: (1) the knot-first ladder (strict) beats graded-v1 on every cohort in coverage, MAE/spread, containment and vol median at once.
(2) SplineVol (point or channel — the same 21,507 / 61,581 rows, within 0.006 MAE/spread) is the most accurate pure family wherever it
serves (cohort5 high_vol 0.044 / 99.8 %, long_dated 0.043 / 99.95 %; dev200b no_symbol_class 0.088 / 99.95 % / 133 bp), at 2-6 pts
less coverage. (3) essvi / svi / c8 as point families buy coverage OUTSIDE the market: cohort5 +42 / +52 / +8 % rows at MAE/spread
1.8-3.1 and containment 75-81 %; SPY 18-34 % coverage at 5-9 spreads; dev200b long_dated svi p99 USD 12.7 (graded 2.2) — all with 0
served violations, so the certificate set does not bound distance to market (a certification gap, not a family gap). (4) Classes no
family serves: SPY `index_etf_dense` (best ladder-strict 14.4 % at 96.3 %; target >= 95 % served), `short_dated` (contained coverage
<= 30 % on cohort5; vol medians 536-2,500 bp everywhere), `sparse` (0.24-0.31 MAE/spread in every arm vs 0.15), mega-cap median
|IV - srVol| 13.5-67 bp vs <= 5 bp. (5) Ladder vol-bp MAE (C6 row), per class, unpaired: all_rows -0.6 % (dev200) / +3.5 % (cohort5) /
+6.0 % (dev200b) with medians 15-31 % better and MAE/spread 11-67 % better = coverage composition (low-vega rows); EXCEPT
`mega_cap_single` +16 / +18 % on both dev cohorts (and dev200 long_dated +16 %) regardless of coverage direction = a genuine wing-tail
regression of the knot spline on dense single names (cf. L315). SPY / high_vol improve 13-39 %.

## 4. Recommendations for the W3-b parameterisation lanes

1. L3q (P0): a served distance-to-market gate + LP/QP in-channel repair, SPY first (cell: index_etf_dense ladder-strict 14.4 % / 0.138 /
   96.3 %; parametric arms 18-34 % at 5-9 spreads). 2. L3r1 (P0): fix the ladder's mega-cap / LEAPS wing tail (+16-18 % vol MAE cells),
   then promote ladder-strict as the default stack (beats graded-v1 on all three cohorts, 0 violations). 3. L3d (P1): sparse /
   short-dated are input / admission problems, not family problems (nine families move them < 0.07 MAE/spread). Not recommended:
   essvi / svi / c8 accuracy lanes; a LinearVariance lane (GAP G1). GAPs for the owner: G1 LinearVariance unreachable under graded
   admission; G2 no per-shape-class family routing; G3 identifiability floor only via min_quotes.
