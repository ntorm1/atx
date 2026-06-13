# Sprint S7 — Portfolio Construction & Production Lifecycle (sprint spec)

**Status:** ⏳ proposed (not open). **Consumes everything** — S3 (alphas), S4 (library), S5 (combiner),
S6 (cost/capacity), P4 (optimizer + risk model).
**Roadmap:** [`ROADMAP.md`](ROADMAP.md) · **Discipline:** [`../docs/sprint.md`](../docs/sprint.md)
**Grounded in:** [`../../research/worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md)
§6 (mega-alpha as one book), §7 (neutralization + turnover control), §10 (alpha lifecycle, decay, dead-alpha
recycling) +
[`../../research/renaissance-technologies-systems-deep-dive.md`](../../research/renaissance-technologies-systems-deep-dive.md)
§6 (Kelly/breadth sizing, the single unified dollar-neutral book) + Kakushadze "Dead Alphas as Risk Factors".

---

## Why this sprint

S1–S6 build a factory that discovers, stores, learns, combines, and prices alphas. S7 makes it **operate as
a book** — the final transformation from "a pipeline that produces a mega-alpha" into "a living portfolio
that runs over time, watches itself decay, recycles its dead, and reports honestly."

This is where the two north-stars fully converge: WorldQuant's lifecycle (discover → allocate → trade →
monitor decay → recycle/retire) and RenTech's single unified, dollar-neutral, capacity-capped book with
breadth-driven Kelly sizing. P4 gave a *single-period* risk-aware optimizer; S7 makes it **multi-period**
(turnover-aware over a rebalancing schedule), adds the **monitoring + recycling** loop, and proves the
**whole pipeline end-to-end**.

---

## Scope — units

### S7.0 — Marker + ledger
Open `sprint-7-progress.md`, freeze scope, base SHA.

### S7.1 — Multi-period / dynamic optimizer
Extend P4's single-period optimizer (`max αᵀw − λwᵀVw − κ‖w−w_prev‖₁` s.t. dollar-neutral / gross / name
caps) to a **multi-period** objective over a rebalancing schedule: turnover-aware across periods, using
look-ahead-safe forecasts (fit-on-trailing) and the **S6-calibrated** cost in the turnover term, with a
**deterministic** fixed-iteration solver (no convergence-dependent early exit — the p0 determinism rule for
optimizers). Capacity (S6.3) bounds gross. *atx-core (Pattern B): L7 QP / projected-gradient refinement.*

### S7.2 — Alpha-decay monitor
Detect **live-vs-backtest drift**: track each live alpha's realized OOS performance against its admitted
backtest, flag statistically-significant decay (reusing S1's metrics + DSR), and **demote** through the S4
lifecycle (live → decaying → dead). Point-in-time (decisions read only ≤ t). This is the "trust slowly /
retire fast" discipline both shops run.

### S7.3 — Dead-alpha → risk-factor extraction
Kakushadze's insight: a flatlined / dead alpha encodes a direction where **no money is to be made** — a
natural thing to *neutralize against*. Extract dead-alpha directions (from S4's `dead` lifecycle state) and
feed them into the **P4 `FactorModel`** as endogenous risk factors (the optional "dead-alpha risk directions"
P4 already reserved a slot for). The library becomes its own, self-generated risk model — no vendor Barra
needed for the endogenous part.

### S7.4 — Capital allocation + book reporting
Allocate capital across the mega-alpha (breadth-aware / capacity-bounded sizing — RenTech §6), and emit
**book-level reporting artifacts** (headless files, anti-roadmap #5): equity curve, exposure/leverage,
factor exposures, turnover, net-of-cost P&L attribution, capacity utilization, lifecycle census. Reproducible
artifacts, not a dashboard.

### S7.5 — Full E2E pipeline integration test + close
The capstone proof: a single deterministic run that goes **data → mine (S3) → store/dedup (S4) →
evaluate/gate (S1) → combine (S5/P4) → optimize (S7.1) → cost (S6) → report (S7.4)**, asserting the carried-
forward invariants top-to-bottom: byte-identical replay, no look-ahead at every fitted boundary (mining
fitness, learned params, regime states, calibrated cost, combiner weights, factor model, optimizer),
no survivorship, honest+calibrated cost, capacity-bounded. This test is the v2 exit gate. Close ceremony +
**module-level close** (the whole p1 roadmap's "what v2 proves").

---

## Exit criteria

- Multi-period optimizer is deterministic (fixed-iteration; byte-identical weights run-to-run) and respects
  dollar-neutral / gross / name-cap / capacity constraints; turnover term uses S6-calibrated cost.
- Decay monitor flags a planted-decaying alpha (non-vacuous) and demotes it through the S4 lifecycle, PIT.
- Dead-alpha directions are extracted and **measurably reduce** the mega-alpha's exposure to those directions
  after feeding the P4 risk model (the recycling actually neutralizes).
- Capital allocation respects the S6 capacity ceiling; reporting artifacts are reproducible (byte-identical
  across two runs).
- **The full E2E integration test passes** — one deterministic, point-in-time, cost-honest run through the
  entire pipeline, every fitted boundary truncation-invariant. This is the v2 done-gate.
- `/W4 /permissive- /WX`, clang-tidy + clang-format clean; test file per unit.

## Invariants this sprint must prove

All seven carried-forward invariants, **end-to-end and simultaneously** — S7.5 is the integration proof that
the factory, library, learned combiner, and calibrated cost compose **without** any of them leaking
look-ahead, breaking determinism, reintroducing survivorship, or under-pricing cost. The deterministic
multi-period solver (no convergence-dependent early exit) is the per-unit determinism crux.

## Dependencies

- **Upstream:** *everything* — S3 (alphas), S4 (library + lifecycle states), S5 (learned combiner), S6
  (calibrated cost + capacity), P4 (single-period optimizer + `FactorModel` + dead-alpha slot).
- **atx-core (Pattern B edge):** L7 QP / projected-gradient solver refinement (S7.1).

## Explicitly NOT in this sprint

No live trading / broker (anti-roadmap #1 — S7 operates a *simulated* book over historical PIT data; live is
p2). No intraday rebalancing (daily-bar schedule; intraday is a residual/later module). No dashboard (files,
not pixels, #5). No cross-machine distributed book (single-box, #4).

## Baton → next (p2 horizon)

With v2 complete — a deterministic, bias-free, calibrated, multicore **alpha factory + portfolio brain** —
the natural p2 frontier is **live**: broker connectivity, real-time PIT data ingest, intraday execution, and
the cross-machine scale-out v2 deliberately deferred. v2's reproducible artifacts and invariant proofs are
the foundation that makes going live trustworthy.
