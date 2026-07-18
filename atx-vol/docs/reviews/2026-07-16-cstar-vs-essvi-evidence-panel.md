# CStar (C16M) vs eSSVI — Evidence Panel & Ladder Recommendation

**Date:** 2026-07-16 (synthetic A/B); **real-OPRA validation added 2026-07-17, Sprint I step S5.**
**Sub-sprint:** S (Surface engine) — task S5
**Author:** Agent S (synthetic); Sprint-I panel agent (real-OPRA)
**Scope:** isolated R&D. CStar is NOT on the production fit path; this doc informs
the Sprint-I ladder decision only. No `curve_selector.cpp` changes were made.

## Recommendation — **KEEP R&D, do NOT include in the ladder yet** (real-OPRA evidence)

> **Updated 2026-07-17 after the real-OPRA run.** The synthetic A/B (below) showed
> a clean CStar win on planted modal structure. The **real Databento OPRA panel
> refutes that as a drop-in result**: on genuine boards CStar does **not** dominate
> eSSVI. The verdict remains **KEEP R&D — not kill, not include** — but the path to
> inclusion is narrower and now has hard blockers.

Real-OPRA headline (SPY + 10 mega-caps × 3 dates, 636 expiries, ~29k paired obs):

- **CStar vol-RMSE is ~2.0–2.3× WORSE than eSSVI** (cohort 0.0258 vs 0.0110; SPY
  0.0325 vs 0.0218). As a drop-in it **fails the sprint's `vol-RMSE ≤ prior`
  economic gate.**
- **Admission collapses on the dense index:** CStar covers only **9/81 SPY
  expiries (89% coverage gap)**; the rest are `butterfly-inadmissible after
  revert-to-seed`. On single names the gap is 14%. This is the opposite of the
  synthetic story, where SPY-like was a clean 100%.
- **It genuinely improves the PRICE fit where it admits:** in-band 42.8% → 70.9%
  and price-χ² 34.4 → 21.5 on the cohort (and 3.6% → 15.6%, χ² 1711 → 1068 on the
  9 admitted SPY slices). CStar trades vol-space smoothness for tighter price
  residuals — better χ²/in-band, worse IV-RMSE.
- **New correctness smell:** CStar raised **3 butterfly-arb flags on SPY + 1 on
  the cohort** and 5 negative-min-Roper-g slices, where eSSVI raised **zero**.
- **Cost:** fit wall 5–8× eSSVI (12–16 ms vs 1.5–3.2 ms/slice).

Net: not a drop-in (fails the vol-RMSE gate), fragile admission on the index it
was designed for, and it introduces arb flags eSSVI does not. But its consistent
price-χ²/in-band improvement on **liquid single names** (GOOGL, META, MSFT, JPM,
XOM) keeps a **narrow, single-name-only** R&D path alive. Blockers before any
ladder entry are now concrete (see "Blockers" below).

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
3. **Synthetic-only at the time of writing (RESOLVED at Sprint I — see the
   real-OPRA section below).** The 25-name recovery cohort and SPY snapshot
   payloads live outside the repo. The per-expiry extraction seam was solved at
   Sprint I **entirely inside the panel tool** with read-only reuse of the shipped
   public loader/session APIs (`load_opra_cbbo_parquet` → `data_install` →
   `Universe` chains; forward/carry per expiry from a read-only `VolaSession`) — no
   Sprint-R TU edits were needed. **The synthetic win did not reproduce on real
   data.**

## Real-OPRA validation (Sprint I, run 2026-07-17)

The panel's `--real` mode was generalized (inside `examples/cstar_panel.cpp` only)
to discover and drive real Databento OPRA cbbo-1m snapshots through the supported
public pipeline: `load_opra_cbbo_parquet` → `data_install` → per-expiry `Universe`
`Chain`s, with the term forward/carry per expiry taken from a read-only
`VolaSession` (Fast preset). Both engines are scored on the **identical**
`build_observations` set per expiry (raw-mid inversion), so the comparison
isolates parametrization shape fidelity, not the de-Am pipeline.

**Which boards ran vs. the PLAN's intended cohort.** The PLAN named SPY + a
25-name *recovery cohort* (`CZR, RPRX, RXT, ROIV, HST, FTV, EQH, IBN, TSLQ, JHX,
MNTS, OKLL, EQX, SIDU, HIMX, GFI, DGXX, VNET, ESI, BFAM, PCOR, HTHT, IBRX, ALHC,
GGG`). **That cohort's OPRA payloads are NOT on disk** — it is **data-gated**. The
only real OPRA snapshots present are the SPY-dispersion collection at
`C:\atx-data\spy-dispersion\opra\<SYM>\<date>.parquet`: **SPY + 10 mega-caps
(AAPL, AMZN, AVGO, GOOGL, JPM, LLY, META, MSFT, NVDA, XOM), dates 2026-01-02 /
-05 / -06.** The run below substitutes that available large-cap set (via
`--symbols`); the steep small-cap recovery cohort remains unvalidated pending its
data. Command:

```
atx-vol-cstar-panel --real --data-root C:/atx-data/spy-dispersion \
  --symbols AAPL,AMZN,AVGO,GOOGL,JPM,LLY,META,MSFT,NVDA,XOM --spy-limit 0
```

### Grand aggregates (obs-weighted over CStar-admitted, identical-obs slices)

| group | boards | expiries | eSSVI ok | CStar ok | coverage gap | paired obs | eSSVI RMSE | CStar RMSE | eSSVI band | CStar band | eSSVI χ² | CStar χ² | CStar arb | modal-win | eSSVI wall | CStar wall |
|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| **SPY** (dense index) | 3 | 81 | 81 | 9 | **88.9%** | 700 | **0.0218** | 0.0325 | 3.6% | 15.6% | 1711 | 1068 | **3** | 0/9 | 3.18 ms | 16.15 ms |
| **Cohort** (10 mega-caps) | 30 | 555 | 555 | 478 | 13.9% | 28 582 | **0.0110** | 0.0258 | 42.8% | 70.9% | 34.4 | 21.5 | 1 | 116/478 | 1.48 ms | 12.06 ms |

*modal-win = slices where CStar vol-RMSE < 0.8× eSSVI. Walls best-of-3, concurrent
host (indicative). `gneg` (min-Roper-g < 0): SPY 3, cohort 2.*

### Per-board (single date shown per name for brevity; all 3 dates consistent)

| board | exp | CStar ok | eSSVI RMSE | CStar RMSE | eSSVI band | CStar band | eSSVI χ² | CStar χ² | CStar arb | modal-win |
|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| SPY / 2026-01-06 | 28 | 2 | 0.0258 | 0.0316 | 1.7% | 20.3% | 2751 | 57 | 0 | 0/2 |
| NVDA / 2026-01-02 | 19 | 15 | 0.0225 | **0.0655** | 10.6% | 44.7% | 153 | 68 | 0 | 0/15 |
| AMZN / 2026-01-06 | 20 | 18 | 0.0092 | **0.0256** | 23.6% | 53.4% | 40 | 32 | 0 | 3/18 |
| AVGO / 2026-01-02 | 18 | 18 | 0.0046 | **0.0208** | 61.6% | 69.5% | 6.0 | 4.2 | 1 | 5/18 |
| LLY / 2026-01-02 | 17 | 16 | 0.0053 | 0.0158 | 91.3% | 90.4% | 0.36 | 0.79 | 0 | 3/16 |
| META / 2026-01-02 | 19 | 17 | 0.0112 | 0.0187 | 34.5% | 73.3% | 22.2 | 5.4 | 0 | 7/17 |
| GOOGL / 2026-01-06 | 19 | 18 | 0.0082 | 0.0108 | 33.1% | **78.2%** | 28.3 | **2.4** | 0 | 6/18 |
| MSFT / 2026-01-05 | 19 | 18 | 0.0047 | 0.0055 | 47.5% | 79.3% | 9.1 | 3.4 | 0 | 8/18 |
| JPM / 2026-01-02 | 17 | 10 | 0.0065 | 0.0089 | 67.8% | 87.9% | 2.6 | 2.9 | 0 | 6/10 |
| XOM / 2026-01-02 | 16 | 9 | 0.0066 | **0.0060** | 70.5% | 93.9% | 4.4 | 0.6 | 0 | 5/9 |

**Reading the real data.**
- **Vol-RMSE regresses everywhere** except a few short-dated single-name slices —
  worst on high-vol steep names (NVDA 0.0225 → 0.0655, ~2.9×; AVGO ~4.5×; AMZN
  ~2.5×). eSSVI's smooth backbone stays closer to the raw-mid IV; CStar's modal
  freedom deviates in IV, especially in the wings.
- **Price fit (χ² / in-band) improves** on most liquid single names — CStar drives
  prices inside the half-spread more often (band +20–45 pts on GOOGL/META/MSFT/
  JPM/XOM) at *lower* χ². The two facts coexist: tighter price residuals, looser
  IV-RMSE (large IV error at low-vega wings costs RMSE but not price-band).
- **SPY admission collapse (89% gap) is the headline risk.** The very chains CStar
  is designed for (dense index) are where it refuses to admit — and where it does,
  it is worse (vol-RMSE and 3 arb flags) than eSSVI.

**Harness caveat.** This is a *standalone* raw-mid European-inversion harness, not
the production surface (no de-Americanization, escrowed-dividend forward, or robust
weighting). Absolute fit quality here is **below** the blessed pipeline (note the
high SPY χ²/low in-band for BOTH engines at long T). The **eSSVI-vs-CStar delta is
the deliverable**; the absolute numbers are not a statement about production eSSVI.

## Blockers before any ladder entry (from the real data)

1. **Vol-RMSE regression.** CStar must not regress vol-RMSE vs eSSVI on real boards
   (sprint gate: `vol-RMSE ≤ prior`). Today it is ~2× worse. Root-cause the wing
   IV deviation (modal basis over-freedom at low vega) before inclusion.
2. **Admission fragility.** The `butterfly-inadmissible after revert-to-seed` path
   drops 89% of SPY expiries and introduces arb flags where it does admit. The
   admission/projection must be robust on dense real chains, not just synthetic
   arb-free bases.
3. **Cost.** 5–8× fit wall stands; a whole-universe CStar fit remains off the table
   — any entry is selective (single-name, structure-gated), eSSVI elsewhere.

## Decision

**KEEP R&D — do not include, do not kill.** The synthetic modal-feature win is
real but does **not** generalize to a drop-in advantage on real OPRA: vol-RMSE
regresses, index admission collapses, and new arb flags appear. It is **not** a
kill — CStar's consistent price-χ²/in-band gains on liquid single names
(GOOGL/META/MSFT/JPM/XOM) are a genuine, reproducible signal worth a **narrow,
single-name-only** research track once blockers 1–2 are closed. The Sprint-I
dispatcher/user owns the ladder call; this doc supplies the evidence and the
blockers, and makes **no** `curve_selector.cpp` / production-wiring change.
