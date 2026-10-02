# US equity long/short platform buildout

Persistent user objective: build ATX into an equity quant long/short platform
comparable in capability and operational discipline to institutional systematic
managers. Preserve the full objective across sessions; a repair sprint is progress,
not completion. User requests sub-agent development, focused critical end-to-end
validation after implementation, and incremental commits.

## Current evidence and operating rules

The preceding goal turn made progress: six commits through `0677669f` repaired
eight review findings and removed redundant research/data work. Native Debug
build and 55 selected checks have passing evidence. This does not establish
investment performance, realistic capacity, or production readiness.

Work directly in `C:/atx/atx-engine`; preserve unrelated `atx-db` edits. Graph MCP
tools are unavailable in this session, so source search is the fallback. Root
owns build configuration and runs one native build at a time. Implementation
agents own disjoint paths and send integration/review evidence before root commits.
Use the dedicated `atx-engine-equity-review-tests` target to avoid compiling or
running unrelated tests. A narrowly necessary consumer migration may touch
`atx-impl`; identify it separately in the completion record.

## Required platform acceptance

| Area | Evidence needed before broad completion |
| --- | --- |
| Data and identity | Reproducible point-in-time universe, publication/revision clocks, permanent security identity, corporate actions and delistings; admitted source evidence and no future information in earlier decisions |
| Research selection | Causal operators and features, reproducible search, cumulative trials, current consistent multiple-testing adjustment, alpha diversity, frozen selection before independent audited holdout confirmation |
| Portfolio | Calibrated forecasts, validated factor/specific risk, gross/net/beta/sector/name/turnover/participation/locate limits, cost-aware optimization against actual holdings, independently checked feasible executions |
| Book accounting | Chronological reconciled cash/positions/claims, signed financing with explicit quote convention, costs, partial orders, transitions, changing membership, recalls/buy-ins, no fabricated held marks |
| Performance and capacity | Representative top-3000 profiling, bounded memory, identified data/model/AUM, calibrated impact/costs and measured capacity rather than synthetic throughput alone |
| Operations | Durable configuration/provenance and recovery, broker order/cancel/fill reconciliation, monitoring and risk controls, paper-trading evidence before live operation |
| Investment qualification | Pre-registered independently held-out results, costs and bias audits, uncertainty and drawdown/capacity evidence; green software tests are insufficient |

Named-firm parity cannot be proven from public descriptions of proprietary
systems. The table defines concrete US equity platform acceptance without treating
the unknown internals or a profitable result as already established.

## Active implementation wave

| Lane | Ownership | Deliverable | State |
| --- | --- | --- | --- |
| Selection | search driver, fitness statistics, search-state codec | Current trial-count DSR for cached/new/resumed candidates without re-backtesting | In progress |
| Admission | factory orchestration/admission and consumers | Explicit research mode; production requires frozen selection and independently audited holdout | In progress |
| Accounting | borrow schedule, claims replay | Explicit zero net-rebate quote and coherent financing/costs across corporate events | In progress |
| Execution | simulator and bounded validation target | Indexed broad-basket order/volume work and observable deferred execution reasons | In progress |

Each lane implements first and adds only critical workflow regressions. Root
integrates, obtains cross-review, runs the affected selection once, fixes observed
failures, and records commits and limitations here. No live order submission is
part of this wave.

## Outstanding program work

Real-source event admission remains separate from synthetic event planner checks.
Residual IC, delayed executable PnL, and marginal alpha-pool value still need one
identified research-to-admission path. True multi-period trade limits, borrow
recalls, calibrated execution/capacity, production order management, and real
independent research qualification remain required. Continue selecting work from
authoritative code/runtime evidence rather than declaring success at the end of
this wave.
