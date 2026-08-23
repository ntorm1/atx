# atx-vol production-target sprint — 2026-08-23

Frozen base: `654a9206c5f9de045e008783921c6e5d0ad73eda` (main, clean tree).
Run id: `atxvol-prod-sprint-20260823`.

**Base state, stated precisely:** the tree BUILDS clean at the frozen base
(`atx-vol-tests` is a ninja no-op). No full test run was made at the base, and
one pre-existing failure is now known: `VolUmbrella.NoFixturePathResolvedOutsideTheSharedResolver`
fails at `654a9206` on three absolute path literals introduced by `e79f2b8d`
(2026-08-18) — `tests/oracle_bench_test.cpp:711`, `:1399`, and
`tools/oracle_bench_main.cpp:86`. The first two are deliberately-nonexistent data
in negative tests and belong in `kPathLiteralExempt`; the third is a real CLI
default. It is not this sprint's regression and is not this sprint's to fix — the
oracle lanes own that file.

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
| atx `fast` preset (7,16,16,4sw) — **shipped default** | 46.7 us/op *(provisional)*, 9.7e-4 max abs err | `docs/al-preset-ladder.md` §4 |
| atx `ql_fast` preset (7,8,32,2sw) | 25.8 us/op *(provisional)* = **1.81x fast**, 1.0e-3 max abs err | same |
| atx AVX2 boundary batch vs scalar | **1.48x** / 1.76x on the gate | `bench/baselines/...-avx2-american-shootout.json` |
| atx full 8 greeks | **750 us/option** (1,334/s) | `bench/baselines/...-sse2-portfolio.json` |
| atx European B76 IV | 1,082,851/s | `...-avx2-iv-shootout.json` |
| Jaeckel LBR, **same host, same grid** | **1,632,037/s (1.51x ours)** | vendored `bench/thirdparty/lets_be_rational` |
| ALO published, individual | 25.6 us/op (39,040 opt/s), 4.1e-5 RMSE | Healy, arXiv:2109.15157 |
| ALO published, **batched over spots** | **5.6 us/op** (179,705 opt/s), same RMSE | same |
| AAD implied greeks cost | ~3-4x one price | Giles-Glasserman NA-05-15 |

Three readings follow, and they are not the same story:

1. **Per-option American pricing is near the frontier already.** `ql_fast` is
   **1.81x cheaper than the shipped `fast` at statistically equal accuracy**
   (9.7e-4 vs 1.0e-3) and is already exposed as `al_bulk_opts()`.
   **Cite the ratio, not the microseconds**: `al-preset-ladder.md:159` marks the
   absolute us/op **PROVISIONAL** — the capture shared a host and 5 of 7 rows
   breach the 5% CV gate — while stating "the relative A/B is citable (ql_fast is
   fastest in every one of 5 reps)". The same doc at `:229` says **"No default
   changes here"**, and L1 established the reason (below): this is not free.
2. **American greeks are 3x to 9.6x off the AAD-implied cost model — the single
   largest performance gap in the library.** Counted from the call sites by L5
   (correcting an earlier "5 solves + 13 evals" estimate in this plan): the
   adjoint path pays **3 boundary solves** (1 taped + 2 cold sigma-plus/minus for
   volga), **2 tangent passes**, and **15 price evaluations** off a boundary
   (5 spot-stencil + 2 vega + 2 rho + 4 vanna + 2 volga).
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

   **L1 measured it, and it is worse than this section assumed.** On a 64-cell
   production-shaped grid (S=K=100, T in {0.5,1,2,5}, sigma in {0.2,0.4,0.6,0.9},
   r in {0.03,0.08}, q in {0,0.02}, put), the shipped `al_fast_opts()`
   (tol 1e-8, 2 JN + 2 FP sweeps) reached tol on **zero of 64 cells** — pure
   Jacobi-Newton needs 17-24 sweeps there — while `andersen_lake` returned `Ok`
   and priced all 64. Achieved residuals at sigma=0.6, r=0.08, q=0: **1.0e-4**
   (T=0.5), **4.1e-4** (T=1), **1.4e-3** (T=5), against a tol of 1e-8. The
   `accurate` rung reaches 5.4e-5 at T=1. **This is not a long-dated corner; it
   is the fast tier's normal operating point.**
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
5. ~~Promote `ql_fast` to the marks / de-Am / cache-sampling default.~~
   **REFUSED ON EVIDENCE, and the refusal is the finding.** L1 established two
   independent blockers:
   (a) It is **already done wherever it is safe** — `FitPreset::Bulk`
   (`session.cpp:1093-1116`) already ships `al_bulk_opts()` for `deam.al_opts`
   and `carry_al_opts`.
   (b) Promoting the **marks/serve** tier would make marks *worse*, not better.
   `n_quad_price` survives **no** `AlOpts` record format, so baking
   `al_bulk_opts()` into a stored pricing config round-trips as
   `n_quad_price = 0`, tying the premium quadrature to `n_quadrature = 8` —
   i.e. **(7,8,8)**, materially worse than `ql_fast` (7,8,32) *and* than `fast`.
   `session.cpp:1114` pins `serve_al_opts = al_fast_opts()` for exactly this
   reason, and `american_bulk_rung_test.cpp:270-292` already pins the behaviour.
   **Unblocking the marks tier requires the archive to persist
   `al_n_quad_price`** — a storage change, not a preset edit. Recorded as a
   `decision` line in the ledger so nobody "finishes" it later by flipping a
   default.

### L2 — Discrete dividends into production (`pool-18`)
Owns `src/pricing/american_discrete_div.cpp`, `dividend.cpp`, and the pricing
route inside `src/fitting/pricer_fitter.cpp`.

**L2 OUTCOME — the wiring point moved, on a measurement.** The lattice cannot go
where this section assumed. Measured per contract on the `dev` preset (ratios are
the signal; the release AL band is 32.6 us):

| route | us | x AL price |
|---|---:|---:|
| Andersen-Lake price, cold | 383 | 1.00 |
| Andersen-Lake **implied vol** | 934 | 2.44 |
| V&N lattice price, 301 steps | 1,276 | 3.33 |
| **V&N implied vol, 301 steps** (15 solves) | **15,771** | **41.2** |
| V&N implied vol, 101 steps | 1,892 | 4.94 |

**The lattice inversion is 16.9x the AL inversion.** The only production pricing
route inside L2's file ownership is `deamer.cpp`'s de-Americanization
*inversion* — exactly where the measurement says the lattice must not go. A
whole-board fit already spends 133 s on ~1.5M inversions and the dividend-bearing
subset (42.1% of rows) only halves that. **Cutting steps is not a throughput
lever either**: truncation against a 1201-step reference is 6.79 / 4.89 / 2.55
ticks at 101 / 151 / 301 steps, and 6.79 ticks is the size of the entire 6.96-tick
accuracy claim. Wiring it there would knowingly ship a 17x regression on the
fit's hot path.

**The affordable wiring point is the MARK path** — `session.cpp:2003/2010`
(`american_price_cached` / `american_price`) and `projection.cpp:773`: one price
per contract at 3.33x AL, i.e. ~109 us/contract against the 32.6 us band audit,
about 86 s serial over the dividend-bearing subset of a 1.38M-contract board.
Those files sit outside L2's ownership; this is now a scheduled follow-up, not a
loose end. What L2 shipped instead is the decision itself: `discrete_div_route` /
`DiscreteDivPolicy` / `DiscreteDivRoute` (`dividend.hpp:226-310`) — made once,
explicit, switchable on one enum, observable, and reusing `forward_div_corrected`'s
instant window byte-for-byte so both routes price the same cash. The decision is
worth **10.61 ticks on an ATM call and 54.59 on the put**, same lattice, same step
count on both sides.

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

   **CORRECTED BY L2 — the omission did not remove American call exercise.** The
   plain American max at the step *below* the ex-step already recovered it one
   `dt` late, so the lattice converged from below with an **O(dt) bias** rather
   than missing the feature outright. The fix is real and smaller than stated
   above: error against an exact reference goes **-1.4539e-2 -> -2.1011e-3 at
   301 steps (6.9x)**, -6.0211e-3 -> -1.3008e-3 at 601, -2.7829e-3 -> -9.4645e-4
   at 1201. On the *terminal* step nothing could recover it, and there the effect
   is absolute: a dividend on an American call's own expiry now moves its price
   by **exactly 0** (was 0.0241).

   L2 also declined to validate against Roll-Geske-Whaley, correctly: RGW is
   exact for the **escrowed** model, so using it as the oracle would have
   conflated the model gap with the bug. It built an exact reference for the same
   model instead — one dividend gives the closed decomposition
   `e^{-r*t1} * E[max(S_t1 - K, C_BS(S_t1 - D, T - t1))]`, quadrature plus
   Black-Scholes — and pinned RGW's 0.1037 offset separately as a model gap.
   Every American **put** anchor in the suite is bit-unchanged, which is the
   check that isolates this fix from the next one.

   The vendor-agreement claim could not be re-measured in this lane (no licensed
   corpus). What is established is that the header's *justification* was
   structurally wrong: it cited vendor agreement against a classical
   no-arbitrage result.
3. **`splice_dividend` flat-clamps continuation at the grid bottom.**
   `american_discrete_div.cpp:117` `if (post <= lo) cont = value[0];` fires on
   the low nodes at every splice with `amount > 0`. The American put's exercise
   floor rescues it; the **European** put — which is what the validating
   put-call-parity identities use — is not rescued.
   `discrete_div_american_test.cpp:434-439` pins `96.146830494183135` where the
   exact answer is `K*e^{-rT} = 97.044553355`; the 0.897723 deficit equals
   `level[0]*e^{-r*0.900332}` to the last digit. **The pinned test value is the
   artifact.**

   **L2 outcome: confirmed** — the pin is now `97.04455335484883` against an
   exact `K*e^{-rT} = 97.044553354850808` (2e-12). And the defect is
   **early-ex-date, not big-dividend**: corner nodes carry weight ~`(1-p)^k` at
   step k, so a tau=0.5 ex-date hides it at machine precision in *both*
   directions. L2's first parity test therefore measured nothing and was
   rewritten. At tau=0.05: **3.2922e-03 -> 1.9966e-12** (D=6) and
   **9.9558e-02 -> 4.9613e-05** (D=12, where the residual is the zero floor — a
   model statement, not an error). Six-dividend parity 5.222871e-05 ->
   5.1823e-05, with the European call and put moving 6.4e-8 / 3.4e-7 *toward*
   each other, the direction the geometry requires.
4. `dP/dD_i` collapses to the European forward-delta chain
   (`american.cpp:3788-3801`), so two schedules with equal PV and different
   timing get identical dividend sensitivities.

   **L2 outcome: shipped on the lattice path, and the demonstration is sharper
   than a same-PV pair.** New `american_discrete_div_dividend_sensitivities`
   (`american.hpp:1019`, impl `american_discrete_div.cpp:487`), two rollbacks per
   in-window event. On S=K=100, T=1, sigma=0.25, r=0.05 with 6.00@0.10 and
   6.00@0.90, the lattice ranks early against late **18.4581x for a call and
   0.9298x for a put** — *opposite orderings on one schedule* — where the shipped
   `american_dividend_sensitivities` returns **1.0408 for both**, asserted
   directly against it.
   **Action carried to L1 / follow-up:** `american.cpp:3788` still composes
   `dP/dD_i = (-dP/dq / (F*T)) * dF/dD_i`. Its two callers are
   `src/backtest/portfolio_pricer.cpp:1625` and `tests/american_test.cpp:3863`.
   It should delegate to the lattice entry whenever a chain carries a cash
   schedule.

### L3 — Implied-vol inversion (`pool-19`)
Owns `src/pricing/implied_vol.cpp`, `american_iv.cpp`.

1. **`american_implied_vol` returns `Ok(0.005)` — a SUCCESS — for an
   unidentifiable quote.** `american_iv.cpp:232` screens only *immediate*
   intrinsic. The true zero-vol American bound is
   `max(immediate intrinsic, df*(F-K)^+)`, and the forward leg is the larger one
   for a call OTM on spot but ITM on the forward — the everyday case on
   hard-to-borrow names. Mode B refuses **35,477** rows upstream rather than
   fixing the inverter.

   **CORRECTED BY L3 — the two-term bound stated above is INCOMPLETE, and
   implementing it as written would have left a residual hole.** The sigma->0
   American value is a **sup over exercise dates**, not a max of two endpoints:
   `max(0, max_{t in [0,T]} [ S*e^{-qt} - K*e^{-rt} ])`.
   `f(t) = A*e^{-at} - B*e^{-bt}` has an interior stationary point at
   `t* = ln(aA / bB) / (a - b)`, and it is a **maximum** when `a < b`. At
   `S=409.4, K=100, T=5, r=0.10, q=0.02` that interior optimum clears **both**
   endpoints by **1.76 price points**. Endpoint-only screening — exactly what
   this plan specified — would have passed a quote that is provably
   unidentifiable. L3 implemented the sup.
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

   **CORRECTED BY L5 — the "no error channel" half is false, and the remedy this
   plan proposed is one the codebase explicitly rejected.**
   `finite_vega_slope` (`src/backtest/portfolio_pricer.cpp:566-577`) screens the
   slope and demotes the lane to `PriceStatus::NumericError` **before** the
   Ok-stamp, so a NaN slope excludes its lane from every book total
   (`portfolio_pricer.cpp:922-931, 946-955`), and the behaviour is pinned by
   `AdjustedGreeks.NaNVegaSlopePropagatesToAdjustedDeltaOnly`. Its own comment
   rules out the "return `g` unchanged" fallback this plan asked for:
   *"Deliberately NOT a silent fallback to the UNADJUSTED delta — that would
   publish a different economic quantity than the caller requested under a column
   it cannot distinguish."* L5 declined the change and added the caller citation
   to the public header so the finding is not re-filed. If it is ever wanted, it
   is a portfolio-pricer decision, not a `skew_adjusted` one.
   The compute-before-guard half was real and is fixed.
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
   **CORRECTED BY L7 — the premise was false for per-case lanes, and this
   matters, because the original claim would have discredited every green run in
   this sprint.** CMake 3.29's own
   `share/cmake-3.29/Modules/GoogleTestAddTests.cmake:196` writes
   `SKIP_REGULAR_EXPRESSION "\[  SKIPPED \]"` onto **every** discovered case,
   unconditionally. Measured: all 7 `AmznEarnings.*` cases already report
   `***Skipped` and appear under "tests did not run". **No lane in this sprint is
   green-by-vacuity in `atx-vol-tests`.** L7 restated the property explicitly on
   all four `gtest_discover_tests` calls anyway — inheriting a gate contract from
   whichever `cmake.exe` is on PATH is not a contract.

   The genuine hole is narrower and real: the **hand-rolled aggregate
   `--gtest_filter` lanes**. `atx-vol-tests.exe --gtest_filter=AmznEarnings.*`
   with every case skipped **exits 0**, and ctest reported `Passed 0.10 sec`;
   it now reports `***Failed  Error regular expression found in output`. L7 used
   `FAIL_REGULAR_EXPRESSION` rather than `SKIP_REGULAR_EXPRESSION` there, because
   CMake documents the latter as marking a test skipped *regardless of the
   process exit code* — on an entry covering hundreds of cases, one fixture-gated
   skip would have swallowed a co-occurring real failure.
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

## Required follow-ups opened BY this sprint

These are consequences of Phase 1 fixes, in files the fixing lane did not own.
They are obligations, not ideas.

1. **`deamer.cpp`'s borrow fixed point must read a refusal as an inadmissible
   iterate, not as fatal.** L3's zero-vol bound is correct and it changes
   `deam_pcp_step` (`src/fitting/deamer.cpp:292-295`), which `ATX_TRY`s the
   inverter at the **candidate** `q_eff` of each borrow iterate. The carry
   solve's first probe is zero borrow, and a short-dated deep-ITM **call** whose
   time value (~0.03 points at sigma=0.20, T=0.04) is smaller than `S*borrow*T`
   (~0.09) sits below the zero-vol bound *at that probe* though not at the true
   carry. It now errors where it used to be handed a fabricated 0.5%-vol leg.
   Measured: `curve_fit_coverage`'s 15-day one-sided chain moved
   `PrepUncovered -> CarryFailed` on 3 refused strikes. The refusal is right; the
   search must treat it as a rejected iterate and continue.
2. **`tools/oracle_conventions.cpp:640-658` restates the old immediate-intrinsic
   screen verbatim** for `discrete_tree_implied_vol` and therefore now carries
   the defect L3 removed from `american_iv.cpp`.
3. **`american.cpp:3788` should delegate per-event dividend sensitivities to the
   lattice** whenever a chain carries a cash schedule. It still composes
   `dP/dD_i = (-dP/dq / (F*T)) * dF/dD_i`, which is the European answer. Callers:
   `src/backtest/portfolio_pricer.cpp:1625`, `tests/american_test.cpp:3863`.
4. **Wire the discrete-dividend lattice into the MARK path** —
   `session.cpp:2003/2010` and `projection.cpp:773` — per L2's measurement above.
   Not the de-Americanization inversion.
5. **`american_boundary.hpp:267` and `american.cpp:1889`** carry the same
   "reverse-accumulation-through-iterations" misnomer L5 corrected elsewhere, for
   the same forward-mode seam.
6. **The archive must persist `al_n_quad_price`** before any marks-tier preset
   change is possible (see L1/T5).

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
