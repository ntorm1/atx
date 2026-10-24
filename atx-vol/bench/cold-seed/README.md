# Cold monetary seed replay

This isolated research target captures the complete requests reaching
`iv_native_prepare_cold_seeds` on production base
`f30c96a80b89051f6670d425590a23538110ecf4`. It does not replace a production price,
change a proof, relax an acceptance budget, or select a different quadrature.

The remaining cold seed path first rejects invalid controls and targets outside
the zero-volatility/ceiling interval. It requires a classified cold price within
the monetary budget, a target strictly above cold `P(0.005)`, and a positive cold
chord for targets in the declared nonmonotone band. A separate operation-local
two-slot cache already shares exact reporting-floor boundaries. The capture adds
only observation after the original proof returns. Its I/O-contaminated fit
clock is not latency evidence.

The replay measures four separate routes in rotating order:

1. Fresh scalar Reference classified target prices.
2. Scalar Reference prices with shared exact expiry geometry. Setup, allocation,
   exact model lookup and destruction are charged. No boundary is shared between
   different volatilities.
3. The existing AVX2 boundary kernel, with call-to-put mapping, original operation
   groups/order, classification and unchanged scalar fallback/tails. Packing and
   allocation are charged.
4. The complete original scalar monetary proof, including the exact floor cache
   and its unchanged floor/chord decisions.

Routes 1–3 are **component prices**, not complete monetary proofs. There is no
batched proof implementation in this experiment. Route 4 must reproduce every
captured acceptance decision. Every requested row remains in the report,
including failures. Repeated prices and classifications must be bit-identical
within each route. The Python verifier joins every request and all four timing
identities, checks price bits and independently recomputes the monetary price
gate. Differences and nonfinite rows are counted, never dropped.

## Arithmetic boundary

`AlProfile::Reference` selects the scalar `std::erfc` CDF, whereas the existing
AVX2 kernel always uses its vector Cody CDF plus vector `exp`/`log`, with its own
BAW seed arithmetic. Passing Reference options preserves the scheme, not scalar
arithmetic identity. A small observed price difference is not a global outward
bound. Therefore this replay cannot justify substituting vector prices into
cold Reference proof merely because the frozen decisions agree.

The previous [family-probe experiment](../../docs/family-validation-probe-batch-experiment.md)
retained 151 scalar retries among 549 probes and saved only about 10–13 ms on its
declared host. Its empirical `1e-11 * max(P,1)` allowance was not a certified bound
and was not promoted. Existing accurate confirms have a different workload from
these remaining repaired-node target prices. Neither result is a whole-fit claim.

The geometry overload explicitly promises scalar bit identity and is a separate
structural candidate: share expiry setup while retaining exact scalar cold
boundary solves and all proof predicates. Its measured gain must include setup;
it cannot be inferred from the number of model groups.

## Reproduction

Use the repository build wrapper from a leased worktree. Capture defaults OFF;
replay defaults OFF. Set `ATX_VOL_QP_FAER_RESEARCH_LIBRARY` empty if the worktree
has a research override. The SPY input SHA256 is
`b3fafd80f09326ce039ebdcfd7c26914ecbeb39817fdd00b35c6d49dcb2bf33e`.

```powershell
scripts/atx-build.ps1 -Preset vol-rel configure -DATX_VOL_CAPTURE_COLD_SEEDS=ON -DATX_VOL_BUILD_COLD_SEED_REPLAY=ON
scripts/atx-build.ps1 -Preset vol-rel -Jobs 2 build atx-vol-quote-bench atx-vol-cold-seed-replay
$env:ATX_VOL_FIT_WORKERS='1'
$env:ATX_VOL_COLD_SEED_CAPTURE='<absolute output>/requests.csv'
build-vol-rel/bin/atx-vol-quote-bench.exe --input '<quotes-SPY.csv>' --output '<predictions.csv>' --report '<native.json>' --model quote-slice --policy atx-vol/bench/policies/spy-joint-feasible-economic-family-always-candidate.json --dividends funding-pv --timing-detail
python atx-vol/bench/cold-seed/replay.py convert '<requests.csv>' '<requests.json>'
python atx-vol/bench/cold-seed/test_replay.py
python atx-vol/bench/cold-seed/native_test.py build-vol-rel/bin/atx-vol-cold-seed-replay.exe '<control directory>'
build-vol-rel/bin/atx-vol-cold-seed-replay.exe '<requests.json>' '<replay.json>' 1
python atx-vol/bench/cold-seed/replay.py summarize '<requests.json>' '<summary.json>' --native '<replay.json>'
```

Native replay accepts optional repetition count and P-core logical CPU. Timing
requires an archived `vol-rel-avx2` executable, a coordinated quiet host, and
inspection of its native affinity/SMT receipt. JSON serialization and disk I/O
are outside component clocks. Setup/packing/output allocation are inside.
Compilation, contended correctness runs and capture clocks are unqualified.

## First correctness receipt

Local evidence is `C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/acceptance/cold-seed-capture-v1`.
The capture has 496 calls in 11 operation batches and 11 exact models; all 496
original decisions are reproduced. Prediction SHA256 is
`e086fd069a81c2c9eb84bb99e25e8a59e5d78fca8b02b0d51556668eff354d5f`, unchanged
from the production integration. Capture provenance is a dirty observation patch
on the named production base, with patch/header/binary hashes in `receipt.json`;
it is not presented as a clean production build. The executable is archived.

All 496 geometry/handle prices match scalar bits. All vector classifications and
price gates agree, with 16 scalar tail prices. Maximum vector price difference is
`2.2737367544323206e-13` USD/share; minimum scalar monetary margin is
`0.0009352708916831034` USD/share. These are frozen-workload observations.
Independent European controls cover four contracts, both sides and two repeated
rotated evaluations. Seven verifier tests include missing rows, forged price bits
and flags, missing/duplicate clocks, nonfinite lanes and option narrowing.

The separate handle attribution retains 496 cold boundary solves and 11 model/
handle setups. In the contended development diagnostic, setup/destruction was
about 2.2 ms, boundary solving 134 ms and evaluation 6.6 ms. These are **not
qualified latency numbers**. They identify setup sharing as a small lever; it is
not proposed for production. A quiet, archived ThinLTO comparison is still needed
for vector component performance. No whole-fit speedup follows from this replay.


## Private empirical vector candidate component

The private component in `american_iv_seed_batch.hpp` has an explicit
`ScalarReference` / `VectorReferenceEmpirical` mode. No fitting caller selects the
new mode in this commit. Vector-origin target prices are identified separately;
unsupported scope, nonmonotone targets, near-exercise states, insufficient margin,
failed floor checks and incomplete packs use the unchanged scalar helper.
The empirical allowance is not a generic certified error bound. Exact scalar
reporting-floor pricing remains mandatory and its owner destruction is included
in the component clock. Nonmonotone targets retain the original full scalar chord.

The optional `empirical` replay mode preserves the old input schema and emits
`atx.cold-seed.empirical-component.v1`. It runs the new component, then independently
re-evaluates every original scalar full decision, classified price and floor.
Output files must not already exist. The component's internal clocks exclude
caller input/output adapter allocations, so they do not qualify an inclusive
full-fit or comparison claim. All current measurements are correctness diagnostics.

```powershell
build-vol-rel/bin/atx-vol-cold-seed-replay.exe requests.json candidate.json 2 6 empirical
python atx-vol/bench/cold-seed/empirical.py requests.json candidate.json summary.json
python atx-vol/bench/cold-seed/test_empirical.py
```

The independent verifier preserves all request/repetition/batch denominators,
checks scalar decisions, returned sigma, scalar residual/floor/classification,
vector arithmetic allowance, boundary-distance model reconstruction and explicit
origin/retry/counter consistency. It reports observed allowance exceedances as
failures. An all-pass component report still sets whole-fit qualification,
generic error certification and timing qualification to false. Full scalar
actual-API SPY acceptance and explicit research-policy wiring remain prerequisites.
See `docs/cold-seed-vector-component-20261024.md` for the frozen development census.


## Whole-fit research build wiring

`ATX_VOL_EMPIRICAL_VECTOR_SEEDS=ON` selects the empirical tier only at the private
repaired-node preparation and consumption sites. It defaults OFF. The linked
fitting library supplies the tier reported under both
`policy_definition.research_build` and `bench.build_info.cold_seed_target`; the
tool cannot independently select a contradictory macro. The original input
policy and its dollar budget stay unchanged. Record the source/binary/policy
hashes plus CMake cache value and tier beside every experiment. Simultaneous
scalar-only request capture and the empirical build are rejected by CMake.

Prepared candidates retain mode, origin, empirical allowance, actual vector
price/class/boundary and every previous exact request/model/family/envelope key.
Scalar consumers refuse records prepared in empirical mode, including scalar
fallbacks within that mode. Every consumer rechecks allowance, edge and residual
conditions for vector-origin records. Existing scalar callers and default
arguments remain scalar; the actual-API audit's cold scalar Reference exporter
is unchanged.

`reimply_cold_seed_attempts/roots/hits/ns` now continue to describe scalar proof
work exclusively. The new `reimply_seed_vector_*` fields describe vector work,
and named retry counts retain every rejected or tail lane. Exact scalar floor
preparations/hits include both origins. Use `reimply_seed_candidate_ns` for the
inclusive whole stage in either mode: it includes grouping, all owners, packing,
publication of prepared records and destruction. Vector and floor substages are
cross-cuts, not independent extra wall time to add to the total.

Native prepared-record tests cover mode changes, origin/allowance tampering,
changed residual/boundary/class, stale family stamp and target/budget identity.
The focused 29-test selection passes, both build modes pass native provenance
checks, and the five owning translation units pass PCH-off checks. These are
implementation checks; the enabled full-SPY scalar actual-API audit is a separate
required receipt before any workload promotion or latency claim.

The archived enabled build now passes that separate full-SPY scalar actual-API
audit with all 13 gates and byte-identical predictions. See
`docs/cold-seed-vector-spy-audit-20261024.md` for complete hashes, canonical policy
bytes, all-row counts and retained strict-IV diagnostics. This is workload
correctness evidence only; no qualified latency or generic vector bound is
claimed. The next proposal/proof attribution design is frozen separately in
`docs/cold-price-candidate-attribution-design-20261024.md`.
