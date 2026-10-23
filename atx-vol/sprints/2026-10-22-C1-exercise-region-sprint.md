# C1 — exercise-region and fallback semantics (r5, 2026-10-22)

Lane C1 of the vol-sota master plan (Sprint 2 package C1; deep review F04, second half).
Base: the r5 root `9f05a40e`. It completes the L3k checkpoint (`a01ee2e3`, `792ea5a1`,
cherry-picked with equal patch-ids) and adds the shared classification. K4 consumes it.
Lane report: `.superpowers/sdd/2026-10-18-vol-sota/lane-C1-exercise-region-r5-report.md`;
evidence: `C:\atx-cache\quote-benchmark\C1-20261022-region\` (`sha256.txt`).

## 1. One classification

`classify_exercise_region(S, K, T, r, q, sigma, is_call, boundary)`
(`api/pricing/american_region.hpp`) decides in the dispatch's own order:

| order | class | when | price on the AL routes |
|---|---|---|---|
| 1 | `expiry_limit` | T <= 1e-12 | the payoff (exact, every regime) |
| 2 | `zero_vol_limit` | sigma <= 1e-8 | closed form max(df (F - K)+ / (K - F)+, spot intrinsic) |
| 3 | `european` | put r <= 0 && q >= r; call q <= 0 && r >= q | Black-76 |
| 4 | `unsupported_regime` | put q < r < 0; call r < q < 0 | none (typed) |
| 5 | `exercise_plateau` | American, internal-put spot <= B(T) | Intrinsic: K - S exactly; KernelFloor: floor >= intrinsic |
| 6 | `continuation` | American, spot > B(T) (or B = NaN) | euro + premium (both rules) |

`B(T)` is the boundary of the INTERNAL put the kernel solves (a call: spot K, strike S,
rate q, yield r) at tau = T, read from the Lobatto end node by
`amer::al_boundary_at_expiry(y_T, xmax)` = `b_from_y(y[n-1], xmax)` — bit-identical to
`al_boundary_now` / `al_boundary_at(bnd, T)` (z = 2 sqrt(T/T) - 1 = +1 exactly, the
exact-node guard returns `y[n-1]`; pinned per solved lane in the three-site test).

All three price sites call it:

- scalar uncached `amer::al_put_price_from_boundary(..., rule, AlExerciseClass* cls)`;
- scalar cached `al_put_price_from_boundary_cached(..., rule, cls)` (+ the test seam
  `al_put_price_from_boundary_cached_probe(..., bind_cache, cls)`);
- laned AVX2 `simd::detail::price_put_pack_at_avx2(pack, spot, strike, xmax, rule,
  AlExerciseClass cls[4])` — per lane, the SAME scalar `al_boundary_at_expiry` on the
  lane's `Y[nb-1]` / xmax and the SAME classifier.

WHY the laned site goes through scalar code: the L3k laned decision used its own
`xmax * exp_pd(-sqrt(Y[nb-1]))` and a vector compare. Mutation M2 restores exactly that
decision: 528 of the 1,320 points at B and +-1 ulp then disagree with the scalar sites
(the laned site calls B and B - 1 ulp "continuation"). Four scalar `exp` per pack call,
on the opt-in paths only.

KernelFloor with no class output is the pre-L3k code at every site (9/9 default hashes
and the fingerprint `53c4df68` match). A classified call whose class is outside the
American arm (the dispatch handles those before any boundary exists) returns its class
and a quiet NaN, never a price.

## 2. The classified entry (the route hook)

`andersen_lake_classified(S, K, T, sigma, r, q, side, opts, rule)` and the geometry
overload return `AlRegionPrice { price, boundary, cls }`: `price` bit-identical to
`andersen_lake(..., rule)`, `cls` the classification above, `boundary` B(T) when one was
solved. The double-continuation regime returns `{NaN, NaN, unsupported_regime}` instead
of `NotImplemented` (so a router does not parse messages); validation and boundary-solve
errors pass through unchanged.

## 3. The L3k bound pin, resolved against the reference

The L3k pin compared KernelFloor with Intrinsic (`|P_I - P_K| / (K max(1, T)^2) <=
1.2e-6`, all presets) and failed for `al_bulk_opts` at 3.64e-6. That comparison cannot
say which rule is right, so each in-region cell is now settled independently:

- converged Andersen-Lake (B0's comparator uncertainty scheme: 32 nodes, quadrature
  64/64, 16 sweeps, tol 1e-14): the cell is inside the converged boundary by a margin
  > 1e-3 and > 100x |B_conv - B_accurate| / B_conv, and the converged price equals the
  intrinsic (<= 1e-9 x scale; its own excess <= 1.5e-7 USD);
- a different method, the Broadie-Detemple binomial lattice
  (`american_discrete_div_price`, no dividends, SplicedBbs) at 2,001 and 1,000 steps:
  exercises at the root, |P - intrinsic| <= 1e-9 K (node rounding ~5e-11 USD).

So the exact value is the intrinsic, Intrinsic's error is zero, and KernelFloor's excess
is the preset's own in-region error. The failing cell: put S 100, K 110, T 2, sigma 0.05,
r 0.02, q -0.03 — reference K - S = 10 (converged B = 107.33542, |B_conv - B_acc| = 3.1e-5,
the spot 6.8% inside; converged - intrinsic = 0; lattice 8,001 / 4,000 steps - intrinsic =
5.1e-11 / 2.2e-11), `al_bulk_opts` KernelFloor 10.001602 — error 1.602e-3 USD = 3.641e-6
of scale. The preset is wrong, not the comparator; the 1.2e-6 pin was aspirational
(al_bulk_opts' documented price accuracy is ~1e-3). Pins = 2-figure round-ups of the
measured maxima, each written beside its pin:

| preset | measured max (x K max(1,T)^2) | pin | worst cell |
|---|---|---|---|
| accurate | 5.011e-7 | 5.1e-7 | call K 62.5 T 5 sigma .30 r .05 q .10 |
| Fast profile | 5.011e-7 | 5.1e-7 | same |
| al_fast_opts | 9.570e-7 | 9.6e-7 | same |
| al_bulk_opts | 3.641e-6 | 3.7e-6 | put K 110 T 2 sigma .05 r .02 q -.03 |

On the wider L1c probe grid the coarse presets' in-region excess is larger (LEDGER
2026-10-19: ambiguous-band top 9.1e-5 al_fast_opts, 3.2e-5 al_bulk_opts); these pins
are the AlMonotone grid's class, not a global bound.

## 4. Behaviour pinned (`tests/american_region_test.cpp`, suite AmericanExerciseRegion)

| test | what | error class |
|---|---|---|
| ClassifierFollowsTheDispatchOrderAndTheCallMirror | 18 boundary cases; call == internal put on 864 inputs | exact |
| ClassifiedEntryIsBitwiseTheRuleEntryAndCarriesItsClass | 1,400 contracts x 2 presets x 2 rules, + geometry overload | bitwise |
| ThreeSitesAgreeOnTheClassificationBitForBit | 16,000 points (1,320 at B / +-1 ulp; all six classes) | bitwise |
| PriceIsMonotoneInSigmaAndFlatOnThePlateau | 60 contracts x 120 sigmas; 37 KernelFloor dips, 0 under Intrinsic | 1e-12 max(1, P) |
| ExercisePlateauIsExactIntrinsicWithZeroVegaAndUnitDelta | 16 points: price == intrinsic, vega == 0 exactly | exact / 1e-9 delta |
| PlateauEdgeIsContinuousInSpotAndSigmaWithinTheErrorClass | 48 edges, each reference-backed | 5.8e-7 x K max(1,T)^2 |
| PriceLimitsExpirySigmaInfinityAndDeepMoneyness | T -> 0, sigma -> 4, deep ITM / OTM | exact bounds |
| CallsWithYieldAboveRateAndPutsAcrossRateSigns | 11 carry rows, both sides | bitwise |
| DoubleContinuationRegimeServesNoSilentPriceOnAnyRoute | 480 contracts x 3 presets, every route | no finite price |

The plateau-edge class: the jump at the edge equals the accurate preset's own error at the
continuation-side point (the converged reference there is the intrinsic, smooth pasting),
measured 5.799e-7 x K max(1, T)^2 (call K 90, T 2, sigma edge) — above the AlMonotone
grid's 5.011e-7 because the edge is where the premium is smallest against its quadrature
error. Pinned at 5.8e-7.

## 5. Known gaps (not changed here)

- **Zero-vol closed form.** `andersen_lake`'s sigma <= 1e-8 value is max(hold to expiry,
  exercise now); the exact deterministic value is a supremum over exercise dates whose
  interior stationary point the closed form misses (put q > r > 0 with S > K r / q, the
  mirrored call, and the double-continuation regime). LEDGER 2026-08-23 records the same
  shape for the IV screen (fixed there). No fit iterates below kIvMin = 0.005; changing it
  would move a default, so it is documented in the header and left for the PM.
- **A fourth KernelFloor site.** `AloPricer::price` (american.cpp, inline euro +
  `al_put_premium` + clamps) is the scalar inversion's thread-local pricer
  (`ScopedAloPricer`, american_iv.cpp). It is not on the classifier yet (K4, below).

## 6. What K4 must wire (no K1/K2/K3 file was edited)

The rule and the class reach each route by switching its price call to the classified
form; branch on `cls` (`exercise_plateau` -> price_only / IV interval, vega 0;
`unsupported_regime` -> typed refusal; `continuation` -> identified root).

- Inversion (american_iv.cpp): `:957` / `:958` (andersen_lake at x / xb), `:1343` (seed-leg
  bump), `:1396` (confirm), `:1583` (cold seed) -> `andersen_lake_classified(..., rule)`;
  the ALO pricer: `AloPricer::price` (american.cpp `:3405`, premium at `:3498`) via
  `ScopedAloPricer` (american_iv.cpp `:269-280`) -> give it a rule and route its floor
  through `al_site_class` / `classify_exercise_region(..., al_boundary_at_expiry(s.bnd))`.
- de-Am cache (deam_iv_cache.cpp): `:645` (cold andersen_lake check of a family root).
- Chain batch: batch_chain_iv.cpp `:214` (PerContract andersen_lake), `:513`
  (LanedAvx2 -> `simd::american_put_boundary_batch`, whose driver
  american_boundary_avx2.cpp `:77` calls `price_put_pack_avx2(pack, spot)` -> the 6-argument
  `price_put_pack_at_avx2(pack, spot, pack.Kv, pack.XMAX, rule, cls)`, classes surfaced
  per lane through api/simd/american_boundary_batch.hpp); batch_chain.cpp `:200`.
- Families: american_sigma_family.cpp `:88` and american_sigma_q_family.cpp `:345` (the
  price seams `al_put_price_from_boundary(bnd, *ws, Sp, Kp, T, sigma, rp, qp)` -> add
  `rule, &cls`), american_sigma_q_family.cpp `:393` / `:395` (cold-vs-family self-check:
  both sides under the same rule).

Other KernelFloor sites on default paths (greeks, slices, records; no rule needed for K4):
american.cpp `:3825` (AlBoundaryHandle), `:3875` (pack_price), `:4257` (put slice,
cached), `:4800` / `:4896` (FD greeks, cached), `:5124-5127`, `:5297`, `:5549-5552`,
`:5626`; adjoint_greeks.cpp `:187`, `:321`, `:483-484`; boundary_interp.cpp `:113`, `:315`,
`:329`; scenario_grid.cpp `:411`; expiry_record.cpp `:167-169`; american_greeks_avx2.cpp
`:156-222`, `:375`.
