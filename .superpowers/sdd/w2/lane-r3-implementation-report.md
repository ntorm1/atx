# W2-R3 factor-space executable costs: source qualification candidate

Pulled forward by the orchestrator on reviewed R1/B1 dependencies. **W2 and R3 acceptance remain open.** Implementation preceded fixtures. No local C++ compilation, test execution, benchmark, market-data read, download or database access was performed. Graph tools were unavailable; source discovery used targeted `rg`.

## Source contract

`CostedSolveRule::AugmentedConeV1` remains the default of `solve_with_costs`, with the original five-span economic recipe and existing arithmetic. New trailing spans are default-initialized. Passing executable limits to V1 fails explicitly; `OptimizerCostTerms::view()` retains its documented economic-only legacy view, while `executable_view()` includes the execution flags/caps. Unknown rule values fail.

Explicit `FactorProxV2` minimizes the same quadratic factor risk and linear return objective plus per-name linear trading costs, 3/2-power impact and asymmetric short holding fees. The previous book is actual marked holdings. Its closed-form trade proximal operator rationalizes the quadratic root and separates exponents, including a representable-subnormal large-ratio case. Box/gross and turnover budgets use bounded, deterministic scalar threshold searches. Hard untradeable and finite nonpositive max-trade values pin the original weight; finite positive trade caps and observed locate caps intersect the holding boxes. Contradictions fail. Existing optional scalar turnover/quadratic impact remain separately declared objective terms; no coefficient is collapsed into a cross-name scalar.

The new path does not use the old scalar-only polish or silently fall back to the augmented solver. It checks input geometry, finite coefficients/warm states, hard metadata and a combined dense-factor/general-row scratch bound before compiling the factor workspace. The returned book is clamped to its execution boxes, exact proximal no-trade points are retained, and the whole original economic set plus final consensus/stationarity are checked. Insufficient convergence is an error. Cones/elastic relaxation remain unsupported. Dense Woodbury storage remains O(M*(K+R)+(K+R)^2): CSR input is not a claim of a universally sparse solver.

`cost::solve_surface_costed` and the actual `risk::PortfolioOptimizer::solve_surface` entry consume the immutable CostSurface. The latter is explicitly named and requires expected returns in NAV-return units; NaN is zero linear tilt and never erases a held position. The existing `solve()` route/defaults are untouched. Instrument order, decision clock and NAV are checked. Surface coefficients preserve the one-way dollar-to-return conversion; unavailable liquidity pins the name. Borrow defaults to requiring prior modeled rates; an explicit disabled policy is available and recorded. Modeled fees never assert a locate. Results return the solver rule, surface recipe/snapshot hashes, borrow convention, assumption count and solved-delta cost breakdown. There is **no stage-level default switch or calibration of preference scores into expected returns**.

## Private implementation and integration

`optimizer.hpp` adds only forward declarations and a method declaration. The optimizer/CostSurface bridge and proximal routines live in existing `src/risk/cost_terms.cpp`. `risk/cost_types.hpp` is a small enum/span API, avoiding a solver include cycle.

`qp_factor_admm.hpp` now retains types and declarations, including the two pre-existing independent projection-oracle helper signatures. Its non-template implementation is in new `src/risk/qp_factor_admm.cpp`; root must register this one CPP. A mechanical move check against `65bdf3da` preserved all 1,109 implementation lines after removing `inline` and exported default-argument declarations, normalized SHA256 `42384e825574624ae231e50b0c934e90c3f131c2a31656800918b0d1f524b80f`. This is source identity evidence, not compiled byte parity.

For independent compile-speed qualification, an ignored **legacy-only** patch was supplied against root `2e3981011478085829596143ebbd905d1283697a`: `build-equity/legacy-factor-private.patch`, with `legacy-factor-private-receipt.json`. That patch reduces its header from 1,051 to 116 lines and preserves all 941 normalized legacy implementation lines (SHA256 `fc1cfadaaa046900ee14e807f61a7f9a5302659407cdeeef655d372cb26205ad`). It contains none of R3's new algorithm/argument changes. Root can qualify that extraction before importing R3; its registration/build is root-owned.

Independent review found an active-quadratic-impact reference-length hole in V2. `fc80a5ee` now validates supplied impact references before the equality/indexing loop, with empty explicitly meaning flat. The exact malformed/NaN refusal cases were added afterward. `6be7f67f` supplies direct factor-ADMM/schedule CPP includes for root's simultaneous private QP extraction.

## Focused postimplementation qualification

New owning TU: `tests/risk/risk_cost_factor_v2_test.cpp`; filter **`RiskCostFactorV2.*`**, seven cases. Root registers/builds it. Coverage is bounded scalar derivative oracles and extreme scales; heterogeneous per-name costs versus an independent diagonal optimum and explicit/default V1; exact pins/caps/contradictory locates; gross/turnover, memory and nonconvergence refusals; real PortfolioOptimizer surface units/identities/live holdings; decision availability and borrow policy; malformed standalone-impact references. Existing `risk_cost_terms_test.cpp` and `risk_qp_factor_admm_test.cpp` remain the legacy owner suites.

No case has run. The small legacy-cone comparison uses its existing practical numerical tolerance and is **not** the required M=200 battery at weight error 1e-6/objective error 1e-8. M=3000/K80 warm costed <=120ms, bounded large-R memory, complete no-trade runtime evidence, PCH-off integration and compiled legacy parity are still pending. Fixed bounded threshold searches and inherited per-iteration temporary vectors are known performance considerations; no timing claim is inferred from the factor formulation.

## X1 registration boundary

Register component/subpath `optimizer_path/factor_cost_v2` with production chain `PortfolioOptimizer::solve_surface -> solve_surface_costed -> solve_with_costs(FactorProxV2) -> solve_factor_admm`. Protected records are weights, execution pins, predicted costs and solver certificates at the supplied decision; recipe/snapshot hashes are separately bound provenance. Visible state is the live prior book, calibrated return estimates, prior risk model and strictly prior liquidity/borrow snapshot. The risk/alpha availability clock must be established by the upstream producer; this low-level solver cannot infer it from a matrix or integer row index.

The supplied boundary fixture rejects a liquidity row available at the decision and unavailable modeled borrow. It does **not** exercise a full-history stage selector, perturb future risk fitting, or qualify all eight shared X1 components. A shared byte-comparison future-mutation adapter and its planted last-date-reference/full-panel-risk controls remain integration work. Do not relabel this limited availability fixture as a complete causality gate. No stage-level preference calibration, default migration or full X1 gate was waived.

## Commits / state

- Base synchronization only: merge `8b18bd5e610cd6560f4e97518b8e9f0f911719a3`, exact tree root `977a93ee`; do not import that merge.
- R3 source: `65bdf3da27cbb8322b2a041227a77fb911baf2e3`.
- Private implementation extraction: `ec6719b2d9817db7d53f113c6a61d6280edfa4fd`.
- Initial six focused fixtures: `db36df91767d49476f9306539286b45a967144d9`.
- Impact-reference review correction / seventh fixture: `fc80a5eed0d4bcc9d2e438218e5a25fd3f35675c`.
- Direct implementation includes: `6be7f67f2dc0a214d525625449c67779a1f1188c`.

`git diff --check` passed. clang-format processed the new source ranges/fixture for formatting only. Source review, compilation, runtime and performance are separate gates; only the source/fixture work and mechanical extraction evidence are claimed here.
