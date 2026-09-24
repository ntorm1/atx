# Zoo-to-book (lane 8) — open integration items

Lane 8 delivered the engine and library pieces for the zoo-to-book path: per-name
replay cost with participation-capped working orders, a borrow schedule, delisting
events and event batches, PreferenceSource, factor-bounded equity allocation, and
research_cost_sim. It did **not** wire them into the native stages. The lane is not
done against its acceptance criteria until each item below is closed. Nothing
listed here has been run on real data.

| # | Item | State | Where to wire |
|---|------|-------|---------------|
| 1 | `stage_equity_book` accepts combo / zoo recipes through `PreferenceSource` (it still hard-rejects every recipe except slow momentum) | open | `stage_equity_book.cpp` recipe gate; provenance and recipe schema in `stage_run` and `stage_report` |
| 2 | Real factor model in `equity_allocation` (it still builds a dummy zero-exposure 1-factor `FactorModel`) | open (needs Lane 7) | the `FactorModel::create` call in `allocate_equity_preference`; take `kappa_i` from `SqrtImpactCost::cost_fraction` for Lane 6 `TradeCostTerms` |
| 3 | `run_all` on policy replay, and retirement of the legacy one-period report | open | `ReplayReportParity` covers only the `detail::accumulate_period` core, not stage_report file output |
| 4 | `research_cost_sim` in `stage_discover` and `stage_sweep` admission (fitness still uses the frictionless `research_sim`) | open | admission gate; the sim now charges full-size uncapped impact and never prices unusable liquidity at zero |
| 5 | `corporate_actions` -> `ReplayEventBatch` conversion | open | the security master carries no delisting or merger terms; use `DelistingRecord` -> `delisting_events_from_records` for now |
| 6 | `DelistingPolicy` plus `delisting_events_from_records` in the native replay, and a rerun of the annual run past PCS 2013-05-01 | open | native stage replay config |
| 7 | One real OOS run reporting net Sharpe, turnover and capacity at $10m / $100m / $1bn, recorded in the ledger | open | depends on items 1, 4 and 6 |
| 8 | Certified factor-neutral book under a binding participation cap | partial | `measure_equity_exposures` now reports the realized beta and sector exposure of the held book (`ZooToBookE2E.BindingCapHeldBookExposureIsMeasuredNotAssumed`). Still open: feed the executable size `min(request, cap * ADV)` into the QP as per-name trade box bounds so the certified book can actually be filled |
| 9 | Cost model, borrow schedule and delisting policy on `replay_scheduled_intents_with_events` (the claims path rejects them) | open | claims replay entry point |
| 10 | Serialize `postfee_beta_exposure` and `postfee_max_sector_net` in `stage_equity_book`'s certificate JSON | open | certificate writer |

## Semantics fixed in review (2026-09-23)

- **Financing.** Short-sale proceeds earn only `rebate_bps`. `cash_bps` accrues on
  free cash, which is settled cash minus short dollars. Before this fix, proceeds
  earned both rates. With rebate equal to cash rate r, a dollar-neutral book now
  earns r on NAV (`BookBorrowSchedule.DollarNeutralBookEarnsThePolicyRateOnlyOnceOnNav`).
- **Research cost.** `ReplayCostModel::unrationed_cost` charges the whole request
  at the uncapped rate. For sqrt impact that is `cost_fraction(|q|) * |q|`, which is
  (q/f)^delta above the rate of a capped fill. A name with unusable liquidity (NaN,
  zero or negative ADV) is charged `unusable_liquidity_penalty_bps`, 1000 by
  default; setting it to NaN rejects the run instead.
- **Working orders.** An order on a name that has passed its delisting
  `last_valid_period` is cancelled even when no units are held.
  `has_working_orders()` is O(1). An order in a name whose liquidity row stays
  unusable remains open by design until the next decision replaces it, and is
  counted in `ReplayResult::open_working_orders`.
