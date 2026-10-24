# Cash D0/D1: the next accuracy-qualified throughput experiment

Source review, 2026-10-24. This extends
[the frontier review](cash-reference-frontier-review-20261024.md) at `5f09ba3f`.
Cash production/reference sources were read at `234b31fe`; their diff against
the merged root `a1ca7d14` is empty. No new cash benchmark was run for this review.
Subsequent reference-admission hardening is recorded below; the production cash
pricing profiles remain unchanged.

The next concrete step is a **price-error frontier harness**, followed by a
prepared, event-aligned PDE workspace experiment using the existing D0 solver.
The baseline must include both D1 routes and all matching-model cases. The useful
architectural comparison is repeated scalar lattice rollback versus reusable
event/grid geometry and batched obstacle solves. Neither increasing the existing
step-count rule nor vectorizing its current scalar loop establishes $0.001/share
accuracy. The current Refined route already misses that target on five historical
matching-model cases, including both SPY-like cases.

## 1. What the existing evidence establishes

The D0 CSV contains 50 cases: 44 spot-liquidator, four escrowed and two
spot-survivor. The production D1 route is spot-liquidator. Its six other-model
comparisons are convention diagnostics, not six production pricing failures or
six accuracy successes. Of the 50 historical labels, 41 are RESOLVED under the
old IV criterion, five are UNRESOLVED under that criterion, two are PriceOnly and
two are convention gaps. The five low-vega UNRESOLVED cases still have reference
price uncertainty far below one mill; the two PriceOnly cases still require
correct prices. Neither group belongs outside the price-error denominator.

I compared every embedded `kD0` reference price, uncertainty and vega in
`american_discrete_div_production_matrix_test.cpp` with the archived D0 CSV:
all 50 match as binary64 values, and all status strings match. The D1 fix1
`run7-final-slow.log` price-rescore lines equal the earlier production run's
lines exactly. This checks reference transcription, not the full duplicated
construction of input schedules in the two C++ tests.

The following is an **archived-evidence rescore**, not a fresh run. For each
printed D1 price error, the analysis includes half its last printed digit and
the full recorded empirical reference uncertainty. No row is ambiguous at the
$0.001 threshold under this rule:

| Route | Within $0.001 including empirical uncertainty | Outside | Other model |
|---|---:|---:|---:|
| Standard | 9/44 | 35/44 | 6 |
| Refined | 39/44 | 5/44 | 6 |

The five Refined misses are:

| Case | Printed signed error, USD/share | D0 empirical uncertainty, USD/share |
|---|---:|---:|
| B2p, cash just before expiry | +0.00106 | 0.0000006852 |
| DSPYp | +0.00161 | 0.0000325169 |
| DSPYc | -0.00164 | 0.0000091556 |
| E20c | +0.00179 | 0.0000005573 |
| G1c, zero rate | +0.00197 | 0.0000002974 |

Standard's printed DSPY errors are +$0.0169 and +$0.0567. Its twelve-event
put/call errors are approximately +$0.0112/+0.0115; Refined reduces those to
+$0.0000443/-$0.0000652. Thus event count alone does not select a qualified route:
some twelve-event prices pass while near-expiry, zero-rate and SPY-like prices
still fail. An IV error in basis points does not replace the price test.

Evidence, including SHA-256 values and every row's rounding interval:

- `C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/calendar/cash-frontier-source-v1/analyze_existing.py`
- The adjacent `historical-price-rescore.json`.
- Inputs: `D0-20261022-fix1/matrix-run-cc89bc89/matrix.csv` and
  `D1-20261023-fix1/run7-final-slow.log` under `C:/atx-cache/quote-benchmark/`.

Reproduce this read-only rescore with:

```powershell
python C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/calendar/cash-frontier-source-v1/analyze_existing.py
```

The maximum recorded D0 uncertainty among the 44 production-model cases is
$0.0000325169. These uncertainties remain empirical convergence estimates. The
Richardson-order caveat in the frontier review still applies; the table does not
claim a rigorous continuum error bound.

## 2. Input and reference discrepancies to close first

1. **The CSV is not a complete model manifest.** It stores event count and maximum
   cash ratio, but not every event time and amount. The two matrix tests duplicate
   their schedule constructors. Export the actual C++ case objects with round-trip
   binary64 numbers, process, exercise rights and route options. Preserve the
   original D0 reference columns and source/artifact hashes. Do not rebuild dates
   by hand from the displayed event count.
2. **D1 only prints rounded prices/errors.** The rescore above safely identifies
   these historical misses, but a new frontier must export unrounded Standard,
   Refined and candidate prices, actual route/step/pre-step decisions, refusals,
   and each Greek's validity. Keep the old labels as provenance alongside new
   price qualification, rather than rewriting them.
3. **Market latency inputs have their own normalization.** The archived
   `build_ddiv_latency_inputs.py` uses chain-median spot, calendar time, rate and
   residual continuous yield. Cash times are `T * days / life_days`. An IV quote
   is present only for positive bid and ask at least bid. The earlier per-row-spot
   file had 2,404 groups; the superseding chain-median file has 398. Freeze the
   exact input file and report normalization instead of mixing these cohorts.
   The source control run inferred its cash schedule from per-expiry cash/PV
   increments: its event inventory has 234 `exact`, 153 `sum_split` and four
   `non_integer_day` entries. Those labels describe the inference procedure;
   they are not an independently verified corporate-action truth set. Preserve
   them in the manifest. The PV identity uses `exp(-rate * days / 365)`, while
   the kernel clock uses `tau=T*days/life_days`; export and reconcile the actual
   discounts if those clocks differ. The control run also explicitly treats the
   supplied `sdiv` as residual yield alongside cash. Keep that model assumption
   visible when comparing to a vendor or external cash dataset.
4. **Historical price timings use heterogeneous sigmas.** `ddiv_latency_main.cpp`
   uses each contract's identified IV, otherwise `--sigma`. The CSV and aggregate
   timing report do not preserve these per-request sigmas for a replay. Export
   them before counting exact reusable pricing keys. Expiry equality alone does
   not imply a common lattice or PDE operator.
5. **A lattice self-round-trip is not independent accuracy.** The synthetic
   latency ladder can fill quotes from the same Standard kernel it later inverts.
   Keep it as a throughput/control workload; it cannot qualify dollar accuracy.
   The historical market cohort measures service and inversion outcomes, not an
   independent reference error for every request.

For synthetic cases use an explicit $0.001/share budget. If a market fitting
claim uses `min($0.001, 1% * original spread)`, retain the original bid and ask;
the current latency input's midpoint-only quote cannot reconstruct that budget.

## 3. A QuantLib cash comparison needs a new model adapter

The existing pinned comparator uses `QdFpAmericanEngine` for continuous carry and
a model clock mapping one synthetic maturity exactly to T. It is not a cash
comparator. Reuse its build/provenance/timing controls, not its result labels.

At pinned QuantLib `e96579fd471b2e9f6bc974c96674d1ccfff248fa`, the finite-difference
engine explicitly distinguishes Spot and Escrowed cash models. Escrowed adjusts
the state and exercise calculator; it is not a substitute for D1's liquidator
process. [Pinned engine source](https://raw.githubusercontent.com/lballabio/QuantLib/e96579fd471b2e9f6bc974c96674d1ccfff248fa/ql/pricingengines/vanilla/fdblackscholesvanillaengine.cpp).

There are two concrete adapter hazards:

- QuantLib's cash jump uses linear interpolation and clamps the post-dividend
  state to the lowest positive mesh stock. D0 instead continues to the exact
  absorbing-zero value below its mesh. This is a finite-domain/jump-discretization
  discrepancy, especially for cash exceeding stock; convergence must measure it.
  The handler selects the first equal event time, so coalesce simultaneous cash
  before constructing its schedule. [Pinned dividend handler](https://raw.githubusercontent.com/lballabio/QuantLib/e96579fd471b2e9f6bc974c96674d1ccfff248fa/ql/methods/finitedifferences/utilities/fdmdividendhandler.cpp).
- QuantLib's composite adds the dividend condition before American exercise,
  retains dates from reference date through maturity, and adds nearby stopping
  times after dividends. D0 ignores events at time zero and applies terminal cash
  analytically. Explicit tests must establish cum/post rights and terminal-event
  agreement; matching a model-name enum is insufficient. [Pinned step conditions](https://raw.githubusercontent.com/lballabio/QuantLib/e96579fd471b2e9f6bc974c96674d1ccfff248fa/ql/methods/finitedifferences/stepconditions/fdmstepconditioncomposite.cpp).

Do not round arbitrary synthetic taus to calendar days. A benchmark-only custom
day counter can map distinct synthetic date labels to the exact ordered event
times and maturity, with `yearFraction(a,b)=F(b)-F(a)`. Assert actual process/event
times bitwise at every input knot and export effective rate, yield and variance.
Alternatively report date-rounded cases as a separate model comparison. Required
adapter controls are no cash, one cash, coincident events, time-zero exclusion,
cash at expiry, first-step cash, cash above spot, and negative-rate boundaries.

## 4. What the prepared API shares, and what it repeats

Source seams are [the batch wrapper](../src/pricing/american_discrete_div_batch.cpp),
[route/inverter implementation](../src/pricing/american_discrete_div_kernel.cpp),
and [lattice rollback](../src/pricing/american_discrete_div.cpp).

`DiscreteDivBatch::prepare` shares validated routes, fine/coarse/Screen event-step
maps, one allocation arena and a maximum-sized workspace. Its schedule spans are
borrowed and must outlive preparation and use. `price`, `implied_vol` and `greeks`
then loop over contracts and invoke scalar routines. The latency tool passes a
span of length one to each of those APIs. Allocation-free calls therefore do not
demonstrate numerical batching.

Each `run_production_lattice` repeats transition construction, terminal stock
exponentials, underlying-level rollback updates, interpolation location/weight
work and the contract's value rollback. Refined repeats this for both grids
except its near-expiry single-grid route. Its work is approximately
`M * (N*(N+1)/2 + N*pre)` node updates per rollback. Inversion repeats rollbacks
at candidate sigmas; Greek bumps repeat them at bumped models.

| Work | Exact reuse condition | What must remain contract-specific |
|---|---|---|
| Route, event placement/retiming | S,T,schedule,route options | Already prepared; no new gain should be claimed |
| Transition probabilities, terminal stock grid, layer evolution | Same full model, sigma and grid | Different sigma needs different coefficients/levels |
| Cash jump destinations and ordinary interpolation weights | Same grid, event and model | Value-dependent boundary extrapolation and exercise decisions |
| Payoff and American maximum | Same contract only | Strike, side and its exercise region |
| PDE event/time mesh and stock levels | Same explicit domain/model mesh | Different strike changes today's automatic domain |
| PDE differential coefficients | Same sigma,r,q and spacing | Changing sigma/rates invalidates coefficients |
| Howard/Thomas factorization | Same time stencil and active policy rows | Different strikes generally have different active sets |

An exact same-sigma lattice block is a useful control for scenario ladders and
common floor-volatility requests. It can share underlying-grid work across
strikes/sides without changing prices. It must not quantize market IVs to create
occupancy. A heterogeneous-sigma SIMD block can vectorize independent rollbacks,
but does not remove their quadratic step-count work. Measure real exact-key
occupancy before selecting either as the main investment.

## 5. Reusing D0 as a production candidate has two prerequisites

The existing [PDE reference](../src/pricing/american_discrete_div_reference.cpp)
already supplies event-aligned graded time steps, damping after every jump,
cell-averaged payoffs, the cash jump, and a Howard obstacle solve. Those are useful
building blocks. `pde_price` presently allocates its arrays each call, and
`jump_at` allocates another output vector at every event. A prepared workspace
can remove these allocations and precompute immutable event/time/grid geometry.

However, **the monotone-matrix assumption is not checked**. `make_operator`
constructs centered coefficients
`a = sigma^2/(2h^2) - nu/(2h)` and
`c = sigma^2/(2h^2) + nu/(2h)`, with `nu=r-q-sigma^2/2`.
The source comments rely on an M-matrix for the obstacle iteration and Thomas
sweep, but `validate_pde_grid` and `pde_price` never establish `a,c >= 0`.
For valid no-cash inputs S=100,K=200,T=1,sigma=.005,r=.10,q=0 and the default
400-space grid, h is about .00193287 and a is about -22.5192. This is a source-level
counterexample to the assumption, not a measured price-error claim. It does not
invalidate the existing 50-case results. Before general production use, check
stencil admissibility and the actual implicit-row conditions, returning a typed
unsupported result or routing to a separately tested monotone discretization.
Do not silently use centered drift outside its justified region.

Follow-up `0bd781ce7c8e77406629b4a7f1bb7192f5ee39ab` implements this admission
guard and a second check on each actual implicit matrix: finite nonpositive
off-diagonals and strict diagonal dominance. The latter is a sufficient M-matrix
condition and prevents large negative rates/time steps from bypassing the spatial
guard. Unsupported grids return `OutOfRange` before their Thomas/Howard solve;
the method does not change its stencil or time steps. This is a supported-grid
restriction, not a general accuracy certificate. The source header now describes
Richardson uncertainty as empirical, with its counterexample retained in a test.

Validation passed 28 focused tests, owning PCH-off compilation and the full
50-case matrix with its existing appendices. Every non-timing CSV column equals
the archived D1 D0 matrix exactly. The source was independently approved by root
and acceptance reviewers. Reproducible comparisons, red/green tests and hashes are
in `calendar/cash-matrix-guards-v2/receipt.json` under the evidence directory above.
This changes reference admission only and supplies no production PDE speed claim.

Second, the cash jump is not entirely precomputable. `apply_jump` locates crossings
of continuation and cum-dividend exercise, then integrates cells split at those
crossings. Crossing locations depend on each contract's values. Immutable point
interpolation geometry can be cached; copying one strike's exercise cuts to other
strikes would change the method. Likewise, a common PDE operator is not permission
to reuse another contract's Howard factorization.

## 6. Exact proposed experiment

This is a proposed private/tool experiment; no production default is authorized
by this report. Freeze the inputs, configurations and acceptance rules before
timing. Keep negative and unsupported results in the output.

**A. Establish the fresh scalar frontier.** Extract a shared test-only case
manifest from the existing C++ matrix. Run all 50 inputs through current Standard
and Refined, retaining the 44 matching-model denominator. Export unrounded values,
reference estimates, route counts, prices at each refinement, and typed refusals.
Run the existing D0 primitive/one-event controls. Add the low-volatility stencil
counterexample as a domain-control test before a PDE production prototype.

Compare the existing single-solve PDE at the predeclared nested grids
400x200, 800x400 and 1600x800 (space intervals x nominal time steps), keeping
width=8, grading=2 and the existing per-event damping. These are three declared
work/accuracy points, not a search over tolerances. Use independent axis/domain
refinements for reference qualification, not the candidate's own three-point
sequence alone. Domain-width=11 is a reference check, not another speed candidate.
Retain exact one-event controls and the independent trinomial/QL comparisons.

**B. Prepare the identical scalar PDE.** Introduce a private owning workspace
with immutable normalized events, exact segment time levels, grid stock levels,
and reusable arrays including the jump output. For a one-contract preparation,
require identical grid, policy decisions, prices and diagnostics to `pde_price`.
No change to interpolation, damping, stopping rule or reference uncertainty is
part of this step. Record allocations, setup bytes and setup/run work separately.

A narrower factor reuse experiment is possible even when policy masks differ.
In `thomas_solve`, every active/Dirichlet identity row resets `cp` to zero. Each
following continuation run uses the same constant `(lo, di, up)` recurrence:
`m[j] = di - lo*cp[j-1]`, `cp[j] = up/m[j]`, starting at `cp[0]=0`.
Thus a step-local table indexed by the current free-run length can reuse these
coefficients for arbitrary active masks. The contract's RHS recurrence and
backward substitution still run independently. Initially retain the original
`dp = (rhs-lo*previous_dp)/m` division, operand order and identity-row treatment;
do not replace division with reciprocal multiplication and assume bit parity.
Test all-active, all-free, alternating, multiple disjoint runs, changing masks,
both exercise sides and unsupported matrices. The table must be invalidated when
any actual rounded coefficient changes. Its setup cost and memory traffic may
outweigh savings at one policy iteration, so measure complete solves and actual
iteration counts before promoting it. This is a source-derived proposal, not an
implemented or measured optimization.

**C. Test a true batch.** Use block sizes 1,8,32,128, plus the complete expiry
block, in original input order. First run exact common-model/sigma groups with
separate values, obstacles and policy vectors, sharing only proven immutable
geometry. Then test a declared common domain spanning the union of strike domains;
this changes discretization and requires a fresh accuracy check. A common grid
may share coefficients, event mappings and each step's setup, but per-contract
Thomas sweeps remain unless active policy vectors are exactly equal. Compare
against the same-grid scalar control, not against a different domain and call the
difference an optimization. One-worker execution only.

Instrument counts for stock-grid exponentials, time-stencil builds, jump point
maps, crossing searches, Howard iterations, Thomas rows, rollbacks, allocations,
and fallback calls. Optional clocks must be absent from normal numerical paths.
These counts distinguish amortized work reduction from a faster isolated loop.

**D. Evaluate IV and Greeks only after price qualification.** The cash inverter's
`confirm_leg` currently accepts a Newton-distance estimate `abs(residual)/slope`
against an IV tolerance on its accurate lattice. Its name does not establish
reference price accuracy. A future monetary stop must charge both model-pricing
error and target residual, retain floor/flat-region classification, and check the
final target with an independently qualified price route. Do not transplant the
continuous-carry family's proof to dated cash. For Greeks, reuse prepared geometry
only for bumps whose exact model key permits it; report unsupported components
and independent bump convergence. Price accuracy alone cannot qualify a Greek.

**E. Measure the qualified frontier.** Export one result per original request:
input/reference hashes, complete model and effective clock, candidate config,
price/error/uncertainty/classification, route/fallback, work counts and timing.
For an empirical price pass require `abs(candidate-reference)+reference_u <= budget`;
report unresolved reference cases separately and label this an empirical gate.
Record price-only, no-IV-market and inconsistent-quote statuses without removing
their valid price requests. Report qualified completed prices per second and
all-input coverage, never successful-only throughput.

Time cold prepare+batch, prepared batch, changed-spot/sigma/schedule update+batch,
and scalar query separately. Include all fallback costs, setup memory, allocation
counts and break-even batch size. Use the release ThinLTO profile, fixed P-core,
one worker, fresh processes and recorded sibling contention; retain failed timing
receipts. Do not compare the historical busy-host D1 latency to a quiet new run.

Existing focused build/test entry points (the prepared-PDE experiment remains
unimplemented; the exact input/price exporter below is available):

```powershell
powershell scripts/atx-build.ps1 build atx-vol-discrete-div-reference-tests atx-vol-discrete-div-production-tests atx-vol-ddiv-latency -Preset vol-rel -Jobs 3
powershell scripts/atx-build.ps1 -Ctest -Preset vol-rel -R "^atx-vol-discrete-div-(reference|production)$"
build-vol-rel/bin/atx-vol-discrete-div-production-tests.exe --gtest_filter=DiscreteDivProductionMatrix.ReScoreAgainstTheIndependentReference
```

### Frozen exact input and price manifest

`tests/support/ddiv_matrix_cases.hpp` is now the single source of the original
50 matrix inputs. Before removing the two older constructors, runtime red/green
tests compared every original model/event bit against each independently written
constructor. Both produced input fingerprint `12110935955959749584`. The retained
tests pin this fingerprint; it is an FNV accidental-drift check, not a cryptographic
evidence hash. No original input, event arithmetic or frozen reference value changed.

The production matrix target exports all 50 cases with original events, process,
exercise rights, residual continuous carry, unrounded Standard/Refined prices,
effective route/options/convention, typed refusals, original reference IV status,
reference value/empirical uncertainty/vega and archived reference CSV SHA256.
Price-only and IV-unresolved rows remain price requests. Six other-model cases
remain explicit `model_mismatch` records; the matching production-model denominator
is 44. The price gate reports pass/fail/uncertain/reference-unresolved/refusal
separately and uses `abs(price-reference)+reference_u <= 0.001` for an empirical
pass. These synthetic inputs have no raw bid/ask spread, so this export cannot
qualify the separate 1%-of-spread budget.

```powershell
# The parent exists; this final directory MUST NOT already exist.
$env:ATX_VOL_DDIV_MANIFEST_OUT = 'C:/atx-cache/quote-benchmark/cash-manifest-new-run'
build-vol-rel/bin/atx-vol-discrete-div-production-tests.exe --gtest_filter=DiscreteDivProductionManifest.ExportsAllInputsAndUnroundedPrices
```

The export refuses an existing directory and preserves its receipt. JSON
round-trip tests preserve all original model/event/reference/price binary64 values.
Focused gates: 29 reference and 31 production tests pass; both owning matrix TUs
compile with PCH disabled. Evidence is
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/calendar/cash-manifest-v1/`:
the two constructor comparisons, exporter red/green tests, final suites, expected
overwrite refusal, `export/manifest.json`, and independent `verify_manifest.py`.
The verifier joins every row to the archived CSV and checks original input and
reference bits. `receipt.json` records source-file, executable and artifact hashes.

The unrounded result confirms **Standard 9/44 pass, 35/44 fail; Refined 39/44 pass,
5/44 fail**, with six model-mismatch records for each. There are no uncertain or
unresolved dollar comparisons in this fixture. The manifest SHA256 is
`def79ede5481a1bc1761fa6e7e126794f5128f9f995905ef2f7d6b1bf95d9810`.
This is a reproducible numerical baseline, not a latency result or rigorous error
certificate. No production pricing source changed for this exporter.

### Three declared single-PDE grids: incomplete accuracy coverage

The diagnostic-only replay calls the existing `pde_price` without changing its
arithmetic or admission. It uses every original case's own cash process, so all
50 PDE/reference comparisons match models; the 44 spot-liquidator subset is also
reported for comparison with the production lattice. Each grid uses width 8,
grading 2, two Rannacher starts per event, no domain-volatility override and no
extrapolation. Nominal time counts cover the complete maturity, with the existing
minimum segment sizes and half-step damping preserved.

| Space x nominal time | All 50: pass / fail / uncertain | Spot-liquidator 44 | Implicit time steps, all 50 | Maximum Howard iterations |
|---|---|---|---:|---:|
| 400 x 200 | 47 / 3 / 0 | 41 / 3 / 0 | 10,378 | 3 |
| 800 x 400 | 48 / 2 / 0 | 42 / 2 / 0 | 20,386 | 4 |
| 1600 x 800 | 49 / 0 / 1 | 43 / 0 / 1 | 40,376 | 4 |

All 150 candidates were priced, with no solver refusals or rounding-tie accepts.
The coarse failures are D12c and both DSPY sides; the middle failures are both
DSPY sides. The finest unresolved case is DSPYp:
candidate `93.31753028217078`, reference `93.31853222508012`, empirical reference
uncertainty `0.00003251686088225297`. Its absolute difference is
`0.001001942909340414`; adding uncertainty gives `0.001034459770222667`, while
subtracting uncertainty drops below `0.001`. It therefore remains **uncertain**,
not passed. No declared grid meets the complete empirical price contract.

The work fields are the reference solver's existing diagnostics. An implicit
time step may perform several Howard/Thomas solves, including an unconstrained
fresh-data seed. The reported maximum policy count is not the total sweep count;
neither field should be presented as total factorization work. Additional optional
counts are needed to attribute a continuation-run factor prototype. The finest
grid alone has over 64 million interior forward-row opportunities before extra
policy sweeps, but that lower bound is not a measured runtime fraction.

Replay command (a new output directory is required):

```powershell
$env:ATX_VOL_DDIV_PDE_REPLAY_OUT = 'C:/atx-cache/quote-benchmark/cash-pde-three-grid-new-run'
build-vol-rel/bin/atx-vol-discrete-div-production-tests.exe --gtest_filter=DiscreteDivProductionMatrix.DiagnosticThreeGridPdeReplay
```

Evidence under `calendar/cash-pde-three-grid-v1/` includes runtime red/green tests,
the archived executable, full `export/replay.json`, independent `verify_replay.py`
and `receipt.json`. All six focused manifest/replay tests and the owning PCH-off
check pass. The verifier preserves every baseline input/reference/Standard/Refined
record and independently recomputes all 150 dollar classifications. Replay SHA256:
`7be5a36e2be3de0e52aef4a94e8cba6a0b9dc040e92dfcb0e4b3bc14d5626d6c`.
No latency, Greek accuracy or production-PDE promotion is claimed. Resolve the
remaining price uncertainty with independent time/space/domain evidence before
calling any subsequent faster execution an accuracy-qualified result.

### Actual Thomas work: modest reuse inside one contract

The private reference entry accepts an optional caller-owned `PdeDiagnostics`.
It resets on entry (including refusal), records no clocks and changes no numerical
arithmetic. With no diagnostics, there is no extra allocation or policy-mask
traversal. The armed census separately visits each actual Thomas mask and counts
forward rows, identity/free rows, complete free-run lengths, fresh-data seeds,
and hypothetical lazy factor rows under exact bit keys `(lo,di,up)`. It is not a
coefficient cache and supplies no cached values to the solver.

| Grid | Actual Thomas sweeps | Continuation rows | Coefficient rows if lazily reused | Avoidable repeated coefficients |
|---|---:|---:|---:|---:|
| 400 x 200 | 11,703 | 4,096,079 | 3,659,938 | 436,141 (10.65%) |
| 800 x 400 | 22,977 | 16,067,975 | 14,529,779 | 1,538,196 (9.57%) |
| 1600 x 800 | 45,688 | 63,867,434 | 57,772,232 | 6,095,202 (9.54%) |

Each grid includes 170 unconstrained fresh-data seed sweeps. Every observed sweep
has one continuation run; the mean lengths are 350.00, 699.31 and 1,397.90 nodes.
This is an observation of these cases, not a permissible assumption about arbitrary
active masks. The complete 1600-grid forward pass visits 73,100,800 rows including
identity rows. Most implicit steps already settle in one policy solve, so the
single-contract cache opportunity is limited: at the finest grid it removes about
4.8% of the two continuation-row divisions, before charging extra table loads,
key checks or run tracking. It does not remove the RHS/backsolve or jump work.
Sharing exact coefficients across multiple compatible contracts could offer more,
but no such occupancy or performance result has been measured.

All 150 original replay fields, including price bits, work diagnostics, routes and
empirical classifications, are unchanged. The finest DSPYp uncertainty remains.
Thirty-one reference tests and six manifest/replay tests pass; owning reference
source and both changed tests compile with PCH disabled. Counter tests cover both
exercise styles/sides, exact output parity, complete row/histogram accounting and
reset after refusal. Evidence is `calendar/cash-pde-work-census-v1/`, including
runtime red/green, full replay, `verify_census.py` and `receipt.json`. The verifier
compares against the prior frozen replay bit for bit and independently recomputes
every row/histogram identity. These counts justify a bounded experiment, not a
claim that the extra cache machinery will be faster.

### Exact coefficient streams across contracts

The follow-up diagnostic records each actual implicit step's rounded binary64
`(lo,di,up)` bits, Thomas sweep count, continuation-row count and longest free run.
This records the operator used by the solver, rather than guessing equality from
volatility, maturity or a nominal grid. The optional census remains caller-owned;
the default solve performs no additional recording, allocation or numerical work.

For each declared grid, all 50 requests form **41 identical ordered coefficient
streams**. Six shared groups contain 15 requests; 35 requests are singletons:
`C8p/C8c`, `D12p/D12c`, `E1c/E5p/E5c`, `E20p/E20c`, `E64p/E64c`, and
`ILp/ILc/ISp/ISc`. Equality includes the actual space count and every coefficient
bit at every step. Equal recurrence coefficients do not imply equal jump maps,
cash models, policies or prices. In particular, IL/IS retain their different
liquidator/survivor models. No cash-model pooling is used to score accuracy.

The inventory simulates one lazy coefficient table per identical stream, clearing
it whenever the exact coefficient key changes. At each step the table need only
reach the longest continuation run across its members; each member retains its
own policy, RHS and backsolve. Replaying this rule for singleton groups exactly
reproduces the prior single-contract recurrence count.

| Grid | Original coefficient rows | Single-contract reuse | Exact grouped reuse | Additional rows avoided by grouping |
|---|---:|---:|---:|---:|
| 400 x 200 | 4,096,079 | 3,659,938 | 3,067,514 | 592,424 (14.46% of original) |
| 800 x 400 | 16,067,975 | 14,529,779 | 12,178,280 | 2,351,499 (14.63%) |
| 1600 x 800 | 63,867,434 | 57,772,232 | 48,449,011 | 9,323,221 (14.60%) |

Total coefficient recurrence reuse at the finest grid is 24.14%. Only one of the
two continuation divisions belongs to this recurrence, so this represents about
12.07% of those divisions before cache loads and lookup costs. It removes none
of the contract-specific RHS, backsolve, obstacle or jump work. For the 44
spot-liquidator requests, 37 exact streams require 43,698,305 coefficient rows
instead of 56,429,343, or 22.56% reuse. These are work counts, not wall-time savings.

A pair of binary64 tables for `cp` and the exact denominator needs at most 25,600
payload bytes for one active 1600-node stream, or 1,049,600 bytes for all 41 streams
if retained simultaneously, before ownership/index metadata. There are 25,622
distinct step keys across the 40,376 implicit steps. Full grid and event setup,
group discovery, exact key comparisons, allocation, policy-mask checks, table
loads and scalar refusals must be charged in any future cold batch measurement.
The inventory export itself is diagnostic overhead and is not a production
grouping implementation. No cache has been introduced or timed.

All 150 prior result fields are bit-identical, including grids, prices, solver
work and empirical price classifications. The finest DSPYp remains uncertain;
the spot-liquidator denominator is still 43 pass and one uncertain. Two focused
census tests and six manifest/replay tests pass. Owning reference source and both
changed test TUs compile with PCH disabled. Evidence is
`calendar/cash-pde-batch-keys-v1/`: runtime red/green, archived executables,
`export/replay.json`, `verify_batch_keys.py` and `receipt.json`. The verifier checks
the old receipt hash, every prior field, per-step accounting, singleton recurrence
counts and exact stream identities. Coefficients are decimal uint64 strings in
JSON to preserve all bits. Replay SHA256 is
`d4b5c2e81c8971cd9d37abd601139217ba8e71161eee38c3ada7c1dba60343cc`.

This ceiling does not justify prioritizing a recurrence cache alone. The next
structural study is SIMD across contracts with heterogeneous coefficients: each
lane can preserve its own exact operator, mask and recurrence while sharing the
instruction stream. That requires a separate inventory of compatible schedules,
inactive-lane divergence, layout conversion and complete cold setup cost. It must
retain every model and requested outcome; no sigma/strike quantization or
reference-driven selection is justified by the coefficient inventory.

## 7. Later methods and promotion boundary

If prepared PDE geometry and batched obstacle work do not win the measured
frontier, retain that result. An RKL explicit stabilized scheme is then a separate
candidate with the same cash/event tests; published American-option convergence
and Greek comparisons motivate an experiment, not a cash-model qualification.
[Le Floc'h, 2021](https://arxiv.org/abs/2106.12049).
An event-segment Volterra/GIT implementation is a larger research lane. Itkin's
paper provides a relevant hybrid-dividend formulation, but matching this
library's liquidation, terminal-event and exercise conventions still requires
reproduction. [Itkin, version 2](https://arxiv.org/abs/2510.18159v2).

Suggested ownership is: one tool/reference lane for the manifest, pinned cash
comparator and accuracy gates; a separate numerical lane for prepared workspace
and batched geometry, after interface review. Freeze each component before
integration. No default route switch or HFT/SOTA throughput claim is justified
until the same inputs pass the monetary-error gate with inclusive timing. The
continuous-carry SPY fitting result is a separate model qualification.
