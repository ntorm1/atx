# atx-vol End-to-End Fitting, Pricing, and Greeks Sprint

**Date:** 2026-07-11  
**Planning baseline:** repository HEAD 01d88b8 plus the reviewed working tree  
**Inputs:** [fitting pipeline review](../docs/reviews/fitting_pipeline_code_review.md) and [pricing/Greeks pipeline review](../docs/reviews/pricing_greeks_pipeline_code_review.md)  
**Estimated effort:** 31-38 engineer-days, organized as two parallel implementation lanes plus shared integration and qualification  
**Primary outcome:** one explicit, correctness-gated path from market observations to an admitted surface and from that surface to deterministic prices and requested Greeks.

## 1. Executive decision

The next sprint should not begin with another optimizer, curve family, or isolated SIMD kernel. Current HEAD already contains strong primitives: AVX2 eSSVI objective/Jacobian evaluation, several curve families, prepared portfolio grouping, persistent pricing workers, American batch APIs, Black-76 SIMD SoA kernels, warm seeds, and deterministic reductions.

The production gaps are at the seams:

1. Fitting behavior changes by curve family because eSSVI uses a legacy observation/surface driver while other families use the generic driver.
2. A partial or non-converged fit can be published as success, preventing fallback and making quality diagnostics unreliable.
3. Incremental mutation is not transactional: portfolio retiming can leave inconsistent state, retained portfolio workspaces can price stale maturities, and incremental surface refit can commit a calendar-crossed surface.
4. The canonical portfolio path prepares aligned/grouped data but returns to scalar American pricing and full-bundle scalar Greeks per lane.
5. The standalone American batch interface cannot reproduce every served surface because it lacks the exact method and Andersen-Lake options, and its per-call ISA selection mutates process-global state.

The sprint therefore follows one rule:

> Establish exact state, admission, model, and error contracts first. Optimize only a route that proves it preserves those contracts end to end.

## 2. Review synthesis

### 2.1 Highest-priority correctness and reliability findings

| Finding | Severity | Sprint response |
|---|---:|---|
| Canonical eSSVI bypasses shared filtering, observation caps, shortcuts, and the parallel generic prepass | P1 | WP4 canonical prepared fitting data |
| Partial surfaces count as success and suppress the fallback ladder | P1 | WP5 surface admission and transactional publication |
| Curve selection compares unequal surviving populations and ignores configured de-Americanization inputs | P1 | WP5 paired selector and admission |
| Dense QP returns success after iteration exhaustion and omits active/KKT diagnostics | P1 | WP3 optimizer truth contract |
| Incremental eSSVI refit is facade-stranded, uses unsafe observation guidance, checks only one calendar neighbor, and commits before validation | P1 | WP6 transactional incremental refit |
| Corpus fits accidentally nest board and expiry parallelism; worker exceptions can terminate | P1 | WP7 bounded, exception-safe scheduling |
| Robust/no-arbitrage meaning varies by curve family | P1 | WP5 consumer-specific invariant oracle |
| Warmed portfolio workspaces retain stale T/group data after some retimes | P1 | WP1 versioned portfolio state |
| Portfolio retime can partially mutate before returning an error | P1 | WP1 transactional retime |
| American batch per-call ISA selection races through a global override | P1 | WP2 local dispatch and error model |

### 2.2 Highest-leverage performance findings

| Finding | Current reality | Sprint response |
|---|---|---|
| PreparedPortfolio and equal-T carry reuse are wired | Preserve and extend | WP8 consumes groups through an exact resolved batch |
| Portfolio American price and Greeks remain scalar per lane | Main throughput gap | WP8 price-only batch, then WP9 selective Greeks |
| Black-76 AVX2 SoA and American batch APIs are standalone/test-oriented | Useful primitives, not drop-in APIs | Adapt behind the resolved batch contract |
| American Auto SIMD deliberately remains scalar | Correct until product-level evidence exists | Revisit only after end-to-end benchmark gates |
| Full Greek results use internal AoS and compute all fields | Wasted work for delta/vega/first-order consumers | WP9 native SoA and per-Greek masks |
| Dense Interval loss expands to dense N+2M matrices | O(M squared) memory and repeated dense solves | WP10 bounded/sparse formulation |
| Legacy aggregate_greeks scans buckets linearly per leg | Quadratic in many-bucket books | WP10 hash/sort-reduce implementation |
| Canonical eSSVI remains serial even though generic prepass and alternate drivers parallelize | Family-dependent latency | WP4 plus WP7 |

### 2.3 Features that exist but are not production-hot

- Surface warm priors exist in the alternate eSSVI driver, but the canonical PricerFitter build has no prior input.
- VolaSession slice warm refit exists, but the owned FittedSurface facade exposes only a const session.
- American price/Greek batches exist, but PortfolioPricer does not call them.
- Black-76 AVX2 price, vega, Greek, and IV SoA kernels exist, but the American portfolio route does not consume them.
- PreparedPortfolio GroupRoute is stored but not consumed.
- Sigma-interpolated boundary slices, implicit boundary-differentiation Greeks, and the QD+ seed remain research/test paths.
- Profile FilterOpts, persisted SymbolFitConfig binding, several calibration fields, and named residual modes are dormant or miswired in the canonical fit.

These are candidates for promotion only when the sprint's exactness, admission, and route-observability gates are in place.

## 3. Target end-to-end architecture

    QuoteFrame / OptionChain
      -> PreparedBoard
           keyed raw quotes
           one configured carry/de-Am result per observation
           accepted rows + rejection reasons
           deterministic train/holdout keys
      -> paired curve selection on common coverage
      -> family slice optimizers
           explicit convergence and numerical diagnostics
      -> family-agnostic surface invariant oracle
      -> FitAdmissionPolicy
           admit primary, run fallback, or retain last-known-good
      -> immutable/versioned FittedSurface snapshot
           complete SurfaceBuildReport
      -> PricedSurface resolved equal-T groups
      -> ResolvedAmericanBatch
           exact method + AlOpts + local ISA + field mask + per-lane error
      -> price-only / selected-Greek / full-Greek kernels
      -> deterministic scatter and reduction
      -> PriceFrame / PriceTotals / PnL explain + route diagnostics

The prepared fitting data and admitted surface become the semantic boundary. The resolved pricing batch becomes the numerical hot-path boundary. Neither boundary permits a silent downgrade.

## 4. Non-negotiable invariants

1. A failed mutation or fit attempt leaves the previously published portfolio/surface and diagnostics unchanged.
2. Success means explicit convergence, required coverage, and a passed consumer-specific invariant oracle; a merely nonempty surface is not success.
3. Selector, final fit, parity scoring, and incremental refit use the same configured observation and de-Americanization policy.
4. A pricing batch receives the exact American method and numerical options used by scalar PricedSurface.
5. Per-call dispatch is local. No hot-path call temporarily mutates process-global ISA state.
6. Per-lane failures preserve a machine-readable reason through the portfolio layer.
7. Deterministic reductions and documented bit-identity promises remain unchanged across thread counts.
8. Performance claims use the canonical facade and must pass correctness/admission gates before comparison.
9. Public configuration is either consumed, explicitly unsupported, or removed; a persisted no-op is not acceptable.
10. Mark surfaces and risk surfaces have explicit, different contracts when their guarantees differ.

## 5. Work packages

### WP0 - Reproducible baseline and red-signal cleanup

**Effort:** 1.5-2 days  
**Dependencies:** none  
**Blocks:** performance acceptance for all later packages

**Deliverables**

- Build current HEAD in a clean, isolated worktree using the pinned compiler/configuration and record commit, ISA, CPU, preset, and working-tree state.
- Run the full atx-vol suite and both focused review filters.
- Investigate the PreparedPortfolio pinned-fingerprint failure. Repin only with a documented algorithmic/numerical reason and an independent-oracle pass.
- Add canonical end-to-end benchmark cases for OptionChain -> PricerFitter -> VolaSession and PreparedPortfolio -> PortfolioPricer, not only low-level calibrators/kernels.
- Add a manifest check so every registered fitting/pricing benchmark is either baselined or explicitly marked informational.
- Add stable route/phase counters needed by later gates: observation rejection reason, optimizer route/convergence, fallback/admission, scalar/batch pricing, scalar/AVX2, Greek field route, and per-lane error class.

**Likely areas**

- [bench/fitting_throughput_bench.cpp](../bench/fitting_throughput_bench.cpp)
- [bench/portfolio_throughput_bench.cpp](../bench/portfolio_throughput_bench.cpp)
- [bench/baselines](../bench/baselines)
- [include/atx/vol/counters.hpp](../include/atx/vol/counters.hpp)
- [tests/prepared_portfolio_test.cpp](../tests/prepared_portfolio_test.cpp)

**Exit gate**

- Current clean build is green or every red test has an owned issue and is excluded from performance claims.
- Baseline files cover canonical fit, warmed marks, totals-only marks, selected Greeks, and full Greeks.

### WP1 - Transactional portfolio mutation and exact workspace invalidation

**Effort:** 1.5 days  
**Dependencies:** none  
**Priority:** stop-the-line correctness

**Implementation**

- Change Portfolio::retime to validate and stage all unique and per-position tenors before any mutation.
- Add a monotonically increasing Portfolio structural version that changes only after a successful commit.
- Store the exact version in PortfolioWorkspace and rebuild PreparedPortfolio on mismatch.
- Remove the first-contract fingerprint as the correctness mechanism. It may remain only as a debug assertion or diagnostic.
- Ensure failed retime leaves positions, deduplicated contracts, UID sets, version, and every warmed workspace result unchanged.

**Acceptance**

- A warmed workspace matches a fresh workspace bit-for-bit after any supported retime in price_into, price_totals, pnl_explain_into, and totals-only P&L.
- Retime failure is byte-for-byte transactional.
- A changed middle contract that alters equal-T ordering cannot reuse stale prepared columns or groups.
- The steady-state version check remains O(1) and allocation-free.

### WP2 - Local SIMD dispatch and lossless per-lane errors

**Effort:** 1.5-2 days  
**Dependencies:** WP0 counters  
**Priority:** stop-the-line concurrency

**Implementation**

- Pass SimdIsa or a resolved local function pointer into the American boundary batch. Remove per-call IsaScope mutation.
- Retain the process-global override only as a coarse default for startup/tests when a request says Auto.
- Define a compact batch error enum that distinguishes invalid input, unsupported exercise regime, numerical failure, and internal failure.
- Preserve PriceStatus as a compatibility projection while exposing the detailed code in diagnostics/results.

**Acceptance**

- Barrier-controlled concurrent ForceScalar and ForceAvx2 calls always report and use their requested routes.
- Scalar requests remain safe on a non-AVX2 configuration even while another thread exercises forced AVX2 tests on capable CI.
- Portfolio results preserve detailed underlying errors and deterministic route counts.

### WP3 - Optimizer truth contract for ConvexDense

**Effort:** 2 days  
**Dependencies:** WP0  
**Priority:** stop-the-line fit correctness

**Implementation**

- Replace a bare QP solution return with a result containing converged, iterations, active_count, stationarity, primal violation, and complementarity.
- Validate max_iter, node_cap, regularization, weights, bounds, and finite inputs before solving.
- Return failure when the active-set loop exhausts unless an explicitly named approximate policy admits a result after independent quality checks.
- Populate ConvexSliceFit::n_active and surface diagnostics from the real solver state.

**Acceptance**

- max_iter=0 and deliberately exhausted cases cannot return ordinary success.
- Known constrained problems meet pinned KKT tolerances.
- A binding fixture reports active constraints.
- Any approximate result is visibly labeled and cannot bypass WP5 admission.

### WP4 - One canonical prepared fitting dataset

**Effort:** 4-5 days  
**Dependencies:** WP0  
**Priority:** core architecture

**Implementation**

- Introduce PreparedBoard and PreparedSlice with stable observation keys, raw NBBO fields, European-equivalent fit rows, carry/de-Am provenance, rejection reason, weights, and deterministic holdout membership.
- Use the exact configured method, Andersen-Lake options, IV tolerances, iteration limits, caches, quote flags, weight limits, anchors, observation caps, and OTM shortcut once during preparation.
- Route legacy eSSVI, generic families, selector scoring, parity metrics, and incremental refit through the same prepared representation.
- Move eSSVI onto the shared parallel preparation path while preserving the sequential steps actually required by its calendar mode.
- Retain an explicit compatibility mode only if a documented consumer requires historical bit identity; do not leave two implicit policies.

**Acceptance**

- Under one policy, every curve family receives the same accepted observation keys and rejection reasons.
- Every public filter/cap/shortcut used by production has a parameterized all-family test.
- eSSVI Auto preparation remains bit-identical across worker counts and shows no regression against the current generic parallel-prepass scaling floor.
- Selector and final fit consume identical configured European rows.

### WP5 - Paired selection, surface admission, and transactional publication

**Effort:** 4-5 days  
**Dependencies:** WP3, WP4  
**Priority:** core correctness

**Implementation**

- Define SurfaceBuildReport with an outcome for every attempted expiry and stage: carry, observation preparation, optimization, parity, invariant validation, fallback, and publication.
- Define FitAdmissionPolicy with minimum expiry/quote coverage, required tenor buckets, maximum consecutive gaps, allowed rejection fractions, and consumer type.
- Score selector candidates on common required keys. Rank lexicographically by admission, coverage, held-out loss/in-band quality, stability, and complexity.
- Choose selector expiries through deterministic liquidity and tenor stratification rather than ascending prefix.
- Build a family-agnostic independent invariant oracle over a common grid: finite domain, price bounds, monotone slopes, strike convexity, calendar total variance, and forward variance.
- Make mark, quote, and risk surface contracts explicit. LinearVariance Hft may be admitted as a mark surface without being silently treated as an admitted risk surface.
- Publish only after admission. On primary rejection, run the configured fallback. On total rejection or timeout, retain last-known-good with explicit stale/degraded state.

**Acceptance**

- A one-slice result from a multi-expiry board is rejected unless an explicit policy permits it.
- A candidate that fits one easy expiry cannot beat an admissible full-coverage candidate solely on average loss.
- Constructed SVI/C8/LinearVariance calendar crossings are repaired by an approved family-agnostic step or rejected/fallback; recording a false flag is not enough.
- Every failed primary surface retains its reasons after fallback.
- No unadmitted risk surface is served by default.

### WP6 - Safe, facade-owned incremental refit

**Effort:** 3 days  
**Dependencies:** WP4, WP5  
**Priority:** live-path correctness and latency

**Implementation**

- Add PricerFitter::refit_expiry using an expiry identifier and the current OptionChain/configuration.
- Build fresh canonical prepared observations internally; callers do not construct ambiguous American fit rows.
- Warm-start from the matching family state and reuse prepared carry/de-Am data only when snapshot/version provenance matches.
- Validate or repair both previous and next calendar relationships.
- Recompute affected parity, quality, counts, rejection reasons, and surface health.
- Commit surface plus diagnostics atomically after admission. Preserve last-known-good on any failure.

**Acceptance**

- A known-truth incremental refit agrees with a full cold refit within the declared family tolerance.
- Upward movement of a middle expiry cannot cross the next expiry.
- Failure leaves surface and diagnostics unchanged.
- The public quote-update narrative and API actually change model IV only after successful refit.
- Warm-refit benchmarks include preparation, validation, and publication rather than timing only the optimizer.

### WP7 - Bounded, exception-safe fit scheduling

**Effort:** 2 days  
**Dependencies:** WP4  
**Priority:** reliability and predictable performance

**Implementation**

- Add an explicit fit worker budget to PricerConfig and propagate it to every surface driver.
- Use a shared executor/budget so board-level corpus work and expiry-level preparation cannot multiply concurrency.
- Capture worker exceptions and return/report them on the caller thread. No exception may escape a jthread body to terminate the process.
- Choose board-versus-expiry partitioning from board count, expiry count, and estimated prepared-row cost.

**Acceptance**

- Instrumented peak concurrent work never exceeds the configured budget.
- Forced allocation/worker failures return an error and preserve last-known-good state.
- Results remain deterministic across supported worker counts.
- Corpus p95 and peak memory improve or remain within an explicit non-regression band.

### WP8 - Exact resolved American price batch in the portfolio hot path

**Effort:** 4 days  
**Dependencies:** WP1, WP2, WP0  
**Priority:** first production throughput change

**Implementation**

- Define a non-owning resolved batch request carrying S, K, T, sigma, r, q, side, exact AmericanMethod, exact AlOpts, field mask, local ISA request, output spans, and per-lane detailed status.
- Integrate it at equal-T runs inside PricedSurface::evaluate_batch.
- Consume PreparedPortfolio group metadata rather than recomputing route decisions lane by lane.
- Implement price-only first. Preserve scalar fallback for unsupported/small/irregular groups.
- Keep BAW and every configured Andersen-Lake preset exact. The existing standalone default-option batch is not a drop-in replacement.
- Add counters proving the canonical PortfolioPricer path reaches the batch route.

**Acceptance**

- Batch price matches scalar PricedSurface for every method, preset, exercise regime, side, surface family, invalid lane, and thread count under the existing numerical contract.
- Per-lane failures match scalar error classifications.
- Price totals remain bit-identical across thread counts.
- Representative warmed price_into(Marks) and price_totals(Marks) improve at least 20% end to end on the pinned host, or the route remains opt-in with the evidence recorded.
- No isolated boundary-kernel result is accepted as the product-level speed gate.

### WP9 - Selective Greeks and native SoA results

**Effort:** 4-5 days  
**Dependencies:** WP8  
**Priority:** major throughput and API completeness

**Implementation**

- Extend field selection from coarse Marks/FullGreeks to per-Greek or well-defined bundles: price+delta, price+vega, first-order, second-order/full.
- Replace internal unique AmericanGreeks AoS with native nullable SoA result columns.
- Route price+delta and price+vega through the existing dedicated fast references before attempting a general vector Greek stencil.
- Integrate the exact resolved American Greek batch with the same method/options/status contract as WP8.
- Preserve the existing full-frame compatibility wrapper.
- Revisit analytic Andersen-Lake and AVX2 shipment only after corpus stability and product-level throughput gates.

**Acceptance**

- Unrequested output spans remain untouched and unrequested boundary solves are absent in counters.
- Every requested field matches the full scalar bundle within its declared identity/tolerance contract.
- Delta-only and vega-only workloads materially outperform full Greeks and reduce boundary solves by the expected route count.
- Full Greeks remain deterministic across 1/2/4/8 threads.
- Greeks are validated against finite-difference and PDE/oracle grids for extreme vol, wings, near expiry, and exercise transitions.

### WP10 - Bounded data structures and algorithm cleanup

**Effort:** 3-4 days; may be split after profiling  
**Dependencies:** WP0; QP portion depends on WP3  
**Priority:** second-wave performance

**Committed items**

- Replace legacy aggregate_greeks linear bucket scan with a reserved hash index or sort/reduce; make its European/raw-quantity semantics explicit in type/name/docs.
- Replace repeated maturity linear scans with lower_bound or pre-resolved batch group indices where profiles show material cost.
- Reuse prepared selector/final-fit state and compact sort indices rather than copying full FitObs records.
- Add per-worker reusable fit/QP workspaces where ownership is clear.

**Dense Interval investigation**

- Benchmark 50/500/5,000 observations with peak memory.
- Replace explicit 2M dense slack variables with a separable/bound-aware formulation or a sparse solver.
- Enforce a configured workspace/observation capacity before allocation.

**Acceptance**

- Legacy aggregation is O(L) expected or O(L log L), with parity for every aggregation mode.
- Interval loss no longer has an undocumented unbounded dense-memory cliff.
- Optimizations do not precede or fork the canonical semantics established in WP4.

### WP11 - Configuration truth and structured observability

**Effort:** 2-3 days  
**Dependencies:** WP4, WP5, WP8  
**Priority:** operational integrity

**Implementation**

- Create a central option-consumption table for profile, calibration, symbol, pricing, and Greek settings.
- For each public/persisted field: wire it, reject nondefault unsupported use, or remove/deprecate it.
- Specifically resolve FilterOpts, SymbolFitConfig binding/enabled, rho modes, asymmetric rho, residual identities, parametric loss kind, max_weight, fallback thresholds, and butterfly-grid controls.
- Replace ambiguous zero diagnostics with explicit disabled/failed/not-scored states.
- Report actual curve DoF, C8 seed reversion, optimizer convergence, shortcut/cache use, admission state, route counts, and detailed pricing errors.

**Acceptance**

- Toggling every public persisted field causes a tested plan/result change or an explicit validation error.
- Diagnostics can distinguish disabled scoring, failed scoring, rejected slices, fallback, seed-only C8, unsupported American regime, and numerical failure.
- Reports/archive round-trips preserve the new state without ambiguity.

### WP12 - Qualification, shadow rollout, and default flip

**Effort:** 2 days  
**Dependencies:** WP1-WP11 relevant release subset

**Qualification matrix**

- Curve families: eSSVI, ConvexDense, LinearVariance, SVI, C8, and every exposed fallback.
- Fit policies: pinned and Auto; Hft/Balanced/Accurate/Robust; mark versus risk contract.
- Pricing methods: Andersen-Lake default/fast/custom, BAW, scalar and eligible batch routes.
- Market regimes: calls/puts, dividend and borrow, near expiry/0DTE, deep wings, negative carry, sparse/partial boards, crossed/flagged quotes, and stressed surfaces.
- Execution: scalar-only, AVX2-capable, 1/2/4/8 workers, concurrent calls, repeated refits/retimes, fresh and warmed workspaces.
- Tooling: release build, sanitizer build, targeted TSAN/logical-concurrency tests, full tests, real-data corpus, and benchmark manifest.

**Rollout**

1. Shadow-build canonical prepared/admitted surfaces beside the old route and compare keys, coverage, prices, and Greeks.
2. Shadow-run resolved price batches while returning scalar results; record route, error, and max difference.
3. Enable price-only batch for qualified groups.
4. Enable selective Greeks by bundle.
5. Remove legacy fitting observation path and process-global per-call dispatch only after compatibility windows close.

**Exit gate**

- No unresolved P1 from either review remains in a supported default path.
- Shadow differences are understood and policy-approved.
- Rollback means selecting the previous immutable surface/kernel route, not reconstructing lost state.

## 6. Delivery sequence and parallel lanes

Two lanes can run concurrently after WP0:

| Week | Fitting/state lane | Pricing/Greeks lane | Shared integration |
|---|---|---|---|
| 1 | WP3 optimizer truth; begin WP4 prepared data | WP1 portfolio versioning; WP2 local ISA/errors | WP0 clean baseline/counters |
| 2 | finish WP4; begin WP5 admission/selector | design and parity-test WP8 resolved batch | shared error/provenance types |
| 3 | finish WP5; WP6 incremental refit; WP7 executor budget | finish WP8 price-only; begin WP9 selective Greeks | canonical end-to-end tests |
| 4 | WP11 configuration/diagnostics; selected WP10 items | finish WP9; selected WP10 aggregation/data work | WP12 qualification/shadow rollout |

Dependency spine:

    WP0 -> WP3 -> WP4 -> WP5 -> WP6
      |             |      |
      |             +----> WP7
      |
      +-> WP1 -> WP8 -> WP9
      +-> WP2 ---^
      +-----------------> WP10/WP11 -> WP12

WP5 and WP8 are the two integration milestones. If capacity contracts, finish WP0-WP8 and ship price-only batching; do not partially ship WP9 or skip admission/transactionality to make room.

## 7. Test and acceptance gates

### 7.1 State and lifecycle

- Successful and failed retime tests with duplicate contracts and changed middle maturities.
- Warmed-versus-fresh workspace equality for price, totals, P&L explain, statuses, and Greeks.
- Failed full fit, failed partial fit, failed fallback, and failed incremental refit all preserve last-known-good.
- Snapshot/config/version provenance prevents reuse across incompatible prepared data.

### 7.2 Fitting and surface admission

- Parameterized observation acceptance equality across families.
- Paired selector fixtures with asymmetric candidate failures.
- QP KKT oracle and iteration-exhaustion tests.
- Independent strike/calendar/forward-variance oracle for every served family.
- Required-tenor and minimum-coverage tests.
- Incremental middle-slice tests against both calendar neighbors.

### 7.3 Pricing and Greeks

- Scalar/resolved-batch equality across method, AlOpts, side, regime, preset, surface family, and invalid input.
- Barrier-controlled ISA isolation test.
- Detailed error propagation tests.
- Requested-field-only tests with poisoned unrequested spans.
- Finite-difference/PDE Greek validation and base-price identity.
- Deterministic totals across worker counts and repeated schedules.

### 7.4 Performance

Every benchmark row must report product rate/latency, route counts, thread count, ISA, allocations or peak memory where relevant, and correctness/admission status.

Required rows:

- Canonical cold full fit by family/preset and real/synthetic board.
- Selector plus final fit versus pinned-family fit.
- Incremental expiry refit including validation/publication.
- Corpus build at 1/N boards with bounded execution.
- Warmed portfolio marks, totals-only marks, delta-only, vega-only, first-order, and full Greeks.
- Scalar versus integrated resolved batch on identical books.
- Dense Mid/Interval scaling at 50/500/5,000 rows.

Performance changes fail if they improve median while materially worsening p95, memory, error rate, coverage, or surface admission.

## 8. Feature-gap backlog after the committed sprint

These are obvious next steps, but they should not displace the P1 and integration work above.

### H1 - Double-continuation American regimes

Implement a two-boundary method or approved PDE/tree fallback for the explicitly unsupported negative-carry corner. Preserve the current explicit failure until the new route passes price and Greek oracle grids around both regime transitions.

### H2 - Versioned, conditioned Greek scheme

Add scale-aware finite-difference steps, Richardson/error estimates on selected lanes, and a per-Greek quality diagnostic. Keep the pinned current scheme available during migration.

### H3 - Implicit/AAD Greek promotion

Evaluate implicit boundary differentiation and/or reverse-mode Greeks only behind WP9's exact resolved request and field mask. Require stability against finite differences/PDE plus a product-level price-and-Greeks factor target.

### H4 - Joint calendar constructor

Replace family-local floors/projections with a weighted minimum-change common-lattice constructor if rejection rates show that admission alone is too costly.

### H5 - Surface-twist and exact scenario support

Implement the reserved surface-twist shock and an exact reprice path beside Taylor explain. Use admitted immutable surface views; do not refit for a pure pricing shock unless the scenario explicitly requests it.

### H6 - Model/config completeness

Implement only the rho modes, residual bases, interval losses, and symbol-level controls with demonstrated product value. Unsupported enum values should remain explicit errors, not aliases.

### H7 - Allocation-free numeric error path

Remove message allocation from noexcept SIMD IV fallback or remove the noexcept promise. Numeric kernels should carry compact codes and format messages at API boundaries.

## 9. Risk register

| Risk | Mitigation |
|---|---|
| Canonical observation unification changes historical surfaces | Shadow both routes, compare keyed rows and outputs, preserve explicit compatibility mode for a bounded migration |
| Stricter admission rejects too many live boards | Start in shadow mode, expose reasons, tune policy by consumer, retain last-known-good instead of weakening invariant checks |
| Batch American API changes model options | Make method and AlOpts required request data; use scalar parity tests before routing |
| SIMD speedup is too small end to end | Keep route opt-in unless WP8 product threshold passes; focus on selective work and solve-count reduction |
| Versioning complicates caller-owned workspaces | Version the immutable logical book/surface, keep O(1) checks, document one workspace per concurrent call context |
| Exception-safe shared scheduling affects determinism | Disjoint output slots, stable reduction, explicit budget, deterministic tests |
| Sparse/alternative QP changes fitted values | Retain the dense solver as an oracle for small problems and gate KKT plus output tolerances |
| Existing dirty worktree contaminates evidence | Use isolated worktree/build for baselines; never overwrite unrelated user changes |

## 10. Definition of done

- [ ] Portfolio retime is transactional and exact versioning invalidates every stale prepared workspace.
- [ ] American batch dispatch is local and concurrent per-call route requests are isolated.
- [ ] Dense QP cannot report ordinary success on unproven convergence and emits useful KKT/active diagnostics.
- [ ] All canonical curve families, selector scoring, parity, and refit consume one configured prepared observation representation.
- [ ] Required coverage and independent surface invariants gate publication and activate fallback/last-known-good behavior.
- [ ] Incremental refit is exposed through PricerFitter, validates both neighbors, refreshes diagnostics, and commits atomically.
- [ ] Fit concurrency is explicitly budgeted and worker exceptions cannot terminate the process.
- [ ] Portfolio price-only hot paths reach an exact method/preset-correct resolved American batch, proven by counters and end-to-end benchmarks.
- [ ] Selective Greek requests avoid unrequested solves/stores and use native SoA results.
- [ ] Public configuration and diagnostics tell the truth about what the hot path consumed and what failed.
- [ ] Full current tests, focused tests, invariant corpus, deterministic-thread tests, concurrency tests, and benchmark manifest are green.
- [ ] No supported default path retains an unresolved P1 from the two reviews.

## 11. First implementation slice

The first mergeable slice should be deliberately narrow and high confidence:

1. WP1 transactional retime plus Portfolio versioning.
2. WP2 local American batch ISA dispatch.
3. WP3 QP exhaustion/KKT result contract.
4. WP0 regression tests and counters for those changes.

This slice removes three silent-state/concurrency failure modes without changing fit selection or pricing numerics. It creates the safe base for the larger WP4 and WP8 integration branches.

