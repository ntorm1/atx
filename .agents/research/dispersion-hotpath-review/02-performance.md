# atx-vol Dispersion Backtest — HOT-PATH PERFORMANCE Deep-Dive (Axis 2)

Scope: `run-surface-backtest` (the SPY vega-flat straddle-book benchmark) per-step
reprice+risk loop, AND the `build-corpus` fit throughput. READ-ONLY; verified against
`main` @ current HEAD. Measured baselines given: 82-session best 218 ms (~1804
lot-repricings), 135-session best 405 ms (~333 sessions/s). Post-WS-S the wall is
PRICING-bound, not load-bound.

**Book shape (the fact that drives everything):** the dispersion book is ONE ATM-forward
straddle per name = 1 call + 1 put at the SAME (K,T) per uid, plus the index straddle. For
the dev 10-name universe that is ~22 lots / ~22 unique contracts. Crucially, each
`(uid, side)` group is a **singleton** (one contract). This shape defeats every
within-group SIMD pack (see L1 below).

---

## STEP COST MODEL (per step, daily-hedge, non-expiry, ~22-unique dev book)

Traced end-to-end. Per step the engine issues these American-boundary-solve passes
(`analytic_greeks=true`, `resolved_price_isa=Auto`, `n_threads=0`→all cores):

| Pass | Where | Solves/unique | SIMD in Auto | Parallel |
|---|---|---:|---|---|
| **execute FullGreeks bundle** (entry-vega + hedge-delta + row greeks + NEXT step's P&L base) | `backtest.cpp:1716` → `price_into(FullGreeks)` | **5** (base + σ± + r±) | **SCALAR** ✗ | yes (fan-out over uniques) |
| compute_step shifted MARK (`EF::Price` @ T_t) | `backtest.cpp:1933`/`961` → `pnl_totals_*` → `solve_pnl_uniques` | **1** | AVX2 marks ✓ | yes |
| compute_step shifted IV (`EF::Iv` @ T_b) | same | **0** (surface eval only) | n/a | yes |
| compute_step base greeks (Taylor coeffs) | same | **0 — REUSED** from execute@prev step via base-risk stamp | — | — |
| settlement marks (expiry steps only) | `compute_step` batched Marks + L2 memo | ~1× n_expiring | AVX2 marks ✓ | yes |

**Steady-state ≈ 6 boundary-solve-equivalents per unique per step** (5 base-greek + 1
shifted-mark), matching the in-code "6 (no-churn)" ledger note (`portfolio_pricer.cpp:1304`).
Of that, **~83% (5/6) is the execute FullGreeks analytic bundle, and it runs SCALAR
per-contract in production Auto.** For the 82-run: ~1804 FullGreeks bundles (22×82) ≈ ~9,000
scalar AL boundary solves + ~1,800 AVX2 marks. The scalar base-greek bundle is the wall.

**Already-optimized (do NOT re-propose):**
- **Base-greek reuse across steps is WORKING.** execute@step i stamps the FullGreeks base
  bundle for `(S_i, book)` (`portfolio_pricer.cpp:1105-1118`); compute_step@step i+1's P&L
  reuses it (`reuse_base` guard, `portfolio_pricer.cpp:1587-1596`, `1641-1650`) so the P&L
  base leg costs **0 solves**. Expiry-day membership shrink is handled by
  `carry_base_risk_subset` (`portfolio_pricer.cpp:1735`). ⇒ **06-review's A4 "base greeks
  solved twice per step" is ALREADY FIXED by L1** — steady state is 6/unit, not 11. Verify
  via `BaseGreekReuseLanes` counter (should ≈ n_unique × n_non-entry-steps).
- book_greeks on recorded rows reuses `ex->book_greeks` (`backtest.cpp:2046-2048`) — no 3rd
  base pass under daily hedge.
- L2 settlement-mark memo avoids duplicate expiry solves (`backtest.cpp:856-941`).
- Marks (shifted price + settlement) already ride AVX2 in Auto (WS-K, `priced_surface.cpp:973-1048`).
- Per-step scratch/frames/ledgers are retained + reused (`ReusablePriceFrame`, `HedgeLedger`,
  `RetainedBookPricer`) — steady-state allocation-free.

---

## PERFORMANCE FINDINGS — ranked by expected speedup on the pricing-bound wall

### L1 — [CRITICAL / biggest] The dominant scalar base-greek bundle cannot be SIMD-packed for a dispersion book: solve groups are per-(uid,side) singletons. Needs CROSS-UID packing.

`portfolio_pricer.cpp:806-817` (scalar `run_ranges` fan-out) and `:642-691` (`solve_span`)
call `PricedSurface::evaluate_batch` **once per (uid,side) group**. For the dispersion book
each such group is ONE contract (1 straddle/name), so:
1. In Auto the greeks arm falls to the scalar per-contract loop (`priced_surface.cpp:1141-1157`);
2. Even under `ForceAvx2`, the laned dispatch (`priced_surface.cpp:1070-1139`) packs only
   within one `evaluate_batch` call's T-run — `cnt==1` → the 4-wide `american_put_greeks_batch`
   flush runs a **1-lane pack (75% empty)** → ~zero SIMD win.

The AL boundary solve is **uid-agnostic once resolved** (it takes S,K,T,σ,r,q per lane — the
laned-greeks seam confirms "boundary spot-independent", the marks pack is already cross-lane).
So the fix is a portfolio-level **cross-uid pack**:
```
// two-phase base-greek bundle for singleton-group books
phase A (scalar, cheap): for each unique contract, resolve against its OWN surface
        -> (S,K,T,σ,r,q, side)                       // ~1 essvi eval each
phase B (laned): gather ALL Put tuples across uids -> american_put_greeks_batch(n≈11)
                 gather ALL Call tuples across uids -> american_CALL_greeks_batch(n≈11)
        (4-lane AVX2 packs; scalar-patch guard/non-early-exercise lanes as today)
scatter each lane's AmericanGreeks back to px[orig] by a recorded index
```
This turns 22 scalar 5-solve bundles into ~6 four-lane packed bundles. Expected **~2–4×**
on the base-greek bundle → **~1.6–2.3× on total backtest wall** (composes on top of the
existing thread fan-out: SIMD is per-core, threads split the packs). This is the single
change that actually moves the dispersion number; H1's flip alone (below) does nothing here
because the packs are 1-wide.
Blockers/prereqs: (a) a **call-side** laned greeks kernel — `american_put_greeks_batch` is
PUT-native and dispersion is 50% calls (`priced_surface.cpp:1127-1135` sends calls to
scalar); add via put↔call boundary transform or a call kernel, else the win halves. (b) the
bit-identity contract (see L2). (c) determinism: fix pack membership as a pure function of
the input order (mirror the marks tile-schedule invariance, `portfolio_pricer.cpp:788-801`).

### L2 — [HIGH] Laned analytic greeks are gated `resolved_price_isa == ForceAvx2` ONLY; production Auto never dispatches them, though the ship gate is already ON.

`priced_surface.cpp:1070-1073`: the laned-greeks arm requires
`resolved_price_isa == simd::SimdIsa::ForceAvx2` **in addition** to
`avx2_greeks_selected(...)`. But `kShipAvx2Greeks = true` already
(`simd/american_boundary_batch.cpp:153`) so `avx2_greeks_selected(Auto)` is TRUE — the ONLY
thing forcing scalar in production is the explicit `== ForceAvx2` literal. The comment
(`priced_surface.cpp:1061-1068`) states the reason: a single-contract `FullGreekSeed`
produced via scalar `evaluate()` must stay bit-identical to the batch solve, and a 1-contract
seed can't match a 4-wide pack. Fix (WS-H's remaining last mile): relax the greeks contract
to an **economic parity gate** (exactly what marks did in WS-K) and change the condition to
`avx2_greeks_selected(resolved_price_isa)`. NOTE: this is a *prerequisite* for L1, not an
independent win on dispersion — with singleton groups the flip alone still packs 1-wide.
Where it *does* help standalone: multi-strike books (the index leg, listed-proxy, any
per-name multi-strike surface). Expected on those: ~1.5–2× on the greek bundle (put lanes).

### L3 — [HIGH, output-changing, PM-gated] Drop the rho (r±) solves from the P&L base bundle: 5→3 solves = −40% of the dominant cost.

The analytic AL bundle's 5 solves = base(1, gives delta/gamma/theta/**charm** free via spot
stencil) + σ±(2, gives vega/volga/vanna) + **r±(2, gives rho only)** (laned-greeks.md ledger:
full=5, {δ,vega}=3). The P&L Taylor consumes `g.rho * dr` (`portfolio_pricer.cpp:1517,1452`),
but `dr` = day-over-day risk-free-rate change ≈ 0, so **pnl_rho ≈ 0 every step**. The K4 tier
selectors are ALREADY WIRED end-to-end (`greeks_resolved` honors `needs.rho`,
`priced_surface.cpp:648-650`; `PriceOptions::greek_needs`, `portfolio_pricer.hpp:562`), but the
backtest requests the full bundle because the P&L reuse guard hard-requires
`base_greek_needs.full()` (`portfolio_pricer.cpp:1595,1649`). Fix: add a "risk-tier" P&L path
that (a) requests `greek_needs={vega,charm, rho=false}` in execute's `price_into`, (b) lets the
reuse guard accept a rho-less bundle, (c) makes `reduce_pnl_totals`/`scatter_pnl_rows` treat
`g.rho·dr` as 0 (fold into `unexplained`, which stays ≈0). Output change: `pnl_rho` column →
0 (attribution only; `nav`/`pnl_total` are the exact reprice, UNCHANGED; the gross-greeks row
carries no rho). Expected: −40% on ~83% of work → **~1.3–1.5× total**, orthogonal to L1/L2
(they compose: L1×L3 ≈ 2.5–3×). Needs PM sign-off (moves the VolTicks solve-count test pin
noted at `backtest.cpp:1780`).

### L4 — [MEDIUM] Snapshot prefetch is SYNCHRONOUS — no load/compute overlap.

`snapshot_cache.cpp:224-255`: `prefetch()` reconstructs the next partition **inline on the
calling thread** before the next step (no worker thread). The RunConfig comment promises
"look-ahead overlaps the next archive open/map with pricing" (`backtest.hpp:322-323`) but the
implementation does not overlap — it just moves the whole-board reconstruct earlier in the same
thread. Because per-step pricing is the wall and the pricing fan-out leaves the calling thread's
scheduling slack, an **async prefetch** (background-thread reconstruct + `with_query_pricing` of
S_{i+1} while step i prices) would hide the residual load. Bounded now (mmap made opens cheap,
WS-S), so expect single-digit-% on the pricing-bound run; larger on universes with many
whole-board surfaces. Fix: spawn the prefetch on `pricing_executor()` / a dedicated loader
thread; the SnapshotCache is already `shared_ptr`-owned and mmap pages outlive readers.

### L5 — [MEDIUM] Strategy overload whole-board loads every surface; only the ~22 traded legs are priced (06-review A5, still current).

`run_backtest(strat,...)` builds a private `SnapshotCache` with NO referenced-uid subset
(`backtest.cpp:1838`), so `MarketSnapshot::load` takes the whole-board `reconstruct_all_with_provenance`
branch (`backtest.cpp:1206-1220`) + runs `with_query_pricing` on every universe surface
(`:1237-1243`) even though only the basket legs are queried. For dispersion the universe ≈ the
traded set, so this is bounded; it bites for a broad universe with a narrow book. Fix: the
strategy can declare its referenced uids (universe basket is known at `universe_at` time) and
feed them to the cache subset path (the machinery exists, `backtest.cpp:1181-1205`). Note the
in-file seam: even subset-loaded surfaces are OWNED-reconstructed, not served as zero-copy
`PricedSurfaceView` — re-pointing `SurfaceSet`/`PortfolioPricer` at views is still pending
(03-review Top-5 #2) and would remove the per-date per-surface heap reconstruct.

### L6 — [MEDIUM] Surface eval (essvi/black76 total_variance) is scalar per contract in the pricing path; the AVX2 essvi backbone is used only in fitting.

`priced_surface.cpp:1113,1146` resolve each contract with a scalar
`resolve_with_carry_and_bracket` → `total_variance(K,T)`. The `essvi_batch_avx2` kernel
(2.59× in `simd_fastpath.md`) is NOT dispatched here. Low priority: the essvi eval is a few
FMAs+sqrt vs the iterative AL boundary solve that dominates. It becomes relevant only after
L1/L3 shrink the boundary cost, or for the "phase A resolve" of L1's cross-uid pack (batch the
resolves 4-wide too). Fold into L1's phase A.

### L7 — [LOW/MEDIUM] `n_threads=0` (all cores, clamped to n_unique≈22) may over-thread the tiny dev book; each of ~3 evaluate_batch fan-outs/step pays a team-dispatch + a `std::vector<std::jthread>` worker allocation.

`RunConfig::price` sets `n_threads=0` (`backtest.hpp:309`) → fan-out over min(cores, ~22). For
the AL bundle (22×~25µs serial ≈ 550µs) the split clearly wins even at 22 uniques, so this is
NOT a regression — but the header flags `n_threads>1` allocates a jthread vector per dispatch
(`portfolio_pricer.hpp:352-355`) and there are ~3 fan-outs/step. Action: (a) confirm the pool
is truly persistent (no per-call thread spawn) — `pricing_executor.cpp:36` says n_threads=1 is
inline; verify n_threads>1 reuses pool workers and only allocates a small worker-id vector; (b)
benchmark an n_threads sweep for the 22-unique dev book vs the 102-lot core book — a book-size
adaptive cap may beat "all cores" on the smallest book. Speculative without a profile.

### L8 — [LOW] Fat AoS `ContractPx` (~100 B: fair_value + AmericanGreeks[9] + iv + vega_slope + status) gathered per position in scatter/reduce.

`portfolio_pricer.cpp:390-396,830,882`. `scatter_rows`/`reduce_price_totals` gather
`px[contract_ix(i)]` — a full-cache-line indirect gather even when only pv/delta are consumed.
Bounded for dispersion (positions ≈ uniques ≈ 22, dedup keeps it small), so low here; would
matter for a large multi-strike book. SoA-per-column unique results would let a Marks reduce
touch only `fair_value`+`status`. Defer.

### Feature-gap notes (SOTA, not immediate levers)
- **No AVX-512 8-lane path** — kernels are AVX2 4-lane f64 (`simd_fastpath.md`); an 8-lane pack
  would double L1's win on a capable host and better fill thin cross-uid packs.
- **No call-side laned greeks kernel** (blocks half of L1).
- Row greeks / P&L always full-bundle; no first-order tier consumed on no-hedge days
  (06-review A6) — daily-hedge dispersion fires every day so this is moot for the benchmark.

---

## BUILD-CORPUS FIT THROUGHPUT (delegated analysis, verified structure)

**Fit cost model:** parallelism is **per-BOARD (symbol×date), each board single-threaded**
(`corpus_board_fit.cpp:278` `n_threads=1`; `surface_db_populate.cpp:278-282` forces inner
`fit_workers=1`). Fan-out = one lock-free bounded queue of boards, LPT-ordered by frame rows
(`surface_db_populate.cpp:202-236`), workers **pinned to P-cores only**, capped at
`performance_core_count()` (`:252-254`), streaming per-date drain for O(dates-in-flight) RSS
(`:355-429`). Per-surface ≈ 0.64 core-s. Dominant inner term = **cold Andersen-Lake
de-Americanization**, a scalar per-strike safeguarded-Newton root-find
(`calib.cpp:1103-1108`, ~40–80 obs/expiry), warm-started only across adjacent strikes;
correction cache deliberately OFF for risk (`pricer_fitter.cpp:1078`). Aggregate ≈ 12.6
surf/s ≈ 0.87 s/session at 11 surf/session.

**Levers (ranked):**
1. **[+40–70%] Idle E-cores.** i7-1260P = 4 physical P (8 SMT) + **8 physical E-cores sitting
   idle**; the P-core-only pin/cap (`surface_db_populate.cpp:252-254`, `fit_scheduler.cpp:193`)
   excludes them. Add an E-pinned worker set with cost-weighted LPT (small boards → E).
2. **[~1.4–1.7× overall] SIMD-batch the fit-time de-Am inversion.** `calib.cpp:1019-1191`
   inverts strikes one scalar AL solve at a time though all strikes in an expiry share (T,r,q)
   and AVX2 `iv_batch`/`american_boundary_batch` kernels exist. Vectorize 4 strikes/lane with a
   per-lane convergence mask. De-Am ≈ 50–60% of fit.
3. **[~10–20%] Cross-date eSSVI-param warm-start (seed, not cache).** `warm_start_chain`
   carries only correction caches, which are empty under risk (`surface_db_populate.cpp:329`,
   `calib_pool.cpp:176` passes `prior=nullptr`). Feed prior-day fitted eSSVI params as the
   calibrator seed to cut `outer_iters` for slow-drifting names.
4. **[situational] Skip whole-date refit on incremental add** (`surface_db_populate.cpp:531-572`
   refits every present cell on partition rewrite) — zero cost for a one-shot build, O(cells)
   waste for incremental universe growth.

**VERDICT: build-corpus is the throughput ceiling, and it dominates at essentially every
session count.** Build ≈ 0.87 s/session vs replay ≈ 3 ms/session → ~290× heavier per session.
For 1000 sessions: one-time build ≈ 14.5 min vs a replay ≈ 3 s. Cumulative replay time only
overtakes the one-time build after **~290 re-runs** over the same corpus. Levers 1+2 could
roughly halve build wall-time (~25 surf/s). If the goal is "longer backtests fast," the corpus
build — not the replay loop — is where the wall-clock is, unless the corpus is amortized over
many re-runs.

---

## PRIORITIZED ACTION LIST
1. **L1 cross-uid greek packing** (the only SIMD change that moves the dispersion number) —
   needs a call kernel + economic gate. ~1.6–2.3× total.
2. **L3 rho-drop risk-tier P&L** (5→3 solves) — orthogonal, PM output sign-off. ~1.3–1.5×,
   composes with L1 to ~2.5–3×.
3. **L2 flip laned-greeks Auto gate** (prereq for L1; standalone win on multi-strike books).
4. **Build-corpus E-cores + de-Am SIMD** (the real wall for long/repeated corpora).
5. L4 async prefetch, L5 subset-load + views, L6 batched resolves — second tier.
