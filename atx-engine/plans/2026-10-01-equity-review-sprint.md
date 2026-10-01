# Equity long/short review and remediation sprint

Date: 2026-10-01. Scope: `C:/atx/atx-engine`, directly in the shared checkout.
Preserve unrelated database and platform work. Coordinator commits each bounded
change. Three sub-agents own execution/accounting, risk, and alpha evaluation;
the coordinator owns data alignment, integration, and this record.

## Sprint 1: repair research and trading correctness

| Item | Problem and deliverable | Acceptance | Status |
| --- | --- | --- | --- |
| S1.1 | Dispatch standalone trade-participation and liquidation constraints; validate optimizer inputs | Public single/multi-horizon solves enforce or explicitly reject constraints; malformed inputs return errors | In progress |
| S1.2 | Validate execution prices, fees, permanent impact, and latency arithmetic before side effects | Invalid modeled fills cannot mutate portfolio, liquidity, or marks; valid later fills remain possible | In progress |
| S1.3 | Support finite negative net short rebates | Flat-price long/short replay books the expected financing debit without duplicate borrow charge | In progress |
| S1.4 | Repair CPCV return/turnover aggregation and consistent deflated-Sharpe sample statistics | Public fitness path preserves real fold returns and chronological turnover; unique-sample deflation | In progress |
| S1.5 | Reject ambiguous instrument IDs and malformed masks; remove redundant alignment work | Dataset-to-panel path preserves instrument mapping and missingness; existing PIT semantics remain intact | In progress |
| S1.6 | Integrate, review diffs, run narrow critical regression selections, commit results | Native build and selected workflow checks, exact commands and limits recorded | Pending |

Implementation precedes targeted regression additions. No broad test campaign,
real-data alpha search, or production trading is part of this repair sprint.

## Sprint 2: consistent alpha admission and scalable research

1. Refresh all search elite DSR scores when the trial count changes; version the
   cache and distinguish raw metrics from trial-dependent admission scores.
2. Profile real top-3000 workloads for the capped optimizer, alignment, and CPCV.
   Preserve certified constraints and numerical contracts when replacing repeated
   whole-book projections or copying. Record wall time, peak memory, and input hash.
3. Bind simulation settings (decision delay, neutralization, universe, costs,
   decay, and truncation) to candidate provenance and admission artifacts across
   both legacy and current evaluation paths. Audit existing coverage before adding
   a second implementation.

Acceptance: changing trial count refreshes all ranked candidates, selection never
uses lockbox observations, and performance changes preserve economic outputs.

## Sprint 3: executable equity long/short book

1. Integrate security transitions, delisting proceeds, and cash-claim settlement
   with admitted point-in-time evidence and the consuming pipeline.
2. Make financing-rate semantics explicit (fee versus net rebate, including zero),
   and connect available stock-loan quantity, recalls, and forced buy-ins through
   optimizer and execution. Audit existing borrow schedule/cap support first.
3. Calibrate chronological impact and participation against execution evidence;
   publish capacity only for an identified AUM, cost model, and liquidity universe.
4. Add broker order lifecycle/reconciliation and operational controls before live
   deployment. Research simulation primitives alone are insufficient acceptance.

Acceptance: one reproducible multi-year book handles changing membership,
corporate actions, short financing, unavailable borrow, and execution restrictions
with reconciled cash/positions and explicitly reported unavailable evidence.

## Validation and completion record

To be populated after integration. Review findings and source evidence are in
`../reviews/2026-10-01-equity-long-short-review.md`.
