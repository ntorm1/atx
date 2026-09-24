# Causal execution representation for constrained equity allocation

Date: 2026-09-20. Iteration 10 research, design, and bounded implementation scope. Native compilation, execution, and measurement belong to the root agent; this note records no new native pass or performance result.

## Observation and recommendation

Checkpoint 9 repaired the QP polish residual and coherent dual handling. Its first 973-name continuous proposal agreed numerically with the independent startup KKT solution. The subsequent replay still stopped at the missing GNW mark on 2013-04-12. The support diagnostic identifies a separate representation problem: 592 coordinates that are exactly zero in that independent solution had nonzero native weights, with maximum magnitude `1.5935755240764225e-27` and total absolute weight about `3.0443545196300966e-25`. Replay correctly treated every nonzero unit count as a holding. The independent solution has 381 nonzero coordinates; the native floating-point vector has 973.

Evidence: `build-equity/audits/iteration9-support-diagnostic.json`, `iteration9-startup-native-comparison.json`, and the frozen `2026-09-20-qp-polish-validation.json`. These records and the failed native attempt remain unchanged. The diagnostic motivates a generally applicable execution representation boundary; the identity or subsequent mark history of a particular security is never an input to the representation rule.

Use explicit `TargetWeight`, `HoldCurrent`, and `Close` instructions. Retain the unchanged continuous vector and its solver diagnostics. Add an explicitly selected, versioned `MachinePrecisionIntentsV1` policy at the allocation boundary, with a small normalized aggregate change budget and independent economic recertification. Preserve the previous `ExactWeights` mode by default. This is a declared floating-point execution convention, not proof that every small coordinate equals zero at the exact optimum, an economically optimal no-trade region, a broker minimum, or an investment qualification.

## Research and alternatives

OSQP specifies primal and dual stopping tolerances and describes polishing as guessing an active set, solving a linear system, and retaining the ADMM solution if the guess fails. Small residuals therefore do not establish exact support or guarantee literal zero bits in every constrained coordinate. This is relevant to ATX's solver architecture, but the new representation policy is ATX's own contract. [OSQP solver documentation](https://osqp.org/docs/solver/).

| Alternative | Meaning and necessary evidence | Decision for this slice |
| --- | --- | --- |
| Structural exact-zero or exact-hold lifting | Mandatory box equalities already supply an exact zero requirement. Two correctly identified binding opposite epigraph rows can imply zero weight or zero change. General automatic inference additionally needs the active rows, correctly signed duals, degeneracy handling, and a validated active-face/KKT certificate. | Retain mandatory equality lifting. Defer inferred active-face lifting; the existing public solver result exposes book and residual diagnostics, not a sufficiently qualified active-face contract. |
| Uniform machine-scale representation | A deterministic, currency-independent policy may replace sufficiently small requested weights by Close and sufficiently small changes by exact Hold. It declares a bounded perturbation, then certifies the actual economic candidate. | Implement `MachinePrecisionIntentsV1`, opt-in at the stage, without altering QP tolerances, iterations, risk inputs, or economic limits. |
| Explicit minimum dollars or share lots | This is an execution feasibility/economic convention. It needs currency, venue, side, asset eligibility, quantity precision, close-position exceptions, and verified conversion from research TRI units to broker shares. A minimum nonzero order introduces a disjunction. | Defer to a separately versioned execution model. It cannot be inferred from numerical solver precision or applied as a physical-share rule to TRI units. |

The third alternative can be useful, but it solves a different problem. For example, Alpaca's current support page states a USD 1 minimum for buy entry orders. That is a specific contemporary broker rule, not a universal cent convention, a short-sale rule, or a historical 2013 assumption. [Alpaca minimum buy entry order](https://alpaca.markets/support/can-we-submit-orders-smaller-than-1-usd-in-notional-value). Convex portfolio problems can include linear transaction costs, while fixed fees and discount breakpoints require different treatment; Lobo, Fazel and Boyd describe relaxations and bounded suboptimal methods for those cases. Their results do not justify arbitrary post-solve rounding or a claim of global optimality for this representation. [Portfolio Optimization with Linear and Fixed Transaction Costs](https://web.stanford.edu/~boyd/papers/portfolio.html).

For structural lifting, gross epigraph equalities `w_i-s_i=0` and `-w_i-s_i=0` imply `w_i=0`; turnover equalities `w_i-z_i=h_i` and `-w_i-z_i=-h_i` imply `w_i=h_i`. These are algebraic implications of actual equalities, not a certificate that a tolerance-based active-set guess is correct. At a degenerate boundary, duals can be zero and active-set inference can be ambiguous. A future active-face API must address that ambiguity and recheck the whole candidate. Even an exact normalized `w_i=h_i` is insufficient for execution Hold: dividing and multiplying dollars and marks can change the original held unit bit pattern.

## Fixed representation rule

Inputs are the frozen decision preference, eligibility, risk snapshot and configuration; the raw continuous QP vector `w`; and the execution-time holdings already marked at observed prices. Let:

- `N` be the full canonical instrument count, including names outside the current solve union;
- `h_i = marked_dollars_i / pretrade_NAV`;
- `eps = numeric_limits<double>::epsilon()`;
- `t` be the unchanged original-unit economic tolerance;
- `c = trade_bps * 1e-4`, `T = turnover_limit`, `r = 1-c*T`;
- `A = 1+c+c*gross_limit+c*name_limit`;
- aggregate ordinary representation budget `B = t*r/(128*A)`;
- `eta_i = min(64*eps*max(1,abs(w_i),abs(h_i)), B/N)`.

The implementation uses sequential divisions and rejects invalid, nonfinite, or underflowed positive budgets. The constants 64 and 128 are frozen policy coefficients. The machine-scale term is not an a posteriori solver forward-error bound; the cap reserves only a small part of the existing economic tolerance. There is no configurable threshold search, security-specific threshold, currency threshold, or inference from later missingness.

Process each canonical coordinate in this fixed priority order:

1. A decision-ineligible or unavailable-for-entry risk coordinate is fixed to zero and represented as Close. Held eligible risk-unready coordinates still fail before representation. This mandatory transformation is measured separately under the existing propagated solver-row allowance.
2. Otherwise, `abs(w_i) <= eta_i` becomes Close, including any tiny already-held residual.
3. Otherwise, `abs(w_i-h_i) <= eta_i` becomes HoldCurrent.
4. Otherwise, use TargetWeight with the original `w_i`.

The decisions use no mark availability test. Held mark validation is mandatory before the rule, including for a later Close. An unheld Close needs no entry mark. A retained nonzero TargetWeight requires its observed execution mark during sizing; a missing mark fails instead of triggering a different intent. No later bar is visible to this boundary.

For ordinary transformed coordinates, sum `abs(represented_request_i-w_i)` and reject if it exceeds `B`. Mandatory fixed-zero lifting has its own `fixed_zero_l1_change`, rather than being silently charged against or exempted within a combined ordinary budget. No normalization, balancing trade, additional solver iteration, or relaxed limit follows a failed representation.

In exact arithmetic, the policy alone changes each ordinary coordinate by at most `eta_i`, so its full L1 change is at most `B`. Net, gross, maximum-name magnitude, and full-L1 turnover are all Lipschitz in that L1 change. Fees change by at most `c*B*pretrade_NAV` before unit-sizing effects. For a feasible gross bound and fee reserve, the usual gross-ratio perturbation is bounded by `B*(1+c*G)/(r-c*B)`. These inequalities explain the small normalized budget and fee amplification. They do not replace checking finite binary64 arithmetic, accumulation, actual unit sizing, or final feasibility. The final economic certificate remains authoritative.

Ordinary Weight unit sizing may itself round. Record the complete `sum(abs(actual_marked_i/pretrade_NAV-w_i))` separately as `actual_representation_l1_change`. This includes mandatory lifting and sizing roundtrip; it is not mislabeled as the narrower ordinary-intent budget. Its economic effects must pass the same independent checks as all other transformations.

## Interfaces and operation order

The engine owns these reusable execution primitives in `atx/engine/book/replay.hpp`:

```cpp
enum class ReplayTargetAction : u8 { TargetWeight, HoldCurrent, Close };
struct ReplayTargetIntent { ReplayTargetAction action; f64 weight; };
struct ReplaySizedTarget {
    f64 tri_units, marked_dollars, dollar_delta, resolved_weight;
};
Result<ReplaySizedTarget> resolve_replay_target(
    const ReplayTargetIntent&, f64 current_units, f64 current_marked_dollars,
    f64 current_mark, f64 pretrade_nav, bool decision_eligible);
```

Hold and Close have finite zero payloads. Hold copies the original units and marked dollars exactly and returns a positive-zero dollar delta. Its resolved weight is diagnostic only. Close returns zero units/value and trades the full negative current value. Weight retains the existing operation order `(weight*NAV)/mark`, followed by `units*mark`. Nonzero resulting positions require original decision eligibility; every held starting position requires an observed positive mark and consistent units times mark. The primitive is pure and allocates no success-path workspace.

The implementation layer adds `EquityAllocationRepresentation::{ExactWeights, MachinePrecisionIntentsV1}` to the frozen allocation config, defaulting to ExactWeights. `EquityAllocationResult` retains `weights`, adds `continuous_weights` and `intents`, and keeps the full canonical union. Existing weights mean resolved requested weights; replay must consume the intents to preserve exact Hold units.

`allocate_equity_preference` still solves through the existing factor-model/QP path with the same memory guard, iteration count and residual gates. It retains the raw full canonical vector, forms intents before requiring proposed entry marks, then resolves and certifies them. The shared pure `represent_equity_allocation(decision, execution, continuous_weights)` boundary allows an independently supplied continuous vector and performs the same state, shape, budget, sizing and economic checks. It never runs the QP: `solver_used=false` and no solver-optimality claim is available from that call.

Order is explicit:

1. Validate frozen configuration/risk cutoffs and all held marks, reconcile pretrade cash/equity/NAV, and admit the required memory bound.
2. For allocation, solve the existing QP and validate its finite raw residual diagnostics; preserve raw weights unchanged.
3. Apply mandatory lifting and the selected representation policy; check its ordinary aggregate budget.
4. Resolve every intent through the same engine primitive used by replay.
5. Recompute cash, actual trade dollars, fees, NAV, normalized exposures, full-L1 actual turnover and fixed-zero requirements. Reject any failure of the original limits or conservation checks.
6. Return raw vector/diagnostics and represented instructions/economic certificate together. Replay executes those same instructions.

The result arrays fit inside the existing `256*N` canonical-array admission allowance; a compile-time check bounds each intent at 16 bytes. The per-name resolver adds no full-vector workspace. This remains an allocation admission policy, not a process RSS guarantee.

## Certificate and provenance

The raw `QpCertificate` applies only to `continuous_weights`; it does not certify the represented candidate's KKT conditions or optimality. Report:

- policy enum/version, frozen financial and solver controls, raw canonical weights and all intent actions/payloads;
- ordinary Close/Hold counts, maximum applied `eta`, ordinary L1 change and budget, separate mandatory fixed-zero change;
- complete raw-to-actual normalized L1 change, including unit sizing;
- unchanged original-unit/requested and actual post-fee net/gross/name/turnover/conservation checks;
- continuous objective, actual represented objective and stable signed objective change.

For the unchanged objective `f(w)=0.5*sum((w_i-a_i)^2)+lambda*sum(v_i*w_i^2)`, compute the objective change directly with `d_i=actual_i-w_i`:

`sum(d_i * ((w_i-a_i) + 0.5*d_i + lambda*v_i*(actual_i+w_i)))`.

Use a compensated signed sum and finite checks, rather than subtracting nearly identical objective totals. The legacy `objective` field retains requested-weight semantics; `represented_objective` uses actual marked-dollar weights. A nonzero objective gap is a measured policy/sizing effect, not a duality gap. Negative gaps are possible for a supplied approximate point; neither sign establishes global optimality.

The stage explicitly selects the new mode and includes it, its fixed formula, intent payloads and certificate scope in attempt identity/reporting. A new attempt uses a new output location. Old failed attempts and frozen validation receipts remain immutable. Subsequent missing held marks still fail for every position that actually exists after representation. No change here authenticates source prices, issuer identity, corporate-action coverage, borrow availability, or market capacity.

## Bounded analytical validation

Three helper fixtures are added, alongside the existing five solver-backed allocation fixtures:

1. Tiny symmetric continuous weights become exact Close before unused absent marks are required; nontrivial entries with those same missing marks still fail. ExactWeights preserves the previous requirement. A solver-backed zero-preference call confirms the same integrated path. The direct representation call explicitly has no solver certificate.
2. A binary64 holding for which an ordinary weight roundtrip changes a unit bit is retained bit-for-bit by Hold with zero trade/fee. Mandatory Close overrides Hold, with its own change measurement. A missing held mark rejects both actions.
3. A net/turnover-infeasible represented book fails the original economic certificate, a feasible ordinary target passes that gate, and an underflowed representation budget rejects rather than expanding the tolerance. Input holdings and cash remain unchanged on failure.

The engine's separate intent fixtures cover execution of retained units and the unchanged older APIs. Root-owned native tests and a fresh full-window attempt supply implementation evidence; this document makes no assertion that those checks have passed. The independent review is in [the representation review](2026-09-20-allocation-representation-review.md).

## Read-only engine integration review

The implementation in `book/replay.cpp` and its public header were reviewed after the helper sources were released for the root-owned build. No blocker was identified in the following scope:

- Hold returns the original unit/value fields and a positive-zero delta. Application copies those fields; zero delta produces no trade record or cash debit. Its diagnostic resolved weight is never used to resize the holding.
- Close checks the current held mark before cancellation, sets zero holdings, and supplies the full negative old marked value as the trade. A missing held mark cannot be bypassed by either Hold or Close.
- `allocate_target` refreshes the original decision eligibility in the borrowed state span before invoking the callback. The intent resolver enforces that eligibility on every nonzero resulting Hold/Weight; Close is permitted for mandatory exits. The old weight APIs retain their existing upfront eligibility validation.
- The state exposes original decision preferences/eligibility and current holdings/marks, without a panel or future rows. The documented immutable-input requirement still applies to any external objects captured by callbacks.
- The intent path sizes into the same application loop and applies the same fee, borrow, marking, and conservation flow. An invalid later instruction can affect only local unpublished replay state; the whole call then returns an error. Callback side effects, such as retained proposal diagnostics, remain explicitly outside transactional rollback.
- The older APIs synthesize TargetWeight instructions and use their original sizing operation order. Their observable numerical equivalence remains a native regression-test obligation, not a conclusion proved solely by this source review.

## Scope after this slice

This creates a reusable distinction between desired continuous exposure and executable research-book instructions. It does not produce an economically optimized no-trade band, add share-lot/broker routing semantics, or resolve an identified terminal cash event. The separate [terminal-cash slice](2026-09-20-iteration10-terminal-cash-slice.md) still requires admissible basis/identity/timing evidence; GNW and MA's source gaps are not reclassified as corporate actions. Future work may add a verified active-face contract or a fully specified broker minimum-order model, each with its own causal inputs, representation objective and independent validation.
