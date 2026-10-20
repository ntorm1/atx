# L2f al-fast-tier sprint note (vol-sota W3, 2026-10-19)

Lane `feat/vol-L2f-al-fast-tier-20261019`, based on the PM prebase `feat/vol-w3-prebase-20261019
@ 53ba6a3f` (root + L2d + L1 + L1c + L2a + L2b). Pool-3, RunId `vol-20261019s-L2f` (checkpoint
`87c803e7` at the user stop), resumed 2026-10-19 on the same pool, RunId `vol-20261019r2-L2f`
(gates, probe, manifests; the source tree is unchanged since `87c803e7`). Plan
`docs/plans/2026-10-17-atx-vol-sota-fit-dag-plan.md` (W3 "L2f"). User directive: anything above
1 s to fit SPY is unusable; this lane owns the microseconds per Andersen-Lake (AL) solve.

Everything here is OPT-IN. Reference and Fast numerics are unchanged: the AL kernel fingerprint
`53c4df68` matches after the edits, and the `AlFastProfile.*` bounds still pass. PM ruling
(2026-10-19): brief steps 4-5 (the `deam_profile = screen` key and the SPY graded-v1 run) are
replaced by (a) selectable through the AlProfile/opts plumbing with the follow-up hook documented,
(b) the us table, (c) the error table, (d) the L1c non-monotone probe on Screen.

All timings are UNQUALIFIED (shared host); the load line is next to each table. Evidence
folders: `C:/atx-cache/quote-benchmark/L2f-al-fast-tier-20261019-*` (manifests `sha256.txt`).

## 1. Profile: where an AL solve + price goes (SPY 23 d)

Inputs: the cohort5 SPY 2026-11-06 chain (23 d, L1's target chain),
`L2f-al-fast-tier-20261019-inputs/spy23-quotes.csv` (322 quotes with bid > 0, extracted from
`fit-timing-20261017-cohort5/input/quotes-SPY.csv`; T = `years`, r = `rate`, q = `sdiv`).
`load_quote_inputs` inverts each mid once on the accurate Reference kernel (outside timing) to
give the contract a sigma; 317 invert. New bench rows (`atx-vol-al-preset-ladder-bench`, env
`ATX_VOL_AL_SPY23=<csv>`): `american/spy23_latency|spy23_profile|spy23_iv/<preset>`.

alprobe zones (`ATX_VOL_AL_PROBE=z`, `ATX_VOL_AL_PROFILE=1`, one preset per process; rdtsc at
2.496 GHz; avx2 binary sha256 `658c4be0...`; `L2f-al-fast-tier-20261019-us/probe-*`):

| per cold solve + price | Reference (accurate, nullopt) | Fast (`al_fast_profile(nullopt)`) | Screen (`al_screen_opts`) |
| --- | ---: | ---: | ---: |
| scheme (nodes, l, sweeps, p) | 12, 24, 2 JN + 4 FP, 48 | same | 7, 16, 2 JN + 3 FP, 48 |
| kernel node evaluations per solve | 11 x 24 x 6 = 1,584 | 1,584 | 6 x 16 x 5 = 480 |
| BAW seed (one Newton per node) | 21.2k cyc (8.5 us) | 15.9k (6.4 us) | 8.3k (3.3 us) |
| static geometry bind (2 exp + sqrt + nb div per pair) | 20.2k (8.1 us) | 20.4k (8.2 us) | 5.9k (2.4 us) |
| one Jacobi-Newton / fixed-point sweep | 60.6k / 59.8k (24 us each) | 25.4k / 24.7k (10 us) | 7.3k / 6.9k (2.9 us) |
| cycles per kernel node evaluation | ~227 (log + 2 std::erfc Phi + exp + cheb) | ~95 (sqrt + 2 Cody Phi + cheb) | ~74 |
| cold boundary solve (zone) | 402.5k (161 us) | 172.9k (69 us) | 52.4k (21 us) |
| price from boundary (euro + premium, p = 48) | 17.7k (7.1 us) | 8.8k (3.5 us) | 7.9k (3.2 us) |
| heap allocations on the path | 0 | 0 | 0 |
| bench `us_per_op` (same run) | 169 | 69.8 | 23.7 |

Where the time goes: 86 % of a Reference solve is the 6 sweeps (1,584 kernel node evaluations at
~227 cycles each, two `std::erfc`-based Phi per node). The Reference cost on SPY 23 d (169-260 us)
is 1.6-2.4x its ladder-grid cost (107 us, al-speed note): consistent with short-dated arguments
pushing `std::erfc` into its slow tail; the Cody Phi of Fast / Screen is argument-independent, so
Fast is 2.4-3.8x Reference here (x1.8-2.3 on the grid). Allocations: none (stack `AlBoundary` +
`AlWorkspace`); the seed and the static bind are ~10 % each.

The inversion (the unit L1 attributed 84-91 % of the SPY fit to), `american/spy23_iv/<preset>`:
american_implied_vol of the contract's Reference price, tol 1e-10, 100 iterations, probe on
the Reference row: 2.29M cycles = 918 us per inversion = 4.6 warm AloPricer prices (153 us each:
sweeps + premium, no seed) + 0.95 cold polishes (211 us). L1's "420-500 us per AL boundary
solve" is inversion wall over the counted cold SEEDS (2.8 per inversion here), not one solve.

## 2. Tier 1 (bit-identical): no step taken

The profile exposes no allocation and no remaining recomputation a bit-identical change can
remove at a measurable level: the al-speed tier 1 already hoisted the sweep-invariant geometry
(geo_zc / weru / wequ / bary / alpha / sqrt_tau), the tip reuse and the tally publishing; what
is left per node is the transcendental work itself (the al-speed note's conclusion: "the special
function, not the iteration count, is the lever, and a bit-identical tier cannot touch it"). The
L2b prepared geometry already removes the static bind (~5 %) where a caller shares an expiry. The
plumbing below is bit-identical for Reference and Fast (fingerprint `53c4df68` at the first
plumbing build `L2f-ladder-wip1` and at the final source, section 7).

## 3. Tier 2: `AlProfile::Screen` (opt-in)

API (`api/pricing/american.hpp`):
- `AlProfile::Screen = 2` — the Fast arithmetic (Cody Phi/phi, log-space boundary, shared BAW
  d1) on whatever scheme the knobs select, with ONE mapping difference in `scheme_from_opts`: the
  boundary-node floor is 4 instead of 6 (Reference / Fast keep 6 byte for byte).
- `al_screen_opts()` — the measured tier: 7 nodes, l = 16, `max_newton_iter` 5 (2 JN + 3 FP,
  early exit at tol 1e-8), p = 48. Never a default; no preset, stored config or record format
  selects it.
- Internally `ws.fast` is now `al_fast_arithmetic(sch)` (= profile != Reference) at the five
  sites that set it; Reference and Fast resolve exactly as before.

Choice: a three-round scan (`american/screen_scan/<set>/<candidate>`, env
`ATX_VOL_AL_SCREEN_SCAN=1`; `L2f-al-fast-tier-20261019-scan/scan-wip1..4`): round 1 nodes 4-9,
l 8/16, sweeps 1-5, p 16-32; round 2 the nodes 6-9 / sweeps 4-6 / p 32-48 neighbourhood; round 3
the chosen point, its neighbours and the early-exit tol (1e-6/1e-7/1e-8: no measurable speed
or accuracy difference — the tol is rarely reached in 5 sweeps). An all-Jacobi-Newton sweep
schedule for Screen was tried (scan-wip2) and changed nothing measurable, so it was reverted.
Findings: sweeps (not nodes) carry the long-dated error; the premium order p carries the
short-dated floor (p = 32 -> 48 takes SPY 23 d from 3.6e-6 to 1.2e-6 relative); l = 16 (vs 8)
takes the dev200 IV max from ~400 bp (one contract) to 0.73 bp. The 4-5 us tier
(nodes 5, l 8, 1 sweep, p 16) is 2.9e-4 relative on SPY 23 d: too coarse for a screen whose
answer a fit trusts, so it stays reachable only through explicit knobs.

Error vs the ACCURATE Reference (nullopt), final binary (`scan-final.json`, load 37 % / 19 %):

| set | points | max abs USD | p99 abs | max rel (P >= 0.01) | p99 rel | IV p99 / max bp (vega-bearing) | worst-abs contract |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| SPY 23 d | 317 | 2.65e-5 | 2.47e-5 | 1.17e-6 | 1.06e-6 | 0.018 / 0.028 | put S 772.99, K 845, T 0.0682, sigma 0.196 (P 72.015) |
| dev200 kernel inputs | 10,694 | 8.70e-3 | 2.33e-3 | 1.48e-5 | 6.80e-6 | 0.043 / 0.73 | put S 1711.84, K 2900, T 2.167, sigma 0.449, r 5.15 %, q 0.25 % (P 1233.87) |
| AL ladder grid | 104 | 2.06e-5 | 1.66e-5 | 2.56e-6 | 2.23e-6 | 0.0023 / 0.002 | put S 600, K 556.4, T 1, sigma 0.167 |
| random (T to 5 y, q to 25 %) | 6,000 | 4.86e-3 | 6.40e-4 | 2.0e-3 | 6.8e-5 | 0.35 / 15.5 | put S 100, K 112.9, T 0.27, r 0, q -1 % (the benign flat corner) |

IV columns: the Screen and the Reference inversions of the SAME Reference price, on contracts
whose Reference sigma +/- 1e-4 bump moves the price by >= 1e-2 * max(P, 1) per unit vol (a price
pinned in the exercise region inverts to a band of vols under any kernel — L1c). Status
mismatches: 0 on every set. The worst inputs (full precision) are in `scan-final.err.txt`.

Ledger contract set: the AlFastProfile 1,350-contract grid plus a 54-contract SPY-like 23 d slice
(`AlScreenProfile.*`): measured 7.7156e-4 * max(P, 1) worst (put S = K = 100, T 4, sigma 0.08,
r 0.10, q 0 — a low-vol long-dated deep-carry boundary that 5 sweeps do not finish), 4.5260e-6
for T <= 0.1, IV 0.47157 bp (call K 70, T 4, r 4.5 %, q 9 %).

Tested bound (`tests/american_test.cpp`, 2-figure round-ups of the measured values; text on
`al_screen_opts`): |dP| <= 7.8e-4 * max(P, 1) on the grid, <= 4.6e-6 * max(P, 1) for T <= 0.1,
|dIV| <= 0.48 bp on vega-bearing contracts.

us per boundary solve + price, SPY 23 d, avx2, main thread pinned to P-core 7, ONE run
(`L2f-al-fast-tier-20261019-us/spy23-us.json`; median of 5 in-process repetitions of per-op p50;
UNQUALIFIED, load 52-70 % before / 29-43 % after; the Fast and Screen rows' last repetitions ran
under a load spike, CV 36 %, so the median repetition is quoted):

| preset | p50 us / solve + price | p99 | inversion p50 / mean us (`spy23_iv`) |
| --- | ---: | ---: | ---: |
| Reference (accurate) | 260.1 | 292.5 | 1,129.6 / 1,318.7 |
| Fast (`al_fast_profile(nullopt)`) | 68.6 | 80.7 | 474.2 / 612.2 |
| **Screen (`al_screen_opts`)** | **20.7** | 31.0 | **107.0 / 119.7** |
| `al_fast_opts` (Reference arithmetic) | 67.9 | 79.8 | 303.3 / 371.7 |
| `al_bulk_opts` (Reference arithmetic) | 16.0 | 19.6 | 102.6 / 99.3 |

The probe run (quieter) gives 169 / 69.8 / 23.7 us. Screen is 7-12x Reference and 3x Fast per
solve, and 10.6x Reference per inversion, at <= 1.2e-6 relative on SPY 23 d. Target <= 30 us: met
(unqualified). `al_bulk_opts` costs the same but is 6e-5 relative on SPY 23 d and 2e-4 on dev200
(scan-wip3 `nb7_l8_m2_p32`): Screen is the same price for ~50x less error.

## 4. Non-monotone region on Screen (L1c probe)

`L2f-al-fast-tier-20261019-nonmonotone/` holds L1c's probe (`nonmonotone_map.cpp`, copy of
sha256 `ba585693...` with ONLY the preset list changed to `fast_profile` + `screen`; the copy is
`2cfb3d3f...`), `build_probe.cmd` (pool-3 vol-rel libraries at `87c803e7`) and `map_summary.py`
(gates 5e-6 / 4e-4). Run 2026-10-19 (resume): `build_probe.cmd nonmonotone_map_screen` exit 0,
`nonmonotone_map_screen.exe map <dir>/map` with `ATX_VOL_FIT_WORKERS=2` exit 0 (5,376 cells, 63 s,
load 13 % before / 20-42 % after), `python -I map_summary.py map/map-cells.csv` exit 0
(`map/map-summary.txt`, sha256 in the folder manifest). S = 100, 8 T x 21 m x 8 (r, q) x 2 sides
per preset, as L1c.

| preset | dip cells | n_dec | outside ex-region | max ex-region excess (rel K max(1,T)^2) | max dip (rel) | gate kappa | headroom | cells over gate |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| fast_profile (= L1c's row) | 387 | 3,394 | 2 | 1.101e-6 | 1.101e-6 | 5e-6 | 4.54x | 0 |
| **screen** | **1,560** | **8,092** | **24** | **4.264e-6** | **3.735e-6** | 4e-4 | **93.8x** | **0** |

The `fast_profile` row reproduces L1c's row exactly (387 / 3,394 / 2 / 1.101e-6, same argmax
cell: call T 1, m 1.02, r 0.02, q 0.07), so the probe is the same probe. Screen:
- Largest relative dip 3.735e-6 (Fast 1.101e-6, 3.4x); largest exercise-region excess 4.264e-6
  at call T 1, m 1.01, r 0.02, q 0.07 (K 99.01, sig_edge 0.0325): 94x under the WIDE gate 4e-4
  that `al_non_monotone_gate` gives every non-accurate scheme (Screen is one: `accurate_scheme`
  matches only nullopt / `al_fast_profile(nullopt)`), and 1.17x under the accurate kappa 5e-6.
- Region bounds are the same as Fast's (T in [0.1, 5], m in [1.01, 1.6]) but the region is
  DENSER: 4.0x the dip cells, and it now covers r = q (295 call + 295 put cells at zero carry
  advantage, where the accurate scheme has none) and 24 cells with a decrease outside the
  exercise region (Fast 2). Zero cells over the gate on either preset; n_err 0.
- FINDING for L2g (not tuned away here): the Screen dips are not an exercise-region artifact
  only; they are the 5-sweep truncation error itself (3.7e-6 relative to K, the same order as the
  tested price bound on these inputs). A Screen inversion that lands in the band
  `price - zero_vol <= 4e-4 K max(1, T^2)` is refused as NonMonotoneRegion over a band ~80x
  wider than the accurate route's; L2g must treat that refusal as "seed unavailable -> accurate
  inversion", never as a quote refusal, and must never classify Screen as the accurate scheme
  (its headroom under 5e-6 is only 1.17x).

## 5. The follow-up hook (`deam_profile = screen`)

Per the PM ruling the wiring is a follow-up lane (needs L2e's inversion strategy and this tier in
one tree). The hook is one enum value:
- `QuoteSliceDeamProfile` (`api/fitting/quote_slice.hpp:111`) gains `Screen`; the policy parser
  accepts `"screen"`; `quote_slice.cpp:1330` admits it.
- `SliceDeAmModel` (`quote_slice.cpp:658-698`) holds `fast_ = al_fast_profile(fit.al_opts)`; a
  Screen kernel holds `al_screen_opts()` instead (independent of `fit.al_opts`).
- `DeAmIvView::kernel()` (`fitting/deam_iv_cache.cpp:67`) maps Screen to its kernel, and
  `invert()` (`:86-102`) must NOT reuse the Fast guard as is: `kDeAmFastMinRelativeVega = 1e-5`
  assumes the Fast price bound 1e-12 * max(P, 1); Screen's tested bound is 7.8e-4 * max(P, 1)
  (4.6e-6 for T <= 0.1), i.e. up to 0.48 bp of IV. So a Screen IV must be a SEED: finish with one
  accurate Newton / polish step on the Reference map (`american_implied_vol(..., warm_start =
  screen_iv)` or one `andersen_lake` + chord), or widen every downstream band test by the bound.
- The cache key needs no change: `deam_iv_key` already hashes `AlOpts::profile` and
  `config.channel.deam_profile`; the carry memo keys on the profile too.

## 6. Inherited fixes

- **AlBoundaryHandle (L2b review m2).** The handle observed its geometry without enforcement.
  Now: `AlBoundaryHandle(const AlExpiryGeometry &&, Side, double) = delete` (a temporary cannot
  bind), and `AlExpiryGeometry::Impl::generation` is bumped by every `prepare()`; `solve()`
  records it and `price_from_boundary` returns NaN when the geometry has been re-prepared since,
  until the next `solve()`. Tests: compile-time `static_assert`s and
  `AlBoundaryHandle.RePreparedGeometryRefusesUntilSolvedAgain` (tests/american_geometry_test.cpp).
  Bit-identical (fingerprint; the 11 L2b identity tests pass).
- **chain_batch_bench status histogram (review-L1c minor).** `bench/chain_batch_bench.cpp` counted
  statuses in a 6-slot array indexed by `ChainRowStatus`; L1c's `NonMonotoneRegion = 6` would have
  written past it. Now 7 slots (`kChainRowStatusCount`) with a `static_assert` tied to the last
  enumerator, a guarded index, and an `st_non_monotone` counter.

## 7. Gates

Checkpoint (`87c803e7`, RunId `vol-20261019s-L2f`):
- AL fingerprint (vol-rel-avx2 `american/ladder_fingerprint`): `53c4df68...dcea` MATCH at the first
  plumbing build (`L2f-ladder-wip1`, exe sha256 `b9079b98...`) and at the final source
  (`L2f-ladder-final`, exe sha256 `658c4be0...`); ten section fnv1a lines equal L2b's
  (`L2f-al-fast-tier-20261019-fp/`).
- vol-rel build of pricing / simd slices, chain-batch tests, quote-slice tests, quote-bench:
  exit 0. Direct run `atx-vol-pricing-slice.exe --gtest_filter=AlScreenProfile.*:AlFastProfile.*:
  AlBoundaryHandle.*:AlExpiryGeometry.*:AlPricePack.*:AdjointGreeksAmerican.*`: 37 passed.

Resume (RunId `vol-20261019r2-L2f`, same source tree, pool-3 rebuilt at the tip; evidence
`L2f-al-fast-tier-20261019-gates/` with `sha256.txt`; `-Jobs 2`, `ATX_VOL_FIT_WORKERS` 2 except
the cap-3 name set):
- `^(pricing|simd)/` ctest at cap 3 (`ATX_VOL_FIT_WORKERS=3`, ctest `-Jobs 2`): exit 8, 923 tests,
  2 failed (the two known: `AmericanIv.PricePolishDefaultOffIsBitForBitControl`,
  `SimdNanSafety.BaryEvalPd_ExactNodeHit_ReturnsNodeValue`), 90 s, load 20 % before / 14 % after.
  Failed/skipped/not-run name set via the review-v2 `gates/names.sh` (`abe9a35c...`): 19 names,
  sha256 `52324ef8...` = the W0 root set byte for byte (the PricingExecutor load flake did not
  appear).
- Default parity, dev200 `--policy auto`, cap 2, lane binary `atx-vol-quote-bench-87c803e7.exe`
  (`b453db05...`): `c89d0739...` MATCH, exit 0, 13 s, stderr 0 bytes.
- `^chain-batch:` ctest: 31/31, exit 0. Direct gtest `AlFastProfile.*:AlScreenProfile.*:
  AdjointGreeks*:AlExpiryGeometry.*:AlBoundaryHandle.*:AlPricePack.*`: 42 passed (= the 37 of the
  checkpoint + the 5 `AdjointGreeksEuropean` the wider `AdjointGreeks*` filter adds); printed
  Screen worst 7.71555e-4 / short-dated 4.52596e-6 / IV 0.471574 bp, Fast worst 1.42e-14 /
  1.49e-8 bp (unchanged).
- PCH-off `vol-hygiene`: `configure -Preset vol-hygiene` exit 0; `check` of 19 TUs (every .cpp
  includer of `american_boundary.hpp` incl. `american_boundary_avx2.cpp`, plus the first-order
  `american.hpp` includers american_geometry_test, batch_chain(+test), american_iv(+test),
  american_batch, deam_iv_cache, american_price_slice, quote_slice) exit 0, 45 s, 0 diag lines.
  The two touched bench TUs are NOT in the vol-hygiene configure (`check: no object target
  found`; bench targets are not built under that preset) — they compile PCH-on under vol-rel and
  vol-rel-avx2 (exit 0 above); pre-existing preset scope, not a lane finding.
- AL fingerprint at the tip source (fresh vol-rel-avx2 build `ede388ef...`): fp blob sha256
  `53c4df68...dcea` MATCH (= the checkpoint's `fp-final.bin` byte for byte), ten section fnv1a
  lines identical (`ladder_grid_rungs e439b788...`, ..., `batch_resolved affa6f14...`). The docs
  commits after `87c803e7` touch no source file (`git diff --stat` in the report).
- Probe: section 4 (Fast row = L1c's; Screen 0 cells over its gate).
- Revert checks (one combined build, each revert guarded by a distinct test; restored blobs
  `american.cpp 849c3cb3`, `american_boundary.hpp d8b692cb`, `american.hpp fc2f60d6` at the time,
  before the comment-only header text): Screen arithmetic -> Fast only, node floor 6, tier at 2
  sweeps, handle generation check removed => 6 tests FAILED (NeverTheDefault scheme, NodeFloor,
  RunsTheFastArithmetic bits, PriceWithinMeasuredBound, ImpliedVolWithinMeasuredBound, handle
  RePrepared); rvalue-ctor delete removed => both `static_assert`s fail to compile.
- Not measured in this lane (ruled out of scope): SPY wall with `deam_profile = screen` (L2g).

## 8. What L2g must assume

- **The Screen bound is a price bound, not a rounding twin.** `al_screen_opts()` vs the accurate
  Reference: |dP| <= 7.8e-4 * max(P, 1) on the ledger grid (worst: a low-vol, long-dated,
  deep-carry put that 5 sweeps do not finish), <= 4.6e-6 * max(P, 1) for T <= 0.1, |dIV| <= 0.48 bp
  on vega-bearing contracts (tests `AlScreenProfile.*`); measured 1.2e-6 relative on SPY 23 d,
  1.5e-5 on dev200 (8.7e-3 USD worst), up to 2e-3 relative on the random set's benign flat corner.
  The Fast-kernel guard `kDeAmFastMinRelativeVega = 1e-5` (built for a 1e-12 bound) does not
  transfer; L2b's certification allowance (1e-10 * max(|P|, 1)) does not either.
- **Seed only (section 5).** A Screen IV is a seed: finish with one accurate Newton / polish on the
  Reference map (`american_implied_vol(..., warm_start = screen_iv)` or one `andersen_lake` +
  chord), or widen every downstream band test by the bound. Never serve a Screen IV or price.
- **Non-monotone region (section 4).** Screen takes the WIDE gate 4e-4 * K * max(1, T^2)
  (headroom 94x; 0 cells over it on L1c's map), but its dip region is 4x denser than Fast's and
  includes r = q. A Screen `NonMonotoneRegion` refusal must route to the accurate inversion, not
  to a quote refusal; Screen must never be classified as the accurate scheme (1.17x headroom
  under 5e-6).
- **Cost (section 3, unqualified).** ~21-24 us per cold boundary solve + price and ~107 us per
  Screen inversion on SPY 23 d (vs 260 / 1,130 Reference, 69 / 474 Fast), so a Screen seed plus
  one accurate confirm (one cold Reference solve ~160-260 us, or Fast ~69 us) is bounded below by
  the confirm; L2g should measure a Fast-confirm variant as the status file suggests.
- **Opt-in only.** `AlProfile::Screen` is reachable only through an explicit `AlOpts`
  (`al_screen_opts()` or the knobs); no preset, policy key, stored config or record format selects
  it. The hook is one enum value (section 5); `deam_iv_key` already hashes `AlOpts::profile`.
