# Phase 4b — Implementation Progress

**Worktree:** NONE — in-place on the active shared branch (the established engine workflow; Phase 3b/3c/4a worked this way too; `.agents/atx-engine/agent.md` is the authority).
**Branch:** `feat/atx-core-stdlib`
**Base:** `feat/atx-core-stdlib` @ `3d09371` (the 4a close)
**Started:** 2026-06-06
**Source plan:** [`phase-4-signal-combination-risk-implementation-plan.md`](phase-4-signal-combination-risk-implementation-plan.md)
**Prior progress:** Phase 4a ([`phase-4a-progress.md`](phase-4a-progress.md))

---

## Plan adjustments vs. the source plan

Sprint 4b covers P4-6…P4-10 (factor exposures → factored covariance V=XFXᵀ+D → WeightPolicy neutralization → turnover-penalized optimizer → integration/capacity/close). The source plan §1–§11 is frozen; the **amendment §0 (2026-06-06)** overrides on conflict.

**As-built PanelView exposes OHLCV ONLY** (`open/high/low/close/volume`, newest-first; no `cap`, no `adv` field, no `IndClass`/sector). So P4-6's §5.4 exposures reconcile as follows: **Momentum, Volatility, Beta, Liquidity** (adv20 = mean(close×volume)) are derived purely from OHLCV; **Size (ln cap) and sector dummies require OPTIONAL EXTERNAL inputs** (a per-instrument `cap` span + a `group_map` span — cap is fundamental data outside the price-volume panel; the sector group map is the §0-H `IndClass` map P4-8 also consumes). When those inputs are absent, the corresponding columns are omitted (config-gated). PanelView has no `t`/`window` index — it is a trailing newest-first view, so the §4 `build(panel, t, window)` interface reconciles to a PanelView (row 0 = current date, PIT structural) + a trailing `window` for the per-date factor computations.

**§0-G** one cost surface: P4-9's κ + P4-10's capacity √-impact read the SAME `exec::ExecutionSimulator` (no second cost path).

**§0-H** group-neutralize (P4-8) reuses the DSL `CsDemeanG` per-group-demean semantics + IndClass map.

**P4-7 reuses the Ledoit-Wolf closed-form** already implemented and corrected in P4-4 (`combine/combiner.hpp` `ledoit_wolf_intensity`, canonical 1/T² normalization) for the factor covariance `F`.

**The fit/apply firewall (§3.1)** proven in 4a extends to the factor model (fitted on a trailing window, applied OOS; P4-10 proves truncation-invariance).

Realistic scope for this sprint:

1. **P4-6** — Factor exposures `X`: OHLCV-derived style factors (Momentum, Volatility, Beta, Liquidity; Size + sector dummies config-gated on optional external inputs), cross-sectionally standardized; `StyleFactor`, `FactorModelConfig`, exposure builder.
2. **P4-7** — Factored covariance `V = XFXᵀ + D`: cross-sectional WLS factor regression, Ledoit-Wolf shrunk `F` (reusing P4-4 LW closed-form), specific variance diagonal `D`, Woodbury inverse; `FactorModel`, `FactorModelBuilder`.
3. **P4-8** — `WeightPolicy` neutralization: wire inert `industry_neutral` (group-demean, `CsDemeanG` semantics + IndClass map), add factor-neutralize (`FactorModel::neutralize`), decay, truncation; all stages default no-op so Phase-2 `WeightPolicy` tests stay bit-identical.
4. **P4-9** — Turnover-penalized risk-aware optimizer: `max αᵀw − λwᵀVw − κ‖w−w_prev‖₁`; recovers the `WeightPolicy` dollar-neutral book at λ=κ=0 bit-for-bit; `OptimizerConfig`, `PortfolioOptimizer`; one cost surface via `exec::ExecutionSimulator`.
5. **P4-10** — Integration/capacity/close: fit/apply firewall truncation-invariance across combiner + factor model, whole-layer determinism hash, capacity curve (`CapacityPoint`, `capacity_curve`), walk-forward combined backtest, bench, 4b close.

---

## Per-unit ledger

| Unit    | Status | Commit     | Notes |
|---------|--------|------------|-------|
| P4b-0   | done   | `ac3b26a`  | scaffold + ledger; RiskScaffold 1/1/0/0 |
| P4-6    | —      | —          | — |
| P4-7    | —      | —          | — |
| P4-8    | —      | —          | — |
| P4-9    | —      | —          | — |
| P4-10   | —      | —          | — |

---

## Phase 4b sprint commits

| Commit    | Unit        | Test counts |
|-----------|-------------|-------------|
| `ac3b26a` | marker (P4b-0) | RiskScaffold 1/1/0/0 |

---

## What Phase 4b proves / Next

_(Fill at sprint close.)_
