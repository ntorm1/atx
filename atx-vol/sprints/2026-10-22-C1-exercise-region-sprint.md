# C1 — exercise-region and fallback semantics (r5, 2026-10-22)

Lane C1 of the vol-sota master plan (Sprint 2 package C1; deep review F04, second half).
Base: the r5 root `9f05a40e`. It completes the L3k checkpoint (`a01ee2e3`, `792ea5a1`,
cherry-picked with equal patch-ids) and adds the shared classification. K4 consumes it.
Lane report: `.superpowers/sdd/2026-10-18-vol-sota/lane-C1-exercise-region-r5-report.md`;
evidence: `C:\atx-cache\quote-benchmark\C1-20261022-region\` (`sha256.txt`).

Fix round 1 (review `lane-review-C1-exercise-region-r5-report.md`: M1, m2, m5) adds the
fourth site, the warm-started ALO pricer, as `AloRegionPricer` (§1); makes the laned site
return NaN off the American arm under KernelFloor with a class output (§1); and restates
the K4 wiring list at root line numbers (§6). Evidence:
`C:\atx-cache\quote-benchmark\C1-20261022-region-fix1\` and
`C:\atx-cache\quote-benchmark\C1-20261023-fix1-resume\` (`sha256.txt` in each).

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
exact-node guard returns `y[n-1]`; pinned per solved lane in the four-site test).

All four price sites call it:

- scalar uncached `amer::al_put_price_from_boundary(..., rule, AlExerciseClass* cls)`;
- scalar cached `al_put_price_from_boundary_cached(..., rule, cls)` (+ the test seam
  `al_put_price_from_boundary_cached_probe(..., bind_cache, cls)`);
- laned AVX2 `simd::detail::price_put_pack_at_avx2(pack, spot, strike, xmax, rule,
  AlExerciseClass cls[4])` — per lane, the SAME scalar `al_boundary_at_expiry` on the
  lane's `Y[nb-1]` / xmax and the SAME classifier;
- the warm-started ALO pricer (fix round 1, review M1) `AloRegionPricer::price(sigma,
  rule, AlExerciseClass* cls)` (`api/pricing/american_region.hpp`, Tier-B). Each branch
  is classified on the internal put: the branches taken before a boundary exists
  (expiry / zero-vol / unsupported / European) have none, and the American arm uses
  `al_boundary_at_expiry` of the boundary that call converged. `AloPricer` (Tier-A
  `american.hpp`, untouched) and `AloRegionPricer` share one state type
  (`detail::AloCore`) and one body (`alo_price`, american.cpp). `AloPricer::price(sigma)`
  is that body under KernelFloor with no class output and takes no classification branch,
  so its bits do not move (pinned over 8,400 calls). The class lives outside `AloPricer`
  so that the frozen Tier-A header is not edited: an edit there rebuilds all of its
  includers in two presets. The scalar inversion still holds an `AloPricer` in
  `ScopedAloPricer`; K4 switches that slot type (§6). `amer::al_alo_boundary(core)`
  (pricing/american_boundary.hpp) exposes the converged boundary to the tests.

WHY the laned site goes through scalar code: the L3k laned decision used its own
`xmax * exp_pd(-sqrt(Y[nb-1]))` and a vector compare. Mutation M2 restores exactly that
decision: 528 of the 1,320 points at B and +-1 ulp then disagree with the scalar sites
(the laned site calls B and B - 1 ulp "continuation"). Four scalar `exp` per pack call,
on the opt-in paths only.

Fix-round mutations: nudging the ALO site's boundary by one ulp (`nextafter`) gives 982
failed four-site checks; reverting the laned NaN contract below gives 7,200, one per
point off the American arm (1,920 european + 2,400 expiry + 1,920 zero-vol + 960
unsupported).

KernelFloor with no class output is the pre-L3k code at every site (9/9 default hashes
and the fingerprint `53c4df68` match). A classified call whose class is outside the
American arm (the dispatch handles those before any boundary exists) returns its class
and a quiet NaN, never a price. That holds at all four sites under either rule: under
KernelFloor with a class output the laned site used to return its floor there (review
m2), and now returns NaN as the scalar sites do. The default laned path, KernelFloor
with no class output, keeps its early return and is unchanged.

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
| FourSitesAgreeOnTheClassificationBitForBit (was ThreeSites...) | 16,000 points, all on the ALO site too (1,320 at B / +-1 ulp; all six classes); laned KernelFloor + class is NaN off the arm | bitwise |
| AloRegionPricerKernelFloorIsAloPricerBitForBit | 8,400 calls x 4 pricers (AloPricer; AloRegionPricer KernelFloor without / with class; Intrinsic), 2 presets, warm sweeps | bitwise |
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
- **The fourth site is classified but not yet wired.** `AloPricer::price` (inline euro +
  `al_put_premium` + clamps) is the scalar inversion's thread-local pricer
  (`ScopedAloPricer`, american_iv.cpp). Since fix round 1 its rule-and-class form is
  `AloRegionPricer` (§1), but the inversion still runs `AloPricer`, which is KernelFloor
  with no class. Until K4 switches the slot type (§6), K4 must not enable `Intrinsic` on
  any other inversion leg. Doing so would mix rules inside the region: a KernelFloor
  residual map with Intrinsic seed and confirm legs gives inconsistent brackets (review
  M1).
- **A fifth inline floor at the r6 root, outside C1's base.** K1's fused sigma/q price +
  vega pass (`american_sigma_q_family.cpp` at `668ad80e`; see §6) prices euro + premium
  with the `al_put_price_from_boundary` clamp order. Its `exercised` flag is its own
  test, not `classify_exercise_region`. At that root it has no production caller (bench
  and tests only).

## 6. What K4 must wire (no K1/K2/K3 file was edited)

The rule and the class reach each route when its price call switches to the classified
form. Then branch on `cls`: `exercise_plateau` -> price_only / IV interval, vega 0;
`unsupported_regime` -> typed refusal; `continuation` -> identified root. Quotes in the
edge gap, between the intrinsic and P(sigma_edge+), map to the edge interval and do not
count as "unbracketed".

**Line numbers.** Every number below is at the r6 integration root `668ad80e`, checked
with `git show 668ad80e:<file> | sed -n <n>p`. Only two cited files changed between
C1's base `9f05a40e` and that root (`git diff --stat 9f05a40e 668ad80e`):
`deam_iv_cache.cpp` and `american_sigma_q_family.cpp`. For those two the `9f05a40e`
number is given as `[base :n]`. Every other number is the same at both SHAs. Owners
follow the master plan's file lists (its section 5 work packages and section 6
ownership table).

- **Inversion, `american_iv.cpp`** (fitting owner, K4):
  - `:407` `alo->get().price(sigma)` is the ALO residual map and the main KernelFloor leg.
    Its rule-and-class form already exists as `AloRegionPricer` (§1), so K4 switches the
    `ScopedAloPricer` slot type `AloPricer` -> `AloRegionPricer`. That means
    `ThreadAloSlot::pricer` `:269`, `fallback_` `:312`, `pricer_` `:313` and `get()` `:308`,
    plus an include of `api/pricing/american_region.hpp`. The `reset` / `emplace` calls at
    `:285`, `:287` and `:293` compile unchanged because the constructor and `reset`
    signatures match. Then `:407` becomes `price(sigma, rule, &cls)`. Under KernelFloor
    with no class output the switch moves no bits
    (`AloRegionPricerKernelFloorIsAloPricerBitForBit`). The NaN-means-collapse check at
    `:408-411` stays valid: the price is NaN exactly where `AloPricer`'s is.
    K4 edits nothing in `american.cpp` / `american.hpp`.
  - `:414` (cold residual without an ALO pricer) and `:740` (cold polish, missing from
    round 0's list; review m5) both call `american_price(..., method, opts)`. That
    reaches `andersen_lake` under KernelFloor through `american_price`'s Andersen-Lake
    branch, `american.cpp:4148`. `american_price` is Tier-A (`american.hpp`) and has no
    rule parameter. Two options: switch both call sites to
    `andersen_lake_classified(..., rule)` when `method == AndersenLake`; or, with a Tier-A
    exception from the PM, add the rule at `:4148` so both sites inherit it (the review's
    preference).
  - `:957` / `:958` (andersen_lake at x / xb), `:1343` (seed-leg bump), `:1396` (confirm)
    and `:1583` (cold seed) become `andersen_lake_classified(..., rule)`.
  - Not boundary sites: `:229` / `:234` (`american_price_cached`) and `:240` / `:246`
    (`cached_price_and_vega`, used at `:459`) price through the correction interpolant,
    which solves no Andersen-Lake boundary. They have no class to read, so K4 decides how
    a cached-map root inside the plateau is routed.
- **de-Am cache, `deam_iv_cache.cpp`** (K4): `:692` `[base :645]`, the cold
  andersen_lake leg of `deam_family_probe_worst` (`:657` `[base :610]`). It is compared
  with `family.price` at `:691`, so both sides take the same rule.
- **Chain batch** (not in any lane's file list; PM assigns):
  - `batch_chain_iv.cpp:214`, the PerContract andersen_lake.
  - `batch_chain_iv.cpp:513`, the LanedAvx2 route `simd::american_put_boundary_batch`. Its
    driver `american_boundary_avx2.cpp:77` calls `price_put_pack_avx2(pack, spot)`, which
    becomes the 6-argument `price_put_pack_at_avx2(pack, spot, pack.Kv, pack.XMAX, rule,
    cls)`, with classes surfaced per lane through `api/simd/american_boundary_batch.hpp`.
    The driver's scalar patch at `:82-87` (`!eligible[l] || !std::isfinite(pr[l])` ->
    `scalar_put` -> andersen_lake) must carry the same rule and return the class. Its
    `!isfinite` guard already catches the NaN lanes from the m2 contract. Patched lanes
    are classified on a scalar-solved boundary.
  - `batch_chain.cpp:200`.
  - The SIMD driver is kernel-owner territory (K1/K2/A0).
- **Families** (kernel owner, K1/A0; the PM coordinates):
  - `american_sigma_family.cpp:88` and `american_sigma_q_family.cpp:349` `[base :345]` are
    the price seams `al_put_price_from_boundary(bnd, *ws, Sp, Kp, T, sigma, rp, qp)`; add
    `rule, &cls`.
  - `american_sigma_q_family.cpp:397` / `:399` `[base :393 / :395]` are the
    cold-vs-family self-check; both sides take the same rule.
  - New at `668ad80e`, absent at `9f05a40e`: K1's fused price + vega pass `premium_pass_t`
    at `:848`. It clamps at `:883-897` in the `al_put_price_from_boundary` order. Its
    `exercised` flag (`:902-903`) is
    `clamped || price <= intr + 1e-9 Kp || Sp <= xmax * nb.now_factor`, its own exercise
    test rather than `classify_exercise_region`, and it sets vega 0 on a clamp
    (`:905-906`). Entry points: `price_and_vega` `:1128`, `price_vega_strikes` `:1150`,
    `price_vega_on` `:1353`. There is no production caller at the root (bench and tests
    only). If K4 serves through it, its plateau test must become the classifier on that
    family's B(T).
- **The ALO body itself** (reference, nothing to wire): `AloPricer::price` is root
  `american.cpp:3274`, its premium at `:3367`. At C1's tip the body is `alo_price` `:3429`
  (premium `:3532`), `AloPricer::price` `:3566` forwards to it under KernelFloor, and
  `AloRegionPricer::price` is at `:3596`.

Other KernelFloor sites on default paths (greeks, slices, records), root numbers; none is
an inversion leg:
- american.cpp: `:2899` (`al_solve_put`) and `:3032` (`al_solve_put_prepared`), the
  andersen_lake routes, which C1 put on the classifier through the rule overloads;
  `:3571` (`AlBoundaryHandle::price_from_boundary`), `:3621` (`pack_price`), `:4003`
  (`put_slice_impl`, cached), `:4546` / `:4642` (`american_greeks_fd`, cached),
  `:4870-4873` (`american_vega`), `:5043` (`american_carry_greeks_al`), `:5295-5298`
  (`american_vega_al`), `:5372` (`american_delta`). The round-0 note gave the
  `:3571`-`:5372` sites as tip numbers, each 254 higher.
- adjoint_greeks.cpp `:187`, `:321`, `:483-484`.
- boundary_interp.cpp `:113`, `:315`, `:329`.
- scenario_grid.cpp `:411`.
- expiry_record.cpp `:167-169`.
- american_greeks_avx2.cpp `:156-222`, `:375`.

The Greeks routes stay on KernelFloor. On plateau contracts they report a numerical,
nonzero vega (review m7), so published plateau Greeks must come from the class, not from
the engine.
