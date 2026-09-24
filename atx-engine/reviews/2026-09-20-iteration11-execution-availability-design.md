# Iteration 11: observed execution-price availability

Date: 2026-09-20. Research and design only. No production source changes, native builds, tests, ZIP scans, or new strategy runs were performed for this document. Checkpoint 10 sources and failed attempt remain frozen.

## Recommendation and scope

Add a narrow, opt-in `ObservedCloseEntryConstraintV1` to the existing hypothetical-close allocation profile. At an execution boundary, a name with **exactly zero held TRI units** and an absent, nonfinite, or nonpositive **current execution close** has its weight fixed to zero before the QP solve. Keep every nonzero held name in the canonical union and reject any missing held mark before optimization or policy selection. Preserve the frozen decision preferences, eligibility and past-only risk snapshot unchanged.

The meaning is precise: the supplied observation cannot price a new position in this simulator. It is not evidence that the exchange halted the security, that its real volume was zero, or that an order was rejected. Call the property execution-price availability, not market tradeability. No price is imputed, no current held obligation is erased, no future missingness is examined, and no missing security is permanently removed from the source universe.

Retain the current strict behavior as the default for the public allocation API. The stage may explicitly choose the new versioned research profile after approval of this design. This is a changed execution policy and therefore requires a new attempt identity and output directory. It does not revise the validity of the previous failed replay or qualify historical/live execution.

## Evidence motivating the slice

Iteration 10's first proposal produced the independent startup solution's exact 381-name support under the declared MachinePrecisionIntents policy. The native attempt then failed during a later allocation with:

`equity allocation: candidate intent failed instrument=604: replay: missing/nonpositive required close`

The coordinator identified this as the second callback attempting a new position in canonical instrument 604, source identifier 150340 (GNW), whose current close is missing. This differs from checkpoint 9's tiny pre-existing holding. The native stderr has no decision/execution metadata, so the diagnostic should gain those fields in a future implementation. The current independent proposal checker validates one proposal and six observable intervals; its classification explicitly does not independently reproduce the failed second allocation. Do not promote the coordinator's contextual diagnosis into a second independently verified allocation.

Evidence: `build-equity/audits/iteration10-equity-book-native.stderr.log`, `iteration10-equity-book-measurement.json`, and `iteration10-allocation-proposal-verification.json`. The source reconciliation audit still leaves the GNW/MA gaps unresolved. This proposed policy changes how an unheld name is admitted at execution; it neither fills those gaps nor authenticates the vendor data.

## Primary-source review

The portfolio-optimization formulation in Boyd and coauthors' paper includes no-buy, no-sell and no-trade constraints on the trade vector, as well as volume constraints based on a current-period estimate. For an unheld instrument, no trade is exactly the linear equality `w_i=0`. This supports adding an observed execution restriction as a hard equality in the existing QP, conditional on a valid information-time contract. [Multi-Period Trading via Convex Optimization, section 4.5](https://www.cvxportfolio.com/en/stable/_static/cvx_portfolio.pdf).

Cvxportfolio separates information given to a policy from realized execution filters. Its pinned 1.5.0 simulator supplies past returns, past volumes and current prices to the policy; afterward it cancels trades where realized current volume is nonpositive, then recomputes cash and costs. This is a useful precedent for explicit execution filtering, but not justification for passing realized period volume into an earlier optimizer or identifying a missing vendor price with zero market volume. ATX must also preserve its own hard post-trade constraints, which can fail after individual trades are canceled. [Cvxportfolio 1.5.0 simulator source, `simulate`](https://raw.githubusercontent.com/cvxgrp/cvxportfolio/1.5.0/cvxportfolio/simulator.py).

QuantConnect's equity model conditions fills on order type, exchange hours and data availability; for limit orders it waits for a later bar when the bar end time is at or before the order timestamp, expressly avoiding use of the order's own bar. The documented model is not a universal execution rule, but it demonstrates why timestamp and fill assumptions must be specified separately from a numeric price check. [QuantConnect EquityFillModel documentation](https://www.quantconnect.com/docs/v2/writing-algorithms/reality-modeling/trade-fills/supported-models/equity-model).

NYSE's current auction timeline places normal MOC/LOC entry cutoff at 3:50 p.m., with specified exceptions, before the closing auction begins at 4:00 p.m. Seeing the final closing price, or concluding that a final vendor close is absent, does not establish that the corresponding information was available before submitting an executable close-auction order. These current rules illustrate the ordering issue; they are not retroactively asserted as the exact rules for every security/date in the 2013 sample. [NYSE auction timelines](https://www.nyse.com/trade/auctions).

Actual exchange restrictions have explicit identity and time semantics. Nasdaq's halt data describes halt time, reason, quotation resumption and trade resumption separately. A vendor's missing daily OHLCV row supplies none of those facts by itself. [Nasdaq trading-halt data fields and codes](https://www.nasdaqtrader.com/Trader.aspx?id=TradeHaltCodes).

## Causality and the hypothetical-close limitation

There are three different clocks:

1. The earlier alpha decision freezes preferences, universe eligibility and risk inputs.
2. The execution boundary observes the portfolio and the supplied current price/availability snapshot.
3. Orders are submitted and fills occur under an execution model.

The existing research replay collapses steps 2 and 3: it sizes using the execution close and fills at that same close. An availability restriction can be causal **within that declared simultaneous sampled-price model**, because it uses only the same boundary snapshot already supplied for marked-holdings allocation. The new constraint does not make that model a proven executable MOC strategy. It must remain labeled `hypothetical-close-sizing-using-marked-holdings`, with an additional explicit availability convention.

The ZIP's session/date labels do not provide release timestamps, receipt timestamps, freshness, venue reachability or a data-completeness watermark. In a real system, an absent observation becomes actionable only relative to a known clock and feed-health policy; a price received after an order cutoff cannot justify a pre-cutoff decision. Without those records, `available_by_order_submission` is **unverified**, not inferred from row position or set to true. Restrict the first implementation to the current research profile.

A later production interface should consume an independently recorded execution snapshot with security identity, market/event time, receive time, freshness/status and order-submission boundary. When timing cannot support same-close execution, use a subsequent executable quote/bar or explicitly modeled auction order submitted from earlier information. That is a separate model change; this slice must not silently alter delay, fill price or order type to claim causality.

Do not add an execution-day volume filter to this slice. A past-only volume estimate belongs in capacity constraints; realized volume can belong in a fill simulator. Zero or absent vendor volume can have feed-specific meanings, and the total volume of the execution session may be unavailable at an earlier order cutoff. The narrow rule uses only the validity of the current supplied close, without declaring an exchange halt or liquidity guarantee for names that pass it.

## Alternatives and their consequences

| Policy | Result when an unheld desired entry lacks a current mark | Consequences |
| --- | --- | --- |
| Strict requested-mark failure | Keep the current all-or-error result. | Preserves the existing experiment; appropriate when source timing/coverage cannot support a conditional execution assumption. It remains the API default. |
| Conditional QP entry restriction | Fix the unheld name to zero before solving and optimize the full remaining book. | Recommended opt-in research slice. Preserves mathematical neutrality/caps/turnover through the actual optimization and subsequent certificate. It changes allocations in other names and therefore changes the execution policy. |
| Reject only that order after optimization | Hold that name's zero position; execute other proposed trades. | May destroy neutrality or another hard constraint. Requires a fill/order ledger, actual-exposure checks and an explicit residual-risk policy. A post-solve clip cannot retain the original QP certificate or hard-book claim. |
| Reject the entire rebalance without fills | Keep exact current units and cash, with no trade fees, and continue carry accounting. | Requires a distinct rejected-rebalance event, rather than reporting an accepted target. Drifted holdings or newly ineligible names can violate the intended target constraints; mandatory exits are not fulfilled. Missing held marks still prevent valuation. |

Canceling a whole rebalance is not equivalent to returning Hold instructions as a successfully certified new book. The current helper imposes net/caps and original decision eligibility on the accepted result. A failed-to-execute order basket has different semantics and may leave unresolved exposures. A later rejection mode must record that fact and define its risk response, rather than bypassing the helper's acceptance gate.

## Narrow implementation proposal

Add a separate frozen configuration enum, for example:

```cpp
enum class EquityExecutionAvailability : u8 {
    RequireRequestedMark,
    ObservedCloseEntryConstraintV1
};
```

The default remains `RequireRequestedMark`. Keep this independent of `EquityAllocationRepresentation`: availability determines a QP equality before solving; machine representation determines precise execution instructions afterward. Neither flag changes the alpha decision snapshot.

For each canonical coordinate at execution:

`unheld_unpriced_i = (tri_units_i == 0) && !is_finite_positive(current_mark_i)`.

After the existing validation of **all held marks** and pretrade NAV, derive this predicate from the current execution state. There is no caller-supplied override that can mark an actually held position unheld or suppress its required valuation. In strict mode it is merely diagnostic; in the opt-in mode it is an additional fixed-zero reason. Keep the original union rule, `decision eligible and risk ready OR nonzero held`, and retain unpriced eligible/risk-ready names in that union. Do not shrink the universe, mutate decision eligibility or recompute ranks after seeing prices.

For an eligible/risk-ready, unheld/unpriced union member, change its existing identity box row to `lower=upper=0`. Reuse the same risk diagonal, objective, net/gross/name/turnover constraints, fixed iteration count, factor budget and memory admission policy. Its previous weight is exactly zero, so this restriction incurs no forced exit turnover. Constraints on all other coordinates remain unchanged. If the restricted problem is infeasible or fails its numerical certificate, return an error; do not retry without the restriction or increase the budget.

Factor the fixed-zero reason calculation so QP box assembly, post-solve exact lifting, intent selection and economic recertification agree. Required zero reasons include frozen decision exclusion and execution-price unavailability, while eligible held risk-unreadiness remains an error. Additional fixed-zero coordinates remain within the existing `F <= M` propagated-row bound; verify that reasoning explicitly rather than changing economic tolerances. Publish their lifting deltas separately by reason, and keep the machine-scale ordinary representation budget unchanged.

The pure representation API must apply the same explicit availability mode and certify final exact zeros. Its direct calls still have `solver_used=false`: a supplied raw vector subsequently forced to zero is not a QP optimum certificate. The solver-backed path's raw certificate covers the QP that already contains these equalities. The raw vector, fixed-zero lifting, machine representation, actual unit sizing and post-fee economics remain separately auditable.

No engine replay change is needed for the conditional-QP slice. Replay still validates required held marks before the callback, consumes exact intents, rejects missing marks for actual entries, and marks all remaining holdings on every subsequent observation. A later valid close can permit a fresh entry if that later callback's frozen eligibility/risk and constraints allow it; an absent close never creates a synthetic exit or permanent retirement.

The stage records the opt-in mode and timing qualification in the recipe/identity. Failure messages add decision and execution period/session, plus canonical instrument mapping in the stage, without dropping the underlying reason. Do not overwrite checkpoint 10's failed output or change its checker classification after the fact; a new independently qualified diagnostic may be published alongside it.

## Audit and economic effects

Record, per execution proposal:

- full canonical and solve-union counts;
- number of unheld invalid current marks in the canonical universe, and separately the eligible/risk-ready subset receiving an additional QP equality;
- number of nonzero held positions and confirmation that all their current marks passed; a failure carries its exact coordinate/time rather than a successful certificate;
- required-zero counts by reason and the deduplicated union count; reason codes or canonical indices must support independent reconstruction;
- digest/source identity of the current observation and availability reason vector; distinguish NaN/nonfinite from nonpositive values without encoding invalid JSON numbers as valid prices;
- continuous weights, exact intents, original preferences/eligibility/risk identity, fixed-zero lifting by reason and represented economic certificate;
- actual turnover, trade fees, post-fee NAV and all unchanged constraints.

An unavailable unheld name produces no units, no order fill and no trade fee. Reoptimizing other names can increase or decrease their turnover and costs; do not assume a cost reduction. Charge only actual represented dollar trades. Preserve the fee reserve `1-c*T` and full actual-holdings L1 turnover limit. All-cash startup is still allowed to remain underinvested. A conditional feasible set with fewer entry opportunities can produce lower gross or no risky book; there is no gross normalization or compensation outside the QP.

No unconstrained counterfactual solve is required merely to report a “blocked intended trade.” Without such a solve, an excluded preference is not a measured rejected order or forgone return. Report the input preference and additional equality, not invented attempted dollars. The first bounded iteration should not add extra counterfactual solves.

## Selection bias and experiment identity

Historical missingness can correlate with liquidity, corporate events, feed quality and returns. Avoiding entries on missing data can alter portfolio composition and apparent performance. Even when the rule reads only the current row, a modern reconstructed vendor snapshot may encode availability or revisions that were not known historically. This policy addresses the simulator's ability to price new holdings; it does not establish unbiased point-in-time data.

Freeze the rule before the next attempt and apply it uniformly to every name and every execution. Do not use the eventual failure date, later observed recovery, delisting labels, realized return, or terminal-event research to select affected names. Do not require coverage over the full future holding period. Preserve the original window, alpha recipe, financial assumptions, solver budget and failed attempts. Any resulting performance is conditional on this new execution model and remains unqualified for alpha quality, capacity or live implementability.

## Two focused analytical checks

1. **Conditional restriction changes the solve, not just the output.** Three unheld names have preferences `(0.4,-0.3,-0.1)`, identity tracking risk (`risk_penalty=0`), net zero, gross/name limits that do not bind, and a turnover bound above `0.2`. Only the first execution mark is invalid. Strict mode rejects its desired entry. The conditional QP must return `(0,-0.1,+0.1)` within the existing numerical contract, with exact Close for the first coordinate, full union retained, unchanged frozen preference/eligibility, and one additional execution-zero equality. With NAV 1000 and 5 bps, actual trade dollars are 200 and fee 0.1, subject only to binary64 sizing. Merely zeroing the unrestricted `(0.4,-0.3,-0.1)` would give net `-0.4`, so this fixture detects post-solve clipping. Repeat the supplied current snapshot with unrelated later data changed: the result must be identical. A later *current* valid price may lift the execution restriction, but only at that later callback.
2. **Missing held marks remain fatal.** Give the missing-price name a nonzero held unit count and test both frozen eligible and mandatory-exit states. Both modes must fail before QP or intent application, preserving all supplied state. Include a replay boundary where an available entry is accepted and its next held mark is absent: the new entry policy must not anticipate that absence, cancel the earlier entry, or convert the later missing valuation into a Close. Retain the existing missing-held-mark fixture; there is no need for a broad new battery.

These checks establish the bounded model behavior, not executable-auction causality. Root-owned build and a fresh same-window native attempt can measure the new implementation once authorized. Further data gaps, terminal events, or allocation infeasibility may still stop that attempt; none warrants relaxing the contract.
