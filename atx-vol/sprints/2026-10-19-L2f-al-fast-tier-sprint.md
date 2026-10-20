# L2f al-fast-tier sprint note (vol-sota W3, 2026-10-19)

Lane `feat/vol-L2f-al-fast-tier-20261019`, based on the PM prebase `feat/vol-w3-prebase-20261019
@ 53ba6a3f` (root + L2d + L1 + L1c + L2a + L2b). Pool-3, RunId `vol-20261019s-L2f`. Plan
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

## 4. Non-monotone region on Screen (L1c probe) — NOT RUN (user stop)

Prepared, not built or run: `L2f-al-fast-tier-20261019-nonmonotone/` holds L1c's probe
(`nonmonotone_map.cpp`, copy of sha256 `ba585693...` with the preset list cut to
`fast_profile` + `screen`), `build_probe.cmd` (pool-3 vol-rel libraries) and `map_summary.py`
(gates 5e-6 / 4e-4). Next: `build_probe.cmd`, then `nonmonotone_map_screen.exe map <dir>` with
`ATX_VOL_FIT_WORKERS=2`, then `python -I map_summary.py <dir>/map-cells.csv`; the fast_profile
row must reproduce L1c's (387 dip cells, 1.101e-6) before the screen row is read.

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

## 7. Gates (WIP checkpoint at the user stop)

- AL fingerprint (vol-rel-avx2 `american/ladder_fingerprint`): `53c4df68...dcea` MATCH at the first
  plumbing build (`L2f-ladder-wip1`, exe sha256 `b9079b98...`) and at the final source
  (`L2f-ladder-final`, exe sha256 `658c4be0...`); ten section fnv1a lines equal L2b's
  (`L2f-al-fast-tier-20261019-fp/`).
- vol-rel build of pricing / simd slices, chain-batch tests, quote-slice tests, quote-bench:
  exit 0. Direct run `atx-vol-pricing-slice.exe --gtest_filter=AlScreenProfile.*:AlFastProfile.*:
  AlBoundaryHandle.*:AlExpiryGeometry.*:AlPricePack.*:AdjointGreeksAmerican.*`: 37 passed.
- Revert checks (one combined build, each revert guarded by a distinct test; restored blobs
  `american.cpp 849c3cb3`, `american_boundary.hpp d8b692cb`, `american.hpp fc2f60d6` at the time,
  before the comment-only header text): Screen arithmetic -> Fast only, node floor 6, tier at 2
  sweeps, handle generation check removed => 6 tests FAILED (NeverTheDefault scheme, NodeFloor,
  RunsTheFastArithmetic bits, PriceWithinMeasuredBound, ImpliedVolWithinMeasuredBound, handle
  RePrepared); rvalue-ctor delete removed => both `static_assert`s fail to compile.
- NOT RUN at the stop: `^(pricing|simd)/` name-set ctest at cap 3, default parity `c89d0739`,
  chain-batch ctest, PCH-off `vol-hygiene` on the touched headers, the L1c probe (section 4).
