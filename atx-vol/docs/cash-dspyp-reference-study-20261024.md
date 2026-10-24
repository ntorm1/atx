# DSPYp cash-reference accuracy study

Status: completed predeclared correctness experiment, 2026-10-24. No production change or
performance result. The original 50 cases and all 150 three-grid outcomes remain
the qualification denominator; this selected study cannot promote a new policy.

## Question and fixed experiment

The finest previous PDE price for DSPYp is 93.31753028217078 USD/share. Its frozen
reference is 93.318532225080119 with empirical uncertainty
0.000032516860882252968. The difference is 0.001001942909340414, so uncertainty
straddles the 0.001 monetary threshold. This is an uncertain case, not a pass.
The 1600-by-800 frontier remains 43/44 matching spot-liquidator cases passing,
plus six separately classified cash-model cases; none may be dropped.

The predeclared selected cases are DSPYp and its same-calendar call control DSPYc.
Both are taken directly from `tests/support/ddiv_matrix_cases.hpp`, including
the original binary64 event arithmetic. Every original input is exported so the
selection and all omitted computations remain explicit. No baseline price or
reference value enters the native calculation.

Use unchanged `ddiv_ref::pde_price`, base space 1600 and nominal time 800,
width 8, grading 2, two Rannacher steps, default domain sigma. Run exactly:

| Space level | Time level | Effective space | Nominal time | Purpose |
|---:|---:|---:|---:|---|
| 0 | 0 | 1600 | 800 | Exact previous-result control |
| 1 | 0 | 3200 | 800 | Spatial error direction |
| 0 | 1 | 1600 | 1600 | Temporal error direction |
| 1 | 1 | 3200 | 1600 | Interaction check |
| 2 | 0 | 6400 | 800 | Spatial order diagnostic |
| 0 | 2 | 1600 | 3200 | Temporal order diagnostic |

Levels multiply the existing per-event integer step counts, preserving nested
time nodes. Replacing the base with a larger integer would reapply per-segment
rounding and is deliberately not this experiment. Retain all twelve results,
including typed refusals, all original PdeResult fields and actual Thomas work.
No wall clock is recorded. Baseline results must match the frozen three-grid
receipt exactly. Source/executable/input/output hashes accompany the report.

The readout separates price increments along each axis and the mixed increment.
Observed order and extrapolation remain empirical diagnostics, never an error
bound. A successful selected row does not establish full-matrix accuracy for
that grid. No adaptive extra grids or per-case tuning are part of this run.

Frozen input/reference replay SHA256:
`d4b5c2e81c8971cd9d37abd601139217ba8e71161eee38c3ada7c1dba60343cc`.
Historical PDE and trinomial sequences are recorded in
`docs/american-cash-dividend-reference.md` and the immutable D0 matrix receipt.
The forthcoming report will retain those uncertainties and distinguish this
same-engine diagnosis from an independent numerical reference.

## Results of the declared twelve solves

All twelve solves succeed. The two baseline controls match the prior price,
policy and census bits. All original inputs and all prior 150 results are retained.
The experiment declaration was committed as `be0551a8` before native execution.
No price, grid or stopping rule was changed after seeing a result.

| Space / nominal time | DSPYp price | Absolute error + old uncertainty | Thomas forward rows | Relative work | DSPYc price |
|---|---:|---:|---:|---:|---:|
| 1600 / 800 | 93.31753028217078 | 0.001034460 | 1,849,600 | 1.000 | 149.65355909789142 |
| 3200 / 800 | 93.31792618565349 | 0.000638556 | 4,460,800 | 2.412 | 149.65325034918206 |
| 1600 / 1600 | 93.31784209368396 | 0.000722648 | 3,292,800 | 1.780 | 149.65357195751193 |
| 3200 / 1600 | 93.31825679385476 | 0.000307948 | 7,571,200 | 4.093 | 149.65326322229217 |
| 6400 / 800 | 93.31803187066163 | 0.000532871 | 12,364,800 | 6.685 | 149.65318568916334 |
| 1600 / 3200 | 93.31790691780422 | 0.000657824 | 5,987,200 | 3.237 | 149.65357369373808 |

Every refined DSPYp result passes the existing empirical monetary check; its
baseline remains uncertain. DSPYc passes at all six settings. These classifications
use the unchanged historical reference and uncertainty, not a new fitted target.
The work ratios count actual scalar Thomas rows, including policy iterations;
they are neither elapsed-time ratios nor complete PDE cost ratios.

DSPYp's spatial increments are +0.000395903483 and +0.000105685008; the temporal
increments are +0.000311811513 and +0.000064824120. The observed orders are 1.905
and 2.266. The mixed increment is +0.000018796688, so a purely additive error
model is imperfect. DSPYc has spatial increments -0.000308748709 and
-0.000064660019; its much smaller temporal increments are positive. This rules
out treating the selected put's bias as a universal signed correction.

There is enough information to stop this local refinement study. Doubling time
alone is the cheaper tested way to resolve DSPYp under the old empirical gate,
but adopting it globally would require a new full-50 matrix and cost comparison.
No DSPY-specific grid exception is proposed. The next structural reference
experiment below addresses independent jump semantics and removes unnecessary
time marching in an explicitly supported call domain.

## What the old reference establishes

The historical DSPYp PDE uses four separate space and time levels, with finest
9600 space intervals and 5240 nominal time steps. Its printed space sequence is
93.317459671, 93.318244462, 93.318446904, 93.318499708; its time sequence is
93.317871085, 93.318331900, 93.318463368, 93.318499708. Both approach from below,
with observed orders about 1.94 and 1.86. Richardson corrections produce the
frozen reference and empirical uncertainty stated above. The printed sequence
has only nine decimals; the reference CSV and new JSON retain binary64 values.

The separate trinomial sequence ends at 93.318913220 after 16,000 nominal steps;
its corrected estimate is approximately 93.3183331 with uncertainty 0.00058.
Agreement of those empirical intervals is useful, but this lattice is not a
tight independent certification of the current near-threshold price. Moreover,
both reference engines share `cell_average` and `jump_at` in
`src/pricing/american_discrete_div_reference.cpp`: the lattice call is near line
921 and the PDE call near line 1127. A shared cash-jump defect can survive both.
`reference_price` combines the two estimates, rather than proving either bound.

The new axis study narrows a practical discretization diagnosis. It does not
remove this shared-operator limitation or turn Richardson uncertainty into a
mathematical enclosure. A reference upgrade should first remove that dependency.

## Primary-method assessment

Itkin's version 2 develops event-segment Volterra/GIT boundary calculations,
including additional cash-date terms (Algorithm 2 and Appendix E). Full prices
also require European value, transition density and the exercise premium. The
cash examples emphasize boundary plots; their comparator omits discrete cash
dividends, so they do not certify our dollar error. The paper relaxes the
pathwise cash-below-stock condition; an explicit liquidator zero-state extension
is therefore needed before treating it as our full-domain oracle. Multiple
exercise boundaries are excluded. The quoted C++ speedup is a prediction, not
our measurement. I inspected the 43-page PDF stamped arXiv v2, 5 January 2026,
with cover date 6 January; the HTML renders a different date. The archived PDF
is the version authority here. [Itkin, v2](https://arxiv.org/pdf/2510.18159v2).

Cosma and coauthors describe recursive state-price projection, discrete-event
cash adjustments and matrix reuse for equal intervals. Their call formulation
needs exercise decisions at dividend dates; puts require a Bermudan time
approximation. The method supplies a useful independent architecture, but its
published experiments do not qualify our frozen corpus or zero-state rules.
The proposed analytical piecewise-linear integration below is our experiment
design, not a claim that their paper already implements it.
[Cosma et al., 2 March 2016](https://access.archive-ouverte.unige.ch/access/metadata/3cd39fab-57cb-4036-8950-da5211f36c1f/download).

Archived primary-source hashes are `f91593a1f6ae3b4399ea19cd3783f8902c93795532c143d5d0023a830c67db8f`
(Itkin) and `82e885d6c5b0330fa50052b09ede04bd67c79535e69950ee54e2b0743eb120e6`
(Cosma). `primary-sources.json` records the download URLs and sizes. No paper's
runtime is transferred to this implementation or used as a throughput forecast.

## Proposed independent reference experiment

Implement a private scalar, double-precision event-segment Gaussian rollback
with piecewise-linear values in **stock price**, not strike. Keep a distinct
absorbing state at zero. Within a positive-price GBM interval, integrate each
piece `A + B*S` against the lognormal transition analytically using the normal
CDF and first truncated moment. For `log(S_next) ~ Normal(m,v)`, interval
`[a,b]` contributes, before discount,

`A * [Phi(z_b)-Phi(z_a)] + B * exp(m+v/2) * [Phi(z_b-sqrt(v))-Phi(z_a-sqrt(v))]`,

where `z_x=(log(x)-m)/sqrt(v)` and endpoint limits cover zero and infinity.
Use stable tail differences and a separate deterministic zero-variance path.
This is exact integration of the represented function, not an exact American
price: spatial representation, exercise-time restriction and tail representation
remain distinct errors.

At a cash event construct `V_before(S)=max(payoff(S), V_after(max(S-D,0)))`, with
the post-event value already exercising where permitted. Move every stock-value
knot by the exact cash amount, preserve the zero branch, and insert payoff
intersections explicitly. Do not call the PDE/lattice `jump_at`, cell averaging,
cubic interpolation or their grid constructor. Liquidator puts need not remain
globally convex at the cash cap, so do not assume a convex interpolation bound.
Preserve terminal-event, duplicate-date, cash-exceeds-stock and both-sided
exercise conventions through independently tested adapters.

The smallest useful implementation sequence is:

1. Validate discount, mass and first moment with constant/linear payoffs and
   European no-cash Black-Scholes controls. Validate a one-cash European price
   by separately integrating over the pre-event lognormal stock and applying
   the exact liquidator map. Include cash above spot, zero-rate and negative-rate
   controls, and deterministic limits. This tests the jump independently before
   any American comparison or speed experiment.
2. Price the original spot-liquidator **calls** with `r>=0` and `q=0` using only
   pre-dividend exercise opportunities plus expiry. Between events these declared
   conditions rule out advantageous exercise; an unsupported model must take a
   separate general route. All 22 matching calls in this corpus meet those
   conditions. DSPYc needs ten diffusion segments, compared with its existing
   823 implicit steps and 833 Thomas sweeps. This is a work-count opportunity,
   not a prediction of speed: dense quadrature can cost more per segment.
3. For puts, use independently constructed nested exercise grids containing
   every event, first with fixed space and then fixed time. The intended pilot
   is original A1p/c, E64p/c, ILp/c and DSPYp/c, with every pilot row and all
   remaining original IDs explicitly retained. Run fixed spatial sizes
   256/512/1024 and nominal put exercise counts 128/256/512; refine one axis
   at a time. Use calls' event-only clock. Do not tune one contract, discard a
   refusal or claim the selected subset is all-case qualification.
4. Require independent agreement with the old reference within both reported
   empirical uncertainties, and demand less than 0.0001 USD empirical reference
   uncertainty before using a new estimate to judge the 0.001 gate. Any mismatch
   stays unresolved. Do not replace the frozen reference in-place. Only after
   those controls run the full 44 matching rows; keep four escrowed and two
   survivor cases under their own six-case denominator until their models are
   implemented and independently verified.

Predeclare these pilot resolutions before implementation. The arithmetic
baseline should retain all interval contributions; no tail cutoff, FFT or
interpolation shortcut in its first correctness version. A dense rollback costs
`O(M*N^2)` scalar pair contributions and `O(N^2)` memory if its matrix is cached;
a 512-by-512 matrix occupies about 2 MiB, 1024-by-1024 about 8 MiB. Ten 512-node
call segments involve about 2.62 million pair contributions before changing
event knots. A 512-step put would involve about 134 million, plus exact event
subdivisions and knot growth. These are base work estimates; record actual
state counts and contributions, and enforce a typed resource budget.

Uniform log-state segments could later use a Toeplitz/convolution implementation,
or batched exact transition matrices for shared model/time keys. Those are
separate performance changes, after the scalar reference exists. Exact event
times, amounts and sigma remain unquantized; grouping must not change models.
All normal-CDF setup, knot construction, exercise decisions, event handling and
output/destruction must be included in eventual cold timings.

Existing `american_markov.cpp` provides useful lifetime, event and work-budget
patterns, but its explicit diffusion is a three-point approximation (around
lines 419-459); its variance resolvent is another numerical transition. Neither
is the analytical GBM semigroup. `american_markov_pwl.cpp` represents values in
**strike** and requires ExplicitEuler; it cannot be relabeled as the proposed
stock-state integration oracle. Reuse public model types and test inputs where
appropriate, while keeping the new numerical jump/transition implementation
independent. A later GIT experiment should first match these single-cash and
zero-state controls before tackling the full boundary equation.

## Reproduction and retained evidence

The private target is under the existing default-off
`ATX_VOL_BUILD_CASH_THOMAS_RESEARCH` option. It invokes the original scalar PDE;
it does not dispatch to the SIMD driver, alter a default or record a clock.

```powershell
./scripts/atx-build.ps1 build atx-vol-cash-dspy-refinement -Preset vol-rel -Jobs 2
./build-vol-rel/bin/atx-vol-cash-dspy-refinement.exe > <new-native.json>
python atx-vol/bench/cash-pde-simd/refinement_audit.py --reference <frozen-replay.json> --report <new-native.json> --output <new-summary.json>
python atx-vol/bench/cash-pde-simd/refinement_test.py --reference <frozen-replay.json> --report <new-native.json>
```

Evidence directory: `calendar/cash-dspy-axis-v1/` under the r7 cache. It retains
the archived native executable, all twelve unrounded outcomes, summary, source
and artifact hashes, literature copies, build/owning-PCH-off logs and reader
controls. Native report SHA256 is
`314db7cd89d1f2314953911859b00839c674a8bba9ce5820ca28dfbbc518ce18`.
The reader verifies every original input/event bit, selection/order, exact nested
grid counts, baseline price/policy/census, result-bit round trip and work-accounting
identities. Thirteen invalid mutations are rejected; a genuine typed refusal is
retained and prevents axis inference. The first hygiene check found the new target
not configured; the first explicit configure argument was malformed. Both logs
remain beside the successful reconfigure and owning-TU compile. No numerical
trial was dropped or rerun to select a preferred outcome.
