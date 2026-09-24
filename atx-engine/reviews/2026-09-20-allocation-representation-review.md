# Allocation representation review, 2026-09-20

Recommendation: use explicit `TargetWeight`, `HoldCurrent`, and `Close` instructions, with an opt-in, versioned machine-precision representation policy and complete economic recertification. This is a useful implementation boundary for later execution policies. It does not establish an economic minimum trade, broker share-lot feasibility, an optimal no-trade region, or investment performance.

This research/design review was performed in `C:/atx/.worktrees/equity-platform`. It changes no engine/implementation source, frozen receipt, native output, or source panel, and reports no new build or test result. Concurrent implementation requires its own source review and validation.

## Evidence motivating the boundary

The frozen [checkpoint 9 receipt](2026-09-20-qp-polish-validation.json), SHA-256 `f01564db748ad5e64a1f58a1dd9bf9a98471fd369cf107d0b8def669783e7fc1`, records one independently checked allocation followed by a required-close failure at evaluation period 6, 2013-04-12, canonical instrument 604 / vendor security ID `150340` (archive annotation GNW). There is no completed performance result.

The [support diagnostic](../../build-equity/audits/iteration9-support-diagnostic.json) compares 1,661 canonical names: native output has 973 nonzero weights, while the independently solved startup oracle has 381. The other 592 native nonzeros have maximum absolute weight `1.5935755240764225e-27` and total absolute weight approximately `3.0443545196300966e-25`. GNW's native weight is `1.051474416021963e-27`; the oracle assigns exact zero. These are diagnostic observations, never a runtime security allowlist or permission to delete existing holdings.

The original baseline held a material GNW position: first target weight `0.0007039690445176616`, approximately $70,396.90 at the $100 million initial execution and $74,264.46 at its last valid period-5 mark. Those values come from the unchanged baseline books and evaluation panels using the independent APNL decoder. Original books artifact ID: `615019e7c83c34adc950bf41bda2e28923960249fbef0fccd8b84e14c251eb88`; payload SHA-256: `d47cf12586bd3e86f09885a23b5edf0155fb3c769ad68c8af5c02be6713f8960`. Numerical representation cannot resolve that original substantive holding or the unresolved GNW/MA source gaps.

## What the primary sources support

[Cvxportfolio 1.5.0's simulator](https://www.cvxportfolio.com/en/1.5.0/simulator.html) treats integer-share rounding and rejecting trades below an absolute cash amount as distinct execution settings. This supports separating target construction from executable instructions; it is not evidence that independent rounding preserves portfolio constraints. ATX currently holds total-return-index units, not actual shares. Rounding those units as share lots would mix accounting bases.

[MOSEK's Portfolio Optimization Cookbook, sections 6.1, 6.2 and 6.5](https://docs.mosek.com/portfolio-cookbook/transaction.html) includes transaction costs in the self-financing budget, distinguishes proportional from nonconvex fixed costs, and models a minimum nonzero trade as a semi-continuous choice: zero or an amount within declared lower/upper bounds. Its mixed-integer construction is materially different from ignoring any small resulting position. Its leverage example also includes costs in the leverage calculation.

[Skaf and Boyd, section 4.2](https://stanford.edu/~boyd/papers/pdf/dyn_port_opt.pdf), derive a no-trade zone from zero-trade feasibility and marginal-value/transaction-cost optimality conditions. A numerical proximity test has no corresponding economic optimality claim. ATX's hard turnover budget can produce no-trade coordinates through its constraint multiplier, but that alone does not calibrate trading costs, alpha decay, capacity, or an economically optimal holding period.

## Explicit instructions preserve accounting meaning

Let `V` be pretrade NAV, `q` carried units, `I` the current valid mark, and `h` their already computed marked dollars.

| Instruction | Units/value and actual trade | Required interpretation |
| --- | --- | --- |
| `TargetWeight(w)` | Preserve the existing operation order: `desired = w*V`, `q_new = desired/I`, `h_new = q_new*I`, `trade = h_new-h`. | Certify actual representable values; requested dollars alone are insufficient. |
| `HoldCurrent` | Copy `q` and `h` exactly; trade is canonical positive zero. | Rebuilding `h/V` and resizing it can create rounding trades, so a weight-only approximation cannot guarantee Hold. |
| `Close` | Set units/value to canonical positive zero; trade is `-h`. | Closing a material position is a real transaction with fees and turnover, even when the requested final target is numerically tiny. |

All held positions require valid current marks before the allocation callback, including positions that a later intent would close. A held missing mark must fail before any intent can rescue it. A flat Close/Hold needs no unused mark. A nonzero resulting position still needs its current mark and original decision eligibility. Mandatory exits precede Hold; existing held-position risk-readiness checks also remain in force.

## Bounded machine-precision policy

The proposed rule is defensible as a numerical representation contract. Define binary64 epsilon `eps`, original feasibility tolerance `tol`, full-L1 turnover limit `T`, trade cost rate `c`, gross/name limits `G`/`B`, canonical count `N`, and positive conservative fee reserve `r = 1-c*T`:

```text
aggregate_budget = tol*r / (128*(1+c*(1+G+B)))
eta_i = min(64*eps*max(1, abs(raw_i), abs(previous_i)), aggregate_budget/N)
```

Use causal raw targets and current marked holdings only. Apply declared mandatory Close first; then `abs(raw_i) <= eta_i` means Close; otherwise `abs(raw_i-previous_i) <= eta_i` means Hold; otherwise retain TargetWeight. Decide instructions before checking proposed-entry marks. Selection must not inspect source-gap masks, future data, ticker identities, native failures, or subsequent P&L. The available current mark may be used for holding valuation and execution sizing, never to selectively suppress an otherwise identical proposed entry.

For 1,661 names with absolute weights below one, the epsilon cap permits at most about `2.36e-11` total weight correction, roughly $0.00236 at $100 million NAV, before the additional aggregate cap. This bounds a representation perturbation; it does not prove feasibility. A raw candidate can already lie on the original tolerance boundary.

Freeze constants, exact `<=` tie behavior, priority, precision model and canonical-count definition in the recipe identity. Check finite arithmetic and positive budget inputs; reject budget underflow or explicitly define zero allowance without snapping nonzero values. Keep mandatory-zero corrections separate from optional representation changes. Preserve raw targets and record action counts, maximum eta, raw-to-intent L1, raw-to-actual represented-weight L1 including ordinary Weight roundtrips, and the resulting objective difference. A raw solver KKT certificate does not certify optimality of the changed candidate. Mathematical exact-zero support would require a separate active-set/dual certificate for the complete problem; magnitude alone is insufficient.

## Recertification and minimal next slice

One pure intent resolver should be shared by certification and replay application, so both use identical units, deltas, fees and operation order. Apply no mutation until the whole candidate passes. Preserve the existing weight-only behavior through its adapter; make the new representation mode explicit.

After sizing actual units, reconstruct cash and positive NAV, actual dollar full-L1 turnover, post-fee net/gross/name exposures, eligibility and risk readiness, and every configured coupling constraint. Keep the original `1e-8` economic tolerance and current full-L1 convention. Bound optional correction L1 explicitly; record any additional ordinary floating-point sizing difference. A small correction budget never substitutes for these checks. Suppressing one small trade can break net/sector/beta balance; suppressing a reduction can retain a name-limit breach; fees change exposure denominators. If recertification fails, fail closed with evidence. Do not adjust thresholds or retry until a known missing-mark failure disappears.

The minimal vertical slice is explicit intents, shared pure sizing, opt-in bounded representation, and proposal evidence containing raw target, chosen action and actual resulting economic amounts. Three compact analytic fixture families are enough initially: exact Hold plus fee-bearing material Close; a coupled-constraint boundary that rejects an invalid represented book; and held-mark failure before callback together with a causally suppressed flat numerical entry. A future-data perturbation must not change an earlier intent. Then run the same unchanged original data window with original fees, borrow and fresh-attempt provenance; retain any later required-mark failure and avoid performance claims without completion.

Economic minimum-dollar trades, signal-aware no-trade regions, actual shares/lot sizing, and general terminal-event integration are subsequent policies on this boundary. They should be chosen using economic evidence and declared before evaluation. This slice supplies precise execution semantics needed by a low-turnover platform; it does not itself produce low-turnover alpha or resolve source correctness, borrow availability, impact or capacity.

## Iteration 10 observed outcome

After the coordinating agent released the completed native process, the new independent intent checker exited 0 and wrote [its verification receipt](../../build-equity/audits/iteration10-allocation-proposal-verification.json), SHA-256 `e5eaf851f4d94309d37246d6a567040dfd913bb3eaf9c33c03776e1b5cf49a7d`. It verified one proposal: 973 raw nonzero coordinates became 381 represented positions, with 1,280 Close instructions and no Holds. Ordinary representation L1 was `3.0443545196300966e-25`, within the declared `7.803875854765961e-11` budget. Including ordinary unit-sizing roundtrip, actual raw-to-represented L1 was about `6.8152273980435525e-18`; the independently reconstructed stable objective change was about `4.9785594239900515e-22`. Tiny-metric comparisons used magnitude-scaled allowances rather than a generic absolute floor. Eligibility, risk history, fee/borrow accounting and original economic limits passed. All 63 hashed inputs, selected sources and executable snapshots remained unchanged.

The checker completed six observable intervals and stopped at the unrecorded next proposal, execution period 6. GNW was exactly flat throughout that carry, so its missing 2013-04-12 mark no longer caused a held-position failure. Native execution instead reported `equity allocation: candidate intent failed instrument=604: replay: missing/nonpositive required close` from the next allocation callback. No second raw candidate or certified proposal was retained; its choice of entry cannot be independently reconstructed from that message.

[Fresh boundary notes](../../build-equity/audits/iteration10-observed-boundary-notes.json), SHA-256 `e02610dda4bfb854b184a7c9d5566a9db974090c2554ae80e1fba069af3f6516`, verify instrument 604 maps to vendor ID `150340`, that it was eligible at original decision period 5, and that its period-6 execution close is NaN. They clarify the verifier's generic `solver/internal` fallback wording: this specific native message describes an uncertified candidate-mark failure, and `native_failure_independently_reproduced=false` remains correct. The independent oracle infers no unpublished second target and computes no later intervals. Both original and new native attempts remain failed, with no completed performance result or source repair.
