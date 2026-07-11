# atx-vol SIMD / AVX2 fast-path kernels

Vectorized batch kernels for the four hot paths of the options-analytics
pipeline — **pricing, greeks, fitting, portfolio P&L-explain** — plus a bonus
implied-vol inverter. Each kernel keeps a scalar baseline (the numerical source
of truth) and an AVX2 (4-lane f64) fast path selected at runtime, and each is
gated by a parity test proving the vector path reproduces scalar to
machine-precision-scale error.

## Architecture

- **Runtime dispatch** (`atx/vol/simd/cpu.hpp`): `simd::have_avx2()` detects
  AVX2+FMA via CPUID (leaf 1 `FMA`/`OSXSAVE`/`AVX`, leaf 7 `AVX2`) **and**
  `XGETBV` (OS has enabled YMM state) — the clang-cl/MSVC-correct check (no
  `__builtin_cpu_supports`/`__cpu_model`, which don't link on this toolchain).
  Detected once, cached. Every public batch entry calls it and dispatches to the
  AVX2 kernel or the scalar loop.
- **Per-file ISA, not library-wide**: the AVX2 kernels live in dedicated
  `src/simd/*_avx2.cpp` translation units compiled `-mavx2 -mfma` per-file. The
  rest of atx-vol stays baseline x86-64, so the binary loads on any CPU and only
  *calls* the AVX2 kernels when CPUID confirms support. `CMakeLists` globs
  `src/simd/*.cpp` (with `*_avx2.cpp` getting the ISA flags), so new kernels need
  no build wiring.
- **Shared 4-lane transcendentals** (`atx/vol/detail/vector_math.hpp`): `log_pd`,
  `exp_pd`, `norm_pdf_pd`, and `norm_cdf_pd` — Φ via a 48-term Chebyshev–Clenshaw
  expansion built once from `atx::core::norm_cdf` (erfc), so the vector path
  tracks the scalar source of truth with no hand-transcribed constants.
  `norm_cdf_pd2` fuses the two independent Chebyshev recurrences for Φ(d₁),Φ(d₂)
  into one loop, hiding the latency-bound Clenshaw dependency chain.
- **Economically-exact parity**: degenerate (T≤0 or σ≤0) and deep-wing
  (|d|>6, where the Chebyshev Φ loses relative accuracy under the F·Φ(d₁)−K·Φ(d₂)
  cancellation) lanes are patched through the exact scalar kernel, so parity is
  bit-exact there and ≤1e-6 absolute everywhere else.

## Measured speedups

i7-1260P (Alder Lake), clang-cl 18, `-O2` Release (`rel` preset). Google
Benchmark, process pinned to one P-core at High priority, median of 5 reps.
Throughput = contracts (or positions) processed per second; parity = max
absolute deviation of the AVX2 result from the scalar kernel over the workload.

| Kernel | Category | scalar | AVX2 | speedup | parity |
|---|---|---:|---:|:---:|:---:|
| Black-76 value+vega | **PRICING** | 11.7 M/s | 19.0 M/s | **1.62×** | 0.0 |
| Black-76 greeks (8 + price) | **GREEKS** | 4.90 M/s | 9.55 M/s | **1.95×** | 0.0 |
| eSSVI backbone w(k) over strikes | **FITTING** | 243.8 M/s | 630.6 M/s | **2.59×** | 0.0 |
| Taylor P&L-explain (SoA, cache-resident) | **PORTFOLIO PNL** | 31.6 M/s | 71.2 M/s | **2.25×** | 0.0 |
| Implied vol (SR-2017 + 2 Halley) | IV (bonus) | 0.94 M/s | 0.97 M/s | 1.03× | 0.0 |

Reproduce: `cmake --preset rel -DATX_BUILD_BENCH=ON && cmake --build build-rel
--target atx-vol-simd-bench && build-rel/bin/atx-vol-simd-bench`.

## Why the wins vary — where SIMD actually helps

The AVX2 win is largest where the compiler **cannot** auto-vectorize the scalar
path:

- **Fitting (2.6×)** — the scalar `essvi_backbone_w` is an opaque per-element
  function call with a branch (`rho_eff`); the compiler can't vectorize across
  it, but the arithmetic (one `sqrt` + FMAs, no transcendentals) is ideal for
  SIMD. Biggest win. (The strike-varying asymmetric-rho blend uses `tanh`; that
  regime falls back to scalar to preserve ~1e-12 parity — the common symmetric
  path is fully vectorized.)
- **P&L-explain (2.25×, cache-resident)** — the scalar loop scatters to 9 output
  columns, which clang does **not** auto-vectorize (`scalar_novec` ==
  `scalar_autovec` in the bench), so the explicit SoA AVX2 kernel is a genuine
  win. **Caveat:** at book scale (≫10⁵ positions, several MiB) the kernel is
  DRAM-bandwidth-bound and AVX2 ≈ scalar ≈ ~57 M/s; the 2.25× compute win only
  shows when the working set is cache-resident. Both regimes are reported
  honestly.
- **Pricing / greeks (1.6–1.95×)** — these fight fast, well-scheduled scalar
  `libm` (`erfc`/`exp`/`log`) that the vector Chebyshev-Φ + Cody-Waite exp/log
  can only moderately beat on Alder Lake. Greeks wins more (1.95×) because it
  amortizes the shared d₁/d₂/Φ/φ work across eight sensitivities.
- **IV (~1×)** — iterative and hard to beat: scalar `implied_vol` early-exits
  (~1 Halley) on fast `libm erfc`, while the vector kernel runs a fixed 2 Halley
  steps each with a latency-bound Chebyshev Φ, netting ~parity here. Correctness
  is exact; a bigger win needs hardware with slower scalar transcendentals or
  multi-group interleaving to hide the Clenshaw latency.

## Correctness gates

`atx-vol-tests --gtest_filter=Simd*` — 23 parity tests across all five kernels:
broad grids, both sides, degenerate/deep-wing/near-band-edge lanes, every
`n % 4` tail residue, and zero-length no-ops. Vector results match the scalar
source of truth within a combined abs+rel tolerance (≤1e-6 abs for price-like
quantities; ~1e-12 for the pure-arithmetic fitting/P&L kernels); patched lanes
are bit-exact.
