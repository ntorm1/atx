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
| `Converged` | root found (Newton distance, or evaluated bracket <= 2 tol) |
| `LowVega` | cap / no progress with P' <= floor, not on the plateau: σ not identified to tol |
| `ExerciseFlat` | P' <= floor on the exercise plateau (K1's `exercised`) with the target at or below it |
| `SigmaBelowBox` / `SigmaAboveBox` | the evaluated box edge is on the wrong side of the target |
| `QOutsideBox` | q outside the family's q box (no evaluation) |
| `NonConverged` | cap with a usable slope; bracket retained |
| `InvalidInput` | non-finite / non-positive K, target, S; non-finite q; wrong side; inverted bracket; bad anchor |
| `UnsupportedIsa` | `InverseIsa::Avx2` required and not executable (host, or a family source) |
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
