# Sprint S6 — Cost Calibration & Capacity (sprint spec)

**Status:** ⏳ proposed (not open). Calibration core has **no Phase-4 dependency** (opens with S1/S2);
mega-alpha capacity needs P4.
**Roadmap:** [`ROADMAP.md`](ROADMAP.md) · **Discipline:** [`../docs/sprint.md`](../docs/sprint.md)
**Grounded in:** [`../../research/renaissance-technologies-systems-deep-dive.md`](../../research/renaissance-technologies-systems-deep-dive.md)
§4 (cost = the strategy; at a 50.75% hit rate a fraction of a bp of cost error flips the sign; concave
√-impact; Almgren-Chriss temp/perm split; the throttling effect; capacity) +
[`../../research/worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md) §8
(linear cost `C~1/T`; turnover-aware fitness; returns scale with volatility not turnover).

---

## Why this sprint

RenTech's "secret weapon" is **cost fidelity**. The whole P&L of a weak-signal book lives in the last
fraction of a basis point of cost accuracy — understate impact by 0.3 bp and a profitable backtest is a
break-even reality. p0 shipped the √-impact *model* but with **uncalibrated 2001-era defaults** (`δ=0.5,
Y=1.0, γ=0.314, …`); p0 itself flagged calibration as deferred Phase-5 work.

S6 makes the cost model *true*: fit the coefficients to realized fills, get the temporary-vs-permanent split
right (so temporary cost never leaks into the forward mark), compute the **capacity curve** (the AUM at which
impact erodes net edge to zero — RenTech "report capacity, not just return"), and feed cost back into the
factory's and combiner's objectives so they price turnover correctly. An honest factory needs an honest cost
model, or it mines high-turnover alphas that look great gross and lose money net.

---

## Scope — units

### S6.0 — Marker + ledger
Open `sprint-6-progress.md`, freeze scope, base SHA.

### S6.1 — Cost-coefficient calibration harness
Fit the `ExecutionSimulator` coefficients — √-impact exponent **δ** (research range 0.45–0.65), impact
scale **Y**, permanent-impact **γ**, slippage/spread **η** — to **realized fills** (or a reference impact
dataset), via robust regression of observed slippage on `σ·(Q/ADV)^δ`. Report fit quality (R², residual
distribution) so the calibration is *auditable*, not asserted. The fit is itself a **fitted object** under
the fit/apply firewall — calibrated on a trailing window, applied forward (no look-ahead in the cost model).
*atx-core (Pattern B): L7 robust regression / nonlinear least-squares.*

### S6.2 — Almgren-Chriss temporary/permanent split refinement
Sharpen the temp-vs-perm decomposition p0 already structures (temp → fill price, perm → mark shift): ensure
**no temporary-cost leakage into the forward mark** (temp reverts after the trade; only perm persists), and
fit the split ratio. This is what lets the cost model correctly **throttle** — decline trades whose
microscopic edge doesn't clear their cost (RenTech: capable of second-scale trades, averages ~2 days, because
cost scrutiny declines the rest).

### S6.3 — Capacity curve
Compute, per alpha and per mega-alpha, the **net-edge-vs-AUM** curve and the capacity point (AUM where
√-impact drives net edge to 0). Per-alpha uses `AlphaStreams`; per-mega-alpha runs the P4 combined book
through the calibrated sim at increasing notional. Capacity becomes a first-class reported metric alongside
Sharpe/fitness (and a future allocation input for S7).

### S6.4 — Cost-aware fitness / gating hook
Wire calibrated cost + capacity into **S3's fitness** (turnover/cost budget so the factory stops rewarding
high-turnover alphas that lose net) and into the **P4 combiner/optimizer** turnover penalty (the `κ‖w−w_prev‖₁`
term now uses calibrated, not default, cost). Closes the loop: discovery and combination both price cost
honestly.

### S6.5 — Borrow / short-financing accrual + close
Add the **short borrow-cost accrual** p0 left as a hook (a per-day financing charge on short positions),
completing the net-cost picture for a dollar-neutral (half-short) book. Close ceremony.

---

## Exit criteria

- Calibration recovers known coefficients on a **synthetic** fill set (inject `δ,Y,γ`; the harness fits them
  back within tolerance — the differential proof the fit is correct), and reports honest fit quality on a
  realistic set.
- Calibrated coefficients are a fit/apply-firewalled object: trained on a trailing window, applied forward,
  truncation-invariant (no cost-model look-ahead).
- Temp/perm split has **zero** temporary-cost leakage into the forward mark (proven: a round-trip's temporary
  cost does not appear in any later mark).
- Capacity curve is monotone-decreasing in AUM and identifies a finite capacity point on a fixture with a real
  edge; per-mega-alpha capacity runs the P4 book through the calibrated sim.
- Cost-aware fitness demonstrably down-ranks a high-turnover-net-losing alpha vs a lower-turnover-net-winning
  one (the WQ §8 thesis, calibrated).
- `/W4 /permissive- /WX`, clang-tidy + clang-format clean; test file per unit.

## Invariants this sprint must prove

No look-ahead (calibrated coefficients are fit-on-trailing/apply-forward; the cost model never peeks),
honest cost (the entire point — now *calibrated* + *capacity-bounded*), determinism (fitted coefficients are
byte-stable given the same data; capacity curve replays identically), differential correctness (recover
injected synthetic coefficients).

## Dependencies

- **Upstream:** p0 Phase-2 `ExecutionSimulator` (the model being calibrated), Phase-3 `AlphaStreams`
  (per-alpha capacity), P4 combined book (per-mega capacity). The calibration **core** (S6.1/S6.2) needs only
  p0 — opens with S1/S2.
- **atx-core (Pattern B edge):** L7 robust regression / nonlinear least-squares (S6.1).

## Explicitly NOT in this sprint

No live market-impact measurement (calibrate to historical fills / reference data, not live — v2 is a
simulator, anti-roadmap #1). No limit-order-book microstructure simulation (the exec sim models fills +
impact + capacity, not a matching venue, anti-roadmap #2). No intraday cost model (daily-bar book; intraday
is a p0 residual / later module).

## Baton → next

S6 makes cost *true* and capacity *known* — so S3 mines net-profitable (not gross-mirage) alphas, S5's
combiner penalizes turnover honestly, and S7 can size and allocate the book against a real capacity ceiling.
