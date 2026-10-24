# Pinned QuantLib American-put comparator

This default-off executable compares the public QuantLib QdFp pricing path with
ATX on the **same** frozen continuous-yield contracts used by
[the first public comparator](../public-american/README.md). It retains every
case, error and unsupported contract. No QuantLib source is modified or linked
into shipping ATX targets. Pricing, fitting, cash dividends and Greeks are
different workloads; this tool measures only continuous-yield put prices.

## Source and dependency isolation

`upstream.json` pins [QuantLib commit e96579fd](https://github.com/lballabio/QuantLib/tree/e96579fd471b2e9f6bc974c96674d1ccfff248fa),
the downloaded archive SHA256, the entire extracted `ql/` source tree plus
`LICENSE.TXT` digest, and every selected translation unit's SHA256. The original
QuantLib license stays in that external cache. Configure verifies all files and
does not access the network. Source modifications, including unlisted header
changes, fail the tree check.

The comparator builds 42 unmodified official translation units for QdFp/QdPlus,
the public option/process/curve/date path and its numerical dependencies. It
omits unrelated bond, swap, calibration, cash-dividend and FD components from the
985-TU full library. Configuration uses the official generated-header templates:
C++20, standard shared pointers, faster lazy objects enabled, sessions/tracing/
thread-safe observers/OpenMP disabled. Boost headers come from the existing
toolchain (1.91.0 in this run); the native report records `BOOST_VERSION`.
No dependency installation or shipping dependency change is required.

The public object is `OneAssetOption` with `PlainVanillaPayoff`,
`AmericanExercise`, `BlackScholesMertonProcess` and `QdFpAmericanEngine`.
At the pinned commit, `VanillaOption`'s constructor only delegates to
`OneAssetOption`, and its NPV/arguments/engine dispatch are inherited unchanged.
Using that public base avoids the unused `VanillaOption::impliedVolatility` TU,
which pulls in finite-difference and uBLAS dependencies. This is labelled the
**public QuantLib pricing path**, not a VanillaOption-constructor benchmark.
No formula or edge-case branch is reimplemented in the adapter.

## Exact model clocks and self-tests

The synthetic reference date is 1 January 2020, and the maturity date is exactly
365 days later. A dedicated `DayCounter` maps this interval to the requested
floating-point `T` exactly. Its definition is `(date2-date1)/365 * T` for every
date pair; it is a benchmark model clock, not a recommended market day count.
This avoids converting `.25` years to 91/365 or another rounded calendar maturity.
Funding/dividend curves and variance all use the same clock and original
annualized `r`, `q`, `sigma`.

The tool exports each effective `T`, discount, dividend discount, variance and
rate reconstructed by QuantLib. Python independently joins every model back to
the frozen group. `T` must match exactly; discount/variance roundoff limits are
eight machine epsilons. Reconstructed rate roundoff is bounded by
`64*epsilon*max(1,abs(rate*T))/T`, reflecting QuantLib's `-log(discount)/T` API.
The model is never changed to improve agreement. Missing evidence fails closed.

Before timing claims, the native `--self-test` path reproduces four premium
values from the official `testAndersenLakeHighPrecisionExample` using FP_A and
FP_B, four exact-time checks, nine independent Black-Scholes European-limit
prices, and three fresh-scheme versus official-factory output checks. The Python
verifier independently pins the expected premium values and tolerances; it does
not trust arbitrary expected values supplied by a native report. Every corpus
report carries the same self-test evidence, outside its measured clock.

## Profiles, cold setup and reuse

| Profile | Scheme |
|---|---|
| `ql_fast` | Official Legendre `(l,m,n,p)=(7,2,7,27)`, Auto equation |
| `ql_accurate` | Official Legendre/tanh-sinh `(25,5,13,1e-8)`, Auto |
| Reference only | Official highPrecision tanh-sinh `(m,n,eps)=(10,30,1e-10)`, Auto |
| `atx_default` | Cold nullopt Reference, resolved 12/24/48/6, tolerance 1e-10 |
| `atx_bulk_fast` | Explicit Fast 7/8/32/2, tolerance 1e-8 |
| Cross-check only | ATX Reference 32/64/64/32, tolerance 1e-14 |

QuantLib's official factories cache immutable schemes in function statics. For
the charged cold path, we construct fresh instances of those **exact** scheme
classes and parameters. Their output parity with the official factories is
tested. This deliberately includes scheme/quadrature setup instead of charging
it only once invisibly; the reused path amortizes it explicitly.

* QuantLib cold includes fresh quotes, curves, process, payoff/exercise, option,
  scheme and engine construction, NPV calculation and destruction per contract.
* QuantLib reuse constructs that state once per `(K,T,sigma,r,q)` group, then
  changes only spot. Its engine recalculates the boundary on each changed spot;
  this is **engine/market/scheme reuse**, not solved-boundary reuse.
* ATX cold is the public scalar price call. ATX reuse constructs geometry and
  solves a boundary once per group, then prices the seven spots through it.
* Reuse setup, evaluation and complete total are all exported. The total covers
  setup, every evaluation and destruction; report its amortized cost together
  with the reuse kind. Shared Settings initialization, JSON and independent
  reference checks are outside both timing loops.

All profiles share one compiler and executable. Every repetition's prices have
an independently verified digest. Cold/reused prices must match exactly; status
and result counts must equal the frozen denominator. Exception/nonfinite paths
remain measured attempts, so diagnostic-corpus timing is not a successful-price
throughput statistic. Engine order is fixed; this is model-state cold, not a
CPU-cache flush or fresh-process latency per individual contract.

## Scoring limits

The primary point comparison is official QuantLib highPrecision. It is a
separate public implementation of the **same ALO method**, not an independent
numerical-method proof. Refined ATX supplies a second implementation check. A
case is unresolved when either reference refuses or their disagreement exceeds
5% of the synthetic `$0.001` or `$0.0001` budget. Every residual, failure model
and status is retained. These synthetic budgets are not the SPY raw-spread rule.

For `r<=0,q>=0`, Python also calculates the European Black-Scholes put directly;
that is the limited independent analytical check. No result asserts universal
American accuracy, double-boundary support, cash-dividend or Greek accuracy.

## Reproduce

Obtain the exact public commit archive at the URL in `upstream.json`, verify its
SHA256, and extract it into an external cache while retaining `LICENSE.TXT`.
Use the leased worktree's normal build wrapper; replace the paths below with
the actual worktree/cache:

```powershell
python quantlib_comparator.py --verify-source C:/atx-cache/QuantLib-e96579fd471b2e9f6bc974c96674d1ccfff248fa
powershell C:/atx-wt/pool-33/scripts/atx-build.ps1 -Preset vol-rel configure -DATX_VOL_PUBLIC_QUANTLIB_DIR=C:/atx-cache/QuantLib-e96579fd471b2e9f6bc974c96674d1ccfff248fa
powershell C:/atx-wt/pool-33/scripts/atx-build.ps1 -Preset vol-rel -Jobs 2 build atx-vol-quantlib-comparator
$env:ATX_QUANTLIB_COMPARATOR_BINARY='C:/atx-wt/pool-33/build-vol-rel/bin/atx-vol-quantlib-comparator.exe'
python -m unittest test_quantlib_comparator
```

Use `vol-hygiene` for owning PCH-off checks. For speed measurements, rebuild the
same target under `vol-rel-avx2` (ThinLTO), coordinate a quiet machine window,
then run on a verified physical P-core with its SMT sibling idle:

```powershell
python quantlib_comparator.py --binary C:/atx-wt/pool-33/build-vol-rel-avx2/bin/atx-vol-quantlib-comparator.exe --upstream C:/atx-cache/QuantLib-e96579fd471b2e9f6bc974c96674d1ccfff248fa --corpus ../public-american/corpus-common-v1.json --output C:/atx-cache/quantlib-common-001 --reps 30 --affinity 6 --require-pcore
```

Run the 357-case diagnostic corpus separately and retain its unsupported cases.
New output directories are mandatory. Receipts freeze the source commit/dirty
state, binary/corpus/source/cache hashes, compiler/flags/IPO, process identity,
affinity/SMT activity and every raw repetition. The source tree and binary/input
hashes are checked again afterward. The benchmark does not itself assert that
the machine was idle; inspect the receipt before citing its timings.
