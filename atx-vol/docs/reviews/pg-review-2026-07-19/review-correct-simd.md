# rev-correct-simd report — verbatim

## FINDINGS

1. **`black76_batch_avx2.cpp:159-161, 232-234` + `american_boundary_avx2_kernel.hpp:480-482` — MEDIUM — wing relative accuracy.** AVX2 put legs use complement form `df·(K·(1−Φ(d2)) − F·(1−Φ(d1)))`; scalar `black76_price` (black76.cpp:36) uses `Φ(−d2)/Φ(−d1)` directly; `euro_put_sk` same. After K2 removed deep-wing scalar patch, deep-OTM put lanes: F=100,K=50,T=0.1,σ=0.2 (d2≈+10.9) → batch returns exactly 0.0 vs scalar ≈2.6e-26·df — 100% relative error. Parity tests assert absolute bounds only → invisible. Economically nil but breaks consumers needing relative wing accuracy (log-price residuals, wing IV re-inversion). Note value/vega batch + IV kernel internally consistent (their scalar refs also complement form). Fix: compute put leg from Φ(−d1),Φ(−d2) (negate before `norm_cdf_erfc_pd2` — Cody kernel symmetric), or reinstate wing escape for put lanes; at minimum fix "≈1e-16 everywhere" comments + add relative-error wing test.

2. **`cpu.cpp:70-80` / `black76_batch.cpp:38,49`, `greeks_batch.cpp:52,65`, `essvi_batch.cpp:74-114`, `pnl_batch.cpp:52`, `batch.cpp:127,176,242,269` — MEDIUM — ISA override / MathMode not honored.** `set_simd_isa_override(ForceScalar)` / `set_math_mode(Reference)` only affect American boundary/greeks batches (`american_boundary_batch.cpp:94-105,149-158`). Every other batch dispatch gates on `have_avx2()` alone. Reference mode still gets FastDeterministic AVX2 numbers silently. `cpu.hpp:33-34` admits scoping ("intentionally NOT rewired, T13 scope"). Fix: rewire remaining dispatchers to `use_avx2()` or document loudly.

3. **`parallel_for.hpp:105-119` — MEDIUM — static `parallel_for` terminates on throwing worker.** No try/catch in jthread bodies; escaping exception (e.g. bad_alloc) → `std::terminate`. Dynamic overloads (148-174, 206-236) capture/rethrow; serial nt≤1 propagates. Behavior diverges by thread count and overload. Fix: capture-first-exception/rethrow-after-join in static variant. No exception/determinism test for static variant.

4. **`detail/vector_math.hpp:44-87` — LOW — `log_pd` silently wrong for denormal/0/inf ratio inputs.** Decodes exponent bits assuming positive normal. F/K underflow to denormal (−709.04 vs −713.8), to 0 (−709.09 vs −inf), overflow inf (+709.98 vs +inf). Resulting d1/d2 finite garbage → `nonfinite_mask(d)` doesn't fire. Impact masked (Φ saturated |d|≳39). Fix: `|lnFK| ≥ 708` escape or F/K normality patch; document domain.

5. **`detail/vector_math.hpp:99-132` — LOW — `exp_pd` returns +inf at top of clamp.** x near kExpHi → Nf rounds 1024 → inf where std::exp gives 1.798e308. Reachable via `exp_pd(RmQ·tau)` extreme rates → inf·0 NaN → caught by final isfinite scalar patches (unnecessary detour, not wrong number). Optional fix: clamp N to 1023, fold factor 2 into p.

6. **`essvi_batch_avx2.cpp:132-185, 248-279` — LOW — inconsistent guard coverage.** w-batch refuses vector on `!slice_vector_admissible` + patches non-finite-k; grad batch (135) + sigma batch (250) check only `blend_active`. |ρ|≥1/non-finite → NaN-propagation accident not construction; no tests. Also `essvi_backbone_w4` (70) + `svi_total_w_batch_avx2` (199) use fmadd where scalar mul-then-add (vol_surface.cpp:65,157) — ≤1 ulp cross-host divergence; sibling `svi_qe_basis_batch_avx2` (216-218) forbids fma for bit-parity. Fix: mirror w-batch guards; decide fma policy per kernel.

7. **`iv_batch_avx2.cpp:55-68, 378-390` — LOW — accept gate notional-dependent.** |resid| < 1e-4 absolute + vega ≥ 0.005·(F+K)·df → σ-error ≈ 0.02/((F+K)·df): fine F≈100, bad for F,K ≪ 1. Currently dark (R-24 scalar route). Fix if lit: scale `kIvProbeResidTol` by df·max(F,K) like scalar K1 noise floor (implied_vol.cpp:287-288).

8. **`greeks_batch.cpp:20` vs `greeks_batch_avx2.cpp:210-212` — LOW — asymmetric null `price_out` contract.** AVX2 AoS sink null-checks px; scalar fallback dereferences unconditionally. nullptr works on AVX2 host, crashes non-AVX2. Latent (only in-tree nullptr caller AVX2-gated). Fix: null-check scalar loop or assert at dispatcher.

9. **`american_greeks_avx2.cpp:106-107` — LOW — over-conservative eligibility + inconsistent unrequested-column content.** `lane_ok` requires `r − hr > 0` unconditionally; scalar applies only when need_rho (american.cpp:2627). {delta}-only batch with 0<r≤1e-4 needlessly patched (correct, wasted lane). Handled lanes leave unrequested greeks 0; patched lanes fill full bundle — mixed semantics. Fix: condition guard on needs.rho; zero unrequested in patch path.

10. **Stale kernel docs contradicting shipped math — LOW.** `iv_batch_avx2.cpp:9-27` describes SR-2017 seed/ITM parity rewrite/wing patch — none exist (Choi-L3 at 365, wing retired 358-363). `math_mode.hpp:9-13,47-52` FastDeterministic described as Chebyshev Φ + wing patch — now Cody erfc, no patch; `kFastDeterministicPhiBound` gates production-unused function. `greeks_batch.hpp:10-12` claims wing patch — retired.

**Verified non-findings:** adjoint_greeks reverse sweep verified term-by-term, no sign errors, seeds correct (theta = −dT calendar line 450); second-order matches Haug; Christianson tangent indexing consistent; guarded by cold-re-solve vega self-check + FD fallback. cpu.cpp CPUID correct. Tail loops all correct + tested. R-23 aliasing invariant holds. 4-lane American pack geometry sharing sound; converged-lane freezing matches scalar. Greek combine formulas match `american_greeks_al` exactly. IV accept gate can't admit outside-no-arb price. NaN-collapse min/max intentional.

## COVERAGE MAP

| Kernel | Parity tests | Gaps |
|---|---|---|
| black76_price_batch_avx2 | simd_batch_test, nan_test, norm_cdf_erfc_test, batch_test | No relative-error wing-put assertion (F1) |
| black76_value_vega (+shared-T) | simd_batch_test, batch_test SuppliedSqrtT | — |
| greeks batch AoS+SoA | simd_greeks_test (bit-exact degenerate, SoA≡AoS) | Wing bounds absolute-only |
| iv_batch_avx2 (dark) | simd_iv_avx2_direct_test, simd_iv_test | Only F∈{25,100,400}; no small-notional (F7) |
| essvi w backbone | simd_essvi_test, batch_test nonfinite-grid | grad/sigma: no nonfinite-k/non-admissible tests (F6) |
| pnl_batch_avx2 | simd_pnl_test | — |
| vector_math | simd_vector_math_test, scalar_erfc_test | No denormal/inf-ratio log_pd test (F4) |
| american boundary AVX2 | simd_american_test, american_batch_test ISA-routing, slice_batch_test | Economic (absolute) gates only |
| american greeks AVX2 | american_batch_test MatchesScalarAl, Laned, mask parity | — |
| american_price_batch/resolved | american_batch_test (bit-identical scalar default, ForceAvx2 parity) | — |
| adjoint_greeks | 16 tests: FD parity, mark parity, domain scan, fallback | SIMD variant out of scope |
| parallel_for | parallel_for_test (dynamic exactly-once, exception ×2) | Static: no exception test (would terminate, F3), no determinism test |
