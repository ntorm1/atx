# American equity options: public frontier and reproducible comparator plan

Research frozen 2026-10-24. The original source review and experiment plan below
precede the executable comparisons. Follow-ups now measure both
[the pinned public American-put implementation](public-american-comparison-20261024.md)
and [official QuantLib fixed-point profiles](quantlib-american-frontier-20261024.md)
on 252 common-domain and 357 diagnostic contracts, with setup clocks and complete
failure coverage. Independent general-American accuracy, cash-dividend and Greek
qualification remain open. The local-main checkpoint `96cd56db` passes all
thirteen SPY correctness gates with the explicit two-row identification review.
Its three fresh ThinLTO fits took 1.1647743,1.0251590 and1.0989175 seconds; a
subsequent archived-baseline comparison encountered variable system load and
failed the SMT-idle gate, so no complete-fit speed gain is established. The
previous geometry/carry release measured1.0411881 seconds median. See
[current acceptance](spy-cold-acceptance-r7.md) for exact receipts and historical
comparisons. None qualifies the100-process subsecond goal. The experiments do
not widen the approved monetary budget.

The highest-value direction for the current subsecond objective is to reduce
the number of expensive pricing and inversion problems solved during a complete
fit. A faster individual pricing kernel helps, but cannot compensate for repeatedly
fitting, repairing, projecting, and re-implying the same contracts. Develop the
price-first global projection path alongside bounded final-price re-implication;
use the public implementations below to establish a measured accuracy/latency
frontier for the remaining pricing work.

There is no evidence here for an unconditional claim that any library is the
fastest American equity options library. A defensible claim must identify the
model, supported cases, accuracy, outputs, hardware, preparation costs, and
tail-latency distribution. A scalar American put price, a prepared batch of spot
scenarios, and a complete arbitrage-checked market fit are different products.

## 1. Current evidence and unchanged SPY contract

The historical independent audit at the original research cutoff is source
`0a5b2bc1acfe0d0cc45caa0e9aa0a5925fb386ff`. Its separate one-core development
fit took **13.0317839 seconds**. This is neither a ThinLTO qualification nor a
100-process subsecond result. The actual API audit passed all twelve correctness
gates: 33 valid expiries, 12,866 eligible rows served, 12,851 inside original
bid/ask boxes (**99.88341365%**), 12,430 identified rows and 436 genuine price-only
rows, with zero demotions from the frozen identified baseline. Maximum cold
Reference price residual was **$0.0007096053236/share**. Strict `1e-7` IV accuracy
still has 1,271 diagnostic failures; Greek accuracy has not been established.
The exact binary/policy/prediction hashes and receipts are in
[the acceptance report](spy-cold-acceptance-r7.md).

Retain these requirements for every performance experiment:

- Input SHA256 `b3fafd80f09326ce039ebdcfd7c26914ecbeb39817fdd00b35c6d49dcb2bf33e`;
  freeze eligibility from raw prerequisites, not the candidate's success flags.
- Serve all 33 positive-time valid expiries and all 12,866 eligible rows.
  At least 99.5% must lie in their original raw boxes; at most 64 rows may depart.
  The fifteen analytically contradictory asks explain the current unavoidable
  departures. Floor recovery changes projection constraints, not scoring boxes.
- For every identified final node, cold Reference price error must satisfy
  `E_i = min(0.001 USD/share, 0.01 * (raw_ask_i - raw_bid_i))`, with the oracle's
  separately reported numerical roundoff allowance. Duplicate raw rows each
  constrain the contract; locked quotes have zero economic budget. Crossed
  quotes do not acquire a budget by taking an absolute spread.
- Preserve all frozen baseline Success row IDs, and at least 12,183 identified
  rows. A legitimate change in identification needs an explicit baseline review;
  silently converting difficult roots into PriceOnly cannot make a candidate pass.
- Check actual published prices, statuses, intervals, and conventions, including
  strike interpolation, union grids across expiries, terminal floors, shape,
  call/put bounds, and calendar checks under the declared model. Node prices
  alone do not prove that the serving representation is correct.
- Require 100 distinct fresh-process successful trials on one physical core,
  one worker, the same frozen binary/policy/input, an idle SMT sibling under the
  existing gate, and native cold calibration time below one second in every trial.
  Audit instrumentation is off in timing runs. Join the performance and API audit
  by actual hashes and prediction identity, not a summary label.

These are necessary executable checks for the declared mark representation.
They do not establish a common stochastic process for independently implied
volatilities, arbitrary path-dependent payoffs, or certified hedge sensitivities.

## 2. What the primary sources actually establish

### Andersen, Lake and Offengenden: spectral free-boundary pricing

The 2016 journal paper uses transformed-boundary Chebyshev collocation,
Jacobi-Newton iteration and numerical integration. Table 9 reports 68,027
options/second with maximum absolute error `5.0e-3`, versus 3,650/second with
maximum error `4.2e-5`, on the authors' 2 GHz machine with Visual C++ 2013.
The test contains 4,495 options and excludes prices below 0.5. These are historical
price-throughput/error observations, not current hardware measurements or a
whole-chain fitting guarantee. The largest errors, omitted cheap contracts, and
boundary preparation matter more to our acceptance than the headline throughput.
Use this algorithm as a core continuous-carry comparator and reproduce the
parameter grid with the cheap contracts restored. [ALO, journal issue pp.39-87;
Table 9 on printed p.75](https://engineering.nyu.edu/sites/default/files/2019-03/Carr-adjusting-exponential-levy-models.pdf),
[author paper record](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2547027).

### QuantLib: current fixed-point and QD engines

Pinned QuantLib `e96579fd471b2e9f6bc974c96674d1ccfff248fa` supplies public fast,
accurate and high-precision fixed-point schemes. The fast preset is
`QdFpLegendreScheme(7,2,7,27)`; accurate is
`QdFpLegendreTanhSinhScheme(25,5,13,1e-8)`; high precision is
`QdFpTanhSinhIterationScheme(10,30,1e-10)`. The implementation explicitly refuses
the put double-boundary region `q < r < 0`. Thus it is a strong comparator within
its domain, not a universal oracle. The final quadrature tolerance alone is not
an error bound on the complete price. [Pinned implementation](https://github.com/lballabio/QuantLib/blob/e96579fd471b2e9f6bc974c96674d1ccfff248fa/ql/pricingengines/vanilla/qdfpamericanengine.cpp),
[scheme parameter declarations](https://github.com/lballabio/QuantLib/blob/e96579fd471b2e9f6bc974c96674d1ccfff248fa/ql/pricingengines/vanilla/qdfpamericanengine.hpp).

Its public QD base engine applies an intrinsic-value floor and call/put symmetry;
measure the public result as well as any explicitly labelled internal kernel.
The bulk test uses a reduced paper grid, compares schemes, and permits maximum
differences of `2.5e-3`; passing those upstream tests does not imply our per-row
budget. Preserve the upstream tests, then add the stricter common corpus.
[Public QD path](https://github.com/lballabio/QuantLib/blob/e96579fd471b2e9f6bc974c96674d1ccfff248fa/ql/pricingengines/vanilla/qdplusamericanengine.cpp),
[official American-option tests](https://github.com/lballabio/QuantLib/blob/e96579fd471b2e9f6bc974c96674d1ccfff248fa/test-suite/americanoption.cpp).

### Healy: negative rates and iterative boundary reuse

Healy's 2021 paper extends the boundary treatment to negative-rate cases and
discusses stability and two exercise boundaries. Its Table 6 distinguishes
individual pricing from a batch of ten spot values sharing a boundary. That
batch is not ten unrelated implied volatilities or ten different carry models.
The paper also uses finite-difference/TR-BDF2 comparisons; difficult crossing
boundaries make a robust independent fallback valuable. Hardware was not
established in this review, so its throughput figures are not used to rank
libraries. [Pricing American options under negative rates](https://arxiv.org/abs/2109.15157).

The author's AQFED implementation separates an `AndersenLakeRepresentation`
from subsequent `priceAmerican` calls and supplies an
`AndersenLakeNRepresentation` for negative rates. Its American module also offers
an RKL finite-difference route. Measure representation construction separately
from prepared evaluations, and measure Julia process/JIT startup separately
from compiled steady-state work. [Pinned module guide](https://github.com/jherekhealy/AQFED.jl/blob/602633022b07dfd79fcddd0e5a12c19801d62ce9/src/american/README.md),
[boundary implementation](https://github.com/jherekhealy/AQFED.jl/blob/602633022b07dfd79fcddd0e5a12c19801d62ce9/src/american/andersenlake.jl),
[negative-rate implementation](https://github.com/jherekhealy/AQFED.jl/blob/602633022b07dfd79fcddd0e5a12c19801d62ce9/src/american/andersenlaken.jl).

### New header-only ALO implementation

`simu-ai/american_put` is a small C++ implementation suitable for a separately
built continuous-carry put comparator. Its example timer starts after `FastPut`
construction; the constructor generates quadrature, transforms time nodes and
initializes Chebyshev data. The reported timed section therefore does not include
all preparation. The README's microsecond ranges lack a sufficiently specified
hardware context for a comparative claim. Reproduce upstream timing, then add an
end-to-end timer around construction plus pricing and a prepared-object timer.
Its presence does not establish cash-dividend, fitting, Greek, or difficult-regime
coverage. [Pinned README](https://github.com/simu-ai/american_put/blob/e7d138093e69fa8cdd6ce7dc7c3371d2899f87f4/README.md),
[timer](https://github.com/simu-ai/american_put/blob/e7d138093e69fa8cdd6ce7dc7c3371d2899f87f4/test_table2.cpp),
[constructor](https://github.com/simu-ai/american_put/blob/e7d138093e69fa8cdd6ce7dc7c3371d2899f87f4/fast_put.hpp).

### Cash dividends: integral transforms and recursive projections

Itkin's October 20, 2025 preprint develops integral equations and generalized
integral transforms for hybrid continuous, proportional and cash dividends with
time-dependent coefficients. Its numerical section principally times exercise
boundary construction in MATLAB, not a complete fit. A suggested roughly
100-fold C++ improvement is an expectation, not a measured result. Some illustrated
comparisons use a binomial model without the discrete dividends; those are not
same-model accuracy validations. This is a promising research candidate requiring
an independently converged cash-dividend oracle before performance conclusions.
[Paper and numerical section](https://arxiv.org/html/2510.18159v1).

Cosma, Galluccio, Pederzoli and Scaillet's recursive-projection work handles
discrete dividend effects through transition-density projections and backward
recursion. It provides another route to complete value functions rather than a
single point and studies richer dynamics. Shared transition matrices can be
valuable when the model and time-step structure permit reuse. Matrix preparation,
exercise-time discretization and spatial convergence must be included in our
comparison. [Authors' March 2016 paper](https://access.archive-ouverte.unige.ch/access/metadata/3cd39fab-57cb-4036-8950-da5211f36c1f/download),
[paper record](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2091236).

### Independent numerical oracle and Greeks

Reisinger and Witte analyze policy iteration for the discrete complementarity
problem arising from American-option PDE discretization. It is a useful independent
algorithm family for a converged reference and difficult-domain fallback. Its
iteration/complexity analysis is not a promise of a particular production latency.
[Policy-iteration paper, revised September 2011](https://arxiv.org/abs/1012.4976).

In 't Hout's 2024 Greek-accuracy study shows why price convergence alone is
insufficient: Crank-Nicolson can produce poor Delta/Gamma convergence even with
Rannacher smoothing, while suitable DIRK methods exhibit better behavior in the
studied problems. A separately validated time integrator and grid-refinement
study belong in the Greek oracle. We have not established that the current fit's
point IVs or price interpolation yield accurate Greeks. [Primary study](https://arxiv.org/abs/2401.13361).

### Practitioner performance lead requiring reproduction

Jesper Andreasen's 2021 CQF presentation, *American Option Pricing in a Tick*,
is a relevant lead for joint dividend/volatility calibration. Its primary PDF
was not retrievable directly during this review; indexed fragments do not
establish a complete hardware, tolerance, coverage and cold-start contract.
Retrieve the original and reproduce its workload before turning any slide timing
into a benchmark target. Do not substitute a third-party summary for that evidence.
[Primary presentation URL](https://www.cqfinstitute.org/sites/default/files/4%20-%20Jesper%20Andreasen%20american%20cqf%202021.pdf).

### Additional 2026 scope checks

Andersen, Itkin and Kazbek's June 2026 work extends integral equations to
time-dependent Heston dynamics, using COS and damped-Sinc expectations. Its
abstract reports approximately one to two seconds per contract against a refined
finite-difference comparator. This is relevant to a future common stochastic
volatility model, but it is a different model and workload from the current
constant-volatility American inverse. It does not set a competitive target for
our continuous-carry point kernel. Only the abstract was reviewed here; no
implementation or hardware comparison was performed.
[Primary paper](https://arxiv.org/abs/2606.27335).

Itkin's October 20, 2026 preprint extends damped-Sinc pricing to non-affine
stochastic-volatility models and reports agreement within one basis point of
strike against its finite-difference benchmarks. A strike-relative tolerance
cannot be equated with the approved raw-spread price budget. Treat this as a
model-extension research lead, with independent convergence and full setup cost
still to assess. [Primary abstract](https://arxiv.org/abs/2610.28847).

Fast-Vollib's June 2026 revised announcement explicitly covers European
Black/Black-Scholes/Merton prices, IVs and Greeks. Its vectorized and GPU IV
implementations are useful European component comparators, but their timings
are not American early-exercise comparisons.
[Primary announcement](https://arxiv.org/abs/2604.27210v2).

## 3. Freeze comparator versions and execution boundaries

The pinned revisions below were resolved through upstream Git/API metadata on
the research date. The companion
[machine-readable research manifest](american-options-public-frontier-20261024.json)
records source hashes and evidence status. Pins are reproducibility anchors,
not claims that every repository is a released or production-qualified library.

| Comparator | Frozen revision | Initial role |
|---|---|---|
| QuantLib | `e96579fd471b2e9f6bc974c96674d1ccfff248fa` | QD+, fixed-point fast/accurate/high-precision and finite differences |
| AQFED.jl | `602633022b07dfd79fcddd0e5a12c19801d62ce9` | Iterative boundary, negative rates, independent numerical implementation |
| american_put | `e7d138093e69fa8cdd6ce7dc7c3371d2899f87f4` | Small independent C++ ALO put implementation |
| atx-vol | Candidate source/binary/policy hashes per run | Existing public API, experimental algorithms separately labelled |
| GIT / recursive projections | Paper revision, then source hash if executable code is acquired | Cash-dividend research; no executable comparator claimed yet |

Build adapters as isolated benchmark tools; do not import competitor code into
the shipping library merely to conduct a comparison. Preserve upstream licenses
and numerical options in the artifact manifest. For native comparisons use the
same compiler version, architecture, floating-point semantics, standard library,
optimization and LTO policy where practical. Report unavoidable differences.
No fast-math changes hidden in one participant's configuration.

Use a shared immutable contract record: side, spot, strike, exact volatility
clock, financing clock, effective rates/carry, volatility, exercise interval,
dividend events and convention, model enum, price units, and raw quote references.
An adapter must either preserve the exact contract or return Unsupported with a
reason. Do not round fractional maturities to whole calendar dates to fit an API.
If a QuantLib adapter needs a custom day counter or an exposed continuous-time
kernel, label that adapter and time its construction; do not modify the pricing
algorithm while calling it stock QuantLib.

Benchmark public results and kernel-only experiments separately. Cache keys must
include every model parameter, clock, option and dividend event affecting the
boundary. Include preparation, validation, allocation and publication wherever
they occur in the production operation. Compiler elimination is prevented by
consuming and hashing all results; errors and unsupported cases remain visible.

## 4. Six workloads, six distinct performance claims

| Track | Timed boundary | Required accompanying evidence |
|---|---|---|
| Cold point price | Fresh object/family construction through one returned price/status | Maximum price error, preparation split, allocations, supported-domain count |
| Prepared family | Preparation plus N calls, and calls alone, for N = 1, 10, 100, 1,000 | Break-even N, parameter-sharing rules, bytes prepared, errors on all scenarios |
| Inverse | Target price and contract through typed IV/interval and cold repricing | Every raw-row economic budget, classification, calls, fallbacks, conditioning |
| Whole cold fit | Raw normalized input through final usable surface and first-query readiness | All SPY gates, stage/call counts, no prebuilt data-dependent caches |
| Live serving/update | Snapshot query or market-update publication through visible result | p50/p95/p99/p99.9/max, allocations, reader/writer load, stale-snapshot duration |
| Price plus Greeks | Same model inputs through prices and requested sensitivities | Independent Greek accuracy, event/boundary cases, complete derivative cost |

For the prepared-family track run both favorable identical-model spot scenarios
and heterogeneous strike/volatility/carry contracts. Report
`total(N) = prepare + evaluate(N)` and the observed break-even point against cold
calls. A fixed-boundary spot batch cannot stand in for a smile whose sigma and
carry vary by strike. Test cache invalidation as part of correctness.

For process cold starts, report both native calibration time and launcher-observed
process time. The former is the current SPY acceptance clock; the latter reveals
loader, initialization, parsing and serialization costs. Explicitly distinguish
empty algorithm caches from OS page-cache state. Do not selectively flush caches
for one competitor. For Julia also report first execution with compilation and
execution after compilation; never compare one against the other's opposite state.

For microsecond operations use many varied contracts per timed sample with
per-result correctness outside the timer. For cold SPY use the existing distinct
process identity/FILETIME and native-affinity evidence, not repeated calls in a
single warmed process. Log CPU model/microcode, OS, core topology, selected core,
SMT activity, frequency/power configuration, compiler/linker, flags, memory,
runtime, binary hashes and trial identifiers. Serialize heavy benchmark sessions
so concurrent agent builds cannot become an unreported variable.

Report error and latency distributions by regime, plus the maximum and every
failure. A global average must not hide cheap-wing, near-exercise, long-maturity,
negative-carry or cash-dividend failures. For a combined score keep a fixed
denominator and explicitly report unsupported contracts; publish a conditional
performance table as well as the complete-coverage table.

## 5. Common corpus and independent accuracy

Use three independently versioned inputs: frozen raw SPY for continuity, a
deterministic synthetic contract grid for numerical coverage, and held-out market
dates/underlyings for generalization. Vendor IVs and classifications may be
comparison labels only. They must never enter fitting, eligibility or reference
price construction.

The synthetic grid should deliberately retain cases omitted from convenient
throughput tests: prices below $0.50, almost-zero time value, very short positive
maturity, exercise plateaus, both option sides, deep ITM/OTM strikes, zero and
negative rates/yields, `q < r < 0`, near-boundary crossings, long maturities and
extreme but finite volatilities. Add duplicated rows with different spreads,
locked quotes, crossed quotes, invalid clocks, and quantization-scale carry
conflicts to test wrapper behavior. Publish valid, invalid, unsupported and
unidentified populations separately before timing any implementation.

The cash-dividend corpus requires zero, one and multiple events; events almost
at valuation/expiry; cash, proportional and mixed dividends; and spot values
near the cash deduction and exercise boundary. Freeze the stock jump convention,
limited-liability treatment, exercise ordering immediately before/after the
event, discounting and carry. An escrowed-dividend approximation is a separate
model, not a faster implementation of a spot-jump model.

Reference hierarchy:

1. Use independently derived closed forms in applicable limiting cases and exact
   exercise payoffs where classification is established.
2. Compare converged boundary pricing against an independently implemented
   PDE/complementarity method for continuous carry. Multiple ALO ports alone may
   share the same mathematical or quadrature error.
3. For cash dividends require a refined event-aware numerical oracle under the
   same jump convention, with independent space/time/event interpolation studies.
4. Increase boundary order, quadrature, iterations, spatial/time resolution and
   domain extent separately. Preserve all convergence sequences and sign/status
   changes; one high-precision preset label is insufficient.

For the comparator program, a proposed reference-uncertainty target is at most
5% of the tested positive economic budget. This is an experimental requirement,
not a theorem from successive-grid agreement. Locked/zero-budget cases need
their own numerical evidence and explicit roundoff handling. Do not loosen the
approved SPY gate to accommodate an inconclusive external reference; mark that
comparison unresolved and keep the existing gate intact.

Inverse accuracy uses the **actual final published price** as target. Validate
cold reference residual at the returned sigma, all raw duplicate-row budgets,
model/clock/carry identity, and the typed interval/status. Retain strict IV
straddles and local residual/vega as diagnostics, not promotion criteria under
the approved economic contract. Flat prices can make a point IV poorly identified
even while the price is accurate; inventing a precise root or hiding it through
mass PriceOnly demotion is unacceptable.

## 6. Fundamental changes worth testing now

### A. Solve final prices once, then identify volatilities once

Implement a private price-first draft: normalize raw contracts, construct one
global feasible price problem, project slice/calendar constraints, and only then
build final IV metadata and the serving representation. Preserve raw boxes for
metrics and permit only the already specified terminal-floor conflict recovery.
The draft must preserve every valid slice even if an older IV-first rung would
have refused it. Keep an independently verifiable price-only representation
available where IV identification is genuinely impossible.

Instrument a work ledger before tuning: initial inverse count, repaired inverse
count, repeated-target count, accurate cold calls, family builds/validations,
projection iterations and allocations, and publication/query work. Success means
eliminating a documented category of work while passing the complete API oracle.
Do not replace repeated fits with a single projection that silently weakens
between-node/calendar/terminal constraints. This direction is an engineering
inference from the current pipeline and audits, not a result established by a
public pricing throughput table.

### B. Spend numerical effort in price units

Use a cheap family/native candidate as a sigma seed. Accept it only after the
declared cold Reference price and classification checks satisfy the original-row
budget, with the existing non-flat/positive-sensitivity safeguards. Fall back to
the robust inverse when it fails. Avoid a second full Search merely to obtain a
seed that is already acceptable. Keep approximation acceptance opt-in and part
of immutable model/budget identity.

The important counter is total expensive reference work per final identified
node. Measure how often candidate, confirmation, failure and fallback each price
the same point. Batch independent final nodes without allowing cached warm
boundaries to change the declared cold accuracy map. Do not tune the economic
cap or raw spread fraction to make a benchmark pass.

### C. Reuse compatible boundaries and control interpolation error

Share immutable preparation only across compatible models. Separate constants
and quadrature shared globally, exercise boundaries shared by model parameters,
and evaluation state specific to each price. Prepare only regions actually
needed by the quote set. Evaluate a region/family's full preparation plus
validation cost against the accurate prices it displaces.

For interpolation across sigma or carry, use adaptive refinement driven by the
tightest local raw budget and independently checked error. Keep corner, exercise,
near-zero-time and low-vega regions explicit. Native vectorization improves the
remaining homogeneous batches; it does not remove family construction, failed
admission, scalar confirmation or repeated publication work. Reject a new cache
or interpolation scheme if end-to-end cost rises at the actual workload size.

### D. Develop general cash-dividend and Greek support as a separate lane

Compare event-aware finite differences with recursive projection and the GIT
research method under identical cash jumps. Exploit interval reuse between
events and batched right-hand sides where valid. A robust numerical fallback
with measured error is more useful than claiming continuous-yield equivalence
for all equities. Retain event schedules in preparation identities and query
metadata so an ex-dividend update cannot reuse stale boundaries.

Longer term, investigate fitting a shared positive transition operator or local
model, then pricing calls/puts and exercise decisions from that model. This may
give a coherent path to model Greeks and wider products. It is a larger model
change: exact containment of arbitrary noisy quotes is not guaranteed, and
European surface no-arbitrage alone does not prove American consistency. Keep
this experiment separate from the current serving-mark contract until its
coverage and model assumptions are independently established.

## 7. Greek accuracy needs its own contract

Define each requested quantity first: fixed-vol model Delta/Gamma/Vega, a
recalibrated smile hedge, financing/carry sensitivity, and time roll across a
dividend event are different calculations. Returning a derivative of a PWL mark
curve is not evidence for any of the others.

As a proposed engineering experiment, use deterministic spot and volatility
shocks `hS = 0.001*S` and `hSigma = 0.0001` and require each sensitivity error's
first relevant Taylor contribution to consume at most one quarter of the
positive price budget:

`abs(delta-delta_ref)*hS <= E/4`

`0.5*abs(gamma-gamma_ref)*hS*hS <= E/4`

`abs(vega-vega_ref)*hSigma <= E/4`

These proposed shock choices are not an already approved risk tolerance or a
universal industry standard. Validate them against actual hedging use before
promotion. Also compare complete bumped prices: cancellation can make a
derivative-only check misleading. At exercise/event nonsmoothness use explicit
one-sided sensitivities and scenario P&L; do not force a nonexistent smooth Gamma.

Reference Greeks require independent grid/time refinement and finite-difference
step studies away from nonsmooth points. If differentiating a boundary solver,
include boundary dependence, solver convergence and exercise classification.
Automatic differentiation through a fixed iteration count is not by itself a
convergence certificate. Measure price-plus-risk cost, allocation and worst-case
fallbacks together, then measure the production query path under concurrent
snapshot publication.

## 8. Ordered delivery plan and decision gates

| Order | Deliverable | Completion evidence / stop condition |
|---|---|---|
| 1 | Preserve full SPY oracle and fix serving correctness defects | All actual-API gates, metadata, no identification demotions; any bypass blocks performance promotion |
| 2 | Price-first draft and one final identification pass | Exact work-count reduction, unchanged eligibility/raw metrics, final-node cold price budgets and all-domain serving checks |
| 3 | Candidate-seed acceptance and remaining native batches | Reference-call counts, all failures retained, integrated same-binary audit, quiet one-core timings |
| 4 | Pinned external C++ adapters and corpus | Upstream tests plus exact contract round-trip and independent maximum-error report; unsupported cases visible |
| 5 | Cold/prepared/inverse frontier | Pareto plots of latency versus maximum error with preparation included; reject comparisons mixing models or timing boundaries |
| 6 | General carry/cash-dividend oracle and fallback | Independent convergence on event corpus, typed unsupported behavior, separate cold/batch evidence |
| 7 | Greeks and live-update serving | Explicit sensitivity semantics, independent risk-error limits, tail latency and allocation/staleness evidence |
| 8 | Public performance qualification | Reproducible machine/source/data manifest; full SPY 100-process gate plus held-out cohort and supported-domain tables |

Subsecond SPY leaves an average of about **77.7 microseconds per raw eligible
row for the entire fit**, before accounting separately for 33-slice preparation,
global constraints and publication. That arithmetic explains why eliminating
duplicate work is essential. It is not a promised per-price allowance: shared
work, paired contracts and typed intervals change the actual distribution.
Allocate stage budgets only after the price-first and final-identification
profiles exist; do not forecast a subsecond total by adding unrelated isolated
microbenchmark gains.

The comparator runner should produce a manifest, immutable contract/reference
records, per-case output/error/status records, stage counters and native trial
reports. Include every maximum-error case and every unsupported/failure context.
Store public-source downloads and hashes separately from generated binaries;
record compiler/runtime versions and preserve exact options. A useful eventual
claim is: fastest among named pinned implementations on a published workload,
at a stated maximum error and coverage, on specified hardware. Evidence for that
claim remains to be collected.
