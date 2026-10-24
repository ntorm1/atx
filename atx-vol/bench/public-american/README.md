# Pinned public American-put comparison

This opt-in tool compares the unmodified MIT-licensed [simu-ai AmericanPut
implementation](https://github.com/simu-ai/american_put/tree/e7d138093e69fa8cdd6ce7dc7c3371d2899f87f4)
with atx-vol on identical continuous-yield GBM puts. It does **not** establish
the fastest American-option library, full-domain accuracy, SPY fit latency,
cash-dividend accuracy, or Greek accuracy. QuantLib/AQFED and an independent
American reference remain necessary comparator work.

`upstream.json` pins the upstream commit, ten file hashes, original license,
and source date. All public files live in an explicitly selected external cache;
they are neither vendored nor linked into the shipping library. CMake refuses
missing/modified sources and only creates the comparator when
`ATX_VOL_PUBLIC_AMERICAN_PUT_DIR` is set. Configure never downloads code.

## Frozen contracts and clocks

Each group fixes `(K,T,sigma,r,q)`, then varies only spot over
`[40,70,90,100,110,140,200]`, with `K=100`. All three rates use the same year clock:
`sigma*sqrt(T)`, discount `exp(-r*T)`, and carry `exp(-q*T)`.

| Corpus | Groups / puts | Purpose |
|---|---:|---|
| `corpus-common-v1.json` | 36 / 252 | Common single-boundary domain: `T={1/365,.25,1,3}`, `sigma={.05,.2,.8}`, `(r,q)={(.03,0),(.03,.01),(.03,.08)}` |
| `corpus-v1.json` | 51 / 357 | Same grid plus negative-yield groups, zero-rate European checks, `q<r<0` double-boundary diagnostic and negative-rate European checks |

Cheap/deep-wing options remain in both corpora. No price threshold filters them.
The broader corpus keeps unsupported and nonfinite results in the attempted
denominator. Its time per attempt is not a comparable successful-pricing rate:
an early refusal or NaN can be cheap. The common corpus is declared by model
inputs before timing, not by dropping observed errors. Residual failures remain
failures there too.

The four profiles are `simu_fast` (Chebyshev order7 / 8 boundary points,
15-point fixed-point quadrature, 41-point price quadrature, 3 iterations),
`simu_finer` (order16 / 17 points, 31/61 quadrature, 16 iterations),
`atx_default` (nullopt Reference, resolved12/24/48/6, tolerance1e-10), and
`atx_bulk_fast` (explicit Fast,7/8/32/2, tolerance1e-8). These are measured
configurations, not asserted equivalent accuracy tiers. Exact values appear in
every native report.

* **Cold:** a fresh pricer for every contract. The public constructor's tanh-sinh
  quadrature generation, initial boundary, all fixed-point iterations, pricing,
  and destruction are charged. ATX's cold call includes its corresponding work.
  This is model-state cold, **not** OS-cache/CPU-cache cold or process startup.
* **Prepared setup:** construct and solve once per immutable group. Both engines
  reuse only that boundary across the group's seven spots. A changed volatility,
  strike, expiry or carry requires a new group and charged setup.
* **Prepared evaluation:** price the seven spots through that solved boundary.
* **Prepared total:** the complete group loop including setup, evaluation,
  destruction and timer/loop overhead. It must cover setup plus evaluation.

Result arrays are allocated before timings. Each repetition's output digest is
retained; all repeated prices must be deterministic. Cold/prepared equality is
reported and checked by tests. `FastPut::calc()` prints and starts its own timer
after construction, so the adapter invokes the same public numerical operations
without printing. Outside timing, the tool compares **every** public profile and
contract against the original `calc()` result bit-for-bit, including NaN payloads.
No intrinsic clamp, fallback, or closed-form repair is added to the public code.
Only `.S` changes during a group's prepared evaluation; its boundary equations
do not depend on spot. The adapter is single-threaded, including mutable upstream
Clenshaw workspace.

## Accuracy evidence

The general American comparison uses two explicit, finer ATX Reference profiles:
`24/48/64/16,tol=1e-12` and `32/64/64/32,tol=1e-14`. Their difference is a
refinement diagnostic, **not a proven error bound or an independent oracle**.
At each synthetic budget `$0.001` and `$0.0001` per share, a case is unresolved
when refinement disagreement exceeds5% of that budget or the reference refuses.
Otherwise the report counts residual passes/failures against the finer value.
These fixed synthetic budgets do not imply the SPY raw-spread acceptance rule.

For `r<=0,q>=0`, early put exercise offers no advantage; a separately implemented
Black-Scholes formula using `std::erfc` supplies an independent closed-form
check. This limited independent check does not validate the exercise boundary in
the general American cases. Every output states
`independent_general_american_accuracy_established=false`.

Always compare speed **and** successful accuracy coverage together on the same
corpus. A faster profile with residual errors or unsupported contracts is not a
replacement for a slower passing profile. The summary preserves each failure's
model and absolute residual, all nonfinite cases and ATX error status/context.
It never silently widens the error budget or converts a failure to a pass.

## Reproduce

From this directory, choose new output directories each time:

```powershell
python public_comparator.py fetch --directory C:/atx-cache/public-american/e7d138093e69fa8cdd6ce7dc7c3371d2899f87f4
powershell C:/atx-wt/pool-33/scripts/atx-build.ps1 -Preset vol-rel configure -DATX_VOL_PUBLIC_AMERICAN_PUT_DIR=C:/atx-cache/public-american/e7d138093e69fa8cdd6ce7dc7c3371d2899f87f4
powershell C:/atx-wt/pool-33/scripts/atx-build.ps1 -Preset vol-rel -Jobs 2 build atx-vol-public-american-comparator
$env:ATX_PUBLIC_AMERICAN_BINARY = 'C:/atx-wt/pool-33/build-vol-rel/bin/atx-vol-public-american-comparator.exe'
python -m unittest test_public_comparator
```

Use the owning `vol-hygiene` target for the PCH-off include check. For performance,
configure/build the same opt-in target using **`vol-rel-avx2` (ThinLTO)**, then
coordinate a quiet machine window and run:

```powershell
python public_comparator.py run --binary C:/atx-wt/pool-33/build-vol-rel-avx2/bin/atx-vol-public-american-comparator.exe --upstream C:/atx-cache/public-american/e7d138093e69fa8cdd6ce7dc7c3371d2899f87f4 --corpus corpus-common-v1.json --output C:/atx-cache/public-american/run-common-001 --reps 30 --affinity 6 --require-pcore
python public_comparator.py run --binary C:/atx-wt/pool-33/build-vol-rel-avx2/bin/atx-vol-public-american-comparator.exe --upstream C:/atx-cache/public-american/e7d138093e69fa8cdd6ce7dc7c3371d2899f87f4 --corpus corpus-v1.json --output C:/atx-cache/public-american/run-diagnostic-001 --reps 1 --affinity 6 --require-pcore
```

Replace pool/CPU paths with the actual leased worktree and topology. Native JSON
contains compiler/flags/build type, process identity, logical-CPU/P-core/SMT
evidence and every raw repetition. Python receipts include source commit/dirty
state, binary/corpus/source hashes, pinned public source hashes, command,
Python/platform and timestamps. Inputs are rechecked after execution. The
launcher fixes worker environment variables to1; neither pricing loop launches
workers. Failed affinity, source mismatch, overwrite, adapter mismatch, changed
input, model/count mismatch or repeated-output nondeterminism refuses completion.

Record CPU/SMT activity and test results alongside any timings. All four engines
share one compiler/flags/binary. Runs use fixed engine order and do not normalize
frequency/thermal state; repeated fresh-process measurements and randomized
order are further controls needed for a strong performance-frontier claim.
The upstream Makefile's GCC `-O3 -march=native` is not reproduced by our common
clang-cl build. No `fast-math` option is enabled.

The pre-existing shootout comment was corrected to distinguish the ALO paper's
historical 2 GHz / Visual C++ 2013 measurements from current hardware and its correlated
in-house residual. Its numerical behavior is unchanged.
