# L2b al-geometry sprint note (vol-sota W2, 2026-10-18 / resume 2026-10-19)

Lane `feat/vol-L2b-al-geometry-20261018`, based on the PM prebase `7cebbbd3`
(= the W0-integrated root source tree). Plan: `docs/plans/2026-10-17-atx-vol-sota-fit-dag-plan.md`
§4 W2 "L2b". Everything here is OPT-IN API; no existing caller changes path and no default changes.

## 1. What landed

| Piece | Where | Contract |
| --- | --- | --- |
| `AlExpiryGeometry` (+ `prepare_al_geometry`) | `api/pricing/american.hpp`, `src/pricing/american.cpp`, seam `src/pricing/american_boundary.hpp` | Per (T, r, q, opts) the node grid, table pointers and the sweep-invariant static bind, for the puts' orientation (r, q) and the calls' McDonald-Schroder internal put (q, r); immutable after `prepare()`, move-only PIMPL |
| `andersen_lake(geometry, S, K, sigma, side)` | same | bit-identical to `andersen_lake(S, K, T, sigma, r, q, side, opts)`, errors included |
| `american_greeks_al(geometry, ...)` | same | bit-identical; base and sigma +/- boundaries through the geometry, rate-bumped ones fresh |
| `AlBoundaryHandle` | same | one solve per (anchor, sigma); prices every contract with that internal strike (calls at one spot, puts at one strike) bit-identically, NaN otherwise |
| `al_price_pack` | same | prices and sigma + 1e-4 bumped prices bit-identical to fresh; `Status` for unprepared geometry / span lengths, nothing written on refusal |
| Model-priced certification `AmericanPriceSlice::create(nodes, model, bounds, kernel, screen, stats)` | `api/fitting/american_price_slice.hpp`, `src/fitting/american_price_slice.cpp` | defaults (Scalar, AccurateOnly) = `andersen_lake` per price then the price-input `create()`; opt-in Batched (pack on one geometry per equal-yield run, bit-identical); opt-in FastThenAccurate screen (below) |

Refactor inside `american.cpp` (code motion only, proven by the fingerprint): `al_solve_put_boundary`
split into its prefix and a shared `al_seed_and_sweep` tail; `andersen_lake_core` runs through a
shared `andersen_lake_dispatch` (validation, degenerate T / sigma, regime switch); `american_greeks_al`
body moved to `american_greeks_al_core(..., base_geo)`.

## 2. Bit identity: why it holds

A prepared solve loads the orientation's static state (bound once at K = 1, so the stored
`geo_alpha[j]` is exactly `exp(-(rate - yield) tau_j)`), writes `geo_alpha[j] = K * alpha_exp[j]`
(the same product the cold bind forms), recomputes `xmax` from the strike, binds sigma and runs the
SAME seed + sweep code. Every value the kernel reads equals the cold path's.

## 3. FastThenAccurate certification screen

Nodes are priced in strike order on `al_fast_profile(opts)` in chunks of 8; after each chunk the
priced prefix is checked with every inequality of `create()` widened by the Fast allowance
`1e-10 * max(|P|, 1)` per price read (100x the `AlFastProfile` tested bound 1e-12 * max(P, 1)):
node bounds by the node's allowances; vertical-spread / call-slope running extrema taken over the
allowance-shifted prices on the weakening side; convexity against the Fast prefix hull's chords
widened by the node's allowance plus the larger end allowance (a chord between any two nodes lies
above the whole slice's lower envelope, so a prefix proof is a whole-slice proof). A violation that
survives is certain for the accurate prices: refused with no accurate price ("certification screen"
in the message). Anything else (no proof, malformed bounds / strikes, a Fast price that errors or is
not finite) runs the full accurate pass and `create()`: the verdict and message are the accurate
kernel's. Accepted slices pay 2N Fast + 2N accurate prices; the lever is refusals.

## 4. Evidence (all in `C:/atx-cache/quote-benchmark/L2b-20261019-cert/`, manifest `sha256.txt`)

- AL fingerprint (vol-rel-avx2, `american/ladder_fingerprint`): `53c4df68...` MATCH at `cc97f387`
  and at `c5badc58` (before the edits: `L2b-20261018-cert`, same hash). The ten section fnv1a lines
  are identical to the base run.
- Default parity: dev200 default `c89d0739` MATCH (`parity/`, binary `be998ea5...` at `c5badc58`,
  `ATX_VOL_FIT_WORKERS=2`).
- Identity tests `tests/american_geometry_test.cpp` (pricing slice, 11 cases): 24,192 geometry
  prices (more than 5,000 of them through a prepared boundary solve, asserted), 960 degenerate /
  invalid inputs, 6,480 Greeks bundles (more than 1,000 American, asserted), handle 216 call + 224
  put prices, pack 512 prices (base and bumped), chord-vs-central bound, Status paths. Five
  revert checks fail as expected (alpha refresh dropped: 8 tests; handle anchor swapped: 3; pack key
  without sigma: 1; Greeks orientation guard dropped: 1; pack bumped-length check dropped: 1).
- Certification tests (`tests/american_price_slice_test.cpp`, suite `AmericanCertification`, 6
  cases): Review Focus #1 (a) a violation tol < v < tol + slack is confirmed by the accurate kernel
  (6 Fast + 6 accurate prices, the accurate message); (b) a large violation is refused on the first
  chunk with 16 Fast and 0 accurate prices (AccurateOnly: 80 accurate prices for the same verdict);
  Batched == Scalar bitwise across varying yields, schemes and screens. Three revert checks fail as
  expected (allowance 0; screen never refuses; geometry not re-prepared per yield run).
- Gates (`ATX_VOL_FIT_WORKERS=2`): `^(pricing|simd)/` failed/skipped name set at head == base
  `7cebbbd3` on this build (22 names each, sha256 `de46fa2c`; the 2 known failures; 3 more
  PricingExecutor skips than the integrator's cap-3 set because they need >= 2 pool workers);
  quote-slice 125/125; chain-batch 27/27; PCH-off on 24 includer TUs of the three touched headers,
  0 diagnostics. `atx-vol-umbrella` fails identically at base and head (Tier-B count 42 vs README 38,
  fixture-path literals in untouched files): pre-existing, not this lane.

## 5. Not done (for L2c / follow-ups)

- The `batch_chain.cpp` Scalar route through the geometry / pack (no `batch_chain*` file touched).
- No caller uses the certification overload yet: `quote_slice.cpp` (forbidden here) would call it
  behind a policy key; that wiring and the SPY 23 d timing belong to L2c / L2a.
- No timing number: the SPY 23 d certification inputs are not extractable cheaply. Expected from
  the counts: a slice refused at node 23 of 148 costs 24-32 Fast-node prices instead of 296 accurate
  prices; an accepted slice costs ~1.5x today.
- Bit-identical geometry saves only the static bind (estimated 5-10 % of an accurate price, not
  measured); certification nodes each carry their own sigma, so boundary sharing never fires there.
