# Delayed execution, panel dataset and costed solver qualification

2026-09-26. Clean compiled and configured source:
`2a3d5c2b17d5fbe93261d9d1d0202a147146f416`.
The original DAG records each source, integration and independent review SHA.

## Runtime result

Six disjoint focused runs passed **249 distinct checks**, with no failures,
errors or skips. They include all 28 new owning checks: dataset 5, costed factor
solver 7, execution ledger 9, execution fitness/search 5 and actual mine 2.
The remaining checks exercise affected legacy algorithms and consumers.

| Artifact suffix (`w2-execution-…-qualified`) | Passed | Native seconds |
|---|---:|---:|
| core | 14 | 0.044 |
| dataset | 5 | 0.198 |
| costed | 37 | 19.400 |
| mine | 30 | 32.751 |
| search | 65 | 19.378 |
| eval | 98 | 15.929 |

The dataset checks cover independent label arithmetic, missing features and
presence, future mutation/truncation, exact endpoint maturity, mapping lifetime,
budget refusal, immutable publication and the actual bounded linear consumer.
Large-layout sizing is arithmetic admission evidence, not measured RSS.

The execution checks cover an independent dollar/cash ledger, delayed fixed
targets, actual participation-limited fills, modeled borrow, absent prices,
unsupported financing, calendar maturity and input identity. Actual fitness,
search and mine consumers agree with the kernel; the mine path re-executes a
selected negative sign. Existing search screening and durable trial accounting
checks pass. Legacy scoring remains explicit and unchanged by default.

The costed solver checks compare heterogeneous trade/impact/borrow costs with
an independent derivative oracle, enforce pins/caps/convergence and malformed
inputs, and exercise the actual PortfolioOptimizer/CostSurface bridge. The
included legacy QP, factor and cost cases pass as well.

The existing timed CPCV benchmark was excluded. No real-market payload,
long mining run or deferred W0 comparison was executed. These counts overlap
earlier qualification packets and must not be added to their totals.

## Build evidence

The first production-leaf build used Jobs2 and passed in **82.0650755 s**,
including a 22.1 s CMake configure and eight C++ compilation actions. The
combined four-target build then used Jobs3 and passed in **359.8567938 s**:
75 affected C++ compilation actions and six links. Both passed on their first
attempt. Existing PCHs and dependency objects remained warm; no source or flags
were changed during qualification. All compiler work belonged to root.

This broad API integration took **441.9218693 s** across both builds and is
not an acceptable routine implementation-edit latency or a speedup claim.
The separately measured private-CPP iteration remains 21.759 s for one actual
source cache miss plus four links; its unchanged build was 3.804 s. Those
measurements and adoption costs are in report `a850d48f`, audit `63e1ee26`.

## Attribution and limits

The companion JSON receipt binds the six XMLs, logs, native exits, filters,
source and executable hashes. The complete local case-name index is
`build-equity/w2-execution-qualification-index.json`, SHA256
`fbd13874219e0653c285a7b6cef61e7646c0263270d495d095ee59c69d448905`.
All 249 names were checked for uniqueness; every recorded executable hash
matched the built file at collection time.

This qualifies the bounded L1/R3/A4 implementations, not entire DAG lanes.
Full out-of-core learning, residual exposure/HAC/half-life objectives, calibrated
preferences, market-data coverage, large-scale performance and full causality
harness gates remain open. The A4 execution recipe is programmatic opt-in;
there is no new CLI snapshot loader, resume support or default switch.
Practical noisy-null IC pruning and general alpha recall remain unqualified.
No alpha is promoted or claimed tradeable from these synthetic checks.
