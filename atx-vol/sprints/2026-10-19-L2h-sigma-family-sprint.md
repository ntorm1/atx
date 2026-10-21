# L2h sigma-family sprint note (vol-sota resume wave 2, 2026-10-19)

Lane `feat/vol-L2h-sigma-family-20261019`, based on L2f's tip `feat/vol-L2f-al-fast-tier-20261019
@ 4ba287c9` (= PM prebase 53ba6a3f + Screen tier + the AlBoundaryHandle generation fix). Pool-4,
RunId `vol-20261019r2-L2h`. Brief: `.superpowers/sdd/2026-10-18-vol-sota/lane-L2h-sigma-family-brief.md`;
research `research-sota-american-hft-report.md` §4 / §9 R1. User directive (binding): fastest AND
most accurate; new algorithms allowed when superior; backwards compatibility not required.

Everything here is OPT-IN by construction: no fitting path, preset, policy key or record format
constructs an `AlSigmaFamily`. The AL kernel fingerprint `53c4df68` and the default parity
`c89d0739` are the proof that nothing default moved (§6). Timings are UNQUALIFIED (shared host,
one run); the load line sits next to each table. Evidence folders:
`C:/atx-cache/quote-benchmark/L2h-sigma-family-20261019-{us,gates}/` (manifests `sha256.txt`).

## 1. What landed

| Piece | Where | Contract |
| --- | --- | --- |
| `AlSigmaFamily` (+ `AlSigmaFamilyStatus`, `AlSigmaFamilyPrice`, `AlSigmaFamilyChord`, `AlSigmaFamilyAxis`, the `kAlSigmaFamily*` guards) | `api/pricing/american.hpp` (beside `AlBoundaryHandle` / `al_price_pack`), NEW TU `src/pricing/american_sigma_family.cpp` (added to the pricing source list) | One (expiry, side) family bound to a `const AlExpiryGeometry &`: `prepare(sigma_lo, sigma_hi, n_sigma, axis)` runs n_σ ∈ [4, 16] prepared boundary solves at the anchor (the geometry's own `AlOpts`, so a Reference geometry gives Reference nodes) on Chebyshev-Lobatto nodes of the box; `price(S, K, sigma)` interpolates y[] at σ (2nd-kind barycentric), rescales to the contract's internal strike (T8 homogeneity) and runs the premium quadrature at the geometry's p (48 for the accurate preset); `price_and_chord` adds the σ + 1e-4 price from the same family. Move-only, ~6 KB heap, immutable after `prepare()`, `price*` const / allocation-free / thread-safe on one const family |
| Typed refusals | same | `prepare`: `InvalidArgument` (unprepared geometry, degenerate box, n out of range, anchor ≤ 0, T below `kAlSigmaFamilyMinTau` = 3/365), `NotImplemented` (side not American on this expiry, collapsed node boundary), `Internal` (table missing) — never half-built (`prepared()` false after any refusal, the earlier preparation discarded). `price`: `NotPrepared`, `GeometryRePrepared` (the handle's generation rule), `InvalidArgument`, `SigmaBelowGuard` (< 0.01), `SigmaOutsideBox` (never extrapolated; the chord needs 1e-4 of headroom). A refusal carries NaN prices and the status; nothing allocates |
| Tests `AlSigmaFamily.*` (11 cases, ctest `pricing/AlSigmaFamily.*` in `atx-vol-pricing-slice`) | `tests/american_geometry_test.cpp` (pricing slice) | §4 bounds pinned at 12 nodes on the default axis (and the Sigma axis's own); node/anchor exactness; default axis; typed refusals; concurrency (8 threads bit-identical to the serial loop); chord shape as `AlPricePack`; moved-spot call; IV on the family vs the Reference inversion |
| Bench rows | `bench/al_preset_ladder_bench.cpp`, env `ATX_VOL_AL_FAMILY=1` | `american/spy23_family/<axis>/n<N>` and `american/family_error/<set>/<axis>/n<N>` for N ∈ {8, 12, 16}, both axes (§4 / §5) |

Not touched: `boundary_interp.hpp/.cpp` (the ~40-line σ-axis evaluator is duplicated in the new
TU with a comment naming the twin — brief item 6 option (b) — so `andersen_lake_{put,call}_slice_sigma`
stay bit-identical by construction; their `SigmaInterp.*` tests pass unchanged, §6), the
`amer::` seam, everything under `src/fitting/`, `american_iv.*`, `batch_chain*`, the AVX2 kernel.

## 2. Design (what the family is and is not)

- **One family per (expiry, side).** The dimensionless boundary y[] = (log(b/xmax))² of the
  internal put depends on (σ, r, q, τ) only. The family stores y[k] at each σ-node (12 nodes ×
  ≤ 32 collocation nodes), the K-independent node grid (z, wbary, x, tau, n, T) and a pointer to
  the orientation's prepared workspace (the premium quadrature reads only its const tables:
  qx/qw_price, n_quad_price, fast, specialize). `State` = 5,776 bytes (`state_bytes()`, the
  bench's `state_bytes` counter on the avx2 build; the 5,408 written at the checkpoint was an
  estimate); the solve scratch (~46 KB) lives on `prepare()`'s stack only.
- **The anchor is the internal-put strike of the node solves**: a reference strike for a put
  family (the slice's choice is the spot), the spot for a call family. AT the anchor and AT a
  σ-node a family price equals the cold solve to 0 ulp on the Sigma axis (the barycentric
  evaluator returns the node's y[] on an exact hit); on the default LogSigma axis the
  σ → log → z → σ round trip leaves 8.44e-10 relative (`NodePricesMatchTheColdSolveAtTheAnchor`,
  worst 8.4357e-10 over 120 prices, pinned 8.5e-10). Any other
  internal strike is served by the homogeneity rescale (bnd.K, bnd.xmax = K min(1, r/q) rewritten;
  y[] untouched) — a few ULP from a fresh solve at that strike (LEDGER 2026-10-19 L574: "a put
  boundary is reusable only at its own strike" — reusable to a few ULP, which PM ruling (a)
  accepts). **A call family therefore survives a spot move**: a family built at S0 prices the chain
  at S1 by the same rescale (measured: 1 % move, worst 1.7979e-9 relative, pinned 1.8e-9,
  `CallFamilyServesAMovedSpotWithinBound`) — no rebuild on a tick.
- **σ-axis.** Chebyshev-Lobatto nodes on σ (`AlSigmaFamilyAxis::Sigma`) or on log σ
  (`LogSigma`); both measured (§4). The default is the axis the tables favour.
- **Allocation-free and thread-safe by construction**: `price()` writes only a stack `AlBoundary`
  (1.3 KB); the family has no mutable member after `prepare()` (unlike `SigmaBoundaryInterp`'s
  mutable scratch). The race-freedom argument (no TSan here): no write after `prepare()`;
  `prepare()` and the geometry's `prepare()` need exclusive access (documented), and a
  re-prepared geometry is detected by the generation stamp on every price.
- **The near-expiry / small-σ guards are the P2.5 slice's** (3/365, 0.01). A query outside the
  box is a typed refusal, never an extrapolation; L2i's box rule is in §7.
- **Not a kernel for the non-monotone band.** The L1c refusal is keyed on an inversion TARGET
  (price − zero-vol value ≤ `al_non_monotone_gate`), which a pricer never sees; it stays in front
  of any family-based inversion (§7).

## 3. Cost (SPY 23 d, avx2, unqualified, one run)

Rows `american/spy23_family/<axis>/n<N>` of `L2h-fix1-20261020/spy/spy-rows-66467db1.txt`
(sha256 `fcc87622…`; `atx-vol-al-preset-ladder-bench.exe` built from the committed source at
`66467db1`, exe sha256 `eb74f0d9…`, run 2026-10-20T19:36:44-04:00). **Provenance (review-L2h M1):**
the SPY 23 d and off-node rows first published in this note came from lane 1's 21:49 pre-checkpoint
build, whose family source was never archived and differs from the committed one in the SPY-only
path; every SPY / off-node digit in §3 / §4 / §5 / §7 was replaced at this SHA by the committed
binary's run (fix round L2h-fix1). Inputs: the SPY 2026-11-06 chain
(`L2f-al-fast-tier-20261019-inputs/spy23-quotes.csv`, sha256 `0c68bb76…`, verified before the run,
317 contracts with their own σ), grouped by (S, T, r, q, side) = 148 families (the file carries a
per-row sdiv: 74 distinct q over 322 rows, see §7 item 6), each built on a Reference geometry over
the chain's box (§7 item 3), build = median of 5 `prepare()` calls per family, price / chord = 3
timed passes per contract. Per-node = build p50 / n.

| axis | n_σ | build per family p50 / max µs | per node solve µs | `price` p50 / p99 µs | `price_and_chord` p50 µs |
| --- | --- | --- | --- | --- | --- |
| LogSigma | 8 | 826.5 / 1,089.4 | 103 | 4.6 / 6.5 | 9.2 |
| **LogSigma** | **12** | **1,240.4 / 1,535.5** | **103** | **4.7 / 6.6** | **9.3** |
| LogSigma | 16 | 1,653.5 / 2,677.1 | 103 | 4.9 / 7.5 | 9.6 |
| Sigma | 8 | 1,146.1 / 1,666.8 | 143 | 6.9 / 9.6 | 13.6 |
| Sigma | 12 | 1,248.5 / 1,552.5 | 104 | 4.8 / 7.3 | 9.3 |
| Sigma | 16 | 1,663.2 / 1,962.4 | 104 | 4.8 / 6.3 | 9.4 |

Against the cold tiers per boundary solve + price on the same chain (L2f note §5, one run each;
the lane's loaded run / the review's / the probe's): **Reference 260.1 / 159.7 / 169 µs**, Fast
68.6 / 71.7 / 69.8, Screen 20.7 / 23.3 / 23.7. A family price at 12 nodes (4.7 µs) is therefore
34-55× below a Reference solve, ~15× below Fast and 4.4-5× below Screen; a chord (two prices from
the same family, the Newton slope) 9.3 µs vs a cold pack's two solves 320-520 µs. Break-even of
one 12-node LogSigma family: 1,240 / (160 − 4.7) ≈ 8.0 Reference prices replaced (4.9 at 260 µs);
≈ 19 Fast prices; ≈ 78 Screen prices. The 16-node rows build 1.33× slower (1,653 vs 1,240 µs: the
per-node solve cost is flat at ~103 µs, so build scales with n_σ) and price within noise of 12 nodes
(4.9 vs 4.7 µs p50) — the 1.5× per-price gap at 16 nodes in the superseded table was host drift in
lane 1's run (its 16-node rows ran last), not the n_boundary × n_σ barycentric. No Fast-node build
time was measured (the bench builds on Reference geometries only; a Fast node solve is ~69 µs vs
~103 µs here, so ~0.8 ms per 12-node family is the arithmetic, not a number).

## 4. Error vs the cold Reference kernel

Every family price is compared with `andersen_lake(g, S, K, σ, side)` on the SAME Reference
geometry at the queried σ (ruling (a): distance to the accurate root). Sets (`family_error/<set>`):
**spy23** = the 317 SPY 23 d contracts at their own σ; **spy23offnode** = the same contracts with
σ replaced by the midpoint of the two family nodes around it (the P2.5 off-node pattern); **grid**
= the ledger 104-contract set; **fastgrid** = the `AlFastProfile` 1,350-contract grid (S 100,
T {0.02, 0.1, 0.5, 1.5, 4}, σ {0.08, 0.25, 0.7}, r {0.005, 0.045, 0.1}, q {0, 0.02, 0.09}, m
{0.7 … 1.4}, both sides) — one family per (T, r, q, side) over the WHOLE grid box [0.076, 0.735]
(the hard, wide-box shape); **deepcarry** = Screen's worst corner (put S = K = 100, T 2-5 y,
σ 0.05-0.12, r 0.05-0.10, q 0-0.03; 640). rel = |ΔP| / P_ref on P_ref ≥ 0.01; IV = a safeguarded
Newton on the family's own chord vs the Reference inversion of the same Reference price, on
contracts whose Reference σ ± 1e-4 bump moves the price by ≥ 1e-2 · max(P, 1) per unit vol
(iv_solved in the last column). The dev200 10,694-contract set of brief item 4 was NOT run (its
row is registered behind `ATX_VOL_AL_DEV200`; order-of-magnitude rule — listed as remaining work).

### 4.1 LogSigma axis (the default)

| set | n_σ | priced / refused | max abs USD | p99 abs | max rel | p99 rel | IV max / p99 bp | iv_solved |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| spy23 | 8 | 317 / 0 | 4.08e-4 | 3.47e-4 | 1.27e-5 | 1.04e-5 | 0.304 / 0.278 | 301 |
| spy23 | **12** | 317 / 0 | **4.96e-9** | 4.01e-9 | **1.49e-10** | **1.39e-10** | **0.0104 / 0.0052** | 301 |
| spy23 | 16 | 317 / 0 | 1.01e-9 | 2.61e-10 | 1.41e-11 | 7.96e-12 | 0.0104 / 0.0052 | 301 |
| spy23offnode | 8 | 317 / 0 | 4.59e-4 | 4.17e-4 | 1.23e-5 | 1.17e-5 | 1.792 / 0.448 | 296 |
| spy23offnode | **12** | 317 / 0 | 7.67e-9 | 5.23e-9 | **2.02e-10** | 1.61e-10 | **0.0947 / 0.0065** | 296 |
| spy23offnode | 16 | 317 / 0 | 1.03e-9 | 2.94e-10 | 1.43e-11 | 6.57e-12 | 0.0375 / 0.0092 | 300 |
| grid | 8 | 104 / 0 | 4.89e-11 | 3.31e-11 | 2.60e-12 | 2.20e-12 | 3.80e-4 / 2.81e-4 | 88 |
| grid | **12** | 104 / 0 | 8.79e-12 | 6.28e-12 | **1.28e-12** | 6.18e-13 | 3.80e-4 / 2.81e-4 | 88 |
| grid | 16 | 104 / 0 | 9.44e-12 | 3.88e-12 | 4.78e-13 | 4.05e-13 | 3.80e-4 / 2.81e-4 | 88 |
| fastgrid | 8 | 1,125 / 225 | 2.63e-2 | 3.57e-3 | 9.46e-3 | 1.27e-3 | 13.30 / 0.686 | 814 |
| fastgrid | **12** | 1,125 / 225 | 1.55e-5 | 1.93e-6 | **1.85e-6** | **2.23e-7** | 1.066 / 0.0443 | 814 |
| fastgrid | 16 | 1,125 / 225 | 7.01e-7 | 8.88e-8 | 3.19e-8 | 7.19e-9 | 1.061 / 0.0443 | 814 |
| deepcarry | 8 | 640 / 0 | 8.30e-6 | 6.47e-6 | 1.14e-5 | 8.04e-6 | 0.0238 / 0.0207 | 640 |
| deepcarry | **12** | 640 / 0 | 7.15e-8 | 1.77e-8 | **1.22e-7** | 2.69e-8 | 0.0236 / 0.0203 | 640 |
| deepcarry | 16 | 640 / 0 | 2.95e-10 | 1.28e-10 | 6.46e-10 | 1.86e-10 | 0.0236 / 0.0203 | 640 |

### 4.2 Sigma axis (selectable)

| set | n_σ | priced / refused | max abs USD | p99 abs | max rel | p99 rel | IV max / p99 bp | iv_solved |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| spy23 | 8 | 317 / 0 | 5.68e-5 | 5.13e-5 | 2.03e-6 | 1.85e-6 | 0.0635 / 0.0424 | 301 |
| spy23 | 12 | 317 / 0 | 6.06e-7 | 5.15e-7 | 1.83e-8 | 1.53e-8 | 0.0113 / 0.0053 | 301 |
| spy23 | 16 | 317 / 0 | 7.83e-9 | 6.97e-9 | 2.68e-10 | 2.05e-10 | 0.0104 / 0.0052 | 301 |
| spy23offnode | 8 | 317 / 0 | 6.25e-5 | 5.57e-5 | 1.95e-6 | 1.77e-6 | 0.154 / 0.0567 | 295 |
| spy23offnode | 12 | 317 / 0 | 7.10e-7 | 5.82e-7 | 1.87e-8 | 1.71e-8 | 0.0784 / 0.0069 | 298 |
| spy23offnode | 16 | 317 / 0 | 9.17e-9 | 7.53e-9 | 2.86e-10 | 2.30e-10 | 0.0362 / 0.0122 | 296 |
| grid | 8 | 104 / 0 | 1.51e-11 | 1.18e-11 | 1.66e-12 | 1.20e-12 | 3.80e-4 / 2.81e-4 | 88 |
| grid | 12 | 104 / 0 | 1.51e-11 | 1.37e-11 | 1.49e-12 | 9.84e-13 | 3.80e-4 / 2.81e-4 | 88 |
| grid | 16 | 104 / 0 | 2.06e-11 | 1.00e-11 | 1.47e-12 | 1.02e-12 | 3.80e-4 / 2.81e-4 | 88 |
| fastgrid | 8 | 1,125 / 225 | 3.24e-3 | 7.23e-4 | 2.10e-3 | 2.80e-4 | 5.00 / 0.193 | 814 |
| fastgrid | 12 | 1,125 / 225 | 1.55e-4 | 1.39e-5 | 5.39e-5 | 7.63e-6 | 1.056 / 0.0442 | 814 |
| fastgrid | 16 | 1,125 / 225 | 5.11e-6 | 5.03e-7 | 1.78e-6 | 1.92e-7 | 1.062 / 0.0445 | 814 |
| deepcarry | 8 | 640 / 0 | 3.39e-6 | 2.21e-6 | 3.55e-6 | 2.99e-6 | 0.0236 / 0.0202 | 640 |
| deepcarry | 12 | 640 / 0 | 5.81e-8 | 3.11e-8 | 1.23e-7 | 5.46e-8 | 0.0236 / 0.0203 | 640 |
| deepcarry | 16 | 640 / 0 | 1.81e-9 | 7.86e-10 | 3.96e-9 | 1.28e-9 | 0.0236 / 0.0203 | 640 |

### 4.3 Reading the tables

- **Axis.** At 8 nodes Sigma is tighter on EVERY set (spy23 2.03e-6 vs 1.27e-5, fastgrid 2.1e-3 vs
  9.5e-3, deepcarry 3.5e-6 vs 1.1e-5; IV fastgrid 5.0 vs 13.3 bp, spy23offnode 0.15 vs 1.79 bp).
  At 12 and 16 nodes LogSigma is tighter on every set in price (spy23 1.49e-10 vs 1.83e-8,
  spy23offnode 2.02e-10 vs 1.87e-8, fastgrid 1.85e-6 vs 5.39e-5, deepcarry 1.22e-7 vs 1.23e-7, grid
  1.28e-12 vs 1.49e-12); the rows reproduced at `66467db1` did not flip this. The one column where
  Sigma reads lower at 12 nodes is the spy23offnode IV max (0.078 vs 0.095 bp; the superseded table
  had the same ordering, 0.062 vs 0.067): both are the deep-ITM K 811 put, where the family's IV is
  within 1e-8 of the queried σ while the Reference inversion of the Reference price sits 0.07-0.09 bp
  from it — the cold inversion's tolerance floor on a low-vega contract, not the interpolation (see
  "IV floors" below). The header comment on `AlSigmaFamilyAxis` states exactly this; LogSigma is the
  default, Sigma stays selectable.
- **Refusals.** The only refusals in any set are fastgrid's 225 = 15 cold groups × 15 contracts: the
  **q = 0 CALL orientations** (5 T × 3 r). `classify_regime(rate = q = 0, yield = r > 0)` is
  `European` (american.hpp), so `AlExpiryGeometry::prepare` builds no call orientation (`id == 0`) and
  `AlSigmaFamily::prepare` refuses with `NotImplemented` ("side is not in the American regime …");
  the whole (expiry, side) prices cold — where the American call IS the European call. Neither σ
  guard (box low edge 0.076 > 0.01) nor τ guard (T 0.02 > 3/365) refused anything; no price-time
  `SigmaOutsideBox` occurred because the box is taken from the chain's own σ range (§7 item 3).
- **Worst cases at 12 nodes, LogSigma** (SPY inputs in `L2h-fix1-20261020/spy/spy-rows-66467db1.err.txt`,
  the other sets' in `family-spy23-sets.err.txt`): spy23 max rel 1.493e-10 at S 772.99, K 776, T 0.0682
  (ref 10.075000104086472, family 10.07500010258221); max abs 4.96e-9 USD at K 835 (ref
  62.044997620915531, family 62.044997625880356); IV max 0.0104 bp at K 845. spy23offnode max rel
  2.017e-10, max abs 7.67e-9 USD and IV max 0.0947 bp all at K 811. fastgrid max rel 1.850e-6 at
  put S = K = 100, T 4, σ 0.08, r 0.10, q 0 (Screen's worst corner, the low edge of the wide box);
  max abs 1.55e-5 USD at K 140, T 4, σ 0.25, r 0.045; IV max 1.066 bp at K 70, T 4, σ 0.25, r 0.045.
  deepcarry max rel 1.216e-7 at S = K = 100, T 5, σ 0.05, r 0.10. grid max rel 1.28e-12 at S 600,
  K 570.65, T 0.038.
- **IV floors (inference).** grid 3.80e-4 bp, deepcarry 0.0236 bp and fastgrid ≈ 1.06 bp are the SAME
  value at every n_σ ≥ 12 on both axes, each at one fixed contract (fastgrid: K 70, T 4, σ 0.25) while
  the price error moves by 60× between 12 and 16 nodes — the floor of the two inversions' tolerances
  (`american_implied_vol` tol 1e-10 vs the family Newton's 1e-13 · max(P, 1) on price) on contracts
  admitted at the 1e-2 · max(P, 1) vega threshold, not the interpolation. At 8 nodes the IV figure
  IS interpolation-limited (13.3 / 5.0 bp at K 110, T 4, σ 0.08).
- **Pinned bounds** (`AlSigmaFamily.*`, 12 nodes, 2-figure round-ups of the measured worst; each fails
  on revert, §6): default LogSigma at the sets' own σ — profile grid 2.1e-7 (2.0080e-7), SPY-like
  slice 3.7e-12 (3.6065e-12), deep-carry 4.8e-9 (4.7261e-9); off-node midpoints — 4.1e-6 (4.0552e-6,
  put S = K 100, T 4, σ 0.0863, r 0.10), 4.4e-12 (4.3331e-12), 1.3e-8 (1.2071e-8); Sigma axis
  off-node — 2.4e-5 (2.3136e-5), 2.0e-11 (1.9955e-11), 3.4e-8 (3.3989e-8); node / anchor 8.5e-10
  (8.4357e-10, the LogSigma σ → log → z → σ round trip; 0 on the Sigma axis); moved-spot call 1.8e-9
  (1.7979e-9, 1 % move); family chord vs the cold pack chord 8.5e-9 (8.4852e-9); family IV 0.018 bp
  (0.01773). **These bounds are empirical maxima over the listed sets (SPY 23 d, grid, fastgrid,
  deepcarry, the off-node midpoints), NOT a uniform enclosure**: a wider box, a lower σ edge or a
  longer expiry can exceed them and must be measured. **The Reference kernel's own convergence error
  versus a converged reference is NOT included** in any pinned bound — the family is measured against
  the Reference kernel, and the fingerprint's `convergence n=600` section is the only in-tree
  convergence evidence for the kernel itself.
- The brief's target (≤ 1e-6 relative p99 on SPY 23 d at 12 nodes, max rel ≤ 1e-5) is met by close
  to four orders of magnitude (p99 1.39e-10, max 1.49e-10); the wide-box fastgrid holds max 1.85e-6 / p99
  2.2e-7 at 12 nodes and needs 16 nodes for ≤ 1e-7 (3.2e-8).

## 5. µs table

Two one-run sources, each avx2 with the main thread pinned. The 12 SPY rows (`spy23_family`,
`family_error/spy23offnode`, marked `(66467db1)`) are the committed source's binary at `66467db1`
(exe sha256 `eb74f0d9…`, 2026-10-20T19:36:44-04:00; `L2h-fix1-20261020/spy/spy-rows-66467db1.txt` /
`.json` / `.err.txt` sha256 `fcc87622…` / `e0bb22c6…` / `52c324cb…`). The 18 non-SPY rows (grid,
fastgrid, deepcarry) are lane 1's run of 2026-10-19T21:50:06-04:00 on its 21:49 pre-checkpoint build
(exe `30415b6b…`, archived in `L2h-sigma-family-20261019-gates/`; `family-spy23-sets.txt` / `.json`
sha256 `4a79d9ef…` / `c56fcb41…`); their n12 ERROR digits were reproduced on the committed binary by
the review (`L2h-66467db1-review-v1/family/family-n12.txt`, six rows equal to the digit); their µs
were not re-run. Timings per family (build) and per contract (price, chord); every row's error
columns are in §4.

| row (source build) | build p50 / max µs | price p50 / p99 µs | chord p50 µs |
| --- | --- | --- | --- |
| spy23_family/logsigma/n8 · n12 · n16 (66467db1) | 826.5 / 1,089.4 · 1,240.4 / 1,535.5 · 1,653.5 / 2,677.1 | 4.6 / 6.5 · 4.7 / 6.6 · 4.9 / 7.5 | 9.2 · 9.3 · 9.6 |
| spy23_family/sigma/n8 · n12 · n16 (66467db1) | 1,146.1 / 1,666.8 · 1,248.5 / 1,552.5 · 1,663.2 / 1,962.4 | 6.9 / 9.6 · 4.8 / 7.3 · 4.8 / 6.3 | 13.6 · 9.3 · 9.4 |
| family_error/grid/logsigma/n8 · n12 · n16 | 785.1 / 933.9 · 1,140.4 / 1,264.3 · 2,305.0 / 2,475.0 | 4.9 / 6.1 · 4.9 / 5.3 · 7.3 / 8.5 | 9.7 · 9.7 · 14.5 |
| family_error/fastgrid/logsigma/n8 · n12 · n16 | 779.2 / 918.2 · 1,176.5 / 1,572.7 · 2,425.0 / 3,609.3 | 4.6 / 5.3 · 4.7 / 6.3 · 7.3 / 10.0 | 9.2 · 9.3 · 14.5 |
| family_error/deepcarry/logsigma/n8 · n12 · n16 | 742.1 / 880.9 · 1,111.4 / 1,144.3 · 2,235.3 / 2,484.9 | 4.3 / 5.3 · 4.3 / 4.8 · 6.5 / 7.7 | 8.4 · 8.5 · 12.8 |
| family_error/spy23offnode/logsigma/n8 · n12 · n16 (66467db1) | 826.0 / 1,054.3 · 1,241.3 / 1,550.3 · 1,653.5 / 2,019.5 | 4.7 / 6.4 · 4.8 / 7.0 · 4.8 / 7.1 | 9.2 · 9.5 · 9.5 |
| family_error/grid/sigma/n8 · n12 · n16 | 935.7 / 1,040.2 · 1,140.0 / 1,264.4 · 2,425.3 / 2,605.3 | 5.7 / 6.7 · 4.9 / 5.3 · 7.6 / 9.6 | 11.4 · 9.7 · 15.2 |
| family_error/fastgrid/sigma/n8 · n12 · n16 | 783.8 / 929.1 · 1,178.0 / 1,419.6 · 2,468.8 / 2,892.4 | 4.6 / 5.3 · 4.7 / 5.8 · 7.3 / 9.1 | 9.2 · 9.3 · 14.5 |
| family_error/deepcarry/sigma/n8 · n12 · n16 | 749.8 / 929.0 · 1,111.5 / 1,233.4 · 2,319.0 / 3,236.7 | 4.4 / 5.8 · 4.3 / 5.1 · 6.8 / 10.9 | 8.7 · 8.5 · 13.4 |
| family_error/spy23offnode/sigma/n8 · n12 · n16 (66467db1) | 1,032.3 / 1,569.0 · 1,248.2 / 1,528.7 · 1,663.4 / 2,057.6 | 5.9 / 10.2 · 4.7 / 7.0 · 4.7 / 6.7 | 11.6 · 9.3 · 9.4 |

**Load line (UNQUALIFIED).** SPY rows (this SHA's run): host load 11.8 % before / 12.5 % after
(`Get-Counter '\Processor(_Total)\% Processor Time'`), 4.64 GB free of 15.7, two other lanes live
(SPY1s-owner, integrate-W2b); banner in `spy-rows-66467db1.err.txt`: bench quiet-window protocol,
build ISA avx2, main thread pinned to P-core 7 (`cpu=7 observed=7 pcore=1 affinity=1 priority=1
throttle_off=1 -> PINNED`, pool mask 0x3f). Lane 1's run (the non-SPY rows) did NOT sample the host
load; two other lanes (L2e-resume, research-cold-start-gap) were live then (workspace `progress.md`).
The load spike lane 1's table showed on `spy23offnode/logsigma/n12` (build 1,853 µs, price 8.5 µs)
and the 1.5× per-price cost of its 16-node rows are absent from the reproduced run (1,241 µs / 4.8 µs;
16 nodes 4.9 µs per price) — host drift, not the kernel. Numbers cited elsewhere in this note are the
`spy23_family/logsigma/n12` row of this SHA's run: build 1.24 ms per family (max 1.54), price
4.7 / 6.6 µs, chord 9.3 µs.

## 6. Gates

Lane 1 (2026-10-19, pool-4, uncommitted at the user stop → PM checkpoint `850b6d4f`, ungated) and the
resume lane (2026-10-20, pool-3, RunId `vol-20261020r3-L2h`, `-Jobs 2`, `ATX_VOL_FIT_WORKERS = 2`;
`B` = the tree's `scripts\atx-build.ps1`). Verbatim outputs are in the lane report
(`.superpowers/sdd/2026-10-18-vol-sota/lane-L2h-sigma-family-report.md`); new outputs in
`C:/atx-cache/quote-benchmark/L2h-sigma-family-20261020-resume/` (manifest `sha256.txt`).

| gate | command | result |
| --- | --- | --- |
| compile of the checkpoint | `B configure -Preset vol-rel` (the warm slot had no object target for the new TU — re-configure first), `B check atx-vol\src\pricing\american_sigma_family.cpp -Preset vol-rel` | exit 0 / exit 0 |
| focused build | `B build atx-vol-pricing-slice -Preset vol-rel -Jobs 2` | exit 0 |
| focused gtests | `atx-vol-pricing-slice.exe --gtest_filter="AlSigmaFamily.*:SigmaInterp.*:AlFastProfile.*:AlScreenProfile.*:AlExpiryGeometry.*:AlBoundaryHandle.*:AlPricePack.*:AdjointGreeks*"` | exit 0: 60 tests / 9 suites, 59 PASSED, 1 SKIPPED (`SigmaInterp.SolveCount`, counters off — pre-existing); every `[AlSigmaFamily]` printout equal to lane 1's log to the digit |
| ctest anchor | `B -Ctest -Preset vol-rel -R "^pricing/AlSigmaFamily\."` | exit 0: 11/11 (`pricing/AlSigmaFamily.*`, tests #580-590 of `atx-vol-pricing-slice`) |
| fail-on-revert, LogSigma bound | `OffNodeSigmaWithinMeasuredBound` grid 4.1e-6 → 4.0e-6 | FAILED `4.0552295742417674e-06 vs 4e-06`; restored → OK; test blob `558d7386` = HEAD |
| fail-on-revert, Sigma bound | `SigmaAxisPricesWithinItsOwnMeasuredBounds` grid 2.4e-5 → 2.3e-5 | FAILED `2.3136175248833244e-05 vs 2.3e-05`; restored → OK |
| fail-on-revert, concurrency | stack `AlBoundary bnd = grid` → `static` shared scratch in `price_at` | FAILED `mismatched 546` (of 2,464); restored → OK; TU blob `5a8bc78d` = HEAD |
| chain-batch | `B -Ctest -Preset vol-rel -Jobs 2 -R "^chain-batch:"` | exit 0: 31/31 |
| default parity | `quote_benchmark.py run --dataset 20261014-dev200 --binary build-vol-rel\bin\atx-vol-quote-bench.exe … --model quote-slice --policy auto --dividends funding-pv` (cap 2; binary sha256 `ecae8b18…`) | exit 0: `c89d0739…` MATCH |
| name set | `ATX_VOL_FIT_WORKERS=3; B -Ctest -Preset vol-rel -Jobs 2 -R "^(pricing\|simd)/"` → `names.sh` (`abe9a35c…`) | 934 tests, 2 known failures (exit 8 as in every prior run); 19 names, sha256 `52324ef8…` EQUAL to `names-pre-L2b-8be778b5-cap3.txt` |
| PCH-off hygiene | `B configure -Preset vol-hygiene`; `B check <21 TUs: the L1c/L2f american.hpp includer set + american_sigma_family.cpp> -Preset vol-hygiene -Jobs 2` | exit 0 / exit 0: 25 objects observed compiling, `diag_lines=0` (bench TU outside the preset — ledgered) |
| AL fingerprint | `B build atx-vol-al-preset-ladder-bench -Preset vol-rel-avx2 -Jobs 2`; `ATX_VOL_AL_FINGERPRINT=…\fingerprint\fp-850b6d4f.bin atx-vol-al-preset-ladder-bench.exe --benchmark_filter=american/ladder_fingerprint` | exit 0: 48,660 doubles, ten section fnv1a lines equal to lane 1's `fp-lane.err.txt`, `fp-850b6d4f.bin` sha256 `53c4df68…` MATCH (re-run at the final SHA in the report) |
| fix round L2h-fix1 (2026-10-20, pool-3, RunId `vol-20261020r3-L2h-fix1`; review-L2h M1 + m1, docs / comments only) | SPY rows: `ATX_VOL_AL_FAMILY=1 ATX_VOL_AL_SPY23=<csv 0c68bb76…> atx-vol-al-preset-ladder-bench.exe --benchmark_filter='american/(spy23_family\|family_error/spy23offnode)/'` on the `66467db1` avx2 binary (`eb74f0d9…`); then, after the `american.hpp` comment edit: `B build atx-vol-pricing-slice -Preset vol-rel -Jobs 2`; `atx-vol-pricing-slice.exe --gtest_filter=AlSigmaFamily.*`; `B build atx-vol-al-preset-ladder-bench -Preset vol-rel-avx2 -Jobs 2` + the fingerprint run; `B configure -Preset vol-hygiene`; `B check atx-vol\src\pricing\american_sigma_family.cpp -Preset vol-hygiene` | SPY run exit 0 (§3-§5 digits; `L2h-fix1-20261020/`); build exit 0 (149 objects); gtest exit 0: 11/11, every `[AlSigmaFamily]` printout unchanged; avx2 build exit 0 (128 objects); fingerprint exit 0, `fp-L2h-fix1.bin` sha256 `53c4df68…` MATCH, ten section fnv1a equal; hygiene configure / check exit 0 / 0, 1 object compiled, `diag_lines=0` |

Nothing default moved: the fingerprint, the parity hash and the name set are the three proofs; no
caller constructs an `AlSigmaFamily` (L2i). `boundary_interp.*` untouched: the `SigmaInterp.*`
suite (6 passed + the counters skip) ran inside the focused gtest above. Timing numbers in §3 / §5:
the SPY rows are the fix round's run at `66467db1` (review-L2h M1 — provenance, §3), the non-SPY
rows lane 1's one run; no code change forced any re-run.

## 7. What L2i must assume

1. **The Premium iterates (`src/pricing/american_iv.cpp`).** Today every iterate of the Premium
   strategy and the band certification probe price the cold kernel: `andersen_lake(S, K, T, x, r,
   q, side, opts)` (e.g. `scalar_answer_certified`, american_iv.cpp:953-954 prices x and
   x · (1 − kCertProbeRel) cold to certify the root's slope), and `IvOpts::seed_profile`
   (`include/atx/vol/api/pricing/american_iv.hpp`, the `IvOpts` block at :147) selects the seed
   kernel's `AlProfile`. The family replaces the ITERATE price and its slope — `const
   AlSigmaFamilyChord c = fam.price_and_chord(S, K, x); slope = (c.price_sigma_bumped − c.price) /
   kAlPackSigmaBump` — never the final confirm, which stays L2g's policy (`graded.confirm`). The
   safeguarded Newton the bench and the test iterate (`family_invert`: bracket [lo, hi − 2h] on the
   family's own chord, 60 steps, 1e-13 · max(P, 1) on price) converged on 301 of 301 vega-bearing
   SPY contracts with IV max 0.0104 bp / p99 0.0052 bp vs the Reference inversion (§4.1).
   **The chord is the slope of the SAME map the inversion solves**: `price_and_chord` interpolates
   y[] at σ AND at σ + h, so the boundary moves with σ through the interpolant — it is never a
   frozen-boundary premium derivative. Proof: `AlSigmaFamily.ChordSlopeMatchesTheCentralDifference-
   WithinTheVolgaBound` checks (i) the family's forward chord against the family's own central
   difference (prices at σ ± h) within h · |volga| + 1e-8 (`AlPricePack`'s shape) and (ii) the
   family chord against the cold `al_price_pack` chord — whose two prices are bit-identical to
   fresh `andersen_lake` at σ and σ + h, i.e. the boundary RE-SOLVED at both σ — within a relative
   gap pinned at 8.5e-9 (measured 8.4852e-9, put K 130). It does not compare against a cold CENTRAL
   difference at σ ± h (the pack carries no σ − h price); the cold forward chord at the same h is
   the direct comparison, and the family's own central difference closes the volga term.
2. **The certification grid (`src/fitting/quote_slice.cpp` `certify_and_serve`, :1181).** Lines
   1203-1221 price every unique strike × both sides at the curve's σ through `model_price(out,
   strike, side, sigma, q)` → `andersen_lake(m.spot, strike, m.variance_years, sigma,
   d.effective_rate, q_effective, …)` (:208), then `AmericanPriceSlice::create(prices, bounds)`
   (:1232, again :1242 after a repair; `:1157`, `:1820`, `:1853` are the other `create` sites). The
   family replacement is `fam_put.price(spot, strike, sigma)` / `fam_call.price(spot, strike,
   sigma)` on one put and one call family per expiry (anchors below), accepted at a node only where
   the family's pinned bound at that node is below the certificate allowance `1.0e-8 + 1.0e-9 ·
   max(spot, strike)` (:842; also :1685 / :1703) — otherwise, and on any typed refusal, that node
   is priced cold. On SPY 23 d the family's max abs 5.0e-9 USD is under the allowance (≥ 1e-8 +
   7.7e-7) everywhere; on the wide fastgrid box the max abs 1.55e-5 is NOT (K 140, T 4), so the
   acceptance test is per node, not per family.
3. **The box rule** (what the bench used, `family_box` in `al_preset_ladder_bench.cpp`): over the
   chain's admitted bid / ask IV range [σ_min, σ_max] (`DeAmIvView::band`, `deam_iv_cache.cpp:
   119-154` — `bid_iv` / `ask_iv` from the inversions of bid and ask, after the kIvMin / kIvMax
   floor / cap prices) take lo = max(kAlSigmaFamilyMinSigma, 0.95 · σ_min), hi = 1.05 · σ_max +
   2 · kAlPackSigmaBump (chord headroom above the top), and widen a box narrower than 0.02 to
   mid ± 0.01. One box per (expiry, side) from the WHOLE chain, not per (T, r, q) group (item 6).
   A query outside the box is a typed `SigmaOutsideBox` refusal, never an extrapolation: the
   iterate then prices cold (count it; a frequent refusal means the box was wrong — widen and
   rebuild, 1.24 ms). The band floor / cap prices at kIvMin / kIvMax (`deam_iv_cache.cpp:130-131`)
   are outside any chain box by construction and stay cold (or get a dedicated wide family only
   where item 7's count pays for it).
4. **Refusals that route to a cold solve.** `prepare()`: `InvalidArgument` (unprepared geometry,
   degenerate box, n_σ ∉ [4, 16], anchor ≤ 0, T < kAlSigmaFamilyMinTau = 3/365) and
   `NotImplemented` (the side is not in the American regime on this expiry — q = 0 calls, r = 0
   puts; a node boundary collapsed; a node sweep could not move = the kernel's NotConverged, mapped
   as american.cpp's `al_solve_status_error` maps it) and `Internal` (Gauss-Legendre table missing)
   → the whole (expiry, side) prices cold, exactly as the P2.5 slice's `min_tau` guard does. (The
   `prepare()` header comment used to list NotConverged under `Internal`; corrected at this SHA to
   the TU's `NotImplemented` — review-L2h m1, comment only; no caller branches on the two codes.)
   `price()` / `price_and_chord()`: `NotPrepared` / `GeometryRePrepared` → rebuild (the geometry's
   generation stamp moved) or cold; `InvalidArgument` → the cold path's own refusal;
   `SigmaBelowGuard` (σ < 0.01) and `SigmaOutsideBox` → cold solve. **The L1c non-monotone refusal
   stays IN FRONT of every family-based inversion**: test the band first (`in_band`,
   american_iv.cpp:932-936: price − zero-vol value ≤ `al_non_monotone_gate(K, T, opts)`) and route
   band rows to the accurate cold inversion; the family never sees an inversion target.
5. **Ownership / threads.** The family observes a `const AlExpiryGeometry &` that must outlive it
   (a temporary cannot bind: deleted rvalue constructor); `prepare()` needs exclusive access and
   must not overlap the geometry's `prepare()`; after `prepare()` the family is immutable and
   `price*` are const, allocation-free and safe from any number of threads at once (tested: 8
   threads × 4 passes, 2,464 chord prices bit-identical to the serial loop; the shared-scratch
   anti-pattern fails that test with 546 mismatches, §6). Own it per expiry slot beside the IV cache
   in the rung memo and share it by const reference across rungs and the L2d fan-out.
   `state_bytes()` = **5,776 bytes** per family (one heap block; 2 sides × ~30 expiries ≈ 350 KB per
   SPY surface); `prepare()` uses ~46 KB of stack scratch.
6. **Anchors and the per-strike yield.** Call family anchor = the spot (McDonald-Schroder); put
   family anchor = a reference strike (the spot; any other strike is the few-ULP homogeneity
   rescale, LEDGER 2026-10-19 L574, ruling (a)). A geometry is keyed on (T, r, q, AlOpts): the
   quote files carry a per-row effective yield (`calendar_yield_at`, quote_slice.cpp:1204 — 74
   distinct q over 322 SPY 23 d rows), so a family per distinct (T, r, q, side) would be 148
   families × 1.24 ms = 184 ms for 317 contracts (build-dominated, the bench's own grouping); L2i
   must build on the expiry's `effective_yield` (one geometry per expiry, two families) and route
   the strikes whose calendar-yield adjustment moves the price beyond the allowance to the cold
   path — or count calls per geometry key under item 7 before building.
7. **ADAPTIVE build rule** (research-cold-start-gap §3.2 / TAKE 3; PM ruling 2026-10-20, resume 3).
   Build a family for an (expiry, side) ONLY when its expected family-priced calls
   N = 1.8 × inversions + 2 × certification strikes × passes + band edges exceed c · 2 · n_σ, with
   c = 3 to start: at n_σ = 12, **N > 72**. The break-even itself is N · (cold − family) > n_σ · cold
   ⇒ N > n_σ · cold / (cold − family) = 12 · 160 / 155.3 ≈ 12.4 calls (12.2 at cold = 260 µs); c = 3
   puts the threshold 6× above break-even so a mis-estimated count never makes cold start slower
   (the research's warning). SPY 23 d arithmetic, one expiry: 317 quotes × 1.8 ≈ 571 iterate prices
   + 2 × 160 strikes × ~3 passes ≈ 960 grid prices ≈ 1,530 calls ≫ 72 → build; cost 2 × 1.24 ms +
   1,530 × 4.7 µs ≈ 9.7 ms against 1,530 × 160-260 µs ≈ 245-400 ms cold (25-41×). A thin dev200
   chain (6 quotes): 6 × 1.8 + 2 × 6 × 3 ≈ 47 < 72 → stay cold (Screen / Fast). Full board: 57,281
   solved expiries × 2 sides × 1.24 ms ≈ 142 CPU-s if built unconditionally — the rule exists so
   that number is paid only where the chain pays it back. Fast-node build time: not measured (§3).
8. **Recommendation: n_σ = 12 on the LogSigma axis (the default)** — SPY 23 d max rel 1.49e-10 /
   p99 1.39e-10, IV 0.0104 bp max, build 1.24 ms, price 4.7 µs; 16 nodes (1.65 ms, 4.9 µs per price)
   only for a wide box that must hold ≤ 1e-7 (fastgrid [0.08, 0.7] at T 4: 1.85e-6 → 3.2e-8); 8
   nodes are not Reference-class on the real chain (1.27e-5 / 0.30 bp). Pinned bounds: §4.3.
