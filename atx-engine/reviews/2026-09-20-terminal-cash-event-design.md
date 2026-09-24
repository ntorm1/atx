# Identified terminal cash events for TRI replay - 2026-09-20

Design only. Add a narrow, opt-in accounting seam that replaces canceled equity exposure with an identified cash receivable or payable. Do not create replacement OHLC observations, liquidate at a carried price, or classify an absent row as a corporate event. The existing no-event replay remains strict.

The [required-mark audit](2026-09-20-equity-required-marks-audit.md) establishes a need for this seam, but does not yet supply the complete identity, unit-basis, availability, entitlement and settlement evidence needed to apply a real event. In particular, GNW/vendor `150340` and MA/vendor `351548` have observations on both sides of their missing 2013-04-12 session. Neither becomes a terminal cash event. The first failed training book remains failed; this design makes no performance claim.

## Existing seams and their limits

[`book/replay.hpp`](../include/atx/engine/book/replay.hpp) and [`book/replay.cpp`](../src/book/replay.cpp) hold signed total-return-index units `q`, value them at `q * close`, and keep cash separately. Dividends are implicitly reinvested. Every nonzero holding needs a valid observed mark at each required valuation. The allocation callback receives current marked holdings and NAV, while preferences and eligibility belong to the original decision row. A missing required mark rejects the complete replay.

[`data/history_panel.cpp`](../src/data/history_panel.cpp) constructs the source's adjusted price pointwise as `I = P * F`, using raw price `P` and `cumulReturnFactor` `F`. This factor is not asserted to be a pure split factor; raw traded volume remains raw. The source adapter's zero cash-dividend column means dividends are expected to be embedded, not that independent event coverage has been verified. The split-factor-plus-cash-dividend path in [`data/adjust.cpp`](../src/data/adjust.cpp) has different input semantics. Feeding an already total-return-adjusted factor into that path and adding dividends again would duplicate them.

[`data/corporate_actions.hpp`](../include/atx/engine/data/corporate_actions.hpp) and its [implementation](../src/data/corporate_actions.cpp) expose a six-column reference dataset. They lack identified terminal events, revision history, publication/availability clocks, beneficial entitlement snapshots and outstanding claims. Corporate-action values join by event date without a publication guard; missing dividend values default to zero. Those conventions cannot establish the evidence required here. Issuer shares outstanding are never a substitute for the book's own share quantity.

## Evidence required before application

Each event must bind an immutable event ID and revision to the **exact vendor security ID**, source/axis digests, issuer and share class, currency, effective identity interval, and externally supported security identifiers. Keep the historical ticker as an annotation. The archive's observation-time ticker alone is not an attestation that a vendor ID is the issuer/security in a filing; a current ticker is even less sufficient. Store the mapping evidence, its provenance and its review status separately from event terms.

Retain the issuer/exchange/depository source URL, publication date/time and precision, a captured-content digest, source event ID when available, extracted terms and section reference, extraction version, and availability evidence. A downloaded filing's content hash binds what was read; it does not prove when the strategy could have read it. Revision selection must be deterministic and as-of. A later cancellation or correction requires a fresh replay from the appropriate checkpoint, not an unexplained compensating cash entry.

The known candidates illustrate three different treatments:

| Archive annotation | Primary event terms | Additional application gate |
| --- | --- | --- |
| HNZ / `37648` | Eligible common shares converted into rights to USD 72.50 on June 7, 2013; trading was to halt after the close. The filing is dated June 13. | Verify vendor ID/share class, event ordering relative to the final observation, adjustment basis and cash allocation. June 13 publication cannot be silently backdated to June 7. [Heinz 8-K, items 3.01-3.03](https://www.sec.gov/Archives/edgar/data/46640/000119312513258009/d555504d8k.htm). |
| DELL / `35715` | Eligible common shares converted into rights to USD 13.75 at the October 29 merger. A distinct USD 0.13 special dividend applied to shares held at the October 28 close, payable promptly after effectiveness. | Preserve two components and distinct entitlement clocks. Neither a USD 13.88 terminal price nor a dividend on later holdings is justified. Excluded/appraisal shares need separate treatment. [Dell 8-K, items 2.01 and 3.03](https://www.sec.gov/Archives/edgar/data/826083/000119312513416110/d619138d8k.htm). |
| MOLX / `39970` | December 9 merger consideration was USD 38.68, including a USD 0.18 adjustment to USD 38.50. Nasdaq trading ceased before that day's open. | The USD 0.18 is part of the consideration, not another dividend to add. Verify the particular share class/vendor mapping and payment evidence. [Molex 8-K, items 2.01 and 3.01](https://www.sec.gov/Archives/edgar/data/67472/000110465913088986/a13-25556_28k.htm). |

These are sourced issuer terms, not accepted event records. No mapped candidate is ready merely because the cash amount is known.

## Converting TRI units into the event's share basis

At a verified last observation `t`, let `P_t` be the raw per-share price, `I_t` the actual replay price, `F_t = I_t/P_t`, and `q_t` the replay units. Then:

```text
marked exposure h_t = q_t * I_t
economic raw-share equivalent s_t = h_t / P_t = q_t * F_t
```

This algebra supplies a quantity in the raw quote's share denomination. It does **not** prove actual settled beneficial ownership. TRI replay assumes reinvestment and permits fractional units; a production physical-share ledger must separately prove trades, settlements, stock loans and reinvestments. A research conversion may use a verified hypothetical reinvested-share basis, explicitly labeled as such. It must not present that basis as a broker holding or claim a legal entitlement from algebra alone.

A `TriShareBasisCertificate` must bind the exact observed raw/adjusted marks, source rows and adjustment recipe; reconcile their normalization to the held units; establish the event's per-share denomination; and cover intervening splits/distributions through cancellation. Positive finite prices/factors are necessary but insufficient evidence of correct adjustment coverage. Require an explicit evidence ledger stating which components were embedded and at which observations, with no unexplained basis discontinuity. Missing factor semantics, unverified mapping, unknown intervening action or ambiguous reinvestment/entitlement means **do not apply the event**.

If a verified split of ratio `r` occurs after the anchor and before cancellation, and has not already been reflected in the anchor's basis, the event-equivalent quantity is `s_event = s_t * r`. A split already represented in `F_t` must not be applied again. Avoid using a later revised factor to reinterpret old units without rebasing the full affected history. An alternate rescaling `I'=kI`, `q'=q/k` must leave the certified share equivalent and consideration unchanged.

The distinction is not peculiar to this archive: CRSP documents separate price/share adjustment factors and separate distribution declaration, ex, record and payment dates. It also distinguishes a delisting amount/return from ordinary price observations, and some return products already incorporate delisting returns. Those examples motivate an explicit coverage contract; they do not establish this vendor's semantics. [CRSP user guide, distributions/delisting fields and returns](https://www.crsp.org/wp-content/uploads/2023/08/CRSP10-User-Guide.pdf).

For each economic component use `EmbeddedInTri`, `ExcludedFromTri`, or `Unknown`, with supporting evidence. An ordinary dividend already embedded in TRI creates no additional cash leg in this replay. A dividend excluded from TRI requires verified entitlement and an accounting path that does not also reinvest it in a subsequent TRI mark. Unknown or partially embedded treatment is rejected. Terminal consideration must be excluded from the anchor's prior accounting; if a vendor has already encoded a terminal return or cash distribution, this simple adapter must reject or use a separately proven reconciliation, not count both.

DELL is therefore outside the first accepted real-event slice until the separate dividend's coverage and entitlement are proved. Record-date holdings, later cancellation holdings and payment-date holdings need not agree. The engine must never infer one from another or add a presumed dividend to conceal a price discrepancy.

## Clocks, claims and ordering

Represent these independently: public announcement/publication, vendor availability, local ingestion, last tradable instant, legal cancellation/effectiveness, entitlement cutoff, record date, ex-date/due-bill interval where relevant, declared payment date, and actual allocation/settlement time. Preserve unknown values and time precision; do not fabricate midnight timestamps from date-only sources.

Record-date ownership alone is not always enough to establish the ultimate beneficiary; ex-date and due-bill rules can transfer distributions. Apply the rules in force at the event, with security-specific evidence. FINRA's 2017 notice records the T+3 to T+2 transition and related ex-date changes effective September 5, 2017, so current settlement rules cannot be applied retrospectively to these 2013 events. [FINRA Notice 17-19](https://www.finra.org/rules-guidance/notices/17-19), [FINRA distribution/entitlement FAQ](https://www.finra.org/filing-reporting/market-transparency-reporting/uniform-practice-code-upc/faq).

Strict as-of replay requires the applicable identity, terms and status revision to be available by its application cutoff. An announcement of a proposed deal is not confirmation that cancellation occurred. Unknown or later availability fails this mode. A separately labeled retrospective accounting reconstruction may use subsequently verified historical facts, but its manifest must identify that hindsight and must not expose it to earlier signals, eligibility or optimization decisions.

Daily session labels currently do not establish intraday ordering. The first adapter should accept only events mapped by evidence to an unambiguous before-open/after-close phase on an identified exchange calendar; reject ambiguous ordering. Distinguish the last observed tradable mark from cancellation: cessation of trading does not alone prove legal cancellation. Process identified events between valuation endpoints **before** requiring the next equity mark. Waiting for the next rebalance is too late because current replay marks holdings at the end of every interval.

At effective cancellation, for signed certified shares `s` and consideration `K`:

```text
new signed claim R = s * K
old equity units -> zero; old security -> retired
settled cash unchanged
NAV = settled cash + remaining marked equity + net outstanding claims
event P&L bridge = R - previously accounted removed equity exposure
```

Use positive claims for longs and negative claims for shorts; preserve long receivables and short payables separately as well as their net. Cash received/paid only at verified allocation changes `cash += R`, `claim -= R`; this transfer has zero P&L absent separately identified fees, withholding, FX or valuation changes. A declared payment date is not proof that money became available: DTC distinguishes anticipated proceeds from actual intraday allocation information. [DTC allocation-date service](https://www.dtcc.com/products-and-services/data-services/corporate-actions-reference-data/dtc-allocation-date-service).

Pending fixed USD claims may be carried at face under an explicit research valuation convention. Face value is a model choice, not evidence of market fair value or immediate liquidity. Unknown payment dates leave claims outstanding; they never become spendable cash by elapsed time. A production cash-availability claim remains unsupported until settlement evidence exists. A payer default, conditional amount, interest entitlement or uncertain withholding is outside the first profile.

Short distributions are obligations, not positive receipts. A broker describes the borrower's payment-in-lieu obligation for cash distributions, but an event-specific stock-loan closeout still needs its own evidence. [Interactive Brokers payment-in-lieu description](https://www.interactivebrokers.com/campus/glossary-terms/payment-in-lieu-of-dividends/). Do not assume borrow fees stop merely at the last traded price. Record an explicit loan-obligation end time or an identified research convention, split financing intervals at that time, and distinguish a pending cash payable's financing from an outstanding stock loan. The current whole-interval post-trade-short-dollar borrow calculation cannot be reused blindly across cancellation.

A conversion is a mandatory non-trade movement. Record its proceeds and P&L separately from exchange dollar turnover, trade fees and participation. Do not manufacture an exit fill. A retired instrument cannot be reopened by an older delayed preference: expose execution-time tradability separately from frozen decision eligibility and require an exact zero target for retired instruments. Mandatory claim/payment processing may occur at the final valuation; that does not authorize a terminal discretionary trade.

The last observed anchor is retained as an accounting basis only, never emitted as an intervening price. If another required valuation lies between that anchor and cancellation and lacks a mark, replay still fails. Later terminal terms do not repair an earlier missing trading session.

## Proposed bounded engine interfaces

Introduce typed records in a new terminal-event module, leaving the existing six-column corporate-action dataset unchanged. IDs and timestamps must remain integer/string types, not floating-point dataset columns. The names below describe a concrete API seam, not committed implementation:

```cpp
namespace data {
struct TerminalCashEvent;        // Immutable ID/revision, vendor ID, exact class,
                                // USD amount, clocks, terms/mapping evidence IDs.
struct TriShareBasisCertificate; // Exact anchor + source/axis/recipe digests,
                                // share denomination and component coverage.
struct EntitlementSnapshot;      // Component ID, cutoff, signed entitled quantity,
                                // ownership/reinvestment mode and evidence IDs.
struct CashAllocationEvent;      // Claim/event ID, actual signed amount/time,
                                // evidence/revision; independent of declared date.
}
namespace book {
struct TerminalHoldingView;      // Instrument, signed TRI units, observed anchor,
                                // accounting state version; no borrowed future row.
struct PendingCashClaim;         // Signed USD amount/value, event/component ID,
                                // entitlement/basis IDs, unsettled status.
struct TerminalCashTransition;   // Retired units, new claims, non-trade P&L bridge,
                                // reconciliation certificate and evidence IDs.

Result<TerminalCashTransition> plan_terminal_cash_transition(
    const data::TerminalCashEvent&,
    const data::TriShareBasisCertificate&,
    const TerminalHoldingView&,
    const EventApplicationContext&); // Mode, cutoff, phase, approved conventions.

Result<ClaimPaymentTransition> plan_claim_payment(
    const PendingCashClaim&, const data::CashAllocationEvent&,
    const EventApplicationContext&);

Result<ReplayCashEventResult> replay_identified_cash_events(
    const ReplayInputs&, const IdentifiedCashEventSchedule&,
    const ReplayAllocationPolicy&, const CashEventReplayConfig&);
}
```

The adapter validates manifests/mappings and captures source evidence outside the pure engine; the engine performs no web/file I/O. The first transition handles a single unconditional mandatory USD cash cancellation, finite positive consideration, verified ordinary eligible holdings, and no unresolved additional components. General dividends, successor shares, voluntary elections, appraisal holdings, bankruptcy recoveries and fractional-share cash-in-lieu rules are deferred. Evidence objects must carry reviewed source references and validation results, not an unsubstantiated `verified=true` flag.

Use a canonical event order and an atomic validate/prepare/apply batch. Reject duplicate event/component/payment IDs, conflicting revisions, wrong state versions, payments preceding claim creation, nonfinite amounts and inconsistent sums. Failures return no partial accepted replay. Bound event/claim counts and bytes with checked arithmetic before allocations. Keep event certificates, outstanding claims and unresolved evidence in the result and include stable event/recipe/input identities in the report digest.

Extend the callback/accounting state explicitly with settled cash, gross receivables, gross payables, net claim value and execution tradability. The current implementation allocator checks `cash + marked equity == NAV`; it must change to include claims with a stated valuation policy. Passing unsettled claims as cash would hide the mismatch. The allocation's pretrade-NAV denominator includes claim value, while any cash-availability/margin policy must use settled cash and broker-eligible collateral separately. This seam alone does not establish borrowing availability or margin capacity.

## Minimal analytical fixtures and next iteration

1. **Signed cancellation and later payment.** With `q=2`, `P=10`, `F=3`, `I=30`, old exposure is 60 and certified shares are 6. At `K=12`, a claim of 72 replaces the equity and the event bridge is +12; cash is unchanged until payment. With `q=-2`, exposure is -60, payable is -72 and bridge is -12. Payment changes cash/claims oppositely and leaves NAV unchanged. Distinct final-date mandatory payment still creates no discretionary trade.
2. **Share basis and duplicate components.** A verified post-anchor 2-for-1 split and `K=6` still give 72. Applying that ratio twice fails evidence reconciliation. An ordinary dividend already embedded in TRI creates no new claim; unknown coverage or a duplicate component fails. A DELL-shaped separate dividend with different entitlement/cancellation holdings cannot use later holdings. A MOLX-shaped inclusive consideration gets no extra 0.18 leg.
3. **Required marks and ordering.** A supported cancellation before the first absent valuation retires the old units before the mark requirement. An earlier unresolved held session still fails even if cancellation happens later. GNW/MA-shaped gaps with subsequent observations and no event evidence remain failures. Before-open cancellation blocks an old delayed nonzero target; verified after-close ordering preserves only genuinely prior execution.
4. **Availability, mapping and normalization.** Wrong vendor ID/share class, insufficient basis coverage, ambiguous phase or future source availability fails strict mode without mutation. A labeled retrospective run cannot inject later facts into earlier decision inputs. Consistent TRI normalization changes leave share-equivalent proceeds invariant.
5. **Claims and reconciliation.** Unknown payment time keeps a claim outstanding and settled cash unchanged; guessed dates and duplicate payment IDs fail. NAV, removed exposure, event P&L, claims and eventual cash reconcile independently, with conversion excluded from external trade turnover. A short-loan interval crossing cancellation uses its explicit financing convention and does not silently drop accrued fees.

The next implementation should deliver these pure transitions and one opt-in replay path against analytical fixtures, plus an adapter that reports why each researched real candidate is or is not admissible. Acceptance requires identity/basis/time evidence, exact accounting reconciliation and fail-closed missing-mark behavior. Applying HNZ/DELL/MOLX requires the missing evidence first; broadening to successor securities or interpreting a completed replay as validated alpha performance is outside this iteration. No source archive, accepted price panel or failed baseline should be rewritten to make these events fit.

Primary sources were reviewed on 2026-09-20. Their historical event dates establish terms, not historical ingestion availability or this vendor's adjustment semantics.
