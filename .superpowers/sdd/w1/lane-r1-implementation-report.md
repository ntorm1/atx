# W1-R1 sparse constraints: implementation frozen, qualification pending

Owner: pool-4 / `feat/w0-replay-integration-codex-20260925`. Source-only work under the original 2026-09-24 DAG W1-R1; implementation preceded fixtures. No compiler, test, benchmark, market-data or warehouse execution occurred in this lane. Root owns CMake registration of `atx-engine/src/risk/constraints.cpp` and integrated qualification.

## Production changes

- `risk/constraints.hpp` and new `src/risk/constraints.cpp`: explicit `LegacyDenseV1` (API default) and `SparseCsrV2`. CSR stores general rows and represents identity boxes implicitly, retaining the exact original logical row order and lower/upper arrays. Materialization validates shape, finite sparse coefficients, checked index/nnz/byte bounds before allocation. No implicit conversion of old descriptors or solver selection.
- New hard `TradeParticipationCap` enforces `previous_weight +/- rho * dollar_ADV / NAV`; `DaysToLiquidate` enforces `abs(weight) <= days * rho * dollar_ADV / NAV`. Inputs must be explicitly aligned dollar ADV, positive finite NAV and finite decision-time previous weights. Zero ADV binds, unavailable/negative/nonfinite ADV refuses the active cap, contradictory intersections refuse, and an elastic shared position box cannot erase these hard limits. Existing share-ADV `ParticipationCap` remains its versioned position-box recipe.
- `qp_augment.hpp`, `qp_factor_admm.hpp`, `discretize.hpp` and `elasticity.hpp` consume logical row counts/ordered row visits. Sparse pins append CSR equalities. Factor ADMM still owns dense general-row/Woodbury work, bounded before allocation; this is not a claim that every solver workspace is sparse. Augmented assembly uses nnz/index/resource admission instead of a dense squared-dimension assumption. Sparse/AMD copies and two capped LDL factors have separate bounds; relaxed and externally supplied augmented forms also receive admission checks.
- `RelativeEconomicV2` is opt-in. Row tolerance is absolute tolerance plus relative tolerance times the magnitude of the row value/finite bounded endpoints; unbounded +/-1e30 sentinels do not inflate the scale. Actual-weight linear, gross and turnover checks supplement augmented feasibility, without an instrument-count multiplier. Nonfinite active metadata and overflowed totals refuse certification. The direct helper certifies linear/L1 constraints only; the solver owns factor-aware cone certification. Relative V2 refuses elastic metadata. `LegacyAbsoluteV1` retains the old numerical acceptance recipe.
- `equity_allocation.hpp/.cpp`: API default remains `LegacyDenseAbsoluteV1`. `SparseRelativeV2` removes the squared KKT index admission and dense constraint reservation, uses CSR and direct economic feasibility, and retains the same fixed-iteration unscheduled augmented ADMM route. It divides an economic margin between absolute and relative error without dividing by union size. The independent represented-execution, mandatory-zero, post-fee and accounting certificate remains mandatory. Sector accumulation now explicitly refuses overflow instead of allowing NaN to disappear in a maximum.
- `config.hpp/.cpp`, `stage_equity_book.cpp`: actual book application selects `--allocation-rule sparse-relative-v2` by default; explicit `legacy-dense-absolute-v1` is supported. V2 recipe binds storage/solver/LDL budgets and exact tolerance formulas; certificates expose effective tolerances. V1 omits new JSON keys and keeps its old profile/formula. The hash-bound report/manifest contains the selected recipe. This changes the book application numerical policy deliberately, not the library default.
- `mpc_stack.hpp` and frozen `qp_solver_reference.hpp` explicitly require legacy dense storage. Ordinary nonempty CSR already failed their old A/l/u geometry checks; the new guards make the unsupported policy clear, including empty-row cases. No demonstrated dropped-row runtime defect is claimed. Other direct constraint A accesses found in the owning risk inventory are explicitly legacy branches or internal frozen dense-reference systems.

Dollar-liquidity descriptors are implemented and tested at the engine API boundary. This lane does not invent a decision ADV input for equity allocation; observed market-input/cost consumer wiring belongs to the later R3 integration. No claim of executable liquidity or calibrated cost follows from the new descriptors.

## Frozen commits

1. `96aadfb87c5c140e2ca2590dbaf7875eb5c188d1` — CSR, implicit boxes and solver consumers.
2. `42f781a0a84d7ee3aeb58eab2ba08c869988f85e` — hard trade/liquidation caps.
3. `fdc496421f91533e77a0ea3797c0c02ed3716e7a` — versioned relative economic feasibility.
4. `b37283f20472d06770571ef1aeda6bb88e354c9f` — allocation policy and persisted recipe.
5. `454f8f97fde214f61a612ee983c05d1f7fb4f94c` — invalid active metadata rejection, static validation API repair, relaxed/prebuilt resource admission.
6. `760f28f724a16b287a29ac7562ee970db181a35a` — robust metadata included in pin admission.
7. `2308d27f3beb82722300a1fde62bbb1ed0db88bd` — postimplementation fixtures.
8. `fb4702b1991d3ec1082d871ae795a0f3aefad98c` — explicit unsupported-consumer guards.

## Qualification supplied to root

Eight new engine checks in existing `risk_constraints_test.cpp` (5), `risk_qp_solver_test.cpp` (2) and `risk_discretize_test.cpp` (1). They cover ordered dense/CSR coefficient and bound bits; implicit boxes; both augmented and scheduled factor-space book/certificate parity under unchanged legacy tolerances; dollar units, zero/missing liquidity and incompatible hard limits; resource refusal; actual L1 budget violations; malformed active metadata and overflow; and appended pin geometry.

Three new implementation checks: `equity_allocation_test.cpp` (2), `config_equity_book_test.cpp` (1). Existing `stage_equity_baseline_test.cpp` book fixtures additionally bind V2 manifest/certificate fields, determinism and explicit V1 omission. The 5000-name fixture checks the admission plan only: it performs no 5000-name solve or benchmark.

Minimal new-check filters:

```
RiskConstraintsR1.*:RiskQpSparseR1.*:RiskDiscretizeR1.*
EquityAllocationR1.*:ConfigEquityBook.*:StageEquityBaseline.ConstrainedBook*:StageEquityBaseline.ObservedClose*
```

Root also registered existing factor-ADMM, equity-factor and owning risk/implementation cases to qualify consumer compatibility. `git diff --check` passed on this lane's edits; no C++ build or test result is asserted here. Explicit legacy versus CSR bit fixtures still need runtime proof. The explicit legacy path preserves the old valid-domain operation order by inspection; that is not a substitute for the frozen oracle/runtime gate.

## Open acceptance

The original materialization <=5ms / RSS <150MB at M=5000 and actual equity allocation solve at M=5000 remain unmeasured and open. The admission cap is an array/workspace bound, not measured process RSS. Group/sector row emission still scans instruments per group, and factor ADMM still carries bounded dense general-row/Schur work; neither is advertised as an O(nnz) solver. No owner waiver or performance pass is inferred. Root must attach integrated runtime and later bounded performance receipts before marking the full R1 lane accepted.
