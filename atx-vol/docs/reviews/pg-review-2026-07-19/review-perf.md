# rev-perf report — verbatim

## HOT PATH MAP

1. **De-Am / carry (per expiry)** — `deamer.cpp:resolve_chain_carry` -> up to `max_borrow_pairs` co-terminal pairs x borrow fixed-point (`imply_term_borrow_from_base`, <=64 iters, each iter = 2 American IV inversions with deliberately EMPTY caches) -> each inversion residual = one ALO boundary solve through TLS `AloPricer` (warm across sigma inside one inversion, cold at inversion start). Measured (obs 26289): **300–400 AL boundary solves per slice** here.
2. **Observation build (per strike)** — `calib.cpp:1040-1216`: three-tier IV route (OTM shortcut -> shared-boundary batch proposal -> scalar `american_implied_vol`). Cached tier: residual = `american_price_cached` (Black-76 + one 3D Clenshaw traversal), Newton vega = `american_vega` (two more traversals). Cold tier: `AloPricer` warm solves + unconditional 1–2-solve cold polish (`american_iv.cpp:444-462`). Non-trusted routes pay one ACCURATE-preset cold AL solve per row as audit reprice (`audit_european_equiv_iv` -> `american_price(..., std::nullopt)`, `deamer.cpp:113`, `calib.cpp:1137,1162`).
3. **Correction-cache build (per side, per carry)** — `correction.cpp:470-503`: one boundary solve per (T,sigma) node row via `andersen_lake_{call,put}_slice` (96 solves for production 16x8x12 cache), premium quadrature per strike within row.
4. **Model marks / bands** — `pricer_fitter.cpp:2057-2168` fans chunks over persistent `PricingExecutor`; per strike `session.cpp:evaluate_ladder` routes to `american_price_cached` (cached), sigma-interp cold batch (`ColdPriceBatch` -> `andersen_lake_*_slice_sigma`, 8 solves/side/block), or scalar cold. Band IVs invert through cached `CorrectionBlend` map with model-IV warm seeds.
5. **Greeks** — cached: `american_greeks_first_order` = one fused second-order Clenshaw + `black76_greeks`; cold analytic: `american_greeks_al` = 5 boundary solves (K4 mask trims to 1 for delta-only); FD reference: 7–17 solves.
6. **Cold ALO solve anatomy**: BAW seed ~(nb-1) scalar Newton root-finds x ~16 iters x (2 norm_cdf + 3 exp + log); then 2 JN + <=4 FP sweeps x (nb-1) nodes x nq quad points x (log + 2 norm_cdf + barycentric); then premium quad np x (log + 2 norm_cdf + 2 exp). Transcendental-bound; AVX2 exists but dark.

## FINDINGS

**F1. HIGH — cached IV Newton does 3 full 3D Clenshaw traversals per iteration; 1 suffices.**
`american_iv.cpp:205-230,385-416` -> `american.cpp:2164-2191` (`american_price_cached`: one `cheb_clenshaw3d` value traversal) + `american.cpp:2532-2560` (`american_vega` -> `eval_grad`), `correction.cpp:769-779`: `eval_grad` = `eval()` (full traversal) + `eval_partials(dsigma)` (full partial traversal). Each Newton step evaluates tensor 3x at same point. Fixes: (a) `cached_price_and_vega` entry sharing value between f and df — bit-identical, 3 -> 2; (b) sigma is final collapse axis, so `ClenshawD1` on third collapse yields value + dsigma in ONE traversal — 3 -> ~1.05, not bit-identical (needs economic-parity gate). ~**2–2.5x gain on cached IV inversion inner loop** — most-executed loop in fitter.

**F2. HIGH — carry-solve warm start exists but defaults OFF.**
`deamer.cpp:461-482` (`opts.warm_start_carry` gates `seed_borrow/seed_sc/seed_sp` chaining + `skip_redundant_final`), `deamer.cpp:199-243`. Default-off: every pair restarts from borrow=0 with cold Newton seeds + pays redundant final `deam_pcp_step` (2 extra American solves per pair). Cross-pair seeds documented to change only iteration counts (root unchanged; diagnostics < 1e-8). Expected: **~1.3–2x on carry stage**.

**F3. HIGH — audit reprices are per-row ACCURATE cold solves; batchable via slice-sigma.**
`deamer.cpp:113-114` (`audit_european_equiv_iv` -> ACCURATE preset: 12-node boundary, 24 fp-quad, 48-node premium), invoked per audited row from `calib.cpp:1137,1162`, `deamer.cpp:653-670`. Batch per (expiry, side) through sigma-interp slice route (~8 solves/side, max gap 3.8e-5 per Task 11 gate) or fast preset: O(strikes) accurate solves -> O(8). Caveat: audit is independent-accuracy leg — policy sign-off needed; half-spread budget dwarfs both schemes' error. Gain: removes dominant per-row boundary-solve cost on Cache/Fast audit routes (n_strikes-fold on that term).

**F4. MEDIUM — equal-T ladder cached pricing re-collapses T axis per strike.**
`session.cpp:1916-1917` (per-strike `american_price_cached` in `evaluate_ladder`), `bulk.cpp:162-176`, `correction.cpp:115-172`. Pre-collapse j(T) axis once per (ladder, cache) into (i,k) plane: per-strike cost n_k·n_s + n_s ≈ 200 FMA vs ~1536. **~8x cut on correction-eval term of cached board pricing**. Not bit-identical (summation reorder) — economic-parity gate. Pairs with "resolved cached-run" batch entry mirroring `ResolvedAmericanPriceBatchRequest`, hoisting per-strike `exp(-rT)`/`exp((r-q)T)`/`sqrt(T)` (`american.cpp:2179-2186`).

**F5. MEDIUM — slice/stencil premium quadrature recomputes strike-invariant terms per strike.**
`american.cpp:1093-1115` (`premium_integrand_put`): per node computes `al_boundary_at` (nb-term barycentric, nb divides), `exp(-q·t)`, `exp(-r·t)`, `sigma·sqrt(t)` — none depend on S (only per-strike var in `andersen_lake_call_slice` loop `american.cpp:1953-1966`, greeks bundles' spot stencils `american.cpp:2683-2731`, `SigmaBoundaryInterp::price_internal_put` `boundary_interp.cpp:293-309`). Per-boundary precompute `{b_t, v, dq, dr}[np]` leaves log + 2 norm_cdf per (node, strike). Also `euro_put_sk` re-does exps per strike (`american.cpp:66-73`). **~1.5–2x on per-strike premium cost**. Put slice: boundary rescale linear in K — shareable modulo one multiply; check ULP-parity pins.

**F6. MEDIUM — sweep-invariant barycentric weights not hoisted in boundary sweep inner loop.**
`american.cpp:885-906` (`eqn_b_ND_impl`): `qq_i = wbary[i]/(zc − z[i])` and `den = Σ qq_i` sweep-invariant. Extend P2.2 geometry precompute with per-(node,quad) weight vectors + den: removes NB(=7) divides per quad point per sweep (~4k divides per fast solve). Cost +~28 KB per workspace — gate to retained `AloPricer`/slice paths, not stack bundles (`american_greeks_fd` stacks 7 workspaces). Bit-identical if num/den order preserved. ~10–15% of sweep cost.

**F7. MEDIUM — duplicate log/sqrt in sweep tip and derivative kernels.**
`american.cpp:871-872` (tips via `d_plus`+`d_minus` each recompute `sigma·sqrt(tau)`, `log(z)`), `american.cpp:960-964` (`eqn_b_NDd` recomputes both then own v). ~4 redundant log+sqrt per node per JN sweep, ~3–5% of fast solve. Bit-identical if base ± v/2 same op order. Do with F6.

**F8. MEDIUM — `american_vega` pays full 9-output Black-76 greeks bundle for one number.**
`american.cpp:2547,2578` call `black76_greeks(...).greeks.vega`; `black76_value_and_vega` (`black76.hpp:59`, has AVX2 batch kernel) is cheap variant. Inside every Newton step of every cached inversion (also `american_iv.cpp:67-85`). Combine with F1 fused entry.

**F9. MEDIUM — per-strike IV inversions don't chain warm starts on chain driver and public batch.**
`deamer.cpp:641-642` (`de_americanize_chain` -> `european_equiv_iv` no warm-start; adjacent OTM strikes near-equal IV), `american_iv.cpp:484-507` (`american_implied_vol_batch` serial, no warm_start threading). `calib.cpp:1102` already threads `warm_start_deam`. Worth ~1–3 residuals per inversion on cold/fast routes.

**F10. MEDIUM — vectorization built but dark or unwired.**
- AVX2 boundary batch: complete incl. 4-wide BAW seed (`simd/american_boundary_avx2_kernel.hpp:184-282`), economic-parity green (4.1e-13), but `kShipAvx2Boundary=false` (`simd/american_boundary_batch.cpp:74`) — best-of-3 measured 2.15x/2.90x/2.36x under load; quiet-host proof pending. Biggest dormant win on cold-price batch (gate >= 2.0x).
- AVX2 laned greeks bundle: dark (`kShipAvx2Greeks=false`, `:129`), awaits quiet-window A/B.
- `implied_vol_batch` (`batch.cpp:191-219`) always scalar though `iv_batch_avx2.cpp` exists + validated (obs 27671) — dispatch wire absent, unlike b76/greeks/essvi batches in same file.
- `black76_price_from_lnfk_batch` (`batch.cpp:138-156`) no vector impl — cached-pricer euro-leg kernel, relevant if F4 built.

**F11. LOW — `parallel_for` spawns fresh `std::jthread`s per call.**
`parallel_for.hpp:102-118,150-171`; call sites `session.cpp:835`, `curve_fit.cpp:390,588`, `essvi_calib.cpp:1191`, `opra_batch.cpp:433`, `contract_projection.cpp:531`. Persistent `PricingExecutor` (P1.4) fixed this on valuation side; fit-side fan-outs still pay create/join. ~1–2% per-expiry; repeated small fan-outs (curve-repair loops) worse. Route through executor `run_dynamic` keeps determinism.

**F12. LOW — `bulk.cpp` scalar per-lane engine.**
`bulk.cpp:105-236`: per-lane `resolve_expiry_context` re-resolved within run, scalar `black76_greeks` + `corr->eval_grad` per lane (2 traversals, cf F1), `bulk_aggregate` O(lanes x groups) linear scan (`bulk.cpp:275-289`) where `portfolio_greeks.cpp:43-59` uses hash-map. Only if bulk_price on measured hot path.

**F13. Accepted/no-action (verified deliberate):** 32 KB `tmp_jk` stack reservation (`correction.cpp:26-33,613-614`); unconditional cold polish (`american_iv.cpp:431-462`); scalar `eqn_b` generic kernel (xsimd 6.6x slower, no SVML under clang-cl — `american.cpp:907-919`); sigma-node warm-chain rejection (R-11c, `boundary_interp.cpp:248-270`); QD+ seed not shipped (A6 regressed fast-tier accuracy).

## BENCH STATE

Exists: `american_pricing_bench`, `portfolio_throughput_bench`, `contract_projection_bench`, `simd_*` micro, `fitting_throughput_bench` (incl 16.5k-anchor-comparable American-IV case: 200 inversions/iter), `e2e_hotpath_bench` (M3 attribution row), `iv_shootout_bench` (vs Jaeckel LBR ~180ns/op vendored), `american_shootout_bench`, `al_preset_ladder_bench`, `american_greeks_reuse_bench`, `surface_archive_bench`, `backtest_throughput_bench`. Discipline: per-host/per-ISA baselines, ratio-only gating `compare_baseline.py` (fail >1.10 with CV<=5%), M3 quiet-window protocol, `ATX_VOL_COUNTERS`, solve ledger (WS-V).

Gaps:
1. Rows most needed have no trustworthy baseline: `fit/american_iv/{cold,warm}`, `fit/slice_cold`, `american_greeks/*`, all `simd/iv_invert` + `simd/b76_*` excluded as noisy (CV never <=5% on unpinned i7-1260P); `port/price/greeks/u2688/*` quarantined. F1–F9 changes lack gateable before/after rows.
2. No bench row isolates cached-inversion Newton kernel or correction-cache eval/eval_grad micro cost.
3. No dedicated de-Am/carry-solve row (`resolve_chain_carry`/`imply_term_borrow`) despite 300–400 boundary solves/slice.
4. No correction-cache build-time row (96 solves + DCT per side per carry).
5. AVX2 ship gates blocked on quiet-host measurement this box can't reliably produce — bottleneck is measurement infra, not code.

## Suggested priority order
1. F2 (config flip, validated) + F10 `implied_vol_batch` wiring — near-zero effort.
2. F1 tier (a) bit-identical shared value, then tier (b) fused value+dsigma behind parity gate.
3. F3 audit batching per expiry (policy decision + slice-sigma reuse).
4. F5 + F7 premium/tip hoists (bit-identical, mechanical).
5. F4 resolved cached-ladder batch with T-collapse (largest board-pricing win, parity gate).
6. F6 barycentric-weight geometry extension (retained paths only).
7. Re-baseline excluded IV/greeks bench rows on quiet host to unblock AVX2 ship gates (F10).
