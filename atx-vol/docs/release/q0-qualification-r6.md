# atx-vol r6 release qualification (Q0) of `e5438aa5`

Lane Q0, run `vol-sota-r6-20261023`, 2026-10-23. Frozen candidate `feat/vol-resume5-20261022 @
e5438aa5d406f5ae28b21f59ee1fcc15cc4ae782` (final code root `546201d8`). Evidence
`C:\atx-cache\quote-benchmark\Q0-20261023-qualification\` (`sha256.txt`); lane report
`.superpowers/sdd/2026-10-18-vol-sota/lane-Q0-qualification-r6-report.md`. Machine-readable receipts (master plan
section 8, every field, one object per milestone): `q0-receipts-r6.json`. Open items: `known-issues-r6.md`.

Every number below was measured on the frozen root by Q0 unless it names a lane. "Quiet" = no other r6 lease alive
(`lease-worktree.ps1 -Status`) with the host load line recorded before and after the row; "dev" = a development
row on a shared host. Nothing was promoted, tuned or fixed.

## Frozen inputs

Source `e5438aa5` (clean), clang-cl 18.1.8, `vol-rel` `/arch:AVX2 /O2 /Ob2 /DNDEBUG` + PCH (`vol-rel-avx2`: the
same with ThinLTO, for the bench rows), `EIGEN_NO_CPUID` with pinned cache sizes on every Eigen consumer. Policies,
inputs, eligibility (scorer / harness blobs + class manifest `ad323ccb`), references (D0, B0, K3 manifests) and
thresholds (plan section 3.3, the gate table, the `progress.md` rulings) are hashed in the evidence `freeze\` folder,
written before any result.

## Release checks

| Check | Result |
|---|---|
| Full test set build (once; the monolith included) | 75 / 75 bounded wrapper calls exit 0, 0 diagnostics |
| Full labels, ONE run (`atx_vol_fast` 4,785 + `atx_vol_slow` 259) | fast 4,703 pass / 38 skip / 4 disabled / **40 fail**; slow 207 / 35 / 3 / **14 fail**; 6 = the ignore list; **48 = Q0-F1..F48**, all pre-existing (they reproduce on 2026-10-16..19 binaries, before the plan base); none in the r6 product suites |
| 9 / 9 default hashes, workers 2 and 1 | 9 / 9 MATCH at both widths; both `sha256sum -c` logs persisted |
| AL fingerprint (vol-rel-avx2) | `53c4df68` MATCH |
| Name set `^(pricing\|simd)/` at cap 3 | `52324ef8` MATCH (2 failures, both on the ignore list) |
| Four-cohort K3 audit, native route | 0 violations / 0 unresolved on SPY, cohort5, dev200, dev200b |
| C3 matrix arms (51 cells) | 51 / 51 predictions byte-identical to C3's recorded cells; 0 served shape violations |
| U0 sample determinism re-run | byte-identical (72 / 72 jobs; combined `bc5547c7`); served set vs U0's run lost 0 / gained 0; R1-R3 still reproduce |

## Gate table (merged-root numbers)

| Gate | Status | Numbers |
|---|---|---|
| G0 reconciled source + W2c evidence | **PASS** | full test set builds; 9 / 9 at workers 2 and 1; fingerprint `53c4df68`; name set `52324ef8` |
| G1 frozen clocks, cold reset, comparator | **PASS-with-named-gaps** | B0's runner drives the G3 row (P core proven, sibling idle 10 / 10, attribution valid); B0's deferred minors stand |
| G2 qualified kernel + error control | **PASS-with-named-gaps** | four-cohort K3 audit 0 / 0; quiet sub-clocks: confirm 41.2 us (bar 50) met, family side 662 ms (300), final grid 512 ms (200), endpoints 340 ms (120) MISS |
| G3 < 1 s SPY, one core | **MISS** | quiet 10 trials: 2.433 / **2.473** / 2.766 s (min / median / max); inverses 1,000 ms (K3 enclosure 605), final grid 512, endpoints 340 |
| G4 < 1 s + coverage / accuracy | **MISS** | C1 PASS (0 violations); C2 MISS (dev200 88.33 %, dev200b 93.68 %, SPY index 78.25 % = 26 / 33); C3 MISS (SPY MAE / spread 0.215, IV median 9.0 bp); F2 not met |
| G5 cash dividends | **PASS-with-named-gaps** | accuracy class per ruling; drift within u NOT met; cash latency budgets MISS |
| G6 serving / analytics product | **MISS** (one H2 row) | quiet: H1 record 100 ns, book 126 ns p50 (SPY) MET; record IV 198 ns MET; tier-0 300-chain 14.5 us MET; **mixed 300-chain 141-267 us vs 100 us MISS**; H3 miss 0, torn 0; A1 row measured |
| G7 all-core / cohort / universe | **MISS** | quiet F3 0.777 s vs 150 ms; F4 / F5 MISS (P0 dev rows); universe sample deterministic, R1-R3 open |

## Unmet supported-scope requirements (release closure)

1. F1 / G3: 2.473 s quiet vs < 1.000 s (no trial under the bar; the 100-trial qualification was not run).
2. F2: the corrected high-coverage combined arm < 1 s.
3. F3 0.777 s vs 150 ms; F4 p99 236.9 ms vs 100 ms; F5 16.48 s vs 10 s.
4. C2 coverage (dev200 / dev200b / SPY index) and per-class containment cells under 98.5 %.
5. C3 MAE / spread and identified-IV median distance.
6. D1 per-event drift within u; P1 / P2 / P3 cash-dividend latency budgets.
7. Continuous-carry P1 / P2 not re-measured at the frozen root; A0's 300-chain <= 1 ms stretch missed.
8. H2 mixed 300-contract served chain; book p50 under concurrent publication; default-plane ladder IV `Unconfirmed`.
9. The older PricerFitter / curve-fit / SurfaceV2 / mark stack: 48 failing tests (Q0-F1..F48), not routed through
   the serve book.
10. Python wheel DLL bundling and 4 unexplained binding failures.
11. Not qualified in r6: sanitizers (none exist), the public-engine comparison (both frontiers), storage round-trips
    beyond the fit-archive tests, the holdout (Q1).

## Promotion candidates (named with evidence; NOT promoted - Q2)

| # | Candidate | Frozen-root evidence | Open Q2 preconditions |
|---|---|---|---|
| P1 | K5's enclosure-width fix on the keys-ON native route (K4 policy `1a488f48`) | four-cohort audit 0 / 0; SPY id `0c18287f` at workers 2 and at 16 workers (10 / 10) | the route misses G3 (2.47 s); promote the route as a whole (K4 + K5 + K4b) with new full hashes |
| P2 | C3's call floor `serve.call_floor = discounted_intrinsic` with C2's validate | 51 / 51 matrix cells byte-identical to C3's; 0 violations | the parity-coupled clamp first (TIGR 20770 -36 / +34) or an explicit PM acceptance; IEF 20805 re-measured; D2 exchange re-measured; new 9 hashes |
| P3 | K4b's m1 / m2 / A17 typing | dev200b audit 0 / 0 (K5's violating root no longer typed); `1524ce15` MATCH | attribute the 43 BABO rows (up to 0.506 vol) to typed roots |
| P4 | `calendar_parity_clamp` (K4b) | not re-run at Q0 | re-hull the raised puts (K4b m-6) |
| P5 | C4's in-box projection rung | not re-run at Q0 (W2c-m: `c9d14f14` MATCH on the code-identical binary) | PM ruling: no promotion until moved-node re-implication is cheap (cold 17.36 -> 63.50 s) |
