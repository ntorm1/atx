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
| Tests `AlSigmaFamily.*` (10 cases) | `tests/american_geometry_test.cpp` (pricing slice) | §4 bounds pinned at 12 nodes on the default axis; node/anchor exactness; typed refusals; concurrency (8 threads bit-identical to the serial loop); chord shape as `AlPricePack`; IV on the family vs the Reference inversion |
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
  qx/qw_price, n_quad_price, fast, specialize). `State` = 5,408 bytes (`state_bytes()`); the
  solve scratch (~46 KB) lives on `prepare()`'s stack only.
- **The anchor is the internal-put strike of the node solves**: a reference strike for a put
  family (the slice's choice is the spot), the spot for a call family. AT the anchor and AT a
  σ-node a family price equals the cold solve to 0 ulp on the test grid (the barycentric
  evaluator returns the node's y[] on an exact hit, and the σ → z → σ round trip hit the nodes
  exactly: `NodePricesMatchTheColdSolveAtTheAnchor`, worst rel 0 over 120 prices). Any other
  internal strike is served by the homogeneity rescale (bnd.K, bnd.xmax = K min(1, r/q) rewritten;
  y[] untouched) — a few ULP from a fresh solve at that strike (LEDGER 2026-10-19 L574: "a put
  boundary is reusable only at its own strike" — reusable to a few ULP, which PM ruling (a)
  accepts). **A call family therefore survives a spot move**: a family built at S0 prices the chain
  at S1 by the same rescale (measured: 1 % move, worst 1.46e-8 relative, §4) — no rebuild on a tick.
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

(filled from `american/spy23_family/*` — see §5)

## 4. Error vs the cold Reference kernel

(filled from the test suite and `american/family_error/*` — see the tables below)

## 5. µs table

(filled)

## 6. Gates

(filled)

## 7. What L2i must assume

(filled)
