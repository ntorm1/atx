# L2h-c — (σ, q) family BUILD speed: AVX2-batched nodes, exact state sizing (2026-10-21)

Lane L2h-c of the vol-sota sprint. Kernel-only, additive, opt-in (no caller): `AlSigmaQFamily`
(L2h-b, `c16f2bac`) builds its n_σ·n_q node boundaries four at a time through the AVX2 boundary
kernel with the sweep-invariant quadrature geometry hoisted, sizes its node tensor exactly, and
gains the two API items the SPY1s owner asked for (`price_strikes`, the OffNode self-check) plus
review-L2hb's minors m1-m7. Branch `feat/vol-L2hc-family-build-20261021`; evidence
`C:\atx-cache\quote-benchmark\L2hc-20261021-us\` (+ `sha256.txt`).

## 1. What changed
| step | commit | what |
| --- | --- | --- |
| 0 | `16d44805` | m1: the premium-quadrature header from EITHER geometry orientation (q- and side-free) — a q > 0 call box on a geometry whose call side is European now prices, bit for bit as on an American-call geometry; m2 `den`; m3 zero-filled weights + factored tensor sum (no 128-slot scratch); m4 `State` private + friend `sigma_q_family_detail::QFamilyAccess`; m5 `prepare()` split (Lobatto-axis helper, node plan, node solve); m6 the q-axis ACCURACY GUARD; m7 100 columns; `price_strikes`; `prepare(…, AlSigmaQFamilyCheck::OffNode)` + `self_check()`; `state_bytes()` a const member |
| 1 | `5d842b70` | THE BUILD: nodes 4 at a time through the AVX2 kernel (new `src/simd/american_boundary_nodes_avx2.{hpp,cpp}`, additive; the put driver and kernel header untouched); remainder < 4 and declined lanes on the scalar prepared solve; route = `simd::use_avx2()` (ForceScalar keeps the L2h-b scalar build) under Reference profile + Baw seed |
| 2 | `73df2191` | exact node tensor: n_boundary·n_q·n_σ doubles allocated at prepare (kept across a same-size re-prepare, released by a refusal) |
| 3 | `1f641a27` | bench rows `american/sqbuild/*` (scalar vs AVX2 build, parts, `price_strikes300`) |
| 4 | `fe4823ae` | the AVX2 node solve runs the kernel with a ZERO sweep budget (init + 4-wide BAW seed) and then the kernel's own JN/FP sweep loop with eqn_b_ND's sweep-invariant terms bound once per pack (`NodePackScratch`: lane-uniform barycentric terms + their sum, t_u masks; per lane qw·e^{ru}, qw·e^{qu}, σ√t_u). Every hoisted value is the kernel's own intrinsic sequence on the same inputs, consumed in its order — the boundary is unchanged (the AVX2-vs-scalar test numbers are identical to the digit before/after) |

API delta for the owner (all in the `american.hpp` L2h-b sub-block): `prepare(σ_lo, σ_hi, n_σ, q_lo,
q_hi, n_q, axis = LogSigma, check = AlSigmaQFamilyCheck::None)`; `price_strikes(S, span<const double> K,
σ, q, span<double> out) -> AlSigmaQFamilyStatus` (== `price()` bit for bit); `self_check() ->
AlSigmaQFamilySelfCheck{max_abs_per_strike, max_rel, prices, status}`; `state_bytes()` is now a CONST
MEMBER (was static — a static call `AlSigmaQFamily::state_bytes()` no longer compiles); new constant
`kAlSigmaQFamilyMaxQAxisEstimate` = 1e-6.

## 2. Build speed (avx2 tree, one run each, UNQUALIFIED: host 56-58 % CPU, 3.0-4.5 GB free)
`ATX_VOL_FIT_WORKERS=2 ATX_VOL_AL_SQBUILD=1 ATX_VOL_AL_SQFAMILY=1 ATX_VOL_AL_SPY23=…\L2f-al-fast-tier-20261019-inputs\spy23-quotes.csv (0c68bb76…) ATX_VOL_AL_SPYLONG=…\L2hb-20261021-errors\inputs\spy-21533-quotes.csv (da15e28c…) ATX_VOL_AL_SPYALL=…\spy-all-quotes.csv (bdc0ff03…) build-vol-rel-avx2\bin\atx-vol-al-preset-ladder-bench.exe --benchmark_filter='american/sq(build|family)/'`
(`sq-rows-final.{txt,json,err.txt}`). Before = the scalar build in the SAME run (ForceScalar = L2h-b's
algorithm); L2h-b's own published numbers (a less loaded host) in brackets. Build = single thread,
median of 5 prepares per family.

| (n_σ, n_q) | 23 d per side before → after | 2.17 y per side before → after | SPY board Σ before → after |
| --- | --- | --- | --- |
| (12, 3) | 5.26-5.70 [4.85] → **2.00** ms | 5.57 [4.31] → **2.82** ms | 322.8 [237] → **112.5** ms (sqfamily row 99.3) |
| (12, 4) | 6.87-7.32 [7.18] → **2.70** ms | 7.56 [5.61] → **2.97** ms | 434.3 [321] → **153.7** ms (sqfamily row 144.1) |
| (16, 4) | 8.99-9.75 [8.47] → **3.64** ms | 10.08 [7.57] → **4.01** ms | 547.3 [519] → **221.5** ms (sqfamily row 205.2) |

Same-run speedup 2.5-2.9× (board), 2.5× per side at (12, 4). Targets: ≤ 2 ms per side at (12, 4) —
MISSED (2.70 ms; (12, 3) meets it at 2.00); board ≤ 130 ms at (12, 4) — MISSED (144-154 ms); board
≤ 200 ms at (16, 4) — MISSED narrowly (205-221 ms). All on a loaded host.

Where the time goes (`american/sqbuild/parts/{spy23,spylong}`, one (12, 4) build):
| part | 23 d | 2.17 y |
| --- | --- | --- |
| one scalar prepared solve (median of 48 nodes) | 144.0 µs | 161.8 µs |
| one AVX2 pack of 4, inline geometry (step 1, first run) | 289.1 µs (72.3 / node, 1.88×) | — |
| one AVX2 pack of 4, hoisted geometry (step 4) | **224.9 µs (56.2 / node, 2.56×)** | 244.7 µs (61.2 / node, 2.64×) |
| one `al_prepare_oriented_geometry` (the scalar path's per-q setup; the AVX2 path needs none) | 8.2 µs | 9.1 µs |
| whole AVX2 prepare | 2,752 µs | 2,993 µs |
| prepare − 12 packs (inits, node plan, xmax checks, scratch, tensor) | 54 µs (2 %) | 57 µs (2 %) |
| OffNode self-check extra (opt-in) | 700 µs | 835 µs |

So 98 % of the build is the node solves; the quadrature / geometry setup is ~2 %. The remaining per
quadrature point cost after the hoist is one exp (b from y) + one log + two erfc + two divides per
lane per sweep (2 JN + up to 4 FP sweeps); the next levers are listed in §6.

Price path (sqfamily rows, AVX2-built families): p50 7.1 µs (23 d, (12, 4)), 6.5 µs ((16, 4)),
7.2 µs (2.17 y) — L2h-b 6.8 / 6.5 / 5.8 on a quieter host: unchanged within load noise.
`price_strikes` (`american/sqbuild/spy23/price_strikes300`, both 23 d sides, 300 strikes in
[0.7 S, 1.3 S]): 6.80 µs per strike batched vs 7.06 single (1.04×; the interpolation is ~4 % of a
price — the premium quadrature per strike dominates), 0 bit mismatches.

## 3. Accuracy (unchanged)
Every L2h-b pin holds on the AVX2 build with the same printed digits (gtest): spy23 max rel n_q 3 / 4
/ 6 = 2.763e-11 / 2.709e-11 / 2.709e-11; spy-long 4.885e-8 / 2.257e-9 / 2.160e-9; bench: spy23
(12, 4) 1.377e-10, 2.17 y (12, 4) 1.216e-9, board (12, 4) 7.449e-8, board (16, 4) 7.086e-11. The AVX2
and the scalar build agree to 5.6641e-15 (|dP|/max(P,1), 390 of 900 prices differ in the last bits;
pinned 5.7e-15); a collapsed q box is AlSigmaFamily BIT FOR BIT on the scalar build and to 3.55e-15
on the AVX2 build (pinned 3.6e-15 — the σ-family's own tolerance vs the cold Reference on these
boxes is 8.5e-10 node/anchor .. 4.1e-6 off-node 12-node grid set, 5+ orders above); node/anchor vs
cold 1.507e-16 scalar / 1.322e-15 AVX2 (re-pinned UP for the AVX2 build only: 1.4e-15, measured; the
node is the AVX2 kernel's boundary, which the batch's AvxBoundary parity bounds at ~4e-13).

## 4. State size
`state_bytes()` = 1,864 B block + n_boundary·n_q·n_σ·8: (12, 3) 5,320 B, (12, 4) **6,472 B** (was
34,592), (16, 4) 8,008 B — 66 SPY families ≈ 0.43-0.53 MB, inside a 1.25 MB L2.

## 5. The q-axis accuracy guard (m6) and the self-check
Guard: rho = a + sqrt(a² − 1), a = |box centre − r| / half-width; refuse when rho^(−n_q) > 1e-6.
SPY: no family refused (bench cold groups 22 at every n_q, families 54 — L2h-b's counts). A box
touching r is refused at every n_q; [1 %, 3 %] at r = 5 % needs n_q 8; the L2h-b 4 %-wide box
(1.3e-4 max rel at n_q 6) is refused at every n_q and its guard-driven bisection (16 pieces at
n_q 6) prices every strike at its own q within 7.44e-8 (pinned 7.5e-8). The guard measures r only:
q = 0 is singular for calls too but their exercise premium vanishes there (review-L2hb probe).
Self-check (opt-in, ≈ 0.7-0.8 ms): SPY 23 d put 1.02e-11 max rel (chain worst 2.7e-11), call 0;
an under-resolved family (6 σ nodes over [0.08, 1.2]) reports 5.5e-5 |dP|/K vs the chain's 5.7e-4 —
an order-of-magnitude estimate, not an enclosure (the header says so).

## 6. Next levers (not done here)
1. Same-carry σ packs: with n_σ a multiple of 4 the four lanes of a pack share (r, q), so the
   per-lane e^{ru} / e^{qu} tables are one per q node, not per pack (≈ 2·264 exp_pd per pack).
2. Warm-start each σ node from its converged neighbour (fewer sweeps; changes y at the tol level —
   needs a ruling, it is not "the same preset").
3. Cheaper boundary transcendentals in the sweep (the remaining exp/log/erfc per quadrature point).
4. The 66 families are independent: a multi-core build divides the board Σ by the core count (the
   ≤ 1 s bar is single-core, so this is the serve path's lever, not the bar's).
