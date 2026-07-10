# Sprint — SPY short-40Δ-6m-strangle daily-restrike backtest + strike-solver hot-path

**Goal (verbatim):** "use our backtesting engine to build me a backtest of what it looks
like to be short the 40 delta 6m strangle of SPY that is restriked every day. Do this to
2026-01-01 through 2026-07-02. Once you have the backtest confirm it is correct and table
and that we have a good way to bulk store opra option quote slices, surface binary
archives and measure how long a single run takes. Once you have that do another
performance deep dive and optimize atx-vol hot path to be able to handle massive
throughput and calc performance of this kind of sub routine and see how fast you can make
it while maintaining tick level accuracy not necessarily bit identical."

## Decomposition

1. **Backtest deliverable** — short 40Δ 6m SPY strangle, restriked every day, over the
   2026-01-02 → 2026-07-02 business-day window.
2. **Confirm correct + table** — a permanent gate + a printed/TSV tearsheet table.
3. **Confirm bulk storage** — OPRA quote slices + surface binary archives.
4. **Measure single-run time.**
5. **Perf deep-dive + optimize** the hot path for massive throughput at *tick-level*
   accuracy (bit-identity NOT required this cycle).

## What the engine already gives us (no new strategy code)

- `StrategySpec` Strangle: `structure.call_leg / put_leg = {Delta, 0.40}`, `tenor.target_T
  = 0.5`, `size.sign = -1` (short). `DeclarativeStrategy` interprets it.
- **Restrike-every-day** = `Holding::RollAtHorizon` with `roll_at_T` set ABOVE the tenor
  (e.g. 1.0). `lifecycle_decide`: a single cohort, reopened whenever residual
  `T = (front_expiry - base_ts)/yr < roll_at_T`. Since residual T ≈ 0.5 < 1.0 every step,
  the book is cleared and reopened at fresh 40Δ/6m strikes each date. Verified in
  strategy.cpp:336-345. No new lifecycle mode needed.
- Corpus/clock/reprice/tearsheet/TSV all exist (B0–B3, C0).

## Data (standing constraint: no paid Databento pull; synthetic corpora)

No cached real SPY OPRA on disk for this window and no API budget. Build a **deterministic
rolling-expiry synthetic SPY corpus**:
- Business days 2026-01-02 → 2026-07-02 (Mon–Fri; market holidays ignored — documented).
- Seeded (`std::mt19937_64`, fixed seed — never time-based) evolving spot path (S0≈600,
  ~12%/yr realized) and a mean-reverting ATM-vol regime, so the short-vol P&L has genuine
  theta/vega dynamics.
- Per date: an eSSVI `PricedSurface` (the `make_surface` pattern — analytic, no fit, fast
  and deterministic; exercises the 40Δ solver + reprice hot path) with a **rolling** expiry
  ladder (`snapshot + {1w,1m,2m,3m,6m,1y}` via `ns_to_iso_date`) so T=0.5 is always
  interpolable. Written to a per-date ATXVSA v3 archive + manifest.

## Storage confirmation (honest)

- **Surface binary archive** = ATXVSA v3 (`write_surface_archive_file` / `SurfaceArchive`):
  O(1) symbol lookup, CRC-32C integrity. The corpus IS this store; round-trip is already a
  gate (spy_archive_roundtrip_test). Report bytes/surface + reload ns.
- **OPRA quote slices** = `QuoteFrame` (the in-memory OPRA slice from
  `make_synthetic_american_panel`), persisted via the committed self-contained CSV store
  (`load_chain_csv`, panel.hpp). Round-trip one representative slice. (Parquet loader is
  deferred/NotImplemented by house rule — not claimed.)

## Phase 2 — the REAL hot path for THIS subroutine

Daily restrike ⇒ **every step** runs `open_cohort` → for each of 2 legs
`resolve_strike_by_delta`, a bisection (bracket widen + up to 128 iters) that calls
`surf->greeks(K,T,side)` **per candidate strike**. Each `greeks` is a full American greek
solve (analytic 5 boundary solves, FD 7–17). So the strike solver alone is
~2 legs × (bracket 6 + ~30–40 bisection) × 5–17 boundary solves **per day** — it DWARFS
the whole-book reprice (2 lots). This is exactly the "sub routine" the goal points at.

**Levers (deltas only — the solver needs |delta|, not 9 greeks):**
- **L1 delta-only fast path.** `resolve_strike_by_delta` needs only `|delta|`. Δ is EXACT
  off the spot-independent AL boundary: 1 boundary solve + 2 spot price evals, vs 5 (analytic
  greeks) / 7–17 (FD). ⇒ ~5–17× fewer solves per bisection step.
- **L2 strike-invariant boundary reuse.** The AL boundary in the y[] representation is
  strike-invariant (homogeneous degree 1 in K; only xmax scales — established last cycle).
  Across a bisection the *only* thing that changes is K ⇒ reuse ONE boundary solve for the
  whole bracket+bisection, re-evaluating the premium/greek at each candidate K by rescaling.
  Collapses ~40 solves/leg → ~1.
- Accept tick-level (½-tick $0.005 / ~1e-4 |delta|) deviation, not bit-identity.

Sequenced, measured, each validated against the pre-change strikes to |delta| tol.

## Phase 2 — results

**Baseline (release, 130-date corpus, 1 strangle):** backtest run 7.99 s / 129
priced steps = 62 ms/step for a 2-lot book. Confirmed the strike solver dominates:
the reprice is 2 lots (~ms), the rest is `resolve_strike_by_delta` × 2 legs, whose
bisection repriced full American greeks per candidate strike — `PricedSurface::
greeks` = cold FD (`american_greeks_fd`: 7 boundary solves for the put fast lane,
17 for the call) — to consume ONLY `|delta|`.

**L1 — delta-only fast path (SHIPPED, bit-identical).** New free function
`american_delta` + `PricedSurface::delta`, wired into `resolve_strike_by_delta`
(bisection + validation):
- Put/AndersenLake: ONE base-boundary solve + two price-from-boundary spot
  stencils (the boundary is spot-independent) — BIT-IDENTICAL to the FD put delta.
- Call / BAW / degenerate: the same two-price central difference on the cold
  `american_price` the FD path uses — bit-identical, at 2 solves not 17.

Result: **backtest run 7.99 s → 3.97 s = 2.01× faster, strikes and tearsheet
bit-identical** (`total_return` 772.92 unchanged; strategy/backtest determinism +
closure gates all still bit-identical). Gate `AmericanDelta.MatchesFd_PutCallGrid`
locks `american_delta == american_greeks_fd.delta` bit-for-bit over a 150-point
put+call grid. Full suite: 674 tests green.

**Measured breakdown after L1 (per-leg, disabled `SolverBreakdown` gate):**
delta-only is 7-9× cheaper PER eval than full greeks, but `resolve_strike_by_delta`
still cost 2790 µs (put) / 3887 µs (call) — because the pure BISECTION runs ~23-31
evals (linear, 1 bit/iter). Resolve was ~68% of the step, `expand_leg`'s per-leg
vega greeks ~16%, the reprice ~15%. (The Gauss-Legendre tables are a process-global
`static const`, so `american_price` carries no per-call table-build cost — the
call/put asymmetry is just 2 vs 1 boundary solves, not cold setup.)

**L2 — Illinois false-position root-find (SHIPPED, bit-identical).** Replaced the
bisection's midpoint with an Illinois-weighted secant step on the SAME [lo,hi]
sign-change bracket: superlinear convergence for the smooth monotone
`|delta|(k)`, ~6-8 evals vs ~23, at the SAME 1e-7 tolerance (so the resolved strike
is bit-identical — no accuracy tradeoff at all). Interpolation is guarded to the
midpoint when a bracket endpoint is a sentinel (asymptotic imputation) or the guess
escapes the bracket; the Illinois down-weight of a twice-retained endpoint breaks
the false-position stall. Robustness + determinism preserved.

Result: `resolve_strike_by_delta` 2790→600 µs (put, 4.65×) / 3887→989 µs (call,
3.93×). **End-to-end backtest 7.99 s → 1.28 s = 6.23× (62 → 9.9 ms/step; 14.9 →
100.7 steps/s), all outputs bit-identical** (total_return 772.92; the B3 worked
examples 396.3541 / 17.2874 byte-unchanged; closure + determinism gates unchanged).
Full suite: 674 tests green.

**Net Phase 2: 6.2× on the daily-restrike hot path, pricing quality bit-identical
(well inside the tick-size epsilon the goal allowed).**

**Remaining headroom (documented, not taken — diminishing / broader-blast-radius):**
(1) `expand_leg` computes full-FD greeks per leg only to read vega, even for
FixedContracts sizing that ignores it (~16% of the step) — a vega-only path or a
defer-until-needed refactor touches every strategy. (2) The call leg's delta is 2
boundary solves vs the put's 1; a McDonald-Schröder homogeneity delta would halve
it but is tick-level (not bit-identical) and fiddly. (3) The reprice already runs
the analytic 5-solve greeks.
