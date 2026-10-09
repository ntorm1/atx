# Vector Φ notes — K2 pre-collect (WS-K)

Pre-collected sources + in-tree grounding for K2 ("AVX2 boundary batch: push ≥2.5× and
ship"). This is a **starting point for K2**, not a K1 deliverable — written while the K1
research was open so K2 begins from evidence, not a blank page. In-tree measurement wins
over any paper (§9.5 dispatch rule).

## 0. The single most important in-tree fact

**The mask-blend 3-region Cody-erfc Φ the sprint's K2 row asks us to "research and build"
is ALREADY built and shipping in the boundary kernel.** `detail/vector_math.hpp` provides:

- `norm_cdf_erfc_pd2(...)` — full-range **3-region Cody rational-erfc** Φ, all lanes
  evaluate every region branchlessly and select by `blendv` (pure bitwise mask select).
  `kCodyThresh = 0.46875`, `kCodyA/B/C/D/P` are the classic W.J. Cody (1969) rational
  Chebyshev erf/erfc coefficients. Region-3 (deep wing) non-finite math on out-of-region
  lanes is masked out. Comment: `vector_math.hpp:211-232`.
- `exp_pd` / `log_pd` — hand-FMA Cody-Waite range-reduced transcendentals
  (`vector_math.hpp:44-128`).
- `norm_pdf_pd` — φ via `exp_pd`.

The AVX2 boundary kernel (`src/simd/american_boundary_avx2.cpp`) already routes Φ through
`norm_cdf_erfc_pd2` (task "A4 [S1]" made Φ **single-source with the scalar
`andersen_lake`** erfc Φ — see the file header, lines 26-30, 82-89). So:

- The "current per-lane math vs mask-blend 3-region Cody-erfc" comparison the K2 row
  frames is **already resolved in favor of the mask-blend Cody-erfc** — it's in place.
- **SVML is out** on clang-cl (unavailable — §11.1a dead-end; the in-solve xsimd probe
  measured 6.6× *slower*). Do not re-litigate.
- Therefore K2's Φ question is narrower: **is `norm_cdf_erfc_pd2` the bottleneck in the
  1.87× boundary batch, and would a vendored SLEEF u35 (or further FMA tuning of the
  Cody path) beat the current inlined hand-Cody on THIS host?** Measure before swapping.

## 1. The probe already exists

`src/simd/vector_math_probe_avx2.cpp` (+ `simd/vector_math_probe.hpp`) is the "P3.3 bound
test + bakeoff bench" surface: AVX2 array wrappers around `log_pd`/`exp_pd`/`norm_cdf_*`
that stream 4-wide with a padded tail, so the graded/timed values are exactly what the
batch kernels compute. **K2 step 1 = run this bakeoff** (ns/op + max ULP vs a long-double
reference) for `norm_cdf_erfc_pd2` and `exp_pd`, so any SLEEF comparison is head-to-head
on the same host, same grid — the same discipline the IV shootout used to stand atx-vol's
inverter vs vendored Jäckel LBR.

## 2. SLEEF — what it would buy, and its cost

- SLEEF ships **u10 (≤1 ULP)** and **u35 (≤3.5 ULP)** variants; between them there is
  **>10× throughput difference** (u35 is the fast one). `Sleef_erf`/`Sleef_erfc` exist for
  `__m256d` (`Sleef_erfdx_u10`/`u15`, `Sleef_erfcdx_u15`). Our Φ needs only ~1e-3 price
  accuracy at the fast tier (K1 ladder) and ~1e-8 at the reference tier; u35-class erfc is
  ample for the marks/greeks tiers.
- SVML "promises ULP 4.0 but is usually ~2.0"; SLEEF "promises 1.0 or 3.5". Our hand-Cody
  is ~1–2 ULP class and **inlined with no call/ABI overhead** — for a 4-wide inner loop
  that runs `fp·nb·sweeps` times per solve, the absence of a function-call boundary is a
  real advantage a vendored SLEEF call may not overcome. This is the crux to measure.
- Adopting SLEEF means **vendoring** it (isolated, warnings-off static lib, license-checked
  — exactly the LBR pattern in `bench/thirdparty/lets_be_rational`, wired via
  `add_subdirectory` + `target_link_libraries`). Non-trivial; justify with a probe delta
  first.

## 3. Where the 1.87× → ≥2.0 gap most likely is (per the K1 ladder)

The K1 preset ladder (`docs/al-preset-ladder.md`) says the dominant cold cost is the
fixed-point block `n_quad_fp · n_boundary · n_sweeps`, and that the current fast preset
**over-pays it** (fp=16, 4 sweeps) vs a `ql_fast` rung (fp=8, 2 sweeps) that is ~1.8×
cheaper at equal accuracy. The AVX2 batch kernel lays down a **fixed-16-iter 4-wide BAW
seed** and sweeps at the fast preset. So the highest-leverage K2 moves, in order:

1. **Trim the fixed-16 BAW seed iterations + adopt the `ql_fast` fp/sweep budget**
   (fp=8, 2 sweeps, decoupled premium 32). This cuts the *count* of Φ/exp evaluations —
   worth more than making each Φ faster. (Specialize `(7,8)` in `al_fp_specialized` so the
   laned path is hoisted; see the ladder note §4 confound.)
2. **Sort pack membership by (T, moneyness)** so lanes converge in lockstep and the
   active-mask idle-lane waste (§11.5 divergence cliffs, ~16.5% guard fallbacks) shrinks —
   equal-T grouping already exists upstream.
3. **Only then** consider swapping Φ (SLEEF u35 vs hand-Cody) — and only if the probe
   bakeoff shows Φ is a real fraction of the kernel wall on this host.

Rationale: fewer, better-packed solves beats a faster transcendental, because Φ is already
vectorized and single-source. The ladder's `ql_fast` win is the substrate K3's laned
greeks bundle rides on.

## 4. Sources

- SLEEF paper: Shibata & Petrogalli, "SLEEF: A Portable Vectorized Library of C Standard
  Math Functions", [arXiv 2001.09258](https://arxiv.org/pdf/2001.09258).
- SLEEF x86 reference (erf/erfc u10/u15, __m256d): https://sleef.org/x86.xhtml
- SLEEF vs SVML micro-benchmarks: https://github.com/RoyiAvital/Projects/tree/master/SleefVsSvml ;
  discussion: https://medium.com/@himanshi18037/sleef-optimisations-in-svml-functions-f16b81cf6d98
- W.J. Cody, "Rational Chebyshev approximation for the error function" (1969) — the basis
  of `norm_cdf_erfc_pd2`'s `kCodyA/B/C/D/P` coefficients.
- Intel SoA vectorization study: [arXiv 2204.13740](https://arxiv.org/abs/2204.13740);
  masked-lane divergence practice: [arXiv 2606.17065](https://arxiv.org/abs/2606.17065).
- In-tree: `detail/vector_math.hpp` (Φ/exp/log), `src/simd/american_boundary_avx2.cpp`
  (kernel), `src/simd/vector_math_probe_avx2.cpp` (bakeoff probe),
  `american_boundary_batch.cpp:31-73` (the `kShipAvx2Boundary` gate).
- §11.1a dead-end: SVML unavailable on clang-cl; in-solve xsimd 6.6× slower — the lane
  axis is cross-contract, Φ stays inlined hand-FMA unless a probe says otherwise.
