# Public American-put comparison, 24 October 2026

The first pinned public comparison is executable and measured. It establishes
a limited continuous-yield pricing comparison against one public implementation;
it does not establish the fastest library, full-domain American accuracy,
cash-dividend/Greek accuracy, or a sub-second SPY fit.

Source `21a0dffed4bc4d28220079c2ec2aebf5e9a72bff` was clean for both runs.
The binary SHA256 is
`d081776a776d57c55375751111c0c7e35a0b54604d927ee5efe88e7c94a2fa3b`.
Build: clang-cl 18.1.8, Release `/O2 /Ob2 /arch:AVX2`, ThinLTO enabled,
no fast-math. Hardware: Intel Core i7-1260P, process/thread pinned to logical
CPU 6, a P-core, worker environment 1. Its SMT sibling's measured activity was
0.228% in the common-corpus run and 0% in the diagnostic run. OS-reported 2100 MHz
is retained in the receipt; it is not a measurement of effective turbo frequency.

The public source is [simu-ai AmericanPut at e7d13809](https://github.com/simu-ai/american_put/tree/e7d138093e69fa8cdd6ce7dc7c3371d2899f87f4),
MIT-licensed, pinned with ten source/license hashes in
[upstream.json](../bench/public-american/upstream.json). No public code enters
the shipping library. [The runner documentation](../bench/public-american/README.md)
defines the exact corpus, profiles, clocks, reference limitations and commands.

## Common-domain timing and accuracy

The predeclared common corpus has 252 puts: 36 fixed `(K,T,sigma,r,q)` groups,
each queried at seven spots. `K=100`, `T={1/365,.25,1,3}`,
`sigma={.05,.2,.8}`, `(r,q)={(.03,0),(.03,.01),(.03,.08)}`;
spots are 40,70,90,100,110,140,200. Cheap and deep-wing options are retained.
Each reported timing is the median of 30 in-process whole-corpus repetitions,
divided by 252. Cold includes fresh construction/boundary solve for every put.
Prepared total includes construction and one solve per group, seven prices and
destruction, amortized across those seven contracts.

| Profile | Cold microseconds/put | Prepared total microseconds/put | Within $0.001 | Within $0.0001 | Maximum absolute residual |
|---|---:|---:|---:|---:|---:|
| Public fast, order 7 / quadrature 15,41 /3 iterations |25.451|5.324|213/252|178/252|$0.1208289|
| Public finer, order 16 / quadrature 31,61 /16 iterations |567.624|84.890|252/252|252/252|$0.00003612|
| ATX default Reference 12/24/48/6 |117.869|29.138|252/252|252/252|$0.00004696|
| ATX bulk Fast 7/8/32/2 |8.238|10.493|242/252|208/252|$0.00370359|

All prices in this corpus are finite; cold and prepared prices match exactly.
Every public adapter output also matches the original `calc()` bits. The public
constructor's quadrature generation is charged; the original upstream timer
starts after construction, so these numbers must not be equated with its headline
timer. The general reference consists of refined ATX Reference calculations.
Their convergence agreement is an internal consistency test, **not an independent
error proof**. Both synthetic budgets differ from the SPY raw-spread budget.

The two faster profiles do not meet either budget on the full common corpus.
That rules out replacing final reference checks with a blanket fast-profile
assumption. Among the two profiles passing this limited corpus, ATX default is
faster than this particular public finer configuration. Neither this small grid
nor one public implementation establishes the competitive frontier.

Prepared bulk Fast is slower than cold bulk Fast for this seven-spot workload.
The result is real in this receipt, but its cause has not been profiled. Geometry
construction, allocation, unconditional boundary preparation and cold-contract
shortcuts are hypotheses to measure. It is not evidence that boundary reuse is
generally slower; its value depends on group size and contract regime.

## Broader-domain diagnostics

The 357-put diagnostic corpus adds negative-yield groups and explicit zero-rate,
negative-rate European and double-boundary cases. Its one repetition is for
coverage/accuracy diagnostics; early refusals/nonfinite results make its aggregate
time unsuitable as a successful-pricing comparison.

| Profile | Residual pass at $0.001 | Residual fail | Reference unresolved | Nonfinite / unsupported |
|---|---:|---:|---:|---:|
| Public fast |213|39|7|98|
| Public finer |252|0|7|98|
| ATX default |350|0|0|7|
| ATX bulk Fast |336|14|0|7|

Every row sums to 357. ATX refuses all seven `q<r<0` double-boundary puts with an
explicit typed error. The public implementation returns finite numbers for those
seven, but our general reference refuses, so no accuracy conclusion is drawn.
Its 98 nonfinite cases remain in the denominator and are not repaired by the
adapter. ATX's 350 residual passes are **not** full-domain accuracy.

The European regimes `r<=0,q>=0` have a separately implemented Black-Scholes
closed-form check. Only those regimes have an independent analytical reference
in this runner. The model's general American boundary remains unvalidated by an
independent solver. Greeks and discrete cash dividends are outside this corpus.

## Evidence and next comparison

Immutable local evidence lives under
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/acceptance/`:

* `public-comparator-lto-common-v1/{receipt,native,summary}.json`;
* `public-comparator-lto-diagnostic-v1/{receipt,native,summary}.json`;
* `public-comparator-tests-final.log` (five focused Python/native tests);
* `public-comparator-hygiene.log` (owning PCH-off compile).

[The evidence manifest](public-american-comparison-20261024.json) pins those files,
the binary and both corpora. Source/binary/corpus/model joins, original public
calculation parity, repeated-output determinism and cold/prepared parity were
checked. No worker or source changes occurred during either measured process.
All engines share one compiler and binary, but fixed engine order and a single
process per corpus leave thermal/frequency and ordering limits. Repeated fresh
processes with reordered engines are needed before stronger latency claims.

The next comparator is pinned QuantLib QdFp fast/accurate on the **same exact
continuous-time contracts**, with construction charged and exception coverage
retained. Its high-precision scheme provides an independent implementation,
although it is still based on the same ALO method. A different-method American
reference, cash-dividend corpus and independently validated Greeks remain
separate work. These pricing results support keeping explicit monetary error
guards while optimizing setup reuse; they do not change any SPY acceptance gate.
