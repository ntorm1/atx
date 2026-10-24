# Fixed follow-up after the negative Gaussian pilot

Declared before any new numerical result. The prior 32-outcome pilot and its
10 passes / 17 failures / five typed refusals remain frozen at `b478b81d` with
report `c56eb72e`. This experiment cannot requalify or replace those outcomes.

## Eighteen European semigroup outcomes

Use the exact original `DSPYp` and `DSPYc` stock, strike, maturity, volatility,
rate and continuous yield. Record the original cash events but explicitly remove
them from this counterfactual control; exercise is European. The call keeps its
original strike 858 and the put its original strike 700.

For each side, request the complete Cartesian product:

- Positive stock intervals N = 256, 512, 1024.
- Repeated propagation count M = 1, 32, 128.

There are exactly **18 numerical outcomes**, including typed refusals. Each
starts from the intrinsic stock-PWL payoff, repeatedly applies the unchanged
full Gaussian expectation, and re-represents the result on the fixed grid.
There is no exercise projection, cash mapping, density cutoff or model prior.
The no-cash grid follows the approved fixed formula with cash sum zero and
contains zero, spot and strike as exact anchors. Use constant `dt=T/M`; M is a
power of two, so repeated-step timing introduces no independently rounded
event-date subtraction. Propagate the linear tail as before.

Each outcome gets the original finite budget: 8192 maximum PWL knots,
2^24 constructed knots, 2^20 expectations and 2^28 interval contributions.
No larger fallback budget or new resolution is allowed after seeing results.
The independent reader computes European Black76 directly from the recorded
model, using Python normal-CDF formulas without any ATX pricing call. It reports
signed residuals and both axes; this is a representation diagnostic, not a new
American reference or speed test.

Before the 18 outcomes, unit controls check repeated constant and linear moments
on N64 with M1/32/128 (six companion checks), an unchanged one-step European
identity, finite/resource rejection and exact work counts. The six cheap moment
checks are tests, not additional American or pilot accuracy observations.

## Five exact rounded-crossing diagnostics

Rerun only these previously refused requests, with an optional read-only
observer at the existing typed refusal:

| Original ID | N | Time level |
|---|---:|---:|
| A1p | 256 | 1 |
| A1p | 256 | 2 |
| E64c | 256 | 0 |
| E64c | 512 | 0 |
| E64c | 1024 | 0 |

The observer copies the pre-max stock/value knots, linear tail, strike/side,
finite/tail branch, neighboring stock endpoints, already computed endpoint
differences and rounded intersection. It introduces no new evaluation, changes
no arithmetic or status, and is absent by default. Observation failure marks
the diagnostic invalid while retaining the original pricing refusal.

Require exact original typed result and all original work counters for each of
the five requests. Native tests compare observer absent/present status and work,
including a synthetic finite crossing and infinite-tail refusal. The Python
reader reconstructs the crossing of the captured binary64 endpoint-difference
lines with Decimal precision 100 and 200; disagreement invalidates a conclusion.
It reports distance to endpoints/ULPs and whether the exact intersection has a
distinct nearest binary64 representation. Any chord-excess estimate describes
only those captured lines, not a bound on a full option price.

No epsilon merge, snapped knot, invented time value, altered exercise decision
or implicit rounding allowance is implemented. Any later treatment requires a
separate numerical contract and explicit error accounting.

## Evidence contract

Export all 50 original inputs, 18 European controls and all five crossing
attempts. Keep the existing 150-grid/32-pilot receipts unchanged. The independent
reader joins raw model/event bits to the frozen reference manifest and the five
baseline failures to the original pilot hash. Record exact source, executable,
native output, reader and input hashes. Preserve every outcome and every failed
control. No elapsed-time benchmark or promotion claim belongs to this run.
