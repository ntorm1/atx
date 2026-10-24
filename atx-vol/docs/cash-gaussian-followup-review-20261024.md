# Gaussian cash reference: frozen representation diagnosis

The complete predeclared follow-up shows that repeated stock-PWL representation
creates large pricing bias even without cash or exercise. All six one-step
European prices match independent Black76 within 1.43e-13 USD; all twelve
multi-step controls miss 0.001 USD, with errors from +0.08293 to +5.81527 USD.
The five original exercise-crossing refusals reproduce exactly. Each exact
crossing of the captured endpoint-difference lines has an endpoint as its
nearest binary64 representation.

This is diagnostic evidence against the current repeated-PWL representation.
It is not a new American reference, an error bound, a timing result or a change
to the original 50-case, 150-grid or 32-pilot results. No node/time parameter,
exercise rule or rounding threshold was changed after observing these values.

## Frozen experiment and receipts

- Declaration: `ed4139038bca27b3a8a175193f3b290031ad9bae`,
  [fixed follow-up plan](../bench/cash-gaussian/followup-plan.md).
- Executed clean source: `c2a784d045237dad42af0f77abb5ec26da8069ad`,
  after independent root and acceptance source reviews.
- Executable SHA256:
  `d14c6ea959aa1f699b2ccd619355319f156afd344d02990605cf835af20fc699`.
- Native JSON SHA256:
  `e6f2a468b5a78df3a970b625f54cf68569db498da6b0c9e1bdac4d57242eeb91`.
- Independently verified summary SHA256:
  `5f5203824db9c5b51b7280a96309f39bd1200529df51b365fbaebcd655f68744`.
- Receipt SHA256:
  `6d2f03850c7741522ae497d8170da3ba658ac0bd7e4e54ca679820ecdf9c510a`.
- Evidence directory:
  `C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/calendar/cash-gaussian-followup-v1`.

There was one native execution, with exit code zero. The independent reader
verified all 50 original model/event records, all 18 European outcomes and all
five crossing attempts. It also rechecked the immutable previous 32-outcome
pilot against its original reference manifest. No reference price or baseline
output enters the native executable.

The initial archive correctly pinned the clean source commit and copied/hashed
the executable before execution. Its optional source-hash enumeration used two
nonexistent filenames, `operators.hpp/.cpp`, instead of `stock_pwl.hpp/.cpp`.
PowerShell logged those errors and continued. The original archive receipt is
retained unchanged; `source-hash-supplement.json` supplies a complete enumeration
with the same HEAD and clean tree checked during this single run. The final
receipt records this provenance limitation explicitly. There was no numerical
rerun or replacement of failed evidence.

Before execution, 26 native control groups and the owning PCH-off checks passed.
The reader's three controls cover complete/refused outcomes and malformed
model/work/crossing evidence. A peer-found consumed-value/grid lifetime defect
was fixed before any follow-up values were generated; its pre-fix ordinary-
allocator test happened to pass, and is not described as runtime-red evidence.

## All 18 European controls

The original DSPY spot, strike, maturity, volatility, rate and continuous yield
are unchanged. Cash events are explicitly removed and exercise is European.
This counterfactual control is separately labeled; it is not the original cash
contract. The put retains strike 700 and the call strike 858. Each propagation
uses constant `dt=T/M`, the original fixed no-cash grid and every represented
Gaussian interval including the infinite linear tail.

Black76 reference values are 83.15421343365414 USD for DSPYp and
157.1209856122798 USD for DSPYc. Errors below are native minus Black76, in USD.
The three columns retain all nine declared outcomes for each side.

| Original ID | Positive intervals N | M=1 error | M=32 error | M=128 error |
|---|---:|---:|---:|---:|
| DSPYp | 256 | +5.684342e-14 | +1.203334137344 | +4.855151290740 |
| DSPYp | 512 | +5.684342e-14 | +0.331373843384 | +1.352457468797 |
| DSPYp | 1024 | +5.684342e-14 | +0.082929998003 | +0.339373793334 |
| DSPYc | 256 | -1.421085e-13 | +1.444054222011 | +5.815271951290 |
| DSPYc | 512 | -1.421085e-13 | +0.387195606731 | +1.578476851618 |
| DSPYc | 1024 | -1.421085e-13 | +0.097553265084 | +0.398950610960 |

All 18 computations complete; this success count describes numerical completion,
not accuracy qualification. Six one-step controls pass the predeclared 5e-10 USD
check. None of the twelve repeated controls reaches 0.001 USD. There are zero
resource or numerical refusals in this European population.

At fixed M, doubling N from 512 to 1024 gives observed error-reduction orders
1.9985/1.9946 for the put at M32/M128 and 1.9888/1.9843 for the call. These are
empirical two-resolution ratios, not an asymptotic convergence proof or a
Richardson correction. At N1024, M128/M32 error ratios are 4.0923 for the put and
4.0896 for the call; the number of intermediate re-representations increases
from 31 to 127, a ratio of 4.0968.

The exact GBM semigroup would give the same European value for every M.
Consequently, repeated representation/integral roundoff is sufficient to create
this large drift: neither cash mapping nor obstacle projection is necessary.
The smooth-space scaling, nearly proportional repetition growth and accurate
one-step controls support interpolation accumulation as the dominant mechanism.
They do not separately prove a global tail-error bound or exclude every
floating-point contribution.

The mechanism is visible in the source. `expectation` integrates the supplied
PWL, while `repeat_expectation` replaces each smooth propagated function by
straight segments through the fixed grid. A straight chord over a convex smooth
section lies above that section. Repeating that representation can repeatedly
inject positive bias. The finite-grid tail and later nonconvex cash functions
prevent turning that local observation into an unconditional global theorem
for the entire American engine.

Actual European work is **438,893,788 interval contributions**, 580,888 queries,
580,924 constructed PWL knots and 966 completed steps/zero-state queries. For
each side the interval counts are:

| N | M=1 | M=32 | M=128 |
|---:|---:|---:|---:|
| 256 | 518 | 2,088,058 | 8,552,698 |
| 512 | 1,030 | 8,238,970 | 33,750,010 |
| 1024 | 2,054 | 32,730,490 | 134,083,066 |

The identical counts across sides follow from their grid sizes and complete
interval traversal. These are work counts only. No wall-time or HFT throughput
conclusion follows from this run.

## All five original crossing refusals

Each attempt retains the original `OutOfRange: rounded exercise crossing`,
error code 2, absent price and every original work counter. The observer copies
the PWL and already-computed operands without changing any numerical decision.
The reader independently reconstructs the PWL's rounded slope, subtraction and
fused evaluation, then computes the endpoint-difference-line intersection at
100 and 200 decimal digits. Both precisions agree.

| ID / N / time level | Endpoint nearest to exact line crossing | Distance in that endpoint's ULP | Captured-line chord excess, USD |
|---|---|---:|---:|
| A1p / 256 / 1 | high: 152.43369368465858 | 0.001706177486 | 3.952525e-323 |
| A1p / 256 / 2 | high: 134.8745158332954 | 2.809924e-7 | 1.729230e-322 |
| E64c / 256 / 0 | low: 5 | 0.008519745598 | 7.567054e-18 |
| E64c / 512 / 0 | low: 5 | 0.008519745598 | 7.567054e-18 |
| E64c / 1024 / 0 | low: 5 | 0.008519745598 | 7.567054e-18 |

None has a distinct nearest binary64 intersection. This does not mean there is
no representable number elsewhere inside the interval; moving the crossing to
such a number would change the represented function and needs a separate
numerical contract.

For A1p, the high-end continuation-minus-obstacle values are negative
subnormals, -3.9525251667299724e-323 and -1.7292297604443629e-322, while the low
values are positive 1.3746829635216195e-306 and 3.2311656679713311e-302.
The obstacle is zero at these above-strike stock levels. Thus the captured
represented continuation has a minute negative tail that creates a strict
sign crossing. This observation does not certify a bound on the original
Gaussian integration's error.

For E64c, all three grids have exactly the same captured interval
[5, 6.04784344589177] and differences
+7.5670541815548687e-18 / -1.04784344589177. The refusal occurs after one
diffusion, one post-event exercise and one cash jump, before the first complete
event step. Raising N did not change this local event/strike geometry.

For captured endpoint differences a and b of opposite sign, the chord through
the endpoint maxima exceeds the maximum of the two endpoint-difference affine
lines by at most `abs(a*b)/(abs(a)+abs(b))` on that one interval. The table
reports this geometric quantity only. It is not a bound on the rounded PWL
evaluation, previous interpolation/integration, subsequent rollbacks, American
time discretization or the full option price. No epsilon merge, snapped root,
positivity clamp or new acceptance tolerance is authorized by those numbers.

The five replays add 4,634 actual interval contributions. Their preserved
partial work remains part of the denominator despite returning no price.

## Decision and next experiment

Keep this implementation as an independent negative diagnostic. Do not spend
another node/time sweep on repeated PWL to chase a favorable threshold. The
next proposed experiment changes the representation itself: local cubic stock
polynomials with analytically propagated values and first derivatives, complete
moment integration, explicit cash kinks and a separately controlled obstacle
operator. The prospective controls and unresolved proof obligations are frozen
in [the cubic representation plan](../bench/cash-gaussian/cubic-plan.md).

No new representation has been implemented or evaluated in this report. The
older PDE matrix still retains 43/44 finest liquidator passes and uncertain
DSPYp; the original Gaussian American pilot still retains 10 passes, 17 failures
and five refusals. Neither history is replaced by the six simple European
identity checks here.
