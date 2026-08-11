# SPY option-structure selector — ML daily strategy choice

**Goal.** Each trading day pick ONE of four delta-neutral SPY option structures to
hold to the next session; maximize realized 1-day delta-neutral PnL:

| id | structure | exposure |
|----|-----------|----------|
| S1 | long 1M ATMF straddle | long gamma |
| S2 | long 1Y ATMF straddle | long vega |
| S3 | short 1M + long 1Y (vega-matched calendar) | long forward vol |
| S4 | long 1M + short 1Y | short forward vol |

**Normalization.** Every straddle is sized to a constant structure $vega
(`vega_target`, default 1000 in dP/dσ units). With vega-matched legs the
calendar PnLs are exact combinations: `p3 = p2 − p1`, `p4 = p1 − p2`. So the
label pipeline only needs two base PnL series (`pnl_front`, `pnl_back`); the
four-way choice derives in the model layer. The vega-normalized 1M straddle is
gamma-dominant (vega ∝ √T), which is precisely the "long gamma" trade.

**PnL definition (label).** Fresh ATMF straddles resolved at day-t surface,
legs pinned to absolute expiry `now_ts + T·year`; marked on day t+1 surface
with T recomputed from the same expiry:

```
pnl = Σ qty·(P1 − P0) − entry_delta·(S1 − S0)      (frictionless, per-share units)
```

Delta hedge fixed at entry (no intraday rebalance) — matches "delta-neutral pnl
from today to tomorrow". Theta decay is inside the mark-to-mark difference.

**Data.** `C:/atx-data/surface-db-r2/spy-{2019..2026}` — 1,787 daily fitted SPY
surfaces (convex-dense), 2019-01-02 → 2026-07-31, spot/r inside each archive's
`PricingContext`.

## Components (atx-vol)

1. `include/atx/vol/structure_panel.hpp` + `src/structure_panel.cpp`
   - `resolve_atmf_straddle(ps, T, vega_target, sign)` → `ResolvedStructure`
     (legs pinned to expiry ts, entry value/greeks). Reuses `PricedSurface`
     `forward_at/fair_value/greeks`.
   - `delta_neutral_pnl(structure, mark_surface)` → 1-day $PnL.
   - `StructurePanelBuilder` — streaming: push consecutive daily surfaces,
     emits completed `PanelRow` for the previous day once the next-day mark is
     known. Accumulates spot/IV history internally for windowed features.
     Fail-soft: unpriceable day ⇒ `pnl_valid=false` row, counted, never
     fabricated.
2. Feature columns (entry-day only, no lookahead), built on existing
   `analytics.hpp` primitives (`atmf_vol`, `skew_curvature`, `risk_reversal`,
   `butterfly`, `forward_vol`) + `realized_vol.hpp` (CtC on the spot mirror):
   ATM IV {1M,3M,1Y}, term slope, forward vol 1M→1Y and its spread to 1M IV,
   skew/curvature (k_ref = σ√T) at 1M/1Y, RR/BF 25Δ at 1M/1Y, RV {5,21,63},
   IV−RV spreads, spot returns {1,5,21}, ΔIV {1,5,21}, Δslope {1,5},
   vol-of-vol (σ of ΔIV_1M, 21d), trailing VRP mean (63d), unit-structure entry
   gamma/theta.
3. `tools/spy_structure_panel_main.cpp` — CLI: `--db-prefix --year-lo --year-hi
   --symbol --out panel.tsv`; loops yearly SurfaceDb roots ascending, feeds the
   builder, writes TSV.
4. Python `scripts/structure_selector_train.py` — walk-forward model:
   regression of (p1, p2) per day (labels for S3/S4 derive), pick
   argmax{p̂1, p̂2, p̂2−p̂1, p̂1−p̂2}; baselines: each always-on, oracle, random.
   Purged walk-forward (embargo ≥ 1 day; labels are 1-day, so overlap is
   minimal). Metrics: total PnL, ann. Sharpe, hit-rate vs oracle, regret,
   switch turnover.

## Test plan (tests/structure_panel_test.cpp, eSSVI make_surface fixture)

- resolve: K = F(T), Σqty·vega = ±target, short sign flips.
- pnl: unchanged surface +1d ⇒ negative (theta) for long straddle;
  spot gap +3% flat vols ⇒ front straddle profits (gamma > theta);
  parallel vol bump ⇒ both structures profit.
- expiry passed at mark ⇒ error, not a fabricated mark.
- builder: first push → no row; second push → completed row keyed to day 1;
  non-ascending key rejected; RV/momentum NaN until window filled, then finite.

## Status log

- 2026-08-11: recon + design complete; implementation start.
