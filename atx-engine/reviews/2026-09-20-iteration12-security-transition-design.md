# Iteration 12: signed stock-and-cash security transitions

Design only. Build a pure, atomic transition planner for one predecessor security, one successor security and one unconditional USD cash component. This extends the proposed [terminal-cash seam](2026-09-20-terminal-cash-event-design.md) and its [bounded first slice](2026-09-20-iteration10-terminal-cash-slice.md); neither proposal is an implemented event ledger. Initial work should exercise synthetic contracts without connecting historical events to native replay.

Iteration 11 produced four certified callback proposals and then failed at evaluation period 19, May 1, 2013, because held archive security `146189`, canonical index `1063`, annotated `PCS`, lacked a required mark. The [qualified diagnostic](../../build-equity/audits/iteration11-held-gap-diagnostic-qualified.json) records `-1888.5372719122356` TRI units and April 30 marked value `-22360.28129944087` at TRI mark `11.84`. These are research holdings, not authenticated physical shares. The measured short makes signed liabilities essential. Preserve that failed run, its full window, panels and receipts; this design supplies neither a repaired result nor a live execution claim.

## Primary evidence and its limits

| Evidence | Established fact | Scope limitation |
| --- | --- | --- |
| [Issuer closing 8-K, introduction](https://www.sec.gov/Archives/edgar/data/1283699/000119312513193449/d527693d8k.htm) | April 30 recapitalization converted each pre-split common share into half a common share. Cash was USD **4.0491 per pre-split share**, payable to holders of record immediately following effectiveness; the subsequent issuance to T-Mobile Holding followed the split and cash payment. The company continued as T-Mobile US, formerly MetroPCS. | This is a continuing equity exposure plus cash. The report's May 1 cover date is not an April 30 availability attestation or broker cash-allocation record. |
| [Restated certificate, articles IV(B) and XIII](https://www.sec.gov/Archives/edgar/data/1283699/000119312513193449/d527693dex31.htm) | The effective-time wording is **“4:01 p.m. Eastern Standard Time on April 30, 2013”**. Fractions are aggregated by holder and replaced by transfer-agent cash, rounded up to cents, subject to withholding. | Preserve the source timezone wording; do not silently normalize it to a guessed UTC instant. The cited provision supplies no fractional-share cash pricing formula. |
| [OCC memo 32600, April 30, hosted by MIAX](https://www.miaxglobal.com/sites/default/files/alert-files/PCS-TMUS_Memo.pdf) | Effective May 1, an adjusted old 100-share option delivers 50 new TMUS shares plus USD 404.91; new CUSIP **872590104**. Its basket quote is `0.5 * TMUS + 4.0491`. | Options adjustment corroborates the two components. `TMUS1` is the adjusted option symbol. The basket formula is not an observed PCS equity close, an equity fill, or proof of our stock-loan treatment. |
| [Issuer Form 8937, pages 1-3](https://s29.q4cdn.com/310188824/files/doc_downloads/merger_info_docs/1500056517.pdf) | MetroPCS common stock, ticker PCS, old CUSIP **591708102**; action date April 30. The signature is dated **June 14, 2013**. The attachment describes the split and cash as a recapitalization. | Visually inspected scanned pages. Supports retrospective identity/terms, not contemporaneous source availability or the vendor-ID mapping. Tax basis is distinct from the replay's investment-unit basis. |
| [Issuer completion announcement](https://www.t-mobile.com/news/press/t-mobile-and-metropcs-combination-complete-wireless-revolution) | TMUS was to start NYSE trading May 1. The page has an April 30 header and May 1 body dateline. | Preserve both date fields; neither proves capture, ingestion, or pre-decision availability. |

The [issuer merger-information hub](https://investor.t-mobile.com/resources/merger-information/default.aspx) is a discovery route; its separate Sprint material must not be confused with this MetroPCS event. The inspected Form 8937 download has SHA-256 `96be549ec0715d8b2e9d6fed8078a2f94d8449013b76c161c70b1f6fe1182c44`; that capture binds bytes read now, not historical availability.

The two CUSIPs establish external security labels. They do **not** authenticate vendor `146189` as the old class or identify the successor's vendor ID/axis position. Require a reviewed, time-bounded mapping on both sides, with source and axis hashes. Retire the predecessor representation, not the continuing issuer. If a vendor reuses an ID across the change, an identity epoch is needed; the first implementation should reject a same-axis transition rather than guess whether its series already performs the conversion.

## First slice: a pure transition and a separate payment

Use one typed ledger family with a narrow mixed-transition variant. Reuse the terminal design's evidence, claim and payment concepts; do not add an unrelated PCS-specific branch to replay. Suggested API names, not implemented declarations:

```cpp
namespace data {
struct SecurityTransition;     // Immutable event ID/revision and ordered components;
                               // mapped predecessor/successor, clocks, evidence refs.
struct TransitionBasis;        // Both TRI/share bases, component coverage,
                               // entitlement snapshot and source/axis identities.
struct CashClaimPayment;       // Identified actual allocation of a specific claim.
}
namespace book {
struct TransitionHoldingView;  // Borrowed starting quantities, observed marks,
                               // prior accounted value, state version, applied IDs.
struct TransitionAdmission;    // Mode, cutoffs, limits, valuation/unit convention.
struct SecurityTransitionPlan; // Owned fixed-size deltas and signed claim certificate.
struct CashClaimPaymentPlan;

Result<SecurityTransitionPlan> plan_security_transition(
    const data::SecurityTransition&, const data::TransitionBasis&,
    const TransitionHoldingView&, const TransitionAdmission&);
Result<CashClaimPaymentPlan> plan_cash_claim_payment(/* claim, payment, state */);
}
```

The first planner accepts exactly one positive rational stock ratio and one nonnegative USD-per-predecessor-share amount. Encode the researched terms as `1/2` and decimal `40491/10000`; denominator and share denomination are explicit. The cash amount applies to pre-split equivalent entitled shares, even though the source record-holder clock follows effectiveness. Applying it to the half-sized successor quantity would lose half the entitlement. Entitlement quantity and converted quantity remain distinct inputs and must reconcile under the declared contract.

Initially support an explicitly named **research reinvested-share-equivalent** unit convention and synthetic evidence namespace. It permits continuous signed quantities for accounting experiments; it cannot certify legal delivery, broker holdings or fractional cash-in-lieu. Real physical-share treatment requires holder aggregation, entitlement/settlement and stock-loan evidence, rounding, withholding and fractional proceeds; those are outside this slice. Unknown or partially applicable terms reject admission rather than being approximated by rounding holdings.

Given signed predecessor TRI units `q_old`, a certified last-accounted raw/adjusted pair `(P_old, I_old)`, and a certified observed successor pair `(P_new, I_new)` at the plan's valuation cutoff:

```text
s_converted = q_old * (I_old / P_old)
s_successor = s_converted * stock_ratio
R           = s_entitled_pre_split * cash_per_pre_split_share
q_added     = s_successor * (P_new / I_new)
q_new_after = q_new_before + q_added
q_old_after = 0
settled_cash_delta = 0
new_pending_cash_claim = R
```

Require finite positive certified marks and finite representable quantities. Use one documented arithmetic order shared by planner and certificate. Recompute actual successor value from final representable units; report its change relative to the existing successor units at the same observed mark. The synthetic expected value `s_successor * P_new` is a reconciliation target, not permission to insert a cash plug. Bound numerical residuals by a declared, versioned floating-point policy; reject overflow, a nonzero component disappearing through underflow/addition, or unexplained residual. Never dust away a short or a cash obligation.

The accounting bridge is the actual **incremental successor value plus signed claim minus removed predecessor accounted value**. If the old and new marks are different observations, this is a valuation-interval bridge containing intervening market movement; do not label it an instantaneous corporate-action profit. Net asset value includes settled cash, all marked equities and signed pending claims, with gross receivables/payables also retained. Pre-existing successor positions may be combined numerically only after preserving each transition's attribution; netting positions does not authenticate a stock-loan discharge.

Payment is a second identified transition: `cash += R`, `claim -= R`, with zero P&L for a verified full allocation. A negative claim reduces cash on payment. The first payment planner rejects mismatched, partial, duplicate, premature or unverified allocations; it does not infer settlement from an issuer statement that payment occurred. Carrying an unsettled fixed USD claim at face is an explicit research valuation convention and does not make it spendable cash.

Validate the whole plan before mutation: event revision/sequence and not-already-applied status; exact state version; mapped identities; both source/axis/basis bindings; cutoffs and supported boundary; component and entitlement consistency; finite signed deltas; required successor valuation; claim uniqueness; and the accounting reconciliation. A later commit must check the same state version. Any error yields no partial predecessor cancellation, successor holding or cash claim. The pure planner performs no I/O and cannot certify the truth of externally supplied evidence merely because its references are well formed.

## TRI coverage is an admission gate

The existing adapter uses `I = P * cumulReturnFactor`. Its factor may include reinvestment as well as share changes; issuer shares outstanding are not the book's shares. `q * I/P` is an algebraic raw-share equivalent, not proof of settled beneficial ownership. The predecessor basis must anchor before the components being applied, cover intervening actions and identify any retrospective revisions. Independently certify the successor's physical-share-to-TRI normalization; copying old TRI units to TMUS, or simply halving them, is generally wrong.

Track stock conversion, cash distribution and vendor cross-security continuity separately as `ExcludedFromTri`, `EmbeddedInTri` or `Unknown`, with boundary evidence. The first planner admits only the explicitly excluded components it is to apply. Already embedded, partially embedded or unknown treatment requires a separately proven rebasing/reconciliation and is rejected here. In particular, adding cash to a factor that already reinvested that distribution double counts it. A continuous vendor series that already represents successor exposure must not receive another stock leg. Independent rescaling `I'=kI`, `q'=q/k` must preserve certified physical equivalents, economic values and claims on both sides.

## Replay integration remains a later gate

After the pure seam is measured, a separate opt-in replay adapter can process admitted mandatory events between valuation observations, before requiring an obsolete predecessor mark. Keep effective time, entitlement time, boundary phase, valuation cutoff, source publication, known availability and ingestion distinct. `StrictAsOf` rejects unproved availability; `RetrospectiveReconstruction` discloses hindsight and must not supply later event knowledge to earlier preferences, eligibility or risk. Synthetic evidence never qualifies for either real-data admission mode.

Do not use the first May 1 successor close to create an April 30 mark or an April 30 trading decision. At the next actual valuation, a prepared batch may resolve the earlier legal conversion using the then-observed successor basis/mark, provided no intervening required valuation is omitted. Missing successor marks still fail; an earlier unresolved predecessor gap still fails. No synthetic PCS quote, forward fill, options-basket replacement close, fabricated exit fill, volume filter or retrospective permanent exclusion is authorized.

Mandatory conversions create holdings independently of alpha eligibility. They do not automatically transfer old PCS preferences to TMUS or give a delayed old target permission to reopen the retired representation. The successor must already have an admitted axis identity; runtime axis expansion is deferred. The current [replay callback](../include/atx/engine/book/replay.hpp) has no claims field, and the [allocation helper](../../atx-impl/src/equity_allocation.cpp) assumes `cash + equity == NAV`, requires held marks, and can reject an eligible holding without 63 adjacent risk returns. Claims and a new successor therefore need an explicit subsequent policy contract. Do not hide claims in cash, invent successor history, weaken eligibility through `HoldCurrent`, or bypass risk requirements to finish this run.

For the actual short, the stock leg is a successor short obligation and the cash leg a payable. Borrow fees cannot disappear when the old ticker stops printing, and cash netting cannot prove the stock loan ended. A future replay profile must define evidenced loan transfer/end times or a disclosed synthetic boundary convention, continuing successor borrow and separate payable financing. The present whole-interval charge on post-trade short dollars is insufficient to establish event-time financing. Pure signed arithmetic can be implemented now without asserting those operational facts.

## Measurement, memory and admission outcome

Three grouped analytical fixtures are sufficient for the first slice:

1. **Signed mixed transition and full payment:** synthetic `q_old=2`, `P_old=10`, `I_old=30` implies six research share equivalents. At successor `P_new=12`, `I_new=60`, the half-ratio gives `0.6` added TRI units, value `36`, and claim `24.2946`; bridge `0.2946` against old value `60`. Negating the holding negates all exposure/claim/bridge signs; payment of that negative claim debits cash. Include an existing successor holding and separate transition attribution.
2. **Basis and precision:** independently rescale each TRI normalization; preserve the economics. Reject doubled/half-sized cash denomination, embedded or unknown components, unmapped identities, unsupported physical fractional treatment, missing successor marks and nonrepresentable nonzero legs.
3. **Atomic ordering:** repeated event/payment, stale state, bad availability, conflicting revision and settlement before claim creation fail without state changes. An event does not erase a prior required missing valuation. No exchange turnover, trade fee or synthetic fill appears.

Use fixed-size numeric plans and digest/reference IDs; borrow evidence/mark inputs. Each event touches at most two instrument coordinates and creates at most one cash claim. For later ledger ownership, admit checked counts for events, outstanding claims, applied IDs and receipts, plus explicitly capped evidence strings; account for vector capacity and serialization workspace. Target `O(E + C)` retained records, with no event-by-date-by-universe arrays or duplicated price panels. Bind term/mapping/basis hashes, mode, source and valuation clocks, signed input/output quantities, actual unit/value residuals, claim/payment IDs and state versions into result identity. Report mandatory movements separately from allocations and external trades.

**PCS event admission remains rejected.** The vendor mapping on both sides, adjustment-component coverage, entitlement/physical basis, historical source availability, exact operational boundary, successor marks, fractional treatment, cash allocation and short-loan handling are not established by this research. Even after the pure planner passes, an event patch is not admissible to the failed native run on that basis alone. No implementation, native execution, build or tests were performed for this design.
