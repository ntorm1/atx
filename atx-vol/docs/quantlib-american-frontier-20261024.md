# Intermediate American-put pricing profiles, 24 October 2026

Adding ATX accurate Fast and Screen produces useful new speed/accuracy points on
the pinned QuantLib comparison. On the common 252-contract corpus, Screen cost
15.546 microseconds per cold put and passed the $0.001 absolute price comparison
on every contract. Accurate Fast cost 58.351 microseconds, with a maximum price
difference from ATX default of $2.14e-14. These are measured corpus results, not
uniform error guarantees or a whole-library speed claim.

This follow-up preserves the [first report](quantlib-american-comparison-20261024.md)
and all its raw receipts. The new six-profile schema adds accurate Fast
12/24/48/6 and Screen 7/16/48/5; it changes no shipping numerical code. It also
checks exact structured options, reference metadata, price/status consistency,
complete self-test tuples and scheme identities. Seven concrete altered v1
reports that the old verifier accepted now fail closed. Both original v1
summaries reproduce identically under the stronger verifier.

## Frozen experiment

Implementation commit: `8b6d258ac41cd6a1fd71026d1807965cc51d5f5e`.
Measured source: clean `e78dcc2b` (only the v1 binary-retention clarification
follows that implementation). Binary SHA256:
`4ce4694377435c46a09c67eb245a60e539f70b58a9b6d6d88fd2a3f6cc9b7bd7`.
This executable was archived under its SHA256 before either measured run.

Hardware/build remain Intel Core i7-1260P, logical CPU6 P-core, one worker,
clang-cl18.1.8 Release `/O2 /Ob2 /arch:AVX2`, ThinLTO, no fast-math.
Both runs measured 0% activity on the SMT sibling. The 252-case common corpus
has 30 in-process repetitions; the 357-case diagnostic corpus has one.
All preexisting profiles' prices, statuses and reference results match v1
exactly. No cases, budgets or model clocks changed.

The source is unmodified [official QuantLib e96579fd](https://github.com/lballabio/QuantLib/tree/e96579fd471b2e9f6bc974c96674d1ccfff248fa).
The [runner documentation](../bench/quantlib/README.md) defines the full setup,
factories, exact maturity clock, public OneAssetOption pricing dispatch and
reproduction commands. The QuantLib cold clock includes a fresh market, process,
option, scheme and engine; ATX cold is its public scalar call. Reuse totals
charge setup, seven spot evaluations and destruction. QuantLib reuses the engine
but recalculates the boundary per changed spot; ATX retains a solved boundary.

## Common-corpus measured points

The unchanged 252 contracts are the full product of `K=100`,
`T={1/365,.25,1,3}`, `sigma={.05,.2,.8}`,
`(r,q)={(.03,0),(.03,.01),(.03,.08)}` and
`S={40,70,90,100,110,140,200}`. Each row below has 252 finite prices.
Times are the median whole-corpus clock divided by 252; setup is included.

| Profile | Cold microseconds/put | Inclusive reuse microseconds/put | $0.001 pass / fail | Maximum residual |
|---|---:|---:|---:|---:|
| QuantLib fast |114.804|27.637|252 / 0|$0.00091003|
| QuantLib accurate |216.330|138.929|252 / 0|$0.00008524|
| ATX default Reference |129.881|31.959|252 / 0|$0.00005058|
| ATX accurate Fast |58.351|18.760|252 / 0|$0.00005058|
| ATX Screen |15.546|9.167|252 / 0|$0.00025508|
| ATX bulk Fast |6.627|5.156|242 / 10|$0.00370920|

Screen is the fastest tested profile that meets this corpus's $0.001 comparison.
It is not a replacement for accurate final-price checks across an unrestricted
domain. Accurate Fast gives essentially the same prices as default here with
less measured work. Neither conclusion establishes Greek accuracy, cash-dividend
accuracy, SPY's raw-spread-scaled budget or absence of exercise-boundary errors.

At $0.0001 the primary QuantLib highPrecision and refined ATX references disagree
by more than 5% of the budget on 20 cases. These stay unresolved:

| Profile | Residual pass | Residual fail | Reference unresolved |
|---|---:|---:|---:|
| QuantLib fast |221|11|20|
| QuantLib accurate |232|0|20|
| ATX default Reference |232|0|20|
| ATX accurate Fast |232|0|20|
| ATX Screen |225|7|20|
| ATX bulk Fast |204|28|20|

The general references use separate implementations of related Andersen-Lake
methods; their agreement is not an independent-method error proof. The limited
European regimes have a separate closed-form oracle. A converged independent
complementarity/PDE reference is still required to resolve the tighter-budget
cases and test shared method errors.

## Broader-domain results

The unchanged diagnostic set adds negative yields, zero/negative-rate European
limits and seven double-boundary puts. Every profile explicitly refuses the
same seven double-boundary contracts; no refusal is removed from the denominator.

| Profile | $0.001 pass / fail / unsupported | $0.0001 pass / fail / unresolved / unsupported | Maximum residual |
|---|---:|---:|---:|
| QuantLib fast |348 / 2 / 7|297 / 14 / 39 / 7|$0.00173576|
| QuantLib accurate |350 / 0 / 7|311 / 0 / 39 / 7|$0.00008524|
| ATX default Reference |350 / 0 / 7|311 / 0 / 39 / 7|$0.00005998|
| ATX accurate Fast |350 / 0 / 7|311 / 0 / 39 / 7|$0.00005998|
| ATX Screen |350 / 0 / 7|303 / 8 / 39 / 7|$0.00033908|
| ATX bulk Fast |336 / 14 / 7|272 / 39 / 39 / 7|$0.00419616|

Every count sums to 357. All finite cold/reused values match exactly. The broad
set's single-repetition timing includes refusal paths and is not reported as
successful-price throughput. Accurate Fast and default differ by at most
$2.14e-14 across the entire finite set; this is observed agreement, not a
floating-point identity contract for all inputs.

## Timing limits and next decisions

The unchanged QuantLib-fast/default pair reverses its timing order between v1
and v2. QuantLib fast moves from 146.745 to 114.804 microseconds per cold put;
ATX default moves from 113.623 to 129.881. Their prices are bit-identical across
runs. Both receipts are retained. This is evidence that one process with fixed
engine order does not establish a robust close ranking. CPU frequency, scheduling
and code layout have not been isolated, so no single cause is assigned. The
relative speed claims should next use multiple fresh processes with balanced
engine order and the same archived binaries before being promoted to a durable
benchmark baseline.

The practical profile candidates are now explicit: accurate Fast for the tested
near-Reference behavior, Screen where the full monetary error check accepts it,
and bulk only when a separate bound/reference confirms the result. The current
SPY goal still needs its complete cold fit clock and all independent coverage,
containment, shape, parity, calendar and monetary/floor gates. A 15-microsecond
scalar price is not evidence of a sub-second whole fit.

## Evidence and retention

Evidence root:
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/acceptance/`.

* `quantlib-lto-common-v2/{receipt,native,summary}.json`;
* `quantlib-lto-diagnostic-v2/{receipt,native,summary}.json`;
* `quantlib-frontier-tests-final.log` (five focused Python/native tests);
* `quantlib-frontier-hygiene.log` (owning PCH-off compile);
* `quantlib-harness-adversarial-red-green.json` (seven malformed-report checks);
* `quantlib-binaries/4ce4694377435c46a09c67eb245a60e539f70b58a9b6d6d88fd2a3f6cc9b7bd7.exe`.

[The v2 manifest](quantlib-american-frontier-20261024.json) pins these artifacts
and both frozen corpora. The original v1 executable was not retained before its
build path was reused; its receipts preserve the measured before/after hash and
the original source pins. That limitation is explicitly recorded in the
[v1 availability companion](quantlib-american-comparison-20261024.availability.json),
without rewriting either historical receipt or its evidence manifest.
