# atx-vol — Unification & Dispersion-Backtest Readiness Sprint

**Date:** 2026-07-05
**Status:** Design of record → implementation.
**Scope:** `atx-vol`. One phased sprint that (1) closes the correctness debt an
end-to-end review surfaced, (2) unifies the library into one coherent lifecycle
and clean API, (3) builds an OPRA-at-scale ingest + surface-archive corpus, and
(4) stands up a vega-weighted equity-dispersion backtest on that corpus.

This is the "tie it all together" sprint that follows the seven roadmap sprints
(`2026-07-04-atx-vol-sota-hft-roadmap.md`) and the seven `2026-07-05` design
specs (unification, curve family, priced-surface archive v3, portfolio pricer,
American-IV throughput, arb-dense surface, carry/de-Am fix). It does not invent
new pricing math; it hardens, unifies, scales the data path, and adds the
strategy/backtest layer on top.

## Locked decisions (session `/goal` clarification, 2026-07-05)

| Decision | Choice |
|---|---|
| Sprint reach | **Full phased roadmap** (P0 fixes → P1 unify → P2 ingest → P3 archive corpus → P4 dispersion backtest) |
| Dispersion trade | **Vega-weighted straddle dispersion** — short index ATM straddle vs long basket of member ATM straddles, vega-notional weighted, delta-hedged |
| First universe | **Index + ~10 liquid members** (pilot; prove the end-to-end pipeline before scaling) |
| Data budget | **Small paid Databento pull authorized**, cost-capped + free preflight; the paid multi-symbol/multi-date pull is an operator-approved gated step with a `$` cap set at pull time |

## Where we are (baseline)

`atx-vol` is a mature C++20 port: ~29k src LOC across ~50 modules, ~18k test LOC.
The archive/session/facade layer (v3 `SurfaceArchive`, `VolaSession`,
`PricerFitter`/`OptionChain`, `PricedSurface`, `PortfolioPricer`) is
structurally sound — format versioning, schema hash, layered hardware CRC-32C,
polymorphic variable-length round-trip, deterministic parallel evaluation, and
concurrent-const thread-safety all check out. SPY real-board accuracy is 99.5%
price-in-band in-sample / 98.6% OOS (convex-QP dense fit + carry/de-Am fix);
`value_chain` runs ~21k inv/s/core, ~94k on 4 physical cores, bit-deterministic
across thread counts. Prior sessions report the atx-vol suite green (~588 tests);
**P0 re-verifies this from a clean `build-rel`** rather than trusting the claim.

The gaps this sprint closes are: one library-wide correctness bug, three
calibration robustness holes that bite on illiquid names, a single-symbol /
single-snapshot ingest path, two coexisting portfolio systems, scattered
calibrator duplication, and the entirely-absent strategy + backtest layer.

---

## Code-review findings (folded into P0/P1 work items)

End-to-end review, 5 parallel module reviewers + direct verification. Severity:
🔴 correctness / blocks-the-goal, 🟡 robustness / hardening, 🔄 duplication.
"Verified" = confirmed by reading the implementation, not just flagged.

### 🔴 Flagship — European-vs-American greeks/price inconsistency (library-wide)

`american_greeks(..., correction=nullptr)` returns **pure Black-76 European**
price + greeks: in `american_greeks_first_order` (`src/american.cpp:898-903`)
`c_val=0` ⇒ `out.price = euro_price`, and δ/γ/vega/vanna/volga/charm are the
Black-76 values (spot-rescaled). But `PricedSurface::fair_value`
(`src/priced_surface.cpp:123`) reprices **American Andersen-Lake**. On the
ConvexDense/SPY override path `VolaSession::served_cache`
(`include/atx/vol/session.hpp:384-390`) returns null, so the *live session* has
the same split. Consequences (all verified):

1. **PnL-explain axis isolation** (`src/portfolio_pricer.cpp:379-389`):
   `pnl_total = American(shifted) − European(base)`. The base early-exercise
   premium is a constant no Taylor term carries → falls entirely into
   `unexplained`; and European δ/γ cannot reconstruct an American reprice → a
   large residual even for a **spot-only** move (the reported symptom).
2. `PriceFrame.price` is **European** despite the comment
   (`src/portfolio_pricer.cpp:167-169`) claiming "the American Andersen-Lake mark."
3. `VolaSession::greeks().price ≠ VolaSession::fair_value()` on index surfaces —
   the exact surfaces dispersion prices against.

**Fix (P0-1):** add an American greeks path — when no correction cache is
present, finite-difference the cold `american_price` (with the *resolved*
`pricing_.al_opts`/method) for price + δ/γ/vanna/volga/charm, so
`greeks().price == fair_value()` bit-consistent and the coefficients are
American. Route `PricedSurface::greeks` (and the session override path) through
it.

### 🔴 PnL secondary — vol axis leaks term-roll

`src/portfolio_pricer.cpp:323-332`: `dvol = st->iv(K,T_t) − sb->iv(K,T_b)`
measures vol at **different maturities** when `dt≠0`, so `dvol` mixes smile-shift
with term-structure roll and double-counts against theta. **Fix (P0-2):** measure
`dvol` at a common T (e.g. `st->iv(K,T_b) − sb->iv(K,T_b)`), keeping the roll in
theta.

### 🔴 Calibration robustness (fires on illiquid basket members, not on SPY)

- `src/essvi_calib.cpp:928` — theta-band inversion when `theta_floor ≥ band.hi`:
  `std::min` forces `band.lo ≥ band.hi` → `cube_to_natural` yields negative /
  garbage variance. **Fix (P0-3):** clamp `theta_floor ≤ band.hi − 2e-12`.
- `src/svi_calib.cpp:507` — negative-variance floor **masks** degenerate fits
  (`w_pred<1e-12 → 1e-12`), pricing garbage silently. **Fix (P0-3):** reject /
  flag `w_pred ≤ 0` before the floor.
- `src/c8_calib.cpp:94` — gradient failure silently `continue`d → rank-deficient
  Hessian → LM reports **success on a bad fit**. **Fix (P0-3):** count failures;
  error out above a threshold.

### 🟡 Core pricing (1🔴 + assorted 🟡)

- `src/correction.cpp:407` — 32KB stack scratch is now **uninitialized** (the
  "dead memset" the unification spec removed for perf); correctness relies on a
  write-before-read invariant. **Fix (P0-4):** restore a bounded init of only the
  live span, or assert/document the write-before-read invariant so sanitizers and
  future edits stay safe.
- Softer (P1 hardening): `american_greeks` vs `american_vega` degenerate-input
  handling asymmetry (`src/american.cpp:1305` errors vs `:1315` returns 0);
  near-expiry FD stencils (`hT=1e-5` probing outside `[0,T]`); IV Halley
  denominator conditioning (`src/implied_vol.cpp:195`); deamer `q_eff` round-trip
  ULP drift vs its "exactly" comment (`src/deamer.cpp:100`).

### 🔴 Data ingest — the biggest blocker for the goal (P2)

- `src/data.cpp:280`, `:298` — **O(n²)** `build_uid_list` / `build_expiry_inputs`
  → seconds at 500+ symbols. **Fix:** map/set-keyed dedup.
- `src/panel.cpp:364` — CSV loader silently mixes a multi-symbol file under the
  first row's uid. `src/opra_panel.cpp:224` — the `underlying` filter is silently
  ignored when the column is absent. **Fix:** validate/segregate by uid.
- Missing for scale (all P2 work): batch/date-range API, multi-symbol
  validation, **term-structure yield curve** (flat `{T=1,r}` corrupts multi-date
  forwards/discounts), per-file error tracking, expiry-parse caching, progress
  reporting.

### ✅ Archive / session — solid (no blockers)

Format, CRC coverage, polymorphic round-trip, `value_chain` determinism, and
concurrent-const safety all verified sound. Two 🔴 a reviewer raised in `map_all`
(`src/surface_archive.cpp:952,965`) are **false positives** — `Result` is
`tl::expected`, whose rvalue-qualified `operator*` move-constructs correctly.
Only real item: `src/surface_archive.cpp:362` `node_count` silent uint32
truncation — add a bounds guard (P1 hardening).

### 🔄 Duplication for the unification to collapse (P1)

- **Two portfolio systems**: legacy VolSurface-bound (`portfolio_price.cpp`,
  `portfolio_greeks.cpp`, `portfolio_risk.cpp`, `bulk.cpp`) vs new
  PricedSurface-native (`portfolio_pricer.cpp`). Make PricedSurface-native
  canonical; wall off / deprecate legacy.
- Calibrators: `outer_cap()` verbatim in essvi+svi; divergent post-fit sigma
  gates; scattered LM constants; per-calibrator seed/validate logic.
- De-Americanized obs builders split (`build_observations` vs
  `build_observations_european`) — the carry-fix spec already flags folding these
  into one.

---

## Phases

Each phase is independently shippable and holds every cross-cutting gate. File
targets are indicative; TDD per the repo norm (write the gate test first).

### P0 — Baseline + correctness gates

**Goal.** A clean, measured baseline and the correctness bugs fixed, with new
gate tests that make the fixes non-regressing.

**Tasks.**
- **P0-0 Baseline.** Build `build-rel`; run the full atx-vol ctest suite; record
  the green count and the baseline metrics (SPY price-in-band, cold fit ms,
  `value_chain` inv/s, archive MB/s). Fail the phase if the suite is not green.
- **P0-1 American greeks path.** Add `american_greeks` (cold-AL FD) for the
  null-cache path so `greeks().price == fair_value()`; route
  `PricedSurface::greeks` + the session override path through it. *Files:*
  `src/american.cpp`, `include/atx/vol/american.hpp`, `src/priced_surface.cpp`,
  `src/session.cpp`.
- **P0-2 PnL dvol-at-same-T.** Measure `dvol` at a common maturity. *File:*
  `src/portfolio_pricer.cpp`.
- **P0-3 Calibration robustness.** theta-band clamp (essvi), neg-variance reject
  (svi), gradient-failure guard (c8). *Files:* `src/essvi_calib.cpp`,
  `src/svi_calib.cpp`, `src/c8_calib.cpp`.
- **P0-4 Correction scratch.** Bounded init or documented/asserted
  write-before-read invariant. *File:* `src/correction.cpp`.

**Acceptance gate.**
- New test: `PortfolioPricer` spot-only move ⇒ `pnl_unexplained ≈ 0` (within
  third-order Taylor tolerance) on a real SPY board and a synthetic
  known-truth book; vol-only, rate-only, time-only shifts each isolate to the
  matching greek with the others ~0.
- New test: `VolaSession::greeks().price == fair_value()` and
  `PricedSurface::greeks().price == fair_value()` bit-consistent on the
  ConvexDense/SPY path.
- New tests reproduce then fix the three calibration 🔴 on engineered
  sparse/degenerate slices.
- SPY 99.5% in-band held; full suite green; `/W4 /permissive- /WX` clean;
  `value_chain` determinism preserved.

### P1 — Unification + clean API

**Goal.** One coherent, documented lifecycle and a single canonical portfolio
path; calibrator duplication collapsed; the API-consistency items from the review
resolved.

**Tasks.**
- **P1-1 Portfolio unification.** PricedSurface-native `PortfolioPricer` is
  canonical. Deprecate/wall off the legacy VolSurface-bound portfolio (keep as
  reference behind a clear boundary, or retire if no consumer remains). Document
  which to use.
- **P1-2 Calibrator dedup.** Extract shared calibration detail (`outer_cap`,
  post-fit sigma gates, LM constants, seed/validate) into a `detail/`
  header; unify the divergent post-fit sigma gates; fold
  `build_observations` / `build_observations_european` toward one
  de-Americanized obs builder. *Files:* `src/essvi_calib.cpp`, `src/svi_calib.cpp`,
  `src/c8_calib.cpp`, `src/cstar_calib.cpp`, `src/calib.cpp`, new
  `include/atx/vol/detail/calib_shared.hpp`.
- **P1-3 Lifecycle doc + façade.** Document and lock the one lifecycle
  `OptionChain → PricerFitter → FittedSurface/PricedSurface → SurfaceArchive →
  PortfolioPricer` with a single coordinate-convention note (log-moneyness,
  forward/carry, T = 365.25-day year, spot-based greeks). Umbrella-level
  quickstart for the full chain-to-backtest path.
- **P1-4 API hardening.** `node_count` uint32 guard; drop-reason context on
  `apply_quotes` dropped quotes; `american_greeks`/`american_vega`
  degenerate-input consistency.

**Acceptance gate.** One documented lifecycle exercised end-to-end in a test TU
(chain → fit → archive → reload → portfolio price/PnL). Calibrator dedup is
byte-identical on the SPY/XOM boards (no quality change). Suite green; warnings
clean.

### P2 — OPRA ingest at scale

**Goal.** Turn the single-symbol/single-snapshot loader into a bulk
multi-symbol / multi-date pipeline that can ingest the pilot universe, with real
term structure and honest per-file error handling.

**Tasks.**
- **P2-1 Kill O(n²) builders.** Map/set-keyed `build_uid_list`,
  `build_expiry_inputs`, and `Universe::add_expiry`. *Files:* `src/data.cpp`,
  `src/universe.cpp`.
- **P2-2 Multi-symbol validation.** Reject/segregate mixed-symbol parquet/CSV;
  error when a set `underlying` filter has no column. *Files:* `src/opra_panel.cpp`,
  `src/panel.cpp`.
- **P2-3 Term-structure yield curve.** Replace the flat `{T=1,r}` pillar with a
  multi-pillar curve (accept as input or build from data); correct for multi-date
  loads. *Files:* `src/opra_panel.cpp`, `src/data.cpp`.
- **P2-4 Date-range batch API.** `load_opra_daterange(symbols, date_lo, date_hi)`
  → per-`(symbol,date)` `OpraPanel` with per-file `Result` status ("loaded
  N/M"), expiry-parse caching, and progress. *Files:* new
  `include/atx/vol/opra_batch.hpp` + `src/opra_batch.cpp`.
- **P2-5 Bulk pull tool.** Extend `databento_xom_bbo` (or a new
  `databento_bulk_opra`) to loop symbols × dates with the free
  `MetadataGetRecordCount` preflight and a **hard cost cap**; refuse above the
  cap. *File:* `atx-core/examples/databento_bulk_opra.cpp`.
- **P2-6 Pilot ingest.** Operator-approved, capped paid pull of the pilot
  universe (index + ~10 members) over the chosen date range; cache DBN + parquet.

**Acceptance gate.** Bulk load of the pilot universe completes with per-file
status; no O(n²) hot spot (timed on a synthetic 100-symbol frame); multi-date
forwards use the term curve (regression vs a hand-checked case); mixed-symbol
inputs are rejected. No paid pull beyond the approved `$` cap.

### P3 — Surface-archive corpus

**Goal.** Fit every `(symbol, date)` into a `PricedSurface` and persist a
queryable archive corpus the backtest reads.

**Tasks.**
- **P3-1 Corpus fit.** For each `(symbol, date)`: `PricerFitter` with auto
  curve-selection (`curve_selector`: index → ConvexDense, single-name →
  eSSVI/SVI) → `to_priced_surface`. Parallel across the corpus via `calib_pool`.
- **P3-2 Corpus layout + manifest.** One `SurfaceArchive` per date (symbols
  within) — the natural backtest access pattern (load a date, price the book).
  Add a corpus manifest/index (dates, symbols present, fit diagnostics) for the
  backtest to enumerate.
- **P3-3 Integrity + throughput gates.** Round-trip theo bit-identical
  (fit → serialize → reload → price); corpus build throughput + storage
  footprint bench.

**Acceptance gate.** Pilot corpus built and reloadable; per-surface reload theo
bit-identical to the live fit; manifest enumerates the corpus; fit-quality
distribution across members recorded (flag members below an in-band floor).

### P4 — Dispersion strategy + backtest engine

**Goal.** A vega-weighted straddle dispersion backtest over the corpus, with
honest costs and PnL attribution through the (now-fixed) PnL explain.

**Tasks.**
- **P4-1 Strategy layer.** `DispersionBook`: short index ATM straddle vs long
  basket member ATM straddles, weights `w_i` set so basket vega = index vega,
  each leg delta-hedged. Emits an implied-correlation signal
  `ρ_imp = (σ_idx² − Σ w_i² σ_i²) / (Σ_{i≠j} w_i w_j σ_i σ_j)`. *Files:* new
  `include/atx/vol/dispersion.hpp` + `src/dispersion.cpp`.
- **P4-2 Backtest engine.** Event loop over corpus dates: load the date's
  archive → `PortfolioPricer::price` the book → `pnl_explain` vs prior date →
  apply the delta-hedge and roll → accrue costs → append PnL/greek/signal time
  series. *Files:* new `include/atx/vol/backtest.hpp` + `src/backtest.cpp`,
  example `examples/dispersion_backtest.cpp`.
- **P4-3 Honest frictions.** Bid/ask fills (not mid), delta-hedge slippage,
  per-contract transaction costs, straddle roll cadence. Report attribution
  (delta/gamma/vega/theta/unexplained) from the fixed `PnlFrame`.

**Acceptance gate.** End-to-end backtest runs over the pilot corpus and produces
a PnL time series, an implied-correlation series, and a per-axis attribution that
**closes** (Σ Taylor terms + unexplained = total, unexplained small on
spot/vol-only steps). Determinism: same corpus ⇒ bit-identical backtest output.

---

## New modules / files (cumulative)

- P1: `include/atx/vol/detail/calib_shared.hpp`
- P2: `include/atx/vol/opra_batch.hpp`, `src/opra_batch.cpp`,
  `atx-core/examples/databento_bulk_opra.cpp`
- P4: `include/atx/vol/dispersion.hpp`, `src/dispersion.cpp`,
  `include/atx/vol/backtest.hpp`, `src/backtest.cpp`,
  `examples/dispersion_backtest.cpp`
- Tests/benches per phase (gate tests named in each acceptance gate).

## Cross-cutting invariants (every phase)

- SPY 99.5% price-in-band held on the cached real board; the standing quality
  gate not regressed.
- `value_chain` / portfolio / backtest output bit-identical across thread counts.
- Full atx-vol suite green; `/W4 /permissive- /WX` clean.
- No paid Databento pull beyond the operator-approved `$` cap; free preflight
  first; cached slices reused.
- Every performance claim measured with the interleaved A/B throttle-canceling
  discipline; negative results documented as such.

## Non-goals

- No new pricing/fit math beyond the American-greeks FD path (P0-1) — numbers
  still flow through the validated `andersen_lake` / `american_price` /
  convex-QP / eSSVI primitives.
- No SIMD vector transcendentals (toolchain-blocked under clang-cl; measured
  negative in prior sessions).
- Not scaling the universe past the ~10-member pilot this sprint (50-100 members
  is a follow-on once the pipeline is proven).
- Variance-swap-replication dispersion is deferred; the vega-weighted straddle
  book is the first cut, with the replication upgrade path noted for later.

## Open operator gates

- **Paid pull cap ($) + date range** — set at pull time before P2-6. Preflight
  (`MetadataGetRecordCount`, free) reports the estimated cost; the pull refuses
  above the cap.
- **Pilot member list** — the ~10 liquid SPX names to pair with SPY; fix before
  P2-6 so ingest, corpus, and backtest share one universe definition.
