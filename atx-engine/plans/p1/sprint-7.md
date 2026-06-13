# Sprint S7 — Portfolio Construction & Production Lifecycle (user reference)

**Status:** ✅ CLOSED 2026-06-13 (`feat/atx-engine-book`, unmerged). **Spec:** [`sprint-7-portfolio-lifecycle.md`](sprint-7-portfolio-lifecycle.md) · **Plan:** [`sprint-7-portfolio-lifecycle-implementation-plan.md`](sprint-7-portfolio-lifecycle-implementation-plan.md) · **Ledger:** [`sprint-7-progress.md`](sprint-7-progress.md)

S7 is the **operating book** — the first time the v2 factory's mined/learned alphas are *run as a portfolio over time*, not just discovered and scored. It closes the discover→predict→combine→**operate** arc: a deterministic multi-period turnover-aware optimizer over a rebalance schedule, an alpha-decay monitor that demotes live alphas through the S4 lifecycle, dead-alpha→endogenous-risk-factor extraction (Kakushadze) fed back into the P4 risk model, breadth-driven capacity-bounded Kelly sizing of one unified book, reproducible book-level reporting artifacts, and the end-to-end pipeline integration test that is the **v2 done-gate**.

```
atx/engine/risk/   — risk:: extensions   (multi_period.hpp, dead_factor.hpp + the FactorModelBuilder::build_components refactor)
atx/engine/book/   — atx::engine::book   (CostInputs · decay_monitor · allocation · report · pipeline)
```

**The one fact that defines this sprint:** *every primitive S7 needs already existed* — the fixed-iteration optimizer, the PIT lifecycle journal, the capacity curve, the deflated Sharpe, the combined mega-alpha, the calibrated cost. S7 is the **compositional layer** that drives, threads, and accumulates over them as a book. The only genuinely-new numeric paths are the **dead-alpha eigen-extraction** (R6) and the **decay detector** (R5). S7 is **purely additive**: two minimal additive engine-source touches (a behavior-preserving `FactorModelBuilder::build_components` refactor + a `FactorModel::exposures()` accessor; `Library::positions()`/`n_periods()` read-passthroughs), no atx-core changes, no edits to existing optimizer/factor/combiner/library/cost behavior.

---

## What shipped (6 units)

| Unit | Header(s) | What it is |
|---|---|---|
| **S7-0** | `book/fwd.hpp` | Scaffold + the operating-book doc block + the ledger recording the as-built v2 deltas (D1–D6: S6 cost merged; Library positions/n_periods passthroughs; pipeline enumeration; PanelView build_components; `dsr==psr`). |
| **S7-1** | `risk/multi_period.hpp` | **Multi-period optimizer**: a receding-horizon (MPC) driver over the as-built `PortfolioOptimizer::solve` — walks a `RebalanceSchedule`, threads `w_prev` across the real schedule (turnover measured across time, not from flat), calibrated κ in the L1 turnover prox, a Gârleanu-Pedersen partial trade-rate, capacity-bounded gross. Inner solver reused verbatim ⇒ inherits its fixed-iteration bit-determinism. |
| **S7-2** | `book/decay_monitor.hpp` | **Alpha-decay monitor**: a streaming Page-Hinkley down-detector + a realized DSR/PSR-drop detector gated by a Bailey-LdP **MinTRL** significance floor, plus a **cost-flooding** discriminator; a `DecayController` drives `library::Library::mark` with asymmetric **retire-fast / restore-slow** hysteresis (PIT, journal-append). |
| **S7-3** | `risk/dead_factor.hpp` (+ `factor_model.hpp` `build_components`) | **Dead alphas → endogenous risk factors** (Kakushadze): the dead alphas' holdings-overlap matrix is eigen-decomposed (sign-pinned for reproducibility), truncated at **eRank**, and fed as extra factor columns into `V = XFXᵀ + D` so the live book is steered off the no-money directions (measurably reduces `‖X_deadᵀ w‖`). |
| **S7-4** | `book/allocation.hpp`, `book/report.hpp` (+ `FactorModel::exposures()`) | **Capacity-bounded Kelly sizing** (`L = clip(c·SR/σ, 0, min(capacity, max_gross))`) + an effective-breadth participation-ratio cross-check; **reproducible book reporting** — an order-fixed accumulation of equity / gross / net / turnover / P&L attribution / per-period factor exposure (Xᵀw) / capacity utilization / lifecycle census, written to **byte-identical** headless files. |
| **S7-5** | `book/pipeline.hpp`, `bench/book_bench.cpp` | The end-to-end **`BookPipeline`** orchestrator (mine→promote→combine→augment-risk→size→multi-period optimize→monitor decay→recycle dead→report) + the **all-invariants capstone test** (the v2 done-gate) + the micro-bench. |

---

## How the book operates (one cycle)

```cpp
// 1. Mine + admit (S3/S4), then promote admitted alphas to Live (S7 is the first driver past Admitted).
ResearchReport r = ResearchDriver{lib, dsl, panel, sim, policy, gate}.run(cfg.research);
for (u64 a = 0; a < lib.n_alphas(); ++a) if (state == Admitted) lib.mark({a}, Live, go_live_period);

// 2. Combine the pool into one mega-alpha (S5/P4); hold the constituent ISignalSource*s alive.
Combination combo = AlphaCombiner{cfg.comb}.fit(pool, fit_begin, fit_end);
CombinedSignalSource mega{sources, combo, method};

// 3. Build the risk model and AUGMENT it with the dead-alpha directions (S7-3).
FactorComponents base = FactorModelBuilder{cfg.fm}.build_components(panelview, window, cap, group);
DeadAlphaFactors dead  = extract_dead_factors(lib, dead_ids, as_of, M);
FactorModel V          = augment_factor_model(base, dead);

// 4. Size the unified book (S7-4) against the calibrated capacity ceiling.
f64 cap_gross = cost::capacity_point(risk::capacity_curve(seed_book, panelview, sim, aum_grid)) / reference_aum;
CostInputs cost{ cost_aware_knobs(cc, ref_part, ref_sig, horizon).kappa, /*rt_bps*/..., cap_gross };

// 5. Optimize over the schedule (S7-1): multi-period, calibrated κ, capacity-bounded L.
MultiPeriodResult books = MultiPeriodOptimizer{cfg.mp}.run(sched, alpha_at(mega), model_at(V), cost);

// 6. Monitor decay over the forward window (S7-2): a decaying alpha is driven Live→Decaying→Dead.
for (usize s : forward) controller.step(lib, id, realized_r[s], s);
// 7. Recycle freshly-dead alphas as risk factors; 8. accumulate + write the byte-identical report.
```

---

## Invariants the capstone proves (R1–R8, simultaneously)

- **R1 Determinism** — fixed-iteration multi-period solver; same inputs ⇒ byte-identical book schedule + report digest (the schedule walk, trade-rate blend, eigensolver sign convention, and every reduction are order-fixed).
- **R2 No look-ahead** — receding-horizon: the objective may sum over a horizon but the book executes only the first move; every forecast (combined α, factor model, calibrated cost, decay baseline, dead-factor refresh) is fit-on-trailing and **truncation-invariant**.
- **R3 Calibrated honest cost** — the turnover term uses the S6-calibrated κ; the monitor distinguishes genuine decay from cost-flooding; allocation clips at the calibrated capacity point; net < gross.
- **R4 No survivorship** — a NaN/delisted name gets exactly 0 weight and round-trips through the report verbatim; dead-alpha holdings snapshot at death.
- **R5 Lifecycle driven** — demotions (`Live→Decaying→Dead`) and recycling (`Dead→Recycled`) are PIT journal appends; "trust slowly, retire fast" asymmetric thresholds. A planted-decaying alpha is genuinely **monitor-killed** (not pre-seeded); a stable alpha is not.
- **R6 Dead alphas → risk factors** — feeding the dead-alpha directions into `V` **measurably** reduces the optimized book's exposure to them (~45% on the fixture).
- **R7 Capacity-bounded Kelly** — the allocated gross **never** exceeds the calibrated capacity ceiling; the capstone asserts the clip **binds** (would fail if removed).
- **R8 Reproducible artifacts** — two runs ⇒ byte-identical report files (`std::to_chars`, binary mode, fixed NaN tokens) and equal pipeline digests.

---

## Residuals (→ p2 backlog)
- **Three atx-core promotions**: the L7 QP / fixed-iteration ADMM optimizer (shipped on the projected/proximal loop) and the eRank / effective-breadth `(Σλ)²/Σλ²` helper (L6).
- **DSL re-eval adapter (§0.8)**: the pipeline uses `ScriptedSignalSource` doubles as combiner constituents; re-parsing `provenance.expr_source` → compile → `VmSignalSource` at scale is deferred.
- **Standalone PanelView / full book-prefix truncation harness (§0.9 / R2)**: the load-bearing no-look-ahead is proven at the builder boundary; a full end-to-end book-prefix truncation harness is the deferred extension.
- **Gârleanu-Pedersen forward-blended aim**: the myopic + partial trade-rate ships; the full multi-step forward aim is deferred. **S6 borrow-accrual completion** for a half-short book.

---

## Baton → p2

v2 is feature-complete: a deterministic, bias-free, calibrated, multicore alpha factory + portfolio brain that mines, stores, learns, combines, prices, **and operates** a capacity-bounded book that watches itself decay and recycles its dead. The p2 frontier is **live**: broker connectivity, real-time PIT data ingest, intraday execution, and the cross-machine scale-out v2 deliberately deferred. v2's reproducible artifacts (the content-addressed library manifest + the byte-identical book report) and its all-invariants S7.5 gate are the foundation that makes going live trustworthy.
