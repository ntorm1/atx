# atx-vol production-target sprint — 2026-08-23

Frozen base: `654a9206c5f9de045e008783921c6e5d0ad73eda` (main, clean tree).
Run id: `atxvol-prod-sprint-20260823`.

Priority order, as set by the request: **(1) correctness, (2) performance on par
with state of the art, (3) feature breadth for trading American equity options.**

Six research and review agents swept the tree and the external literature before
anything was planned. Every defect below was read in the source; every target
carries a published or in-repo number.

## Where the library actually stands

### Accuracy — the SpiderRock parity north star

Mode A prices at the vendor's OWN `uPrc`/`rate`/`sdiv`/`ddiv` and `srVol`, so it
isolates the pricer from the fit (`bench/oracle/CONVENTIONS.md`).

| metric | symmetric | standard rel | charter target |
|---|---:|---:|---|
| price MAE | 8.66 ticks | 8.66 | <= 1 tick |
| delta | 0.0022 | 0.0022 | <= 0.01 — **the only passing metric** |
| gamma | 0.0179 | 0.0318 | <= 0.01 |
| theta | 0.0639 | 7.3116 | <= 0.01 |
| vega | 0.0240 | 0.0739 | <= 0.01 |
| rho | 0.0217 | 0.6926 | <= 0.01 |
| phi | 0.0245 | 0.9976 | <= 0.01 |
| volga | 0.0831 | 0.1875 | <= 0.01 |
| vanna | 0.0277 | 0.0341 | <= 0.01 |
| delta-decay | 0.0903 | 0.8085 | <= 0.01 |

`mode_a_vol_mae = 0` is an identity (Mode A prices AT `srVol`) and is never cited
as accuracy.

`docs/oracle/NORTHSTAR.md` is **stale**: it still publishes the escrow-era
376-tick price MAE and a "Next" section describing a QuantLib cross-check as in
flight. `CONVENTIONS.md` at `54024add` supersedes it at 8.66. L7 fixes it.

### Performance — measured here, against published SOTA

| | number | source |
|---|---|---|
| atx band audit, per contract | **32.6 us** | `docs/LEDGER.md`, 1,382,283 contracts / 45.1 s serial |
| atx `fast` preset (7,16,16,4sw) — **shipped default** | **46.5 us/op**, 9.7e-4 max abs err | `docs/al-preset-ladder.md` §4 |
| atx `ql_fast` preset (7,8,32,2sw) — in-tree, unused | **26.1 us/op**, 1.0e-3 max abs err | same |
| atx AVX2 boundary batch vs scalar | **1.48x** / 1.76x on the gate | `bench/baselines/...-avx2-american-shootout.json` |
| atx full 8 greeks | **750 us/option** (1,334/s) | `bench/baselines/...-sse2-portfolio.json` |
| atx European B76 IV | 1,082,851/s | `...-avx2-iv-shootout.json` |
| Jaeckel LBR, **same host, same grid** | **1,632,037/s (1.51x ours)** | vendored `bench/thirdparty/lets_be_rational` |
| ALO published, individual | 25.6 us/op (39,040 opt/s), 4.1e-5 RMSE | Healy, arXiv:2109.15157 |
| ALO published, **batched over spots** | **5.6 us/op** (179,705 opt/s), same RMSE | same |
| AAD implied greeks cost | ~3-4x one price | Giles-Glasserman NA-05-15 |

Three readings follow, and they are not the same story:

1. **Per-option American pricing is near the frontier already — the gap is
   configuration, not algorithm.** `ql_fast` is **1.81x cheaper than the shipped
   `fast` at statistically equal accuracy** and is already admitted to
   `al_fp_specialized` (`american.cpp:606-614`) and already exposed as
   `al_bulk_opts()`. It is simply not the default.
2. **American greeks are 3x to 9.6x off the AAD-implied cost model — the single
   largest performance gap in the library.** It still pays 5 boundary solves +
   13 price evaluations per bundle by finite difference
   (`american_greeks_avx2.cpp:149-197`).
3. **The AVX2 boundary kernel is leaving a documented 20-30% on the table**
   because the sweep-invariant geometry hoist the *scalar* path already has
   (`american.cpp:1032-1068`, `american_boundary.hpp:160-172`) was never ported
   to it — `al_init_put_boundary` explicitly skips `al_bind_geometry` and the
   kernel recomputes it inline, per node, per quadrature point, per sweep. Every
   published AVX2-vs-scalar ratio in this repo therefore understates the
   achievable vector win.

Context worth keeping: every documented vendor — Cboe TOPS, OptionMetrics,
ORATS, FactSet, IVolatility, SpiderRock — still prices American equity options
on a **binomial tree** (50 to 1000 steps), which is O(dt) accurate under discrete
cash dividends and loses recombination. atx already runs Andersen-Lake. The
method is not the problem; the defects below are.

## Phase 1 — correctness and breadth (seven parallel lanes)

Each lane owns a disjoint file set, so they integrate without conflict. Every
lane leases a pool worktree at the frozen base and follows `.agents/cpp/agent.md`.

### L1 — American solver core (`pool-16`)
Owns `src/pricing/american.cpp`, `american_boundary*.hpp`, `boundary_interp.cpp`,
`rates_curve.cpp`.

1. **The boundary solve reports `Ok` with no convergence guarantee.**
   `american.cpp:1691-1716` runs a fixed `n_iter_jn + n_iter_fp` budget, assigns
   `resid = s.max_dy`, then `return AlSolveStatus::Ok` — **`resid` is
   discarded**, and `sch.tol` is only an early-exit shortcut. The tree already
   knows this: `boundary_interp.cpp:261` says the solve "returns
   `AlSolveStatus::Ok` carrying an under-converged boundary, silently". Same
   structure at `:1746`, `:1821`, `:1971`, `:2591-2602`. Every accuracy claim in
   the library — price, greeks, IV, correction-cache samples — rides on this.
2. **`YieldCurve::zero(T)` diverges below the first pillar.**
   `rates_curve.cpp:119-124` clamps the *discount factor* flat, so
   `zero(T) = r0*t0/T`. With the suite's own pillars (t0 = 1/365.25, r0 = 4.05%):
   1 day -> 4.05%, 12 h -> 8.1%, 1 h -> **97.2%**, 5 min -> **1108.8%**. Every
   0DTE/intraday path through `MarketEnv::rate_at` inherits it, and no test calls
   `zero()` below the first pillar. The fix is flat-*rate* on the short end,
   which is what `rates_curve.hpp:74-76` already reads as.
3. **BAW critical-price brackets are hard-coded** (`american.cpp:149-150`,
   `:193`). `K=100, r=0.05, q=0.001, sigma=0.2, T=1` puts the true critical price
   past the `K*50` ceiling, so `american_price(..., Baw)` returns `Unavailable`
   for low-yield calls. Fail-closed, but a hard-coded 50x is not a domain.
4. `scheme_from_opts` silent-ignore asymmetry (`american.cpp:733-758`):
   `n_collocation < 6` keeps the *more expensive* default while `n_quadrature < 8`
   floors to the cheapest. The stated rule — a caller asking for cheaper must not
   get more expensive — was applied to one axis only.
5. **Promote `ql_fast` to the marks / de-Am / cache-sampling default.**
   MEASURED 1.81x (46.5 -> 26.1 us/op) at statistically equal accuracy
   (9.7e-4 vs 1.0e-3). `al-preset-ladder.md:206-212` already names it for exactly
   these tiers. It explicitly does **not** clear the greeks tier — FD greeks
   divide price error by the bump, so greeks stay on `fast_p32` (1.3e-4).

### L2 — Discrete dividends into production (`pool-18`)
Owns `src/pricing/american_discrete_div.cpp`, `dividend.cpp`, and the pricing
route inside `src/fitting/pricer_fitter.cpp`.

1. **The measured 20x win is implemented and unwired.** `american.hpp:704-710`
   records: on 9,155 SPY rows with `ddiv > 0`, escrowing costs **142.60 ticks**
   of price MAE where the Vellekoop-Nieuwenhuis spliced lattice costs **6.96**
   (2.29 on true ex-dates); on the 5,202 `ddiv == 0` rows the lattice sits at
   **0.09**, which is what proves the rate/vol/year-fraction conventions were
   never the problem. `LEDGER.md` adds that escrowing "underprices calls by 78
   ticks and overprices puts by 199", with ITM puts **587.65 -> 1.14**. Every
   caller of `american_discrete_div_*` is under `tools/oracle_*`; there are
   **zero consumers in `src/fitting`, `src/backtest`, `src/storage`**. Every
   shipped mark still escrows.
2. **Cum-dividend exercise is never tested at an ex-step.**
   `american_discrete_div.cpp:128-130` applies `max(cont, exercise(post))` — the
   POST-dividend level only — and `:245-252` runs the plain American max only
   when `amount == 0`. The correct condition is
   `V(S_k^-) = max( sgn*(S_k - K), cont(S_k - D) )`. This is not a modelling
   preference: the classical result, re-confirmed numerically by Itkin
   (arXiv:2510.18159 §6.4), is that an American **call** exercises early *only*
   immediately before an ex-date, where the boundary is vertical. The code
   understates exercise value by exactly `D` on every node where exercise binds.
   It is documented as intentional because admitting it "made agreement with the
   vendor mark strictly worse" — that is evidence about the vendor, not about
   the option.
3. **`splice_dividend` flat-clamps continuation at the grid bottom.**
   `american_discrete_div.cpp:117` `if (post <= lo) cont = value[0];` fires on
   the low nodes at every splice with `amount > 0`. The American put's exercise
   floor rescues it; the **European** put — which is what the validating
   put-call-parity identities use — is not rescued.
   `discrete_div_american_test.cpp:434-439` pins `96.146830494183135` where the
   exact answer is `K*e^{-rT} = 97.044553355`; the 0.897723 deficit equals
   `level[0]*e^{-r*0.900332}` to the last digit. **The pinned test value is the
   artifact.**
4. `dP/dD_i` collapses to the European forward-delta chain
   (`american.cpp:3788-3801`), so two schedules with equal PV and different
   timing get identical dividend sensitivities.

### L3 — Implied-vol inversion (`pool-19`)
Owns `src/pricing/implied_vol.cpp`, `american_iv.cpp`.

1. **`american_implied_vol` returns `Ok(0.005)` — a SUCCESS — for an
   unidentifiable quote.** `american_iv.cpp:232` screens only *immediate*
   intrinsic. The true zero-vol American bound is
   `max(immediate intrinsic, df*(F-K)^+)`, and the forward leg is the larger one
   for a call OTM on spot but ITM on the forward — the everyday case on
   hard-to-borrow names. Mode B refuses **35,477** rows upstream rather than
   fixing the inverter.
2. **European IV has no `kIvMax` branch.** `implied_vol.cpp:373` clamps sigma to
   10.0; `:378` tests the *pre-clamp* step. Sigma sits motionless, burns all 16
   iterations, and returns `Err(Unavailable, "exhausted iterations")` — the wrong
   error class. The mirror-image floor bug was found and fixed at `:336-353`;
   the ceiling was left.
3. **Newton differentiates the wrong function.** `american_iv.cpp:463,494` ->
   `american.cpp:3440` is the pure **Black-76 European** vega while solving the
   American map. `LEDGER.md` measured the consequence: it "understeps 2-3x deep
   ITM because the proxy overstates the true AL slope there."
4. Doc drift, load-bearing: `implied_vol.hpp:5-14` still advertises a
   "Stefanica-Radoicic seed + 1-2 Halley iterations" and a seed "uniformly within
   ~2% of the true sigma", refuted by the `.cpp` itself at `:197-199`
   ("L3 verified 18-32% low at |ln F/K| ~ 0.10-0.14").

### L4 — SIMD undefined behaviour, NaN safety, and vector-math cost (`pool-20`)
Owns `src/simd/**`, `src/pricing/scalar_erfc.hpp`.

Correctness first, then the two bit-safe performance items.

1. **`exp_pd(NaN)` returns a finite `-2.0`.** `vector_math.hpp:136`: `underflow`
   is `_CMP_LT_OQ`, so it is false for NaN; `max_pd(NaN, kExpLo)` yields
   `kExpLo`; `N = -1075` makes `biased = -52`, and the shift reconstructs `-2.0`.
   Being finite, it is **invisible to every `nonfinite_mask` guard downstream**.
   This is the worst failure mode in the sweep — a silently wrong number, not a
   NaN.
2. **AVX2 `cheb_eval` divides by zero at an exact node hit.**
   `american_boundary_avx2_kernel.hpp:293` and `:518` have no `dz == 0` guard,
   unlike every scalar counterpart (`american.cpp:544`, `boundary_interp.cpp:54`).
   `clamp01(zz)` saturates to exactly +/-1.0 and the Chebyshev-Lobatto grid
   contains +/-1.0, so `dz == 0` is reachable and yields a NaN boundary.
3. `scalar_erfc.hpp:163` casts a NaN to `int` (UB) — no NaN screen, and every
   guard is an ordered compare that falls through.
4. `essvi_batch_avx2.cpp` divides by `slice.T` on four paths while
   `slice_vector_admissible` validates theta/phi/rho but **not T**.
5. `american_greeks_avx2.cpp:166,175,186,193` (and `:376-403`): only the *base*
   boundary solve checks `ref < 0`. The four bump-state solves do not, and a
   failing solve returns without writing `out`, so the sigma+/- and r+/- stencils
   price against the **stale base boundary**. Correctness rests today on an
   incidental `lane_ok &&` chain.
6. `iv_batch_avx2.cpp:385` omits the `|ln(F/K)| >= 708` escape that both sibling
   kernels carry and document as required; `:380` uses an *ordered* compare for
   the vega floor, so a NaN vega reads as well-conditioned.
7. `black76`/`greeks`/`iv` batch kernels use two different conventions for the
   log argument (patched safe copies vs raw). Unobservable today, one mask
   narrowing from breaking exactly one of the three.
8. **Perf, structurally exact:** `erfc_nonneg_pd` (`vector_math.hpp:200-266`)
   costs **five `_mm256_div_pd` per call**, ten per Phi pair, on every lane.
   Merging regions 1 and 2 into one `(N, D)` blend before the division removes
   one division with *identical operands and operator* — no re-gating needed.
9. **Perf, no numerical change:** there is **no `restrict`/`__restrict` anywhere
   in `src/simd/` or `src/pricing/`**. `pnl_batch.cpp:15-45` alone takes 13 input
   and 9 output pointers with no non-aliasing promise, and
   `docs/simd_fastpath.md:82-86` reports clang does not auto-vectorize exactly
   that loop. Aliasing is already an API precondition; ESTIMATED 1.2-1.6x on the
   scalar SoA fallbacks.

### L5 — Vol derivatives and greeks correctness (`pool-21`)
Owns `src/pricing/derivatives.cpp`, `deriv_analytic_greeks.hpp`,
`adjoint_greeks.cpp`, `adjusted_greeks.cpp`, `docs/adjoint_greeks_design.md`.

1. **CRITICAL — unbounded double-to-`size_t` cast is undefined behaviour.**
   `derivatives.cpp:1676` casts `ceil(intervals)` with no pre-cast bound, and
   `intervals = (n_nodes-1)*kh/floor_half` is unbounded above. The identical
   shape is guarded in `strip_grid.hpp:211-216`, whose comment states it
   outright: *"casting an out-of-range double to `size_t` is undefined
   behaviour, not a saturating truncation."* The guard was written once and never
   mirrored. Short of UB it silently builds a multi-million-node strip and trips
   past `kMaxStripNodes`, which also disables the batched path (~40x slowdown).
2. **`deriv_analytic_greeks` NaN-poisons gamma**, contradicting its own contract
   at `:293-296` ("never propagates a NaN into the total"): `:545-546` checks the
   four *shifted* surface reads for finiteness but not the **center** read, and
   `sig_curv`'s 5-point stencil contains `-30.0*sigma`.
3. **`european_greeks_adjoint`'s sigma-to-0 branch returns the bare spot
   intrinsic** (`adjoint_greeks.cpp:436-446`): `max(K-S,0)`, `delta = +/-1`. The
   correct European limit is `df*max(K - S*e^{(r-q)T}, 0)` with
   `delta = -df*e^{(r-q)T}`. This is verbatim the bug already fixed in
   `american.cpp`, with the fix documented at `american.cpp:3195-3198`.
4. `adjusted_greeks.cpp:15,32` computes `sqrt(wk/T)` **before** the `T > 0` guard
   that protects it; `:68` lets a NaN `vega_slope` silently convert a good delta
   into a NaN with no error channel.
5. `adjoint_greeks.cpp:387` hard-codes an absolute `hvol = 5e-3` against a regime
   guard admitting `sigma > 1e-6`, while `american_greeks_fd` and
   `american_greeks_al` both already handle this with `if (s-h <= 0) h = 0.5*s`.
   Vanna's bump is 10x vega's, so the returned vanna is not the sigma-derivative
   of the returned vega at any consistent order.
6. **Honesty fix.** `adjoint_greeks.hpp:5-7,181` and
   `docs/adjoint_greeks_design.md:133-137` describe reverse-mode AAD with a
   Christianson fixed-point adjoint. The shipped American path
   (`adjoint_greeks.cpp:223-252`) is **forward-mode tangent propagation with both
   Jacobian-vector products finite-differenced**, called once per parameter;
   `grep -c "lambda|jacob|transpose"` on that file returns 0. The code is not
   wrong, the documentation is. This lane moves the documentation and records the
   real gap.

### L6 — Fitting and surface calibration (`pool-22`)
Owns `src/fitting/**`.

0. **The default market mark for SPY/QQQ/IWM is raw quote interpolation with no
   arbitrage check of any kind, and it publishes `Healthy`.** `FitPreset::Hft`
   (`session.cpp:1039-1052`) pins `LinearVariance`, sets
   `enforce_calendar_floor=false`, `calendar_repair=None`, `score_parity=false`.
   `fit_slice_curve`'s LinearVariance branch (`vol_curve.cpp:685-751`) sorts the
   observations and returns the curve — no butterfly gate, no smoothing, no
   convexity constraint. Then `curve_selector.cpp:117-120` **asserts** the
   falsehood:
   `case VolCurveKind::LinearVariance: return 0u; // by-construction arb-free
   (LinearVariance g-check out of scope)`.
   Piecewise-linear total variance is only C0; `w''` is a measure with a
   **negative Dirac at every concave kink**, guaranteed at both outer nodes where
   the wings go flat (`vol_curve.cpp:240-245`). The parenthetical concedes no
   check was done; the returned `0` makes the selector read it as clean. This is
   the route for `IndexEtfUltraLiquid` — **SPY, QQQ, IWM** — and for dense
   mega-cap event boards (`fit_policy.cpp:51-63`). By the codebase's own
   measurement, *"~1/4 of a raw penny-quote SPY board is locally
   butterfly-violating"* (`essvi_calib.cpp:763-766`); interpolating the quotes
   reproduces every one of them. The risk oracle never sees it — a LinearVariance
   risk config is refused outright (`pricer_fitter.cpp:1504-1506`) and rewritten
   to ConvexDense on the risk ladder.
1. `spline_curve.cpp:55` divides by `h[t]` with no zero/pivot check, and
   `fit_spline_vol_slice` never validates that `opts.grid` is strictly
   increasing — it checks only `size() >= 4` (`:395`). `SplineVolParams::z` is
   *documented* strictly increasing (`spline_curve.hpp:97`) and nothing enforces
   it. One duplicated knot makes every served IV NaN.
2. `svi_calib.cpp:435` — `nm_search`'s winning-vertex selection is not NaN-safe
   (`if (f[1] < f[best])` is false when `f[best]` is NaN), breaking its own
   stated contract at `:415` that the result is non-finite *iff* every vertex was
   unusable. A single NaN vertex discards a slice that had good candidates.
3. `vol_surface.cpp:70` vs `resid_basis.hpp:36` — duplicated-and-diverged clamp
   (`clamp(n,1,16)` vs `clamp(n,4,16)`), so a surface with `resid_n_basis` in
   {1,2,3} serves a different total variance than was calibrated.
4. `dense_slice.cpp:432,452,461` — divides by strike-ladder gaps with no
   strict-increase guard, from a `noexcept` pricer. `ConvexDenseCurve::w`
   (`vol_curve.cpp:208`) already has the fallback pattern to copy.
5. `SplineFitOpts::grid` is a `std::span` **member** (`spline_curve.hpp:122`), so
   `SplineFitOpts{.grid = make_grid()}` with a temporary stores a dangling span.
   House style §1 forbids storing a non-owning view without a lifetime guarantee.

### L7 — Breadth, packaging, and the honest dashboard (`pool-23`)
Owns `cmake/atx-vol-install.cmake`, `backtest/portfolio_pricer.hpp` +
`src/backtest/portfolio_pricer.cpp`, `docs/oracle/NORTHSTAR.md`.

1. **Packaging defect.** `cmake/atx-vol-install.cmake:165` installs only
   `include/atx/vol/api/`. `include/atx/vol/alpha/` (7 headers) compiles in-tree
   via `BUILD_INTERFACE` and is **unreachable downstream** — a hard compile error
   for any consumer that includes `atx/vol/alpha/registry.hpp`. `vol.hpp`'s tier
   manifest never mentions `alpha/` at all.
2. **No vega ladder by strike or delta.** `RiskBucketKey` offers only
   `ByUnderlier` and `ByExpiry` (`portfolio_pricer.hpp:605-608`), so a desk
   cannot answer "where is my vega in the smile" from this library. This is the
   most-requested missing risk cut for an equity vol book, and it is a third
   enumerator plus a keying function over data `Portfolio` already carries.
3. `abs_vega` is declared per risk bucket and **never populated** by
   `reduce_risk_buckets` — it returns NaN (`portfolio_pricer.hpp:533-536`).
4. **Fixture-gated tests report PASS when they did not run.**
   `tests/CMakeLists.txt:579` sets `SKIP_RETURN_CODE` for a **pytest** target
   only; `gtest_discover_tests` never gets it. The real-board fixtures are absent
   and gitignored (`find data -name "*.parquet"` returns zero; root `.gitignore`
   excludes `/data/`), and there are **161 `GTEST_SKIP` sites across 46 files**.
   So a fixture-gated C++ test exits 0 and ctest reports **PASS**. Every
   end-to-end real-board fit-quality and no-arb gate is green-by-vacuity in a
   clean checkout. `README.md:422-426` is honest that fixtures are optional; the
   ctest wiring is not. This is the highest-leverage item on the lane — every
   other lane in this sprint is relying on ctest telling the truth.
5. `docs/oracle/NORTHSTAR.md` publishes superseded numbers (see above).

## Phase 2 — performance, after Phase 1 integrates

Deferred deliberately: every item below touches files Phase 1 is editing, and
correctness is the stated first priority. Fresh leases from the integrated SHA.

1. **Hoist the sweep-invariant geometry into the AVX2 boundary kernel.** The
   scalar path already does this (`american.cpp:1032-1068`, tables at
   `american_boundary.hpp:160-172`, with the in-source note "exp(r*u), exp(q*u)
   — now paid ONCE per solve"). The AVX2 kernel is explicitly denied it
   (`american_boundary.hpp:220-225`) and recomputes `zz`, `cheb_eval`, `vq` and
   two `exp_pd` per (node, quadrature point, sweep) — **none of which depend on
   `Y[]`**. Removes 8 of 21 divisions, 2 of 3 sqrts, 2 of 5 exps per inner
   evaluation. ESTIMATED 20-30%; bit-identical if the `qq_j` bits and
   accumulation order are preserved, which the scalar hoist already proved.
2. **Reuse the premium-quadrature boundary evaluation across the greeks spot
   stencils.** `american_greeks_avx2.cpp:156-197` calls `price_put_pack_avx2` 13x
   per pack; inside, `american_boundary_avx2_kernel.hpp:552-586` recomputes
   `b_t`, `dq`, `dr`, `v` per quadrature point — all spot-independent. Only
   `ratio` varies with the stencil. **Bit-identical.** ESTIMATED 15-25% off the
   laned greeks bundle, which is the library's largest SOTA gap.
3. **Template the AVX2 kernel on `<NB, NQ, NP>`**, mirroring the scalar
   `al_fp_specialized` (`american.cpp:606-614`). Runtime trip counts today mean
   nothing unrolls. ESTIMATED 5-12%; bit-identical.
4. **Pack the still-scalar independent boundary solves.**
   `boundary_interp.cpp:276-281` runs up to 16 cold solves at identical
   `(K,T,r,q)` varying only sigma; `correction.cpp:530-560` runs `n_T x n_s`
   independent slices. Both are batch-shaped with no sequencing constraint. Read
   `boundary_interp.cpp:261` first — a previous warm-chaining attempt silently
   produced under-converged boundaries at 1.3e-4.
5. **Give the fitter the persistent pool.** `src/core/parallel_for.hpp:242` and
   `:307,422,492` create and join `std::jthread`s per fan-out; MEASURED ~124 us
   pooled dispatch vs ~1.4 ms create/join. Fitting is **95.35%** of the
   attribution pipeline and scales at only 19% efficiency at 16 threads.

## Explicitly out of scope, and why

- **Do not copy `qdfp.{cpp,hpp}` / `qdplus.{cpp,hpp}`.** These are verbatim
  QuantLib sources (© 2022 Klaus Spanderen) sitting untracked at the repo root,
  including `<ql/...>` headers that do not exist in this tree. They are useful as
  a specification diff — they pin the published node counts
  `fastScheme(7,2,7,27)` and `accurateScheme(25,5,13,1e-8)` — and nothing more.
  Any Andersen-Lake work is implemented from the papers directly.
- **No neural pricer.** The best published American surrogate reaches MAE < 0.01
  at a few microseconds per option — roughly ALO's speed at ~250x worse accuracy
  on the 1-D problem. No credible public evidence exists that any market maker
  prices vanilla American equity options with a network.
- **No full test-suite runs.** Per the request and `atx-vol/CLAUDE.md`, lanes use
  `check <file>` -> `build <owning-target>` -> anchored `-Ctest -R <Suite>` only.

## Recorded, not scheduled — the fitting audit's remaining findings

The calibration sweep found more than one lane can carry. These are real, cited,
and deliberately unscheduled; they belong to the next sprint.

- **Under `Hft` every carry robustness statistic is identically zero by
  construction.** `max_borrow_pairs=1` gives `sel.k = 1` (`deamer.cpp:570-574`);
  the leave-one-out loop runs only `if (diag.n_retained > 1)` (`deamer.cpp:708`);
  dispersion over one pair is 0; `confidence_half_width = fmax(0,0) = 0`. The
  forward on the most-traded boards rests on a **single** put-call pair with no
  possible cross-check. At universe scale, 17,725 of 57,281 solved expiries
  (30.9%) return `n_retained == 1` and "look PERFECT on every robustness
  statistic the fitter computes" (`LEDGER.md:319`).
- **A failed fit returns `Ok()` and the previous surface keeps serving.**
  `pricer_fitter.cpp:1825,2103,1370`. `value_snapshot` (`:2659-2688`) checks only
  `instance_id`/`uid`, and `OptionChain::update_quotes` (`src/core/chain.cpp:201-240`)
  mutates quotes in place, bumping only `quote_revision_`. In a live tick loop:
  new quotes, fit fails, `Ok()`, and today's quotes get priced off yesterday's
  surface. `SurfaceHealth::surface_age_ns` (`surface_policy.hpp:182`) is
  **declared and never assigned anywhere**.
- **No fit-quality floor is armed by default.**
  `FitAdmissionPolicy::min_worst_frac_within_bidask{0.0}` (`fit_policy.hpp:163`)
  makes the predicate `worst < 0.0` unsatisfiable (`fit_policy.cpp:173-178`), and
  `risk_admission_policy()` leaves it at 0.0. **RMSE is never compared to a
  tolerance in any admission path**, and the independent geometry oracle never
  touches a bid or an ask. An arb-free surface that reprices nothing inside
  bid/ask publishes as `Healthy`.
- **The exact-over-R calendar certificate is unreachable in production.**
  `SviCrossingSlice::from_essvi_backbone` returns `nullopt` when
  `resid_scale > 0` (`arb.cpp:434-437`) — which is exactly the base and
  LiquidSingleName profiles — and the shipped substitute is a **25-point scan
  over k in [-3, 3]** (`session.cpp:2391-2395`), spacing 0.25 in log-moneyness.
  A crossing narrower than that is invisible. `certified_over_r()`, the honest
  "did I actually decide both tails" flag, has **zero production callers**.
- **SVI's IRLS is the M-estimator of neither objective**: it minimises a
  total-variance SSE (`svi_calib.cpp:150-154`) while computing robust weights
  from vol-space residuals (`:926-968`) and testing convergence on a third
  quantity. The two spaces differ by `1/(2*sigma*T)`, which varies ~4x across a
  board's tenors.
- **ConvexDense clamps a fitted price into Black's no-arb box before inverting**
  (`dense_slice_price.hpp:37-54`), laundering a sub-intrinsic price into a
  near-zero vol — "invisible to any w-space check" (`arb.hpp:367-375`).
- **The certification band is tenor-blind.** A fixed `|k| <= 0.5`
  (`strip_grid.hpp:71`) is ~±11 standard deviations for a 30-day 15-vol slice and
  **~±1.4** for a 2-year 25-vol slice. Long-dated surfaces are certified over
  almost nothing.
- **eSSVI never sets `termination`** — every eSSVI fit reports `Unknown`
  (`essvi_calib.cpp:1404-1410`) — and the chain export has no health/state/curve
  column, with its `error` column never assigned (`tools/chain_export.hpp:673`).
- **Machinery built, measured, and never called**: Davis-Hobson model-independent
  quote feasibility (`quote_feasibility.cpp`, Math. Finance 2007 Thm 3.1) whose
  own header says it should sit in front of every slice fit; the ConvexDense
  bid-ask **interval** loss (`dense_slice.cpp:933+`), so every shipped fit targets
  the mid on quotes whose median relative spread is 0.348; and
  `confidence_half_width`, computed with a proper Kish effective-n
  (`deamer.cpp:723,958`) and consulted by no branch.
- **The vol-time clock is implemented, fitted to the vendor's own `years`
  column, and used by exactly one config.** `Calendar365` is the default and is
  really a Julian 365.25-day year — a deliberate ~0.07% T bias, about 3 bp of IV
  (`vol_time.hpp:392-407`). The hybrid clock (1638 trading h / 7122 non-trading h,
  alpha=0.7, a trading hour worth **10.1x** an overnight hour) is used only by
  `earnings_repro_config.hpp:160`. `LEDGER.md:306` proves the clock is the
  short-tenor bias driver by weekday sign-flip: Monday tau-ratio 0.894 predicts
  -5.46% against -6.41% observed; Thursday 1.082 predicts +4.00% against +3.97%.
  "NOTHING ELSE makes a bias flip sign between two expiries three days apart."
- **Events are eSSVI-only.** `event_vol.hpp` and `earnings_term_fit.hpp` are
  production-wired and correct in form, but `SessionInputs::events` defaults to
  null and `session.hpp:198-201` states that a session built with a polymorphic
  curve override (ConvexDense / Svi) **never consults `events`** — so the dense
  production path has no event handling at all.

## Known leads recorded but not scheduled

- **De-Americanization bias is rate-driven, and the rate regime has changed.**
  Burkovska et al. (arXiv:1611.06181) found the error is dominated by the
  interest-rate level, with worst-case downstream barrier errors of ~50% (CEV,
  Heston); their benign empirical result held *only* because it was a zero-rate,
  zero-dividend name. They explicitly left dividends to future research and
  expect them to intensify the error. atx de-Americanizes ~1.34M times per board
  at r ~ 4-5% with dividends. Measuring this is a one-day synthetic experiment
  and it is not yet done.
- **Pre-earnings smiles are frequently concave** (Alexiou, Goyal, Kostakis &
  Rompolis, *Review of Finance* 29(4), 2025) because the anticipated jump makes
  the risk-neutral density bimodal. No SVI-family form can represent a concave
  smile. The correct shape is to fit the *ex-event* smile with SVI and carry the
  event as a separate jump-variance layer.
- **`deserialize/convexdense/mmap_open` is 209.1 ms** against 23.0 us for eSSVI —
  the "zero-copy view" path is ~9,000x slower for ConvexDense and is
  indistinguishable from `open_reconstruct_all`.
- **`port/price/greeks/u64` regresses from t4 to t8** (25,384 -> 63,211 us) — a
  scheduling cliff. Its `u2688` sibling was quarantined for 40% CV.
