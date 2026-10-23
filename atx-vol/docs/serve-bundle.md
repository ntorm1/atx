# Immutable fitted serving record (H0-A, r6 phase A)

Status: new public header `include/atx/vol/api/fitting/slice_serve_record.hpp`,
implementation `src/fitting/slice_serve_record.cpp`. Phase A builds a record from a
**completed** `QuoteSliceFit` through a free function. There is no fitter hook, no session or
tick wiring, and no default path constructs a record. Those are phase B / H1. Nothing a
caller already uses changes a number.

## What a record is

`build_serve_record(const QuoteSliceFit&, const ServeRecordPolicy& = {})` returns a
`std::shared_ptr<const SliceServeRecord>`: one immutable record per expiry. It holds:

| Part | Content | Ownership |
|---|---|---|
| `nodes()` | every node of the fit's `AmericanPriceSlice`: strike, the call and put price copied **byte for byte**, C0's typed answer (`query_quote_slice`: status, price-only reason, IV interval, convention), the served σ, the contract's effective yield q (the fitter's per-strike funding-PV carry), the acceptance (K3 vocabulary), the family's price delta, the ladder certificate, the converged exercise edge | copied |
| families | the expiry's (σ, q) `AlSigmaQFamily` per side, rebuilt with the fitter's own recipe (`build_sigma_q`: box, n_q, 16 log-σ nodes, probe bound) | `shared_ptr<const DeAmSigmaFamilies>`; the holder declares its geometries before its families, so they are destroyed after them |
| ladders | per served node side, a Chebyshev plane in (ln S, σ) sampled from the family at the node's own strike and yield (tier 0) | owned |
| curve | a clone of the fitted `IVolCurve` | `shared_ptr<const IVolCurve>` |
| slice | a copy of the `AmericanPriceSlice` (for between-node slice prices) | owned |
| `support()` | tiers, per-side family / ladder counts and boxes, cash dividends, double continuation, τ range, the Greeks each tier serves | owned |
| `version()` | SHA-256 of the served state, of the policy (including the A0/K3 seam switches) and of the fit's inputs, plus build clocks (steady and wall) and `as_of_ns` | owned |
| `bytes()`, `clocks()` | the measured footprint and the construction clocks | owned |

### Lifetime and sharing

- Nothing in a record points into the fit. Destroying the fit changes no answer
  (`SliceServeRecord.TheRecordOutlivesTheFitItWasBuiltFrom`: it builds, destroys the fit, serves).
- Readers share the record through `shared_ptr<const>`. Reclamation is deferred to the last
  reader that drops its copy, and a family can never outlive the geometry it observes
  because one holder owns both.
- Phase B's hook is `detail::build_serve_record_with(fit, policy, families)` in
  `src/fitting/slice_serve_record_detail.hpp`. It takes the cache's already-built holder,
  so tier 1 costs no kernel work. Without it the families are built on demand and counted
  in `clocks().lazy_family_builds`.
- A record is immutable after build. Any number of threads may call `serve()` or
  `implied_vol()` (`ConcurrentServesAreBitIdenticalToTheSerialLoop`). Ladder and Family
  reads allocate nothing (`SliceServeRecordAlloc.LadderAndFamilyReadsAllocateNothing`).

## Tiers and routing

A request names the cheapest tier to try (`tier`) and the dearest it accepts (`tier_max`).
The record walks up from `tier` and answers from the first tier that can. Otherwise it
returns a typed refusal with a route. No tier ≤ 2 ever runs a cold solve.

| Tier | Serves | Price | Greeks |
|---|---|---|---|
| Ladder (0) | served node sides whose ladder was certified at build, spot inside the ladder's ln-S window, continuation side | the ladder plane at the node's σ | Δ, Γ, vega, θ (continuation identity) from the same map; Γ and θ withheld in the near-edge band |
| Family (1) | any strike in the certified domain, any spot, σ and q inside the side family's box | K1's fused price | vega (K1). With A0's bundle (seam on): A0's six Greeks, mask, reasons, `edge_sd` and `near_edge` verbatim |
| Certified (2) | at the fit's spot: the certified slice price (node bytes, or piecewise linear between nodes) with the family's Greeks; at any other spot: the family answer with its price replaced by **one** `AlProfile::Fast` solve | slice price, or Fast | as Family |
| Cold (3) | never run inside `serve()` | — | — |

**Ruling b (F12).** At the fit's own spot (S bit-equal), every tier serves the certified
slice price: the node's bytes at a node, the piecewise-linear slice between nodes. The
Greeks then come from another map. `price_source` says which map, and `price_map_delta`
carries |that map's price − served price|. At any other spot the price and the Greeks come
from one map (`price_map_delta = 0`). The exception is Certified, whose price is the Fast
confirm and whose `price_map_delta` is |family − Fast|.

**Sticky strike.** Every tier prices at the served volatility: the node's served σ at a node,
or the fitted curve's σ(ln K/F_fit) between nodes. A spot move does not move the smile.
`ServeGreekConvention::sticky_strike = true`.

### Refusals (status → route)

| Condition | `status` | `route` |
|---|---|---|
| bad request (strike / spot not positive and finite, side, tier order) | `InvalidArgument` | `None` |
| older than `policy.max_age_ns` | `Stale` | `Rebuild` |
| dated-cash fit | `TierUnavailable` | `ColdPath` |
| strike outside [first node, last node] | `OutOfDomain` | `Refit` |
| price-only node (C0) | `PriceOnly` (the certified price is served at the fit spot) | `None` at the fit spot, else `Refit` |
| σ or q outside every allowed family box | `SigmaOutOfBox` | `ColdPath` |
| no allowed tier has a map | `TierUnavailable` | `ColdPath` |
| (IV) target in the L1c non-monotone band | `NonMonotoneBand` | `ColdPath` |
| (IV) the root lies outside the ladder's or the family's σ box | `SigmaOutOfBox` | `ColdPath` |
| (IV) a family root no envelope bounds, `tier_max < Certified` | `Unconfirmed` (σ set) | `ColdPath` |
| (IV) no accepted root | `NoConvergence` | `ColdPath` |

### certificate_usd

Each tier puts its own error bar on `price`:

- a node price at the fit spot: 0;
- a ladder: its measured distance to the family off its sample nodes, plus the larger of the
  family's probed bound × max(P, 1) and the family's measured error at the node, plus an
  8-ulp floor. A ladder serves only when this is ≤ max(5e-4 USD, 5e-7 · F);
- the family: its probed bound × max(P, 1), and at a node at least its measured error
  there. Between nodes outside the probed band |ln K/S| ≤ 0.25 it claims nothing (NaN).
  This was measured: on the SPY surface the family is 1.6e-4 USD off at K = 230 call,
  ln K/S = −1.21;
- a Fast confirm: `al_profile_price_bound(Fast)`.

## The near-exercise-edge band (A0 fix round, review-A0 MJ1)

`edge_sd` is the contract's distance to its early-exercise edge in units of σ√T:
ln(S/S*)/(σ√T) for a put and ln(S*/S)/(σ√T) for a call. It is > 0 on the continuation side
and +∞ when no finite edge exists (the European regime). A continuation answer with
`edge_sd < kServeEdgeBand` (0.25, A0's `kAlFamilyGreekEdgeBand`) is **in the band**. There,
on every tier, θ and Γ are NaN with `NearExerciseEdge`, and price, Δ and vega are served.
The reason: in the band the identity θ = rP − (r−q)SΔ − ½σ²S²Γ is a cancellation of O(rK)
terms that amplifies any map's Γ error. A0 measured θ up to 138 % off, with the wrong sign,
0.007 σ√T from the edge.

- **Ladder (tier 0).** θ is the continuation identity on the ladder's own Δ and Γ. The edge
  is the converged `exercise_boundary(K, T, σ_node, r, q_node, side, fit.al_opts)`, located
  once per ladder candidate at build (`ServedNodeSide::edge_spot`; 0 / +∞ in the European
  regime). A refused solve makes the side ladder-ineligible.
  - **Which rows are in the band.** Under the default policy a ladder must keep every
    (ln S, σ) sample, σ ∈ σ_node·[0.75, 1.25], on the continuation side, so its low-σ
    samples cross the edge before any served row gets near it. On the fixture (strikes every
    0.5 over 90–120, T = 0.25) every tier-0 row stays ≥ 0.57 σ√T from the edge and no row is
    in the band. Narrower σ windows let ladders reach it: 20 band rows at half-width 0.05,
    34 at 0.02. The test runs at 0.05. Measured on SPY: see the measurement section
    (`near_edge_rows`, `min_edge_sd` on `tier0_price4`).
- **Family / Certified (tiers 1–2).** With A0's bundle, `edge_sd`, `near_edge` and the
  reason are A0's: its own discretised edge B'_now, `AlFamilyGreekReason::NearExerciseEdge`
  mapped value for value. Without it the family serves vega only, `edge_sd` is NaN, and no
  θ or Γ is served.
- The older L3g σ-family Greek bundle still serves a wrong in-band θ with status Ok (A0
  report, open). The record never reads it. θ and Γ come only from the ladder's masked path
  or from A0's (σ, q) bundle.

## Implied volatility at tick rate

`implied_vol(ServeIvRequest)` follows the same tier walk:

1. **L1c gate.** If the target minus the zero-vol American value is at or below
   `al_non_monotone_gate(K, T, al_opts)`, the result is `NonMonotoneBand` → `ColdPath`
   (accurate inversion).
2. **Ladder root.** Lockstep contraction on the ladder's own (ln S, σ) plane at the
   request's spot, accepted on the ladder certificate (`LadderCertificate`).
3. **Family root.** Bracketed Newton on K1's fused price and vega inside the σ box,
   24 evaluations, tolerance 1e-13·max(1, target).
4. **K3 enclosure (seam).** If the side's validated envelope brackets the target over a
   σ interval of width ≤ min(policy tolerance, 1e-7), the root is accepted with
   `Skipped / FamilyEnclosure / RefinementValidated` and no confirm solve.
5. Otherwise `Unconfirmed` when `tier_max < Certified`. Else one `AlProfile::Fast` confirm,
   accepted on its Newton distance ≤ `iv_tolerance` (`Confirmed / NewtonContract`), and
   served Newton-corrected.

## Seams (A0 / K3 not in the base)

`ATX_VOL_SERVE_RECORD_A0` and `ATX_VOL_SERVE_RECORD_K3` (default 0, in
`slice_serve_record_detail.hpp`) compile the A0 bundle and the K3 envelope paths. Both values
are folded into `policy_sha256`. With either seam off, the record's output is everything
documented above minus that lane's fields. Both seams were verified by overlaying the lanes'
tips on this branch (K3 `91d1f669`; A0 fix round `ed2d2e05`). Results are in the lane report.

Integration W2c-i (both lanes in the root) sets both seams to 1. The default policy's
`policy_sha256` moves from `87318ae7…cefe` (both 0) to `57902383…f3fc` (both 1); the
serve-record suite is 28/28 with both on (the 26 plus the A0 and K3 seam tests).

## Measurement (lane H0-A, 2026-10-23, `vol-rel-avx2`, one run, unqualified)

Bench `atx-vol-serve-record-bench` (sha256 `0e1b9c45…0975`, built at `3db98e3b`). The main
thread is pinned to a P-core. Timing is TSC per call: 0.40064 ns/tick, timer overhead
18.0 ns (not subtracted). Each row is the median of 5 repeats.

Datasets:

- **SPY surface.** The served expiries of an ATXVFA archive of the root binary's graded-v1
  fit of `quotes-SPY.csv`: 4 expiries, 705 nodes, 1177 ladders (233 node sides
  ladder-ineligible), 8 families.
- **SPY 23 d.** `spy23-quotes.csv`, fitted once at the first loop profile that serves
  ("ordinary", grade A): 167 nodes, 266 ladders, 68 ineligible.

Spots are moved by `kSpotMove` off the fit spot, so the ladder and family maps serve rather
than the slice.

**Load line.** Total CPU was 24.3 / 17.5 % before and 13.6 / 22.9 % after. During the run,
another lane's `atx-vol-quote-bench-c2` and a stray `find` were busy (about 2.4 logical
cores of 16). The run is unqualified.

### Latency vs targets

| Row (per contract) | SPY surface p50 / p99 / p99.9 | SPY 23 d p50 / p99 / p99.9 | Target |
|---|---|---|---|
| tier 0 Ladder, price + Δ Γ ν θ | **140.6 / 187.9 / 268.4 ns** (1177) | 120.6 / 142.2 / 165.5 ns (266) | ≤ 150 ns p50, ≤ 1 µs p99: met |
| tier 1 Family, price only | 5.62 / 7.92 / 10.9 µs (1282) | 5.44 / 7.48 / 10.1 µs | "≈ µs" |
| tier 1 Family, price + 4 | 5.33 / 7.12 / 10.5 µs | 5.45 / 7.59 / 10.0 µs | |
| tier 1 Family, price + all 6 | 5.01 / 6.48 / 9.60 µs | 5.10 / 6.91 / 9.23 µs | |
| tier 2 Certified (moved spot: family + one Fast solve) | 61.9 / 90.0 / 99.4 µs | 62.2 / 88.0 / 94.0 µs | — |
| record IV (Ladder) | **264 / 405 / 513 ns** | 230 / 344 / 390 ns | ≤ 500 ns p50: met |
| family IV (Family) | 16.8 / 42.3 / 53.2 µs | 15.3 / 38.7 / 43.2 µs | — |

| Chain (one pass, single thread) | SPY surface p50 / p99 | SPY 23 d p50 / p99 | Target |
|---|---|---|---|
| tier 0 | **17.8 / 22.2 µs** (245 of the 300 node sides nearest the money in the largest expiry; the rest have no ladder) | 16.8 / 20.1 µs (232) | ≤ 100 µs: met |
| tier 1 | 1507 / 1675 µs (284) | 1400 / 1610 µs (265) | ≤ 100 µs: **missed ×15** |
| tier 2 | 18.9 ms (284) | 18.2 ms (265) | — |

- **Tier 1 chain miss.** The cost is one full family evaluation per contract, about 5 µs:
  the (σ, q) tensor interpolation of the boundary at every τ node, then the premium
  quadrature. K1's `price_vega_strikes` amortises the first part across strikes that share
  (σ, q). Every served contract has its own σ (the smile) and its own q (per-strike carry),
  so nothing is shared. A per-node cache of the interpolated boundary would cut the cost
  (about 1.4 KB per node side, the same order as a ladder); that is H1's choice. Tier 0 is
  the chain path.
- **Tier 2.** Dominated by the `AlProfile::Fast` solve, about 56 µs per contract.
- **IV statuses.** One untimed pass over the same targets, from a diagnostic rerun of the
  IV rows only (`measure/diag-iv-status.txt`):
  - record IV: SPY surface 1151 Ok, 26 routed by the L1c band gate; SPY 23 d 255 Ok, 11 band.
  - family IV: SPY surface 1255 `Unconfirmed`, 27 band; SPY 23 d 288 `Unconfirmed`, 11 band.
    This is by contract: without K3's envelope a family root carries no validated bound,
    and `tier_max = Family` excludes the Fast confirm.

### Bytes

Layout:

- `sizeof(ServedNode)` = 232 B (both sides and the strike).
- One ladder = 1360 B: a fixed-capacity 16 × 8 plane (12 × 6 used under the default
  policy), the 16-term price and vega rows, the window, the certificate and the edge. Each node also carries two `int32` ladder slots.
- Per family: 6.1–6.5 KB of family state plus 93.2 KB of prepared geometry
  (`sizeof(AlExpiryGeometry::Impl)`).
- Fixed state: 920 B per record.

| | nodes | ladders | families | geometries | fixed | total |
|---|---|---|---|---|---|---|
| SPY surface (4 expiries) | 163,560 | 1,606,360 | 48,704 | 745,792 | 3,680 | **2,568,096 B per symbol** |
| per expiry (SPY surface) | | | | | | mean 642,024 B, max 851,768 B |
| SPY 23 d (1 expiry) | 38,744 | 363,096 | 12,944 | 186,448 | 920 | 602,152 B |

Ladders dominate (63 %), then the two prepared geometries (29 %). There is no per-strike
tensor: the ladder count is bounded by 2 × nodes.

### Construction clocks and peak memory

| | identify | families | ladders | audit | digest | total | per expiry max |
|---|---|---|---|---|---|---|---|
| SPY surface (4 expiries, 8 lazy family builds, 218,960 family evaluations) | 1.6 ms | 29.3 ms | 1399.5 ms | 32.6 ms | 1.0 ms | **1464 ms** | 518 ms |
| SPY 23 d (2 lazy family builds, 49,073 evaluations) | 0.04 ms | 8.3 ms | 319.7 ms | 7.3 ms | 0.06 ms | 335 ms | — |

- Ladders are 96 % of construction. Each ladder costs 12 × 6 family samples (default policy), then
  certification at the midpoints, the ends and the node row, then the edge's
  `exercise_boundary` solve.
- Families cost 3.7 ms each. Tier 1 needs no kernel work when phase B hands over the fit's
  holder (`build_serve_record_with`).
- Tier 0 then still costs about 1.4 s per SPY symbol. H1 should build ladders off the tick
  path.
- Process peak working set: 8.2 MB before the first SPY-surface build, 12.6 MB after
  (+4.4 MB for 2.57 MB of records). For SPY 23 d: 11.3 MB → 14.2 MB.

### Consistency (F12)

| Check | SPY surface | SPY 23 d | Fixture tests |
|---|---|---|---|
| node price == slice node bytes, every tier at the fit spot | 3997 / 3997 answers | 934 / 934 | `BuildCopiesEveryNodePriceByteForByte`, `ServeAtTheFitSpotReturnsTheCertifiedNodePriceOnEveryTier` |
| between nodes at the fit spot: \|family − slice\| ≤ declared `between_node_bound_usd` | 9814 / 9814, max ratio 0.571 | 2324 / 2324, max 0.500 | `BetweenNodesAtTheFitSpotServeTheSliceWithinTheDeclaredBound` |
| `price_map_delta` declared (Family, fit spot) | max 1.57e-4 USD (K = 230 call, ln K/S = −1.21, deep ITM); p50 6.8e-14, p99 1.4e-5 | max 6.2e-9 USD | `FamilyTierServesTheFamilyMapToTheBitAtAMovedSpot` (0 off the fit spot) |
| `price_map_delta` declared (Ladder, fit spot) | max 1.18e-4 USD | max 3.5e-5 USD | `LadderServesPriceAndFourGreeksFromOneCertifiedMap` |
| ladder certificate | max 4.88e-4 USD (gate 5e-4) | max 1.46e-4 USD | |
| Family Greeks == A0's bundle, bit for bit | A0 is not in the base; verified on the overlay of A0 `ed2d2e05` | | `FamilyTierGreeksAreA0sBundleToTheBit` (A0 seam) |
| tier-0 rows in the near-edge band | 0 (min `edge_sd` 0.41) | 0 (min 1.78) | `LadderWithholdsThetaAndGammaInsideTheNearExerciseEdgeBand` |

## Support matrix row

| Surface | Tier 0 Ladder | Tier 1 Family | Tier 2 Certified | Tier 3 Cold | Greeks | IV |
|---|---|---|---|---|---|---|
| serve record (H0-A phase A) | served node sides with a certified ladder, spot within ±clamp(2σ√T, 0.25 %, 4 %) in ln S | continuous-yield and funding-PV carry, T ≥ 3/365, σ and q in the side box | any non-dated-cash fit (at the fit spot the slice, else family + Fast) | dated cash, out of box, out of the tiers asked, price-only off the fit spot (Refit), out of domain (Refit), stale (Rebuild) | Ladder Δ Γ ν θ (Γ, θ withheld in the edge band); Family ν, or all 6 under A0's mask with A0; Certified as Family | Ladder root, then family root (Unconfirmed without K3; K3 enclosure under the seam), then Fast confirm; L1c band gate first |

## What phase B / H1 must assume

- **Publishing.** A record is immutable and shared as `shared_ptr<const>`. Publish by
  swapping the pointer. Readers keep the old record alive until they drop it; nothing else
  reclaims it.
- **Build cost.** Ladders dominate, about 0.35–0.5 s per expiry here. Hand the fit's family
  holder in (`build_serve_record_with`) and build off the tick path. A record without
  ladders (`policy.ladders = false`) serves tiers 1–2 at no extra kernel cost.
- **Refusals.** A refusal is never a silent cold solve. Route by `ServeRoute`: `ColdPath`,
  `Rebuild`, `Refit`.
- **Staleness.** `Stale` requires `policy.max_age_ns > 0` and a `now_ns` in the request. Age
  is measured from `version().as_of_ns`: the snapshot's quote clock, or else build wall time.
- **Sticky strike.** A spot move never moves the smile; only a new record does.
- **θ and Γ.** Absent inside the near-edge band on every tier. The L3g σ-family bundle is
  never a source.
- **Chain path.** The tier-1 chain is about 1.5 ms per 300 contracts. The tick-rate chain
  is tier 0.
