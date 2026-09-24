# Iteration 10: bounded terminal-cash accounting slice

Independent review of [the terminal-event design](2026-09-20-terminal-cash-event-design.md). Recommendation: build two pure transitions, then one opt-in replay seam exercised with synthetic contracts. Keep the existing no-event path unchanged. This develops reusable accounting without admitting any researched historical candidate.

## First deliverable: signed claims and retirement

Add `plan_terminal_cash_transition` and `plan_claim_payment` in one engine module. Inputs are typed, immutable records; functions perform no file or network I/O and return a fully validated delta before mutation. Restrict the contract to unconditional, single-component USD consideration, no additional dividend, split, election, withholding, successor shares, or uncertain amount.

For a certified observed anchor, calculate signed raw-share equivalent `s = q * I / P`, signed claim `R = s * K`, and event bridge `R - q * I`. Cancel equity units, retire the instrument, and preserve settled cash. On a later evidenced full payment, move the signed amount from claim to cash with zero P&L. Zero held quantity still retires the instrument, without creating a nonzero claim. Reject repeated event/payment IDs, wrong state versions or security/axis IDs, unknown component coverage, nonfinite or unrepresentable amounts, conflicting revisions, and payment before claim creation. Preserve gross receivables and payables separately from their net.

Carry pending fixed USD claims at face only under an explicit valuation convention; an unknown payment time leaves them outstanding. Evidence references, basis coverage and application mode are explicit inputs, not a generic `verified=true` switch. Synthetic fixture evidence belongs to a separate namespace/mode and can never satisfy real-event admission. A successful numerical transition does not authenticate the issuer mapping or establish broker ownership.

## Second deliverable: one replay boundary hook

Use the same strict replay accounting with an empty schedule by default. Add a bounded event/claim ledger, execution-time retirement mask, net claims in NAV, and separate event/settlement certificates. Process an admitted cancellation before the next required equity mark. A prior unresolved held mark still fails; no price is filled. Reject a delayed nonzero target in a retired instrument rather than silently rewriting it. Claim settlement remains a mandatory non-trade movement, including at the final valuation, with no trade fee or exchange turnover.

Initially accept only events assigned to an explicit observation-boundary phase by the supplied contract. Synthetic fixtures may declare that phase. Real records additionally need the design's proven timing and availability evidence; session labels alone cannot supply it. Avoid adding an exchange-calendar service or general intraday scheduler in this slice.

Short financing is the main integration trap. Pure transitions support either sign. The first replay profile may support short cancellation only under an explicit boundary convention: the stock-loan obligation ends at that boundary and pending-payable financing is separately declared. Whole-interval borrow then uses only the post-boundary remaining short equity. Reject other short-loan timing instead of silently dropping fees or introducing guessed intraday prorations. This convention is a research model, not broker evidence.

Expose settled cash and claim value separately to callbacks. The current allocator's `cash + equity == NAV` check must include claims before it can use this path; cash availability must not treat claims as settled cash. If that extension is deferred, the initial integration should expose only the fixed-target replay entry point and reject policy callbacks with nonzero claims. Do not hide claims inside the existing cash field.

## Acceptance and exclusions

Use three analytical fixtures: signed cancellation followed by payment; retirement before an absent next mark versus an earlier unresolved gap; duplicate/basis/availability rejection with unchanged state. Include normalization invariance, final-date payment, and boundary short financing within those fixtures. Bound all event/claim allocations and bind their canonical inputs to the result identity.

Publish an application-admissibility summary showing HNZ, DELL and MOLX still rejected for missing identity/basis/timing or component-coverage evidence. Payment admission is a separate gate. Dell also requires a separate entitlement path: its filing distinguishes merger consideration from the special dividend tied to earlier holdings. [Dell 2013 8-K, item 2.01](https://www.sec.gov/Archives/edgar/data/826083/000119312513416110/d619138d8k.htm). Declared payment terms cannot substitute for actual allocation and book cash evidence; DTCC separately reports payment allocations. [DTC Allocation Date Service](https://www.dtcc.com/products-and-services/data-services/corporate-actions-reference-data/dtc-allocation-date-service).

GNW/MA's missing bars remain unresolved. This slice cannot complete the failed native window or establish P&L, alpha quality, or capacity. No source panels, failed attempts, or frozen receipts are changed. No implementation, native build, or test execution was performed for this review.
