# Equity long/short engine review

Date: 2026-10-01. Reviewed the current shared checkout using three implementation
agents and coordinator review. Codebase-memory MCP graph tools were unavailable;
source search was the fallback. This is a risk-prioritized review, not an audit of
every engine function. Native verification results are recorded in the
[sprint plan](../plans/2026-10-01-equity-review-sprint.md).

## Correctness findings repaired in sprint 1

| ID | Severity | Trigger and consequence | Fix and source |
| --- | --- | --- | --- |
| C1 | P1 | A constraint set containing only trade participation or liquidation limits was classified as minimal. Both optimizers could return a book that ignored the requested limits. | Include both descriptors in dispatch; seed sequential horizons with explicit flat holdings; reject stage-relative trade limits in true MPC until supported. `include/atx/engine/risk/optimizer.hpp`, `src/risk/multi_horizon.cpp`. |
| C2 | P1 | Fast optimizer accepted negative caps, infinite alpha, nonfinite prior weights, or invalid risk/turnover/gross configuration. Negative caps violate clamp preconditions; other cases can poison weights. | Validate boundary inputs while retaining documented NaN-alpha semantics. `include/atx/engine/risk/optimizer.hpp`. |
| C3 | P1 | Large adverse sell impact, invalid marks, or unrepresentable prices/fees could emit negative/zero-price fills. Volume and permanent impact changed before conversion succeeded. | Validate modeled price, fee, and permanent mark and checked Decimal conversion before mutations; retain invalid pending intent. Also remove timestamp addition and signed-quantity overflow hazards. `include/atx/engine/exec/execution_sim.hpp`. |
| C4 | P1 | CPCV folds beginning after global index zero passed a real first return to an API that discards its first observation. Joining disconnected position blocks also invented turnover. | Supply exactly one structural zero; select real fold returns; compute each date's turnover on the original calendar before selection. `src/factory/fitness.cpp`. |
| C5 | P1 | DSR combined an average of fold Sharpes with full-stream sample size, skew, and kurtosis. These statistics do not describe the same sample. | Compute per-period Sharpe and all DSR moments on the same unique realized sample. `src/factory/fitness.cpp`. |
| C6 | P1 | Duplicate Dataset instrument IDs were accepted, then collapsed by the alignment map to the first column. Source order could silently choose the security's feature/signal. | Reject duplicate IDs at Dataset creation; enforce documented binary mask and checked shape product. `src/data/dataset.cpp`. |
| C7 | P2 | A successful combiner fit containing NaN/Inf was adopted; hysteresis arithmetic could then prevent recovery. | Treat any nonfinite fitted weight as a failed fit, retain valid prior weights, allow later recovery. `include/atx/engine/combine/walk_forward_combiner.hpp`. |
| C8 | P2 | BorrowSchedule rejected finite negative net rebates, excluding ordinary hard-to-borrow financing cases even though replay arithmetic supported them. | Permit signed finite rebates; preserve mutually exclusive fee/net-rebate rules. `include/atx/engine/book/borrow_schedule.hpp`. |

Negative net rebates are a real financing case when borrow fees exceed proceeds
interest, as described by [Interactive Brokers](https://investors.interactivebrokers.com/en/pricing/short-sale-cost.php?menu=A).

## Performance findings

| ID | Status | Evidence and action |
| --- | --- | --- |
| P1 | Repaired | `aggregate_oos` copied position blocks and recomputed differences for every fold: O(F*T*N) work and repeated allocation. It now differences the chronological stream once, then selects scalar returns/turnover: O(T*N + F*T); scalar scratch is reused. |
| P2 | Repaired | `align_onto` repeated an identical instrument hash lookup on every date, and drop-report classification visited every plug cell. Resolve identity once per instrument and count the sorted future-date suffix once. Output materialization remains O(D*N*C), as required by the dense result. |
| P3 | Deferred | Legacy capped optimizer runs repeated whole-book projections inside fixed iterations and nested cap searches. Benchmark representative top-3000 workloads before changing the numerical contract; prefer the existing cost-aware solver where applicable. `include/atx/engine/risk/optimizer.hpp`. |
| P4 | Deferred | `ExecutionSimulator::replace_pending` and per-instrument volume lookup scan vectors, producing quadratic work for broad baskets. Use indexed scratch with stable FIFO/order semantics and measure allocation/latency before replacing. `include/atx/engine/exec/execution_sim.hpp`. |

The repaired performance claims describe removed work and asymptotic behavior.
No isolated Release benchmark or measured speedup is claimed by this sprint.

## Remaining correctness and integration work

| ID | Priority | Source evidence and required next work |
| --- | --- | --- |
| R1 | P1 | `src/factory/search_driver.cpp` computes generation DSR using prior-generation trial count and caches the haircut. Earlier elites can retain more lenient deflation as trials grow. Version score caches, retain raw sample statistics, and refresh every ranked candidate at a common current trial count. |
| R2 | P1 | `include/atx/engine/factory/factory.hpp` defaults `oos_fraction` to zero. CPCV scoring of formulas selected on the same search window is not independent post-selection qualification. Require explicit production holdout/lockbox policy while preserving research-only operation. |
| R3 | P1 | `src/book/replay.cpp::scheduled_financing` uses `rebate_bps == 0` to select fee-quote semantics. An explicitly zero net rebate cannot be distinguished from a fee quote; cash-interest treatment differs. Add a versioned quote-kind field rather than infer meaning from its numeric value. |
| R4 | P1 | `replay_scheduled_intents_with_events` explicitly rejects cost-model/borrow-schedule extensions. Claims/stock-transition handling and realistic short financing therefore cannot currently be combined through that entry point. Integrate them with source-admitted event evidence and reconciled signed inventory. |
| R5 | P2 | Legacy optimizer documentation describes `P V^-1 P alpha` as an equality-constrained mean-variance solution, but general covariance requires a `V^-1 1` Lagrange correction. Gross normalization also removes positive risk-aversion magnitude. Preserve/version the legacy heuristic and route production policy to the existing constrained solver; do not silently change historical pins. |
| R6 | P2 | Invalid execution economics now retain orders silently because the existing API returns only fills. Add explicit rejection/defer diagnostics and a deliberate sizing policy. Constructor configuration also lacks complete validation; some commission clamps can mask invalid parameters. |

These items require policy/API or cache migration and broader integration than the
bounded repairs. They remain open in sprints 2-3; they are not acceptance claims.

## Capability comparison for large-scale equity alpha research

WorldQuant publicly describes BRAIN as a data-backed alpha creation and simulation
platform. Its public description is a reference for research workflow, not evidence
about proprietary production architecture or a parity checklist.
[WorldQuant BRAIN](https://www.worldquant.com/brain/)

| Capability | Existing ATX evidence | Missing or unverified integration |
| --- | --- | --- |
| Formulaic alpha research | DSL parser/type checker, cross-sectional/time-series/group operators, VM, WQ101 battery, genetic search, subtree cache | Consistent settings/provenance and validated semantics across legacy/current evaluation routes |
| Broad point-in-time equity data | Availability clocks, PIT universe, fundamental artifacts, security transition primitives | Real admitted corporate actions and financing in one executable chronological book; broad real-data acceptance remains unverified |
| Alpha diversity and admission | Correlation/dedup indexes, DSR, CPCV/PBO, robustness battery, trial registry and lockbox tools | Common trial-count refresh, mandatory independent qualification for production admission |
| Causal residual alpha plus net executable value | Residual IC and delayed execution objectives exist | `search_driver.cpp` and `fitness.cpp` explicitly reject nonempty pools for those paths; pool diversification, residual screening, net replay, and final holdout need one identified pipeline |
| Neutral long/short construction | Factor/sector/beta exposure constraints, turnover and cost-aware optimization, locate caps, multi-horizon optimization | True-MPC stage-relative trade caps explicitly unsupported; calibrated expected-return/impact inputs and measured capacity remain required |
| Short lifecycle | Borrow grids, locate quantities, signed rebates, explicit locate clipping/rejection | Recalls/forced buy-ins, explicit quote semantics, and complete broker financing reconciliation |
| Production operation | Simulator pending order replacement, portfolio accounting and replay reports | Broker acknowledgment/cancel/fill reconciliation, operational controls, and demonstrated production readiness |

Prioritize joining these existing components into a reproducible, costed,
independently qualified book before adding more alpha operators. Passing synthetic
software regressions establishes neither alpha profitability nor investment capacity.

## Review process

Implementation preceded regression additions. Risk/data/fitness changes received
independent source review by a second agent; execution/accounting received a third
agent's review. Only critical workflow selections are run; no broad suite or
data-mining campaign is planned. Unrelated `atx-db` edits are
preserved. Commit IDs and native results are in the sprint completion record.
