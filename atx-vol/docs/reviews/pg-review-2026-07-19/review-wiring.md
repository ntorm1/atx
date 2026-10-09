# rev-wiring report — verbatim

**Build-system baseline:** every `atx-vol/src/*.cpp` (98 files) listed in `atx-vol/CMakeLists.txt:17-115`; `src/simd/*.cpp` join via GLOB (line 126) with per-file `-mavx2;-mfma` on `*_avx2.cpp` (line 130). `tests/simd_*_test.cpp` auto-glob into test exe (`tests/CMakeLists.txt:139`). **No orphan source files.** `atx-vol/python/` standalone CMake project, real consumer of C++ API (`python/src/bindings/pricing.cpp`, `surface.cpp`).

## FINDINGS

### A. SIMD layer

1. **Entire Taylor P&L SIMD module unwired** — `include/atx/vol/simd/pnl_batch.hpp:78` (`pnl_taylor_explain_batch`), `src/simd/pnl_batch.cpp`, `pnl_batch_avx2.cpp`. TEST-ONLY. Header claims "portfolio pnl-explain hot path" but `PortfolioPricer::pnl_explain_into` (`portfolio_pricer.cpp:1374 scatter_pnl_rows`, `:1492`) computes eight Taylor components scalar, never calls it. Rec: **wire-in** (route `scatter_pnl_rows` through it) or delete.

2. **AVX2 American boundary kernel dark under Auto** — `src/simd/american_boundary_batch.cpp:74` `kShipAvx2Boundary = false` gating `american_put_boundary_batch_avx2`. UNREACHABLE-DISPATCH (deliberate). Auto path `:103` requires flag; only ForceAvx2 (tests/bench) reaches kernel. Comment `:32-73`: speed gate ~1.6–1.87x < 2.0x, "flip only when quiet-host best-of-3 robustly clears 2.0x". Rec: keep (ship-gate policy).

3. **AVX2 laned American Greeks kernel doubly unreachable** — `american_boundary_batch.cpp:129` `kShipAvx2Greeks = false` gating `american_put_greeks_batch_avx2` (incl new `GreekNeeds` mask `american_greeks_avx2.hpp:20`). UNREACHABLE-DISPATCH x2: (a) Auto never selects; (b) only in-library caller chain `american_greeks_batch` (`american_batch.cpp:313,326`) has NO production callers (finding 10). Rec: keep per K3 dark-ship note but wiring debt two levels deep.

4. **AVX2 IV kernel retired from dispatch** — `iv_batch_avx2.cpp:315` `implied_vol_batch_avx2`. UNREACHABLE-DISPATCH (documented R-24). Wrapper `simd::implied_vol_batch` routes scalar unconditionally (`iv_batch.cpp:38-54`); span API `atx::vol::implied_vol_batch` scalar loop (`batch.cpp:191-218`). Kernel reachable only via extern re-decls in tests/bench. Rec: keep-as-API (retained for AVX-512).

5. **Duplicate `simd::` raw-pointer batch wrappers test/bench-only** — `black76_batch.cpp:34,45`, `greeks_batch.cpp:48,60`. TEST-ONLY. Production (`batch.cpp:128,177,243`) calls `detail::*_avx2` kernels directly. Consequence: `black76_value_vega_batch_avx2` per-lane-T variant (`black76_batch_avx2.cpp:255`) and `black76_greeks_batch_soa_avx2` (`greeks_batch_avx2.cpp:309`) reachable ONLY through test-only wrappers. Rec: route wrappers into span API (one dispatch point) or mark kernels parity-test fixtures.

6. **`simd::essvi_backbone_sigma_batch` unwired** — `essvi_batch.cpp:112` + AVX2 `essvi_batch_avx2.cpp:248`. TEST-ONLY (only `tests/simd_essvi_test.cpp:123`). Siblings production-wired. Rec: delete or wire into residual-vs-market-vol fit.

7. **Math-mode seam no production reader** — `cpu.cpp:84-120` (`set_math_mode` etc). TEST-ONLY / API-by-design (P3.3). Rec: keep.

8. **Chebyshev Φ production-dead** — `norm_cdf_cheb.cpp` + `detail::norm_cdf_pd` (`detail/vector_math.hpp:144`). DEAD. All kernels migrated to Cody-erfc (`norm_cdf_erfc_pd2`). Sprint doc A4 says "drop the wing patch, delete the dead Chebyshev table" — not done. Rec: **delete** table + `norm_cdf_pd` + retarget probe.

9. **`vector_math_probe`** — TEST-ONLY by design (accuracy gate). Keep.

### B. American batch layer

10. **`american_price_batch` (book-level) + `american_greeks_batch` never called from production** — `american_batch.cpp:78,257`; header `american_batch.hpp:253,277` + satellites `GreekFieldMask`, `PriceBatchOutput`, `PricingWorkspace.lane_*_view`. TEST-ONLY. Only production-wired entry: `american_price_batch_resolved` (`priced_surface.cpp:968`, `priced_surface_view.cpp:841`). Rec: keep-as-API if P3.4 SoA book pricing roadmap; else deletable.

11. **`american_implied_vol_batch`** — `american_iv.cpp:484`. TEST-ONLY (only `american_iv_test.cpp:346-378`); scalar-backed, no python binding. Rec: delete or bind.

### C. Scalar pricing layer

12. **Vol-derivatives module zero consumers** — `derivatives.hpp` (`var_swap_fair_strike` etc), `derivatives.cpp` (18.7K). TEST-ONLY / API-by-design (ported C v22). Rec: keep-as-API.

13. **Legacy portfolio trio test-only** — `bulk_price` (`bulk.cpp:308`), `price_portfolio` (`portfolio_price.cpp:265`), `aggregate_greeks` (`portfolio_greeks.cpp:25,123`). TEST-ONLY. Production path is `PortfolioPricer` which re-implements skew-adjusted delta via `priced_surface_skew_slope` (`portfolio_pricer.cpp:432`). Note: I6 skew-adjust wiring in `portfolio_greeks.cpp:101-104` only reachable from tests. Rec: keep-as-API or fold.

14. **`CorrectionCache::query` + extrap-policy machinery unused in production** — `correction.hpp:41-76,174-179`; `correction.cpp:904-940`. TEST-ONLY / UNUSED-CONFIG. Production uses raw evaluators under default Clamp only. Rec: keep-as-API; no path observes non-Clamp policy.

15. **CStar family dormant parallel implementation** — `cstar.cpp` (42K), `cstar_calib.cpp` (24.5K). TEST-ONLY (deferred R&D per sprint V3). Rec: keep.

16. **`Parametrization::CStar16M` enum variant dead-end everywhere** — `vol_surface.cpp:240,287,302,317`, `arb.cpp:50`, `projection.cpp:59,79`, `calib_pool.cpp:186`. UNUSED-CONFIG. Nothing constructs it. Rec: keep for exhaustiveness or delete until CStar wired.

17. **`curve_skew_slope`/`vega_slope_per_spot` curve-level variants** — `adjusted_greeks.cpp:13,61`. TEST-ONLY (surface-level companions production-wired). Rec: keep-as-API.

18. **`imply_borrow_european_pcp` (non-`_from_base`) + vestigial `tol`** — `dividend.cpp:115`. TEST-ONLY wrapper + UNUSED-CONFIG param (R-26, `dividend.hpp:146-152`: tol validated positive, never consumed). Rec: keep; drop tol at next API break.

19. **Killed research spikes still in public header** — `american.hpp:586` `detail::al_temporal_warm_probe` (P2.3), `:630` `al_implicit_diff_put_greeks` (P2.4), implemented in `american.cpp`. TEST-ONLY, kill-decided (Task 12 measurement-based kill). Rec: **delete** or move to bench-only TU. Companion seams `andersen_lake_generic_kernel` (:541), `al_boundary_jn_sweeps_to_converge` (:552), `andersen_lake_seeded` (:561) documented test/bench seams — keep.

20. **`implied_vol_traced`** — `implied_vol.cpp:355`. TEST-ONLY by design. No action. Both IV seed strategies live: Choi-L3 primary (`:281`), SR-2017 + BS/|k| fallbacks reachable on degenerate edge — no unreachable seed strategy.

21. **Span batch API partially unconsumed** — `black76_price_from_lnfk_batch` (`batch.cpp:138`), `black76_value_and_vega_batch` (`:158`), `black76_greeks_batch` span (`:221`). TEST-ONLY. Contrast: `black76_price_batch`, `implied_vol_batch`, `essvi_w_batch` bound in python. Rec: keep-as-API or bind remaining three.

### D. Null results (clean)
- No TODO/FIXME/stub hits in-scope.
- No undefined declarations (checked 24 headers).
- vol_time fully wired; VolTime set in production drivers.
- deamer/dividend/boundary_interp/american_iv/adjoint_greeks wired (adjoint via `PriceOptions::adjoint_greeks` A/B flag, off-by-default but reachable, `portfolio_pricer.cpp:727,1037`).
- `AmericanMethod::Baw` reachable.

### Top recommendations by impact
1. Wire or delete pnl_batch SIMD module (1) — biggest "claimed hot path vs reality" gap.
2. Execute planned A4 Chebyshev deletion (8).
3. Delete two killed research spikes from american.hpp (19).
4. Decide fate of book-level `american_price_batch`/`american_greeks_batch` pair (10) — only consumer of `GreekFieldMask`, second gate on K3 laned-greeks kernel.
