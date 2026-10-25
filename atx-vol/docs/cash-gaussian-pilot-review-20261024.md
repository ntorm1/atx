# Independent stock-state cash reference: fixed pilot result

The first independently implemented Gaussian/PWL cash pilot **does not qualify
as a reference**. All 32 predeclared outcomes are retained: 27 computed prices
and five typed numerical refusals. Against the unchanged historical reference
and its empirical uncertainty, 10 outcomes pass the $0.001 comparison and 17
fail. The five refusals are neither passes nor missing data. There is no timing,
production-dispatch, full-matrix accuracy or rigorous error-bound claim.

The original 50-case cash manifest and all 150 PDE-grid outcomes remain intact.
This pilot selects eight original contracts and explicitly marks the other 42
uncomputed. Its 10 passing outcomes do not form a new reference denominator.

## Frozen identities and controls

| Item | Identity |
|---|---|
| Independent Gaussian/PWL primitive | `80ae1ca27a56b2739627f1c7638c4529e8e7ee10` |
| Predeclared eight-case / 32-outcome plan | `5023cdf4faf31ffa3fc78434ad2a216a200d8c87` |
| Event owner, frozen before American pilot | `06d83e2fb7d8eeb99dc18894a2ee4501dcc86a38` |
| Runner and independent reader | `b478b81d8488aa79e5ad9ff617f1dcc69e9fec83` |
| Archived executable SHA256 | `0ac650a5091c76c9db76be84fe49be8266476bbcf707f2e8be400b7c26dba786` |
| Native 32-outcome JSON SHA256 | `3fe82b1981e3f892cb58683c48a4f0df95781fc0bf795536809d85dbbbfd1c79` |
| Independent summary SHA256 | `b268c17d5aa9009237e3692f52e2072617757b0a2d6e9351d0345180fb30461f` |
| Unchanged historical reference SHA256 | `d4b5c2e81c8971cd9d37abd601139217ba8e71161eee38c3ada7c1dba60343cc` |
| Original model/event binary64 fingerprint | `12110935955959749584` |

Receipts are under
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/calendar/`:

- `cash-gaussian-controls-v1`: primitive runtime RED/GREEN, independent control
  values and owning PCH-off checks. All 10 primitive groups pass.
- `cash-gaussian-owner-v1`: seven unavailable-stub controls fail at runtime, then
  10 owner plus 10 primitive groups pass. Both new owning translation units pass
  PCH-off checks. The first hygiene attempt lacked regenerated target metadata;
  its failure and successful retry remain in the receipt.
- `cash-gaussian-pilot-v1`: clean source/binary archival, fixed native run,
  progress, exit code zero, all 32 outcomes, independent summary, reader RED/GREEN
  controls and 17 malformed-receipt mutants, invalid-CLI refusal, owning pilot
  PCH-off check and final receipt.

The primitive has independent constant/linear/hinge, European Black76,
single-cash call-spread and interior-cash quadrature controls. The owner adds
exact-date normalization, nested exercise clocks, deterministic forward-clock
enumeration, pre/post cash exercise, nonconvex and tail obstacle intersections,
zero-state, overflow, moved ownership and cumulative-resource controls. These
establish component behavior; they do not establish American accuracy.

## What was actually computed

The engine propagates a continuous **stock-price** PWL function through exact
lognormal interval mass and first-moment integrals. It visits every represented
interval and the linear infinite tail, without a probability or moneyness cutoff.
The liquidator map is independent `f(max(S-D,0))` algebra. The event owner never
calls the PDE/Markov event, jump, interpolation, grid or pricing implementations.
The canonical raw input fixture and documented expiry-date convention are shared.
No historical reference price enters the native program.

American calls use event dates only, with the explicit `rate >= 0, yield == 0`
domain. Puts use independent nested uniform exercise clocks per cash-free
segment. Every diffusion is re-represented on the declared fixed stock grid;
exercise adds all representable PWL intersections. Sigma zero uses affine
rollback and zero stock stays absorbing. Neither limiting case inserts an
artificial positive volatility or stock-grid width.

The fixed resolutions are three stock grids for every selected contract
(`N=256,512,1024`) and two additional time levels for each put at `N=256`.
Nominal exercise counts are 128, 256 and 512, with exact event-segment allocation.
The actual DSPY put counts are 131, 262 and 524. No resolution changed after
observing a price. Every request had the same declared finite resource limits.

## Accuracy frontier on the finest declared stock grid

Each row below is `N=1024`, nominal put clock 128 or event-only call clock.
The uncertainty column belongs to the unchanged historical reference. It is an
empirical indicator, not a certified enclosure for either method.

| Contract | Gaussian price | Error to frozen reference | Reference uncertainty | Outcome |
|---|---:|---:|---:|---|
| A1p | 7.189775913474322 | +0.016135455727 | 0.000001445289 | fail |
| A1c | 9.686409204291225 | +0.000115783615 | 0.000000481417 | pass |
| E64p | 2.718075173299456 | -0.000000000000292 | 0.000000000248 | pass |
| E64c | unavailable | rounded exercise crossing | — | refused |
| ILp | 4.478180540978446 | +0.003144281273 | 0.000000448638 | fail |
| ILc | 2.677572466219604 | +0.000000002359 | 0.000000093651 | pass |
| DSPYp | 93.630074268743880 | +0.311542043664 | 0.000032516861 | fail |
| DSPYc | 149.681503001951940 | +0.028322258242 | 0.000009155596 | fail |

All 32 detailed results and all eight spatial/time axes are in `summary.json`.
The complete pass population consists of two A1 calls, five E64 puts and three
IL calls. Five numerically agreeing E64 put resolutions are not five independent
models or an independent uncertainty estimate.

## The structural error exposed by the two axes

| DSPY put request | Price | Error | Actual interval contributions |
|---|---:|---:|---:|
| N256, base time | 98.65831473473204 | +5.339782509652 | 8,793,309 |
| N512, base time | 94.65633590438698 | +1.337803679307 | 34,623,965 |
| N1024, base time | 93.63007426874388 | +0.311542043664 | 137,403,357 |
| N256, time level 1 | 103.91843523055896 | +10.599903005479 | 17,648,519 |
| N256, time level 2 | 113.88398543140977 | +20.565453206330 | 35,359,457 |

The observed spatial increment order is 1.963 for DSPY put and 1.963 for DSPY
call. A1 put and IL put show approximately the same second-order spatial pattern.
Increasing only exercise dates at fixed stock resolution makes IL and DSPY put
errors much larger. The A1 time axis is incomplete because its two refined
requests refuse; it is not assigned a convergence order.

**Inference:** repeated linear value representation is a leading error source,
even though integration of each represented function is analytical. For a smooth
convex continuation value, a chord overestimates the function between stock
nodes. Repeating propagation and re-interpolation can accumulate this error;
increasing the exercise clock also increases the number of representations.
The pilot's two axes support that diagnosis. They do not separately quantify
tail, interpolation and exercise-discretization errors. A semigroup control
without exercise or cash is the next clean way to isolate the representation.

Consequently, neither globally increasing PDE nodes nor merely adding more
Gaussian exercise dates is the proposed next action. Doubling this Gaussian
stock grid already multiplies measured integral work by approximately four.
The current dense method has not met the accuracy needed to justify optimizing
its throughput or fitting a favorable Richardson correction.

## All five numerical refusals remain explicit

A1p at N256/time levels 1 and 2 each refuses during the first exercise projection
after 259 expectation queries and 516 interval contributions. The function has
a strict obstacle crossing that rounds to an existing binary64 stock endpoint.

All three E64c resolutions refuse at the first pre-event exercise projection:
one diffusion, one post-event exercise and one cash jump have been evaluated,
but the first complete step has not been committed. Their interval counts are
518, 1,030 and 2,054. Each reports exactly one rounded intersection.

There is no hidden epsilon, minimum time value, rounded-knot merge or fallback
price. The five refusals are numerical representation failures, not invalid
market inputs. No request exhausted its work budget. Exact local crossing
operands were not captured in this first run; a later diagnostic must retain
them before proposing a principled rounding treatment.

Across all 32 attempts, the owner records **883,517,375 interval contributions**,
1,561,775 expectation queries, 3,223,595 constructed PWL knots, 3,912 completed
steps out of 4,692 planned steps, 4,006 completed exercise projections and 94
cash jumps. Refused partial work is included. These are work counts, not elapsed
time, core throughput or a prediction of a whole-fit speedup.

## Proposed bounded next experiments

These are proposals after the frozen negative pilot, not additional results or
post hoc changes to its qualification.

1. **Isolate representation error without exercise or cash.** On the unchanged
   DSPY stock/strike/maturity/volatility/carry, use European call and put payoff
   controls with N256/512/1024 and M1/32/128 uniform propagation steps: 18 declared
   outcomes. Compare every outcome directly with independent Black76 and retain
   the single-step control. Constant and linear payoffs should preserve their
   known semigroup moments. Use the same full intervals/tail and finite budget.
   This tests whether repeated representation alone explains the large drift;
   it does not alter the original American clock or reference.
2. **Capture the five failed crossings only.** Record the exact pre-max PWL,
   obstacle, neighboring stock/value bits, differences and computed intersection.
   Reconstruct the two lines in higher precision. Any future endpoint treatment
   must account for the resulting local value error explicitly. A dropped kink
   cannot become an unreported absolute tolerance or invented time value.
3. **Choose a new representation after those controls.** A private higher-order
   stock-state representation with analytical truncated lognormal moments is a
   concrete alternative to global node escalation. It needs new constant,
   polynomial, European, cash-kink and obstacle-root controls before an American
   experiment. Nonconvex liquidator puts and the infinite tail remain mandatory.
   No predicted accuracy gain is asserted from its polynomial degree alone.
4. **Only then investigate cost reuse.** Exact model/grid/dt keys can share
   transition weights across multiple rollbacks or contracts. Floating calendar
   differences and extra obstacle/cash knots must remain explicit; no date or
   coefficient quantization. Include construction, all queries, event mappings,
   exercise roots, resource refusals and destruction in any eventual cold timing.

The older independently refined PDE result remains the available cash benchmark:
the 50-case matrix retains 43/44 finest liquidator passes and the unresolved DSPY
put, while the separate fixed DSPY axis study improved agreement without proving
a general error bound. This Gaussian experiment supplies independent negative
evidence; it does not replace that history or the six distinct cash-model cases.
