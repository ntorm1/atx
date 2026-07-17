# CStar (C16M) vs eSSVI — Evidence Panel & Ladder Recommendation

**Date:** 2026-07-16
**Sub-sprint:** S (Surface engine) — task S5
**Author:** Agent S
**Scope:** isolated R&D. CStar is NOT on the production fit path; this doc informs
the Sprint-I ladder decision only. No `curve_selector.cpp` changes were made.

## Recommendation — **KEEP R&D** (conditional path to include-in-ladder)

CStar is now *correct* and *fast enough to evaluate*, and it delivers a real,
differentiated accuracy win on smiles with local (modal) structure — its
designed niche. It is **not yet** ready for a blanket ladder entry: the
per-slice fit wall is ~60–90× eSSVI's, and its admission gate is more
conservative on steep skew. The right next step is a **selective** ladder entry
(CStar only where a board's local structure justifies its cost) validated on a
**real-OPRA** panel at Sprint I — not a kill, not an unconditional include.

## What changed this sub-sprint (why CStar is now evaluable)

Before this sub-sprint CStar was disqualified from any production discussion by a
P1 correctness defect (REVIEW §6.1 #11): the no-arb projection returned `Ok()`
on arb-violating slices. That is fixed:

- **S1 (correctness):** reversed `c2` projection bisection fixed; post-projection
  no-arb validation now *propagates* failure (`Unavailable` / `OutOfRange`)
  instead of a silent `Ok`; raw-shape validity separated from the public variance
  floor; the finite-difference butterfly gate (~8 digits lost to cancellation)
  replaced with a closed-form analytic `w''`. **Zero false butterfly-arb flags**
  on the fixture set (see the `arb` column below — all 0, `ming` > 0).
- **S2 (accuracy):** analytic CStar Jacobian + fused w/gradient pass; fixed-cap
  Eigen (no per-LM-iteration heap alloc).
- **S3 (speed):** table-driven, division-free no-arb projection with incremental
  group damping — **~14.8×** on the arb-projection microbench (the Vola Dynamics
  C-family differentiator cost).

## The panel

Both engines were fit to an **identical** observation set per board: the
production per-slice eSSVI calibrator (`essvi_fit_slice`) and the CStar C16M
calibrator (`cstar_calibrate_slice`, seeded from that same eSSVI fit). Tool:
`atx-vol/examples/cstar_panel.cpp` (standalone; `atx-vol-cstar-panel`).

Metrics per board: fit wall (best-of-3), vol-RMSE vs the market IV, in-band
fraction (model price within the quote half-spread), price χ² (mean squared
half-spread-normalised residual), and butterfly-arb-flag count.

| board | regime | eSSVI wall | eSSVI RMSE | eSSVI band | eSSVI χ² | eSSVI arb | CStar wall | CStar RMSE | CStar band | CStar χ² | CStar arb |
|---|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| SPY-like | dense-index (pure eSSVI) | 0.10 ms | 0.0000 | 100.0% | 0.00 | 0 | 6.80 ms | 0.0009 | 100.0% | 0.06 | 0 |
| cohort-steep | sparse-smallcap (pure eSSVI) | 0.09 ms | 0.0000 | 100.0% | 0.00 | 0 | 6.98 ms | 0.0018 | 100.0% | 0.01 | 0 |
| **bumpy-smile** | **modal feature** | 1.26 ms | **0.0028** | **59.1%** | **1.23** | 0 | 7.27 ms | **0.0005** | **100.0%** | **0.00** | 0 |
| wing-heavy | fat-tails (pure eSSVI) | 0.10 ms | 0.0000 | 100.0% | 0.00 | 0 | 8.86 ms | 0.0010 | 100.0% | 0.03 | 0 |

Mean vol-RMSE: eSSVI 0.00071, CStar 0.00106. Numbers are provisional
(concurrent host; walls best-of-3).

### Reading the table

- **No regression on smooth smiles.** Three of four boards are *pure arb-free
  eSSVI* smiles, which eSSVI recovers exactly (RMSE 0) by construction. CStar
  matches them within ~9–18 bp of vol at **100% in-band, 0 arb flags** — it does
  not degrade the easy cases.
- **Decisive win where it is designed to win.** On `bumpy-smile` (an arb-free
  eSSVI base + a localised IV bump an eSSVI 3-parameter backbone cannot
  represent), CStar's modal basis captures the feature: **vol-RMSE 0.0028 →
  0.0005 (5.6× lower), in-band 59.1% → 100%, χ² 1.23 → 0.00.** This is exactly
  the Vola Dynamics "C\* nested curve" value proposition — local shape fidelity
  on structured (SPX/SPY-class) chains.
- **Zero false butterfly flags.** Every `arb` count is 0 and every `ming`
  (min Roper g) is ≥ 0 — the S1 analytic gate does not spuriously flag
  arbitrage, closing the SPRINT DoD "zero FD-noise butterfly false flags" row for
  the CStar family.

### Costs / caveats (the reason it stays R&D)

1. **Fit wall ~60–90× eSSVI.** CStar's price-domain IRLS-Huber block LM
   (BASE → MODAL → FULL × 4 outer) is inherently heavier than eSSVI's 3-parameter
   cube LM. S2/S3 cut the internal projection/Jacobian cost, but ~7–9 ms/slice
   vs ~0.1 ms/slice is prohibitive for a *whole-universe* cycle unless CStar is
   gated only to boards whose structure earns it.
2. **More conservative admission on steep skew.** The calibrator's independent
   Durrleman accept gate rejects steep-skew slices (observed at eSSVI rho ≈ −0.60:
   `Unavailable: butterfly-inadmissible after revert-to-seed`) that eSSVI still
   serves. On the recovery cohort (steep small-cap skews) this is a coverage risk
   that must be measured before any ladder entry.
3. **Synthetic data.** The 25-name recovery cohort and SPY snapshot payloads live
   outside the repo and require the Sprint-R per-expiry slice-extraction plumbing
   (`calib.cpp` / `prepared_fitting`, another engineer's TUs) to turn a real board
   into the per-expiry `Chain` this tool fits. This panel drives the **real
   calibrators** over **representative synthetic regimes**; the real-OPRA run on
   SPY + the 25-name cohort (`CZR, RPRX, RXT, ROIV, HST, FTV, EQH, IBN, TSLQ, JHX,
   MNTS, OKLL, EQX, SIDU, HIMX, GFI, DGXX, VNET, ESI, BFAM, PCOR, HTHT, IBRX, ALHC,
   GGG`) is a Sprint-I task once that seam is available.

## Decision criteria for Sprint I

**Include-in-ladder** (selective) if the real-OPRA panel shows, on genuine
structured chains (SPX/SPY and the denser cohort names), a material in-band /
χ² / vol-RMSE improvement over eSSVI *and* the coverage loss from the steep-skew
admission gate is bounded (e.g. < 5% of served boards, with eSSVI fallback).
Entry form: CStar candidate enabled only for high-liquidity boards with detected
local structure (a selector predicate), eSSVI elsewhere — never a blanket
whole-universe CStar fit given the fit-wall cost.

**Kill** only if the real-data panel shows no in-band/χ² improvement over eSSVI
on real structured chains (i.e. the synthetic modal-feature win does not
reproduce on real microstructure).

**Otherwise keep R&D** — the current state: correct, fast where it counts,
evidenced on synthetic structure, pending real-data confirmation.
