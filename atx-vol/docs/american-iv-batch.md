# Batch American IV inversion on the (σ, q) family (lane K2, r5, 2026-10-22)

Code: `src/pricing/american_iv_batch.{hpp,cpp}` (seam), `src/pricing/american_iv_batch_status.hpp`
(typed outcome), `src/simd/american_iv_batch_avx2.{hpp,cpp}` (4-lane kernel),
`src/pricing/american_iv_stream.{hpp,cpp}` (recorded request stream + replay), the native
branch of `src/pricing/american_iv.cpp`. Tests: `tests/american_iv_batch_test.cpp` (suite
`IvBatch`), `tests/american_iv_batch_alloc_test.cpp` (binary `atx-vol-iv-batch-alloc-tests`),
`AmericanIvPremium.FamilyNativeOutcomesAreTypedAndTheCarryKeepsTheBracket`.

## 1. What it is

`iv_batch::invert_many(in, out, scratch, work)` inverts N prices of ONE (expiry, side)
family. Every lane has its own strike, target, contract yield q and σ iterate; the map is
K1's fused family price + analytic vega (`sigma_q_family_detail`). Inputs and outputs are
spans of equal length (structure of arrays): `K, target, q, is_call` (required), `active,
seed_sigma, bracket_lo/hi, price_lo/hi, anchor` (optional); out `sigma, bracket_lo/hi,
price_lo/hi, residual, vega, last_sigma, evaluations, status, flags, acceptance`.

Source: a `PremiumTable` (K1's precontracted snapshot; AVX2-capable, per-lane q tile) or the
`AlSigmaQFamily` itself (scalar, `price_and_vega` per evaluation — the integration path,
which has no table).

## 2. The root

Per lane, a bounded bracketed Newton on P(σ) at the lane's (K, q):

1. Bracket: the family σ box, narrowed by the caller's retained bracket (an evaluated end
   carries its family price; an unevaluated one is an assumed bound) and by an anchor lane's
   evaluated points (section 4).
2. Seed: inside the bracket as given; past an end it is moved to that end, and an
   unevaluated end is priced there (a Black-76 seed above the box is priced AT the box top —
   review mn5's box-top seed could never form a chord); none / not finite → the bracket
   midpoint (geometric on a wide bracket). Recorded as `kSeedClamped` / `kSeedFailed`; never
   a terminal outcome.
3. Step: Newton accepted only strictly inside the bracket and shrinking (Numerical Recipes'
   rtsafe test: |step| <= half the step before last); a step past an UNevaluated end probes
   that end (a box miss is settled there; a wrong assumed bound widens back to the box);
   otherwise bisection.
4. Exit: converged when the Newton distance |P(x) − target| / P'(x) <= tol at an evaluated x
   with P'(x) > 1e-8 K (root = x minus the step), or when an evaluated bracket is <= 2 tol
   (root = midpoint). Cap: 16 evaluations (`kInverseMaxEvaluations`).

Every exit writes the evaluated bracket with its end prices, the last residual, vega and
point, and the best iterate: the next route starts from what this one learned.

## 3. Typed outcomes (`InverseStatus`)

| Status | Meaning |
|---|---|
| `Converged` | root found (Newton distance, or evaluated bracket <= 2 tol) with a usable fused vega (> 1e-8 K) at the last point |
| `ConvergedNoSlope` | an evaluated bracket <= 2 tol encloses the root (σ = its midpoint, acceptance `FamilyBracket`) but the last point's vega is NaN (none evaluated: a retained / anchored bracket settled the lane) or <= the floor — a root without the confirm's local slope (fix round 1, review m1) |
| `LowVega` | cap / no progress with P' <= floor, not on the plateau: σ not identified to tol |
| `ExerciseFlat` | P' <= floor on the exercise plateau (K1's `exercised`) with the target at or below it |
| `SigmaBelowBox` / `SigmaAboveBox` | the evaluated box edge is on the wrong side of the target |
| `QOutsideBox` | q outside the family's q box (no evaluation; also a q tile `contract_q` refuses as OutsideBox) |
| `NonConverged` | cap with a usable slope; bracket retained |
| `InvalidInput` | non-finite / non-positive K, target, S; non-finite q; wrong side; inverted bracket; bad anchor; a q tile `contract_q` refuses as Unsupported (both paths refuse the lane before any evaluation; review m4) |
| `UnsupportedIsa` | `InverseIsa::Avx2` required and not executable (host, a family source, or a `PremiumTable::fast` table) |
| `Inactive` | masked off |

A failed seed is a flag (`lane_flag::kSeedFailed`, counted in `InverseBatchWork`), not a
status: with a bracket in hand and a fused vega it never stops a lane.

`LaneAcceptance` is the slot for K3's `RootAcceptance` (lane K3, not on this branch at the
final SHA): today it carries the family-map facts K3's rule consumes (how the root was
accepted, its Newton distance in vol, the evaluated bracket). It is not a bound against the
accurate map and skips nothing.

## 4. Bid → mid → ask

`anchor[i] = j`: lane i (a bid or ask) of the same contract (K, q bit-equal) starts from lane
j's outcome — every evaluated point of j brackets i on the side its price is on, and j's last
point and vega give i a Newton seed. Anchors must be anchor-free; a dependent of a masked or
refused anchor runs from scratch. In a slot, a mid's dependents run right after it on the
same q tile. Monotonicity settles some lanes with no evaluation: an ask behind a mid whose
evaluated box top lies below the mid is `SigmaAboveBox` at 0 evaluations.

In the fit (one lane per request), the same information travels in
`IvPremiumMemory::carry` (section 7).

## 5. ISA and the rounding class

The scalar path IS K1's map bit for bit (`price_vega_on` / `price_and_vega`; pinned by
`IvBatch.ScalarPathIsTheFusedFamilyMapBitForBit`). The AVX2 kernel evaluates four slots — each
its own strike, q tile, carry and σ — with the vector Cody Φ, one exp shared by Φ(x) and φ(x)
(both read e^{−x²/2}), and multiplications by 1/v, 1/σ where the scalar divides. Its tiles are
the ones K1's `contract_q` cuts (bit-identical inputs); per-lane z / dz come from the family's
own `sigma_z`. Measured class (`IvBatch.Avx2KernelPriceAndVegaMatchTheScalarMap`, 80 lanes at
fixed σ): max |ΔP| / max(P, 1) 3.4e-14, max |Δvega| / max(vega, 1) 3.6e-15; roots agree to
5.6e-15 in σ with equal statuses (`Avx2MatchesScalarToTheRoundingClass`). Each lane's result
is a pure function of its own slot, so it does not depend on batch order or its neighbours
(`ResultsAreIndependentOfBatchOrder`). `InverseIsa::Scalar` is the ForceScalar seam; `Auto`
follows `simd::use_avx2()` (so `ATX_SIMD_ISA=ForceScalar` holds).

Fix round 1 (review m3, m4, m6):

- **Fast tables.** The kernel implements the full-precision map only. A table of an
  `AlProfile::Fast` family (`PremiumTable::fast`, `price_vega_on`'s fast arithmetic) runs the
  scalar map under `Auto` (bit for bit), and `InverseIsa::Avx2` on it is `UnsupportedIsa`
  (`FastProfileTablesEvaluateOnTheScalarMap`; the full-precision kernel on a fast table would
  differ from the fast scalar map by 3.5e-14 scaled price, recorded there).
- **Contraction status.** A lane whose q tile cannot be cut is refused with the contraction's
  typed status before any evaluation, on both paths (`AQTileThatCannotBeCutRefusesItsLanesOnBothPaths`).
  Each AVX2 slot keeps its own tile. A slot whose result is not read may hold any data (it is
  evaluated with defined IEEE arithmetic and discarded).
- **The exercise boundary.** Near a flip of the exercise rule (spot inside the current edge, or
  the price clamped to / within 1e-9 K of intrinsic) the rule's inputs sit within rounding of
  their threshold. The kernel's own classification there is rounding noise: at ±8 ulps of 18
  flips (σ at fixed K and K at fixed σ, three q tiles; 342 points) it differs from the scalar
  map's on 118 points. The kernel therefore flags every lane whose inputs lie within
  `kBoundaryRecheckRel = 1e-11` (relative; about 300x the price class) of a threshold, and the
  AVX2 path re-evaluates those lanes on the scalar map (`InverseBatchWork::boundary_rechecks`).
  Measured by `Avx2AndScalarAgreeAcrossTheExerciseBoundary`:
  - every classification difference is flagged (unflagged 0; 336 of 342 points flagged);
  - flagged lanes are the scalar map bit for bit;
  - price class at the boundary 1.75e-15 scaled, vega 6.3e-12 scaled;
  - statuses equal 342/342 (one evaluation at each point, and full inversions), roots to
    8.2e-13 in σ.

## 6. Allocation

`InverseScratch` (PIMPL: lane roles / chain links sized to the batch, one q tile, the AVX2
slots) grows on a larger batch — the only allocation; once it has seen n lanes, `invert_many`
on n lanes allocates nothing on any path (`atx-vol-iv-batch-alloc`, three paths × two sides,
anchors / refusals / retained brackets included). The fit's one-lane call uses a
`thread_local` scratch.

## 7. Integration (`graded.family_native`, opt-in, no new default)

`premium_root`'s family-native branch is one lane of `invert_many` on the family source,
replacing the 12-price secant / chord loop:

- `IvPremiumStats::native_status / native_evaluations / native_flags / native_carried`
  carry the typed outcome (review-R1 MJ2); a non-converged outcome is WHY the price went to
  the seed-leg path.
- `IvPremiumMemory::carry` keeps every native attempt's bracket, last point and fused vega
  (accepted or not; never written by the seed-leg path), so the quote's bid / ask start from
  the mid's bracket and a Newton seed off its vega. A native refusal no longer clears the
  contract's leg memory (the leg path skips native points anyway).
- An accepted root hands the confirm the fused vega at its last point as the slope — local by
  construction (review-R1 MJ1's required slope). The confirm itself is unchanged (K3 owns it).
  Only `Converged` is accepted; `ConvergedNoSlope` (no usable slope) goes to the seed-leg path
  with its own `native_status` (fix round 1, review m1; the acceptance set is unchanged — it was
  already refused for its vega).

Served-set identity with the key ON vs OFF (same keys minus `family_native`): lost 0 /
gained 0 on SPY, cohort5, dev200, dev200b (lane report).

## 8. The recorded stream and the stop/go row

`atx-vol-quote-bench --inverse-replay auto|scalar|avx2` (quote-slice, one rep) arms
`iv_batch::InverseStream` for the run: every Premium request `premium_root` receives,
refusals included, with the native family when native-eligible (snapshotted once as a
`PremiumTable`, held by `unique_ptr`, never moved), the request whose carry it starts from,
and the fit's own native outcome. After the run the stream is replayed five times per
family through `invert_many` (the bid / ask anchored to their mid; the unanchored lanes get
the fit's Black-76 seed) and reported as `bench.inverse_replay`. Timed: seeds + batch calls.
Untimed: grouping, arrays, scratch. Disarmed, recording costs one atomic load per request.

## 9. What it does not do

- The fit still inverts one request at a time (one lane per call); batching a work group's
  requests through one call is K4's wiring (master plan K4 step 2). The replay measures what
  that wiring can reach on this stream.
- No confirm skipping and no acceptance contract (K3); the family's own error is not a bound.
- Per-status counters in `FitTimingCounters` / the bench's fit-route JSON are not added
  (that needs `deam_iv_cache.cpp` and `quote_bench_main.cpp` fit-route hunks); the counts
  are in `IvPremiumStats` and in `bench.inverse_replay.fit_native.by_status`.
  (K4 added them on the native route: `fit_work.native_route.native_lane_status`.)

## 10. The batched native route (lane K4b, `graded.native_batch`, opt-in)

The fit's work group is a chain cache's side (its (sigma, q) family). On the first mid
request, `DeAmIvView::prepare_native_batch` decides every band's edges, then per side runs the
mids through ONE `invert_many` call, the bids / asks through a second one that starts from the
mid lane's bracket (`iv_native_lane_carry`), and the K3 acceptance of every lane the route
accepts (`iv_native_prepare_acceptance`: the enclosure; without a covering envelope the lane's
fused vega places the confirm - the lane-vega hook). The source is the side's `PremiumTable` on
the AVX2 kernel when `simd::use_avx2()`, else the family's scalar map (section 5: the table's
scalar map is not bit-equal to the family's). The requests then run as before and consume a
prepared value only when their inputs are bit-identical to the ones it was computed from
(`IvPremiumGate::prepared`; a mismatch runs its own one-lane call and counts a miss), so:

- scalar ISA: the batched route serves exactly the unbatched route, bit for bit
  (`FitNativeRoute.BatchedRouteOnTheScalarMapIsTheUnbatchedRouteBitForBit`);
- AVX2: section 5's rounding class with identical statuses, acceptance kinds and route
  outcomes (`FitNativeRoute.BatchedRouteOnAvx2KeepsEveryOutcomeToTheKernelsRoundingClass`).

Confirms. The AVX2 boundary batch (`simd::american_put_boundary_batch`, a call through the
McDonald-Schroder put map) prices a lane in about the scalar FAST solve's time (72-84 us vs
61-79 us per lane on the SPY shape); it is ~2x faster only than the scalar ACCURATE scheme
(77-84 vs 151-178 us). The first confirm prices are therefore batched on the accurate confirm
kernel only, and consumed only where the side decision and the far-root placement test clear
`kIvBatchConfirmMarginRel = 1e-11 max(P, 1)` (the batch's tested class of the scalar kernel,
max 1.1e-13 measured), so every certified side is the scalar kernel's. The speed arm's Fast
confirms stay scalar; for the same reason the final grid's Fast nodes are not batched.
