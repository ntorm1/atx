# QuantLib American-put comparison, 24 October 2026

The pinned official QuantLib comparison is executable and measured. On the
252-contract common corpus, ATX default, QuantLib fast and QuantLib accurate all
met the $0.001 absolute price budget. Their measured cold costs were respectively
113.623, 146.745 and 217.044 microseconds per put. ATX bulk Fast cost 6.633
microseconds but missed that budget on 10 contracts. These are continuous-yield
put-pricing results, not whole-fit latency, cash-dividend or Greek results.

Both measured processes used clean source
`3a0071cde2a2b5c76a87d10f8f973c0474cea20d` and binary SHA256
`df4f428503ef062bbb173668d7d6fd7b32079fe7ba9f0ac6380d1f73cc112e39`.
Build: clang-cl 18.1.8, Release `/O2 /Ob2 /arch:AVX2`, ThinLTO enabled,
no fast-math. Hardware: Intel Core i7-1260P, process/thread pinned to logical
CPU 6, a P-core, worker environment 1. The common run measured 4.533% activity
on its SMT sibling; the diagnostic run measured 0%. The first is below the
predeclared 5% threshold but close to it. Native receipts preserve this activity,
system load and affinity. OS-reported 2100 MHz is not measured turbo frequency.

## What was compared

The official source is [QuantLib e96579fd](https://github.com/lballabio/QuantLib/tree/e96579fd471b2e9f6bc974c96674d1ccfff248fa),
dated 23 October 2026, pinned by archive, complete source-tree and owning-TU
hashes in [upstream.json](../bench/quantlib/upstream.json). The standalone target
compiles 42 unmodified official translation units. It uses the public
`OneAssetOption`/`QdFpAmericanEngine` pricing path; the official `VanillaOption`
constructor delegates to that base without changing pricing dispatch. The
unused implied-volatility/finite-difference dependency path is omitted. No
QuantLib code enters the shipping library.

The [runner documentation](../bench/quantlib/README.md) specifies the exact
model-clock construction, scheme parameters, source verification and commands.
Every effective maturity, discount, dividend discount, reconstructed rate and
variance is joined to the input. The synthetic DayCounter represents each
floating-point maturity exactly rather than rounding it to whole calendar days.
Four official premium cases, four clock cases, nine independent European-limit
prices and three official-factory parity checks pass before results are accepted.

| Profile | Declared scheme |
|---|---|
| QuantLib fast | Official Legendre `(7,2,7,27)`, Auto equation |
| QuantLib accurate | Official Legendre/tanh-sinh `(25,5,13,1e-8)`, Auto |
| ATX default | Reference 12/24/48/6, tolerance 1e-10 |
| ATX bulk Fast | Fast 7/8/32/2, tolerance 1e-8 |

The primary point reference is official QuantLib highPrecision, using freshly
constructed tanh-sinh `(10,30,1e-10)` and Auto equation. Refined ATX Reference
32/64/64/32, tolerance 1e-14, provides a cross-check. These are separate
implementations of related Andersen-Lake methods, **not independent numerical
methods or an absolute error proof**. A case is reference-unresolved when the
two reference prices differ by more than 5% of the tested synthetic budget.
Only the `r<=0,q>=0` European regimes have an independent closed-form oracle.

## Common-domain timing and residuals

The unchanged common corpus has 252 puts: 36 fixed `(K,T,sigma,r,q)` groups,
each with seven spots. `K=100`, `T={1/365,.25,1,3}`,
`sigma={.05,.2,.8}`, `(r,q)={(.03,0),(.03,.01),(.03,.08)}`;
spots are 40,70,90,100,110,140,200. Deep wings and cheap contracts remain included.
Times are medians of 30 whole-corpus repetitions in one process, divided by 252.

Cold includes fresh market/process/option/scheme/engine construction, pricing
and destruction for QuantLib; ATX uses its public scalar call with fresh state.
QuantLib's official factories normally share immutable schemes. This cold
experiment deliberately charges their exact fresh scheme construction; factory
output parity is tested. The reuse path amortizes setup across seven spots.

| Profile | Cold microseconds/put | Inclusive reuse microseconds/put | $0.001 pass / fail / unresolved | Maximum absolute residual |
|---|---:|---:|---:|---:|
| QuantLib fast |146.745|35.421|252 / 0 / 0|$0.00091003|
| QuantLib accurate |217.044|138.333|252 / 0 / 0|$0.00008524|
| ATX default |113.623|27.141|252 / 0 / 0|$0.00005058|
| ATX bulk Fast |6.633|9.189|242 / 10 / 0|$0.00370920|

All 252 results are finite for every profile, and cold/reused prices match
exactly. Every repetition's price digest is independently recomputed. QuantLib
reuse preserves the market/process/engine/scheme but **recalculates the exercise
boundary for each changed spot**. ATX reuse preserves a solved boundary. The
inclusive totals include setup, all seven evaluations and destruction; the two
reuse mechanisms are not equivalent APIs. Fixed engine order and one process
limit the timing conclusion. These are model-state-cold measurements, not
CPU-cache-flushed or individual fresh-process latency measurements.

At the tighter $0.0001 budget, the reference comparison cannot resolve 20 cases:

| Profile | Residual pass | Residual fail | Reference unresolved |
|---|---:|---:|---:|
| QuantLib fast |221|11|20|
| QuantLib accurate |232|0|20|
| ATX default |232|0|20|
| ATX bulk Fast |204|28|20|

Every row sums to 252. The largest highPrecision/refined-ATX disagreement is
$0.0000118402, at case 237 (`S=200,K=100,T=3,sigma=.8,r=.03,q=0`). Thus
232 resolved passes are not a 252-case accuracy claim. The synthetic absolute
budgets also differ from SPY's tighter-of-$0.001-and-1%-raw-spread acceptance.

## Broader-domain diagnostics

The unchanged 357-put diagnostic corpus adds negative-yield groups and explicit
zero-rate, negative-rate European and double-boundary regimes. It has one
repetition for coverage; its aggregate time includes failures and is not a
successful-price throughput comparison.

| Profile | $0.001 pass | Fail | Reference unresolved | Unsupported |
|---|---:|---:|---:|---:|
| QuantLib fast |348|2|0|7|
| QuantLib accurate |350|0|0|7|
| ATX default |350|0|0|7|
| ATX bulk Fast |336|14|0|7|

Every row sums to 357. Both libraries explicitly refuse all seven `q<r<0`
double-boundary puts. Their 350 finite results do not establish full-domain
American accuracy. QuantLib fast's largest residual is $0.00173576; ATX bulk
Fast's is $0.00419616. At $0.0001, 39 cases are reference-unresolved for each
profile; the pass/fail counts are respectively 297/14, 311/0, 311/0 and 272/39,
plus those 39 unresolved and seven unsupported cases. The largest reference
disagreement is $0.0000187237 at case 335 (`S=200,K=100,T=3,sigma=.8,r=.03,q=-.02`).

## Consequences for the performance work

This public comparison strengthens the evidence that ATX default is competitive
on this limited continuous-yield corpus. It does not establish the fastest
American library. The bulk profile's residual failures prohibit using its
headline speed as an accurate-price guarantee. The next measured frontier needs
the intermediate ATX accurate Fast and Screen profiles, keeping the same cases,
constructor accounting and references. Their speed/accuracy must be measured;
the default-versus-bulk endpoints do not determine the best profile.

For sub-second SPY fitting, the useful direction remains eliminating repeated
model setup and inversions while checking each accepted monetary error. A
standalone pricing microbenchmark cannot substitute for the complete 33-expiry,
12,866-row fit audit and cold clock. For a broader library claim, add a distinct
converged finite-difference/spectral reference, double-boundary support,
dated-cash-dividend contracts, Greek accuracy and workload-specific reuse groups.
Refine the current two references before treating the $0.0001 unresolved cases
as passes or failures.

## Immutable evidence

Local evidence lives under
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/acceptance/`:

* `quantlib-lto-common-v1/{receipt,native,summary}.json`;
* `quantlib-lto-diagnostic-v1/{receipt,native,summary}.json`;
* `quantlib-tests-final.log` (four focused native/harness tests);
* `quantlib-hygiene.log` (owning PCH-off compile).

[The manifest](quantlib-american-comparison-20261024.json) pins every report,
the source/binary/corpora and verification logs. Both raw receipts remain
unchanged. No shipping numerical code changed for this comparator.
