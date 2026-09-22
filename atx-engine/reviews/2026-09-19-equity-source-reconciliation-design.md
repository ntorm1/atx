# Equity source reconciliation design — 2026-09-19

The first fixed training book rejected a required held mark at evaluation period **6**, session **2013-04-12**, `spiderrock.securityID=150340`. This identifies a missing input needed for valuation; it does **not** establish whether the cause is a source omission, quarantine, transformation error, or corporate event. Root will batch source inspection after the full required-gap inventory is available. No archive scan or production change is part of this design.

The declared evaluation window, original ZIP, `tickerhistory-qa-v1` exclusions, strict held-mark rejection, and failed-run artifacts remain unchanged. Do not shorten the window, delete the security retrospectively, forward-fill its mark, or assign a zero return to obtain a successful report. This is training-only data/accounting diagnosis, not strategy qualification.

## Bounded evidence package

Create one versioned reconciliation report for the actual failed book and its required-gap list. Bind the original source ZIP/member and preparation-manifest hashes, accepted ZIP, ingestion receipt, context/evaluation/books artifact IDs, baseline request/failure receipts, and auditor executable/script hash. Preserve canonical positive integer security IDs and exact session labels; display tickers are annotations only.

For each required key, record:

1. The kernel indices, exact bound date/ID, last effective decision and whether the instrument was held or newly targeted. Retain the complete rejected run; later corrections do not replace it.
2. Every original row with that positive date/ID, original line bytes and SHA256, source row ordinal, and all applicable existing QA reasons. Preserve conflicting duplicates together. Record `source_row_absent` when none exists; absence is not itself evidence of a delisting or suspension.
3. The original neighboring observations for that ID within the already declared training context, including `dn`, raw OHLC, volume, shares, `closePr`, `closeUnadjPr`, `returnFactor`, `totalReturn`, and `cumulReturnFactor`. Retain their QA outcomes even when they were excluded from accepted input.
4. Whether the key survived preparation and exists in the selected native segment; compare raw native values, adjusted panel close and masks. Distinguish `quarantined`, `accepted_but_missing_downstream`, `invalid_adjusted_product`, `source_absent`, and `unresolved_event`. Do not infer a source defect merely from the panel's NaN.

Store only the requested keys and bounded adjacent context, plus aggregate counts. A source scan may stream through preceding date groups without retaining them; stop at the declared training end. Record the actual scan extent and whether a complete ZIP-member CRC was checked. A partial read cannot inherit a claim that it independently verified the entire member CRC. Reuse existing bound preparation/source receipts with their original verification scope clearly stated.

## Definitions and diagnostic identities

The vendor describes `closePr` as the corporate-action-adjusted prior-session close and `closeUnadjPr` as its unadjusted counterpart. It provides a daily factor, daily total return and cumulative factor, but does not publish an exact cumulative recurrence on this page. `dn` is an NMS-calendar trading-day ordinal. These definitions guide comparisons; they do not authenticate the downloaded values. [TickerHistory3 dictionary](https://docs.spiderrockconnect.com/docs/next/HistoricalData/Data%20Dictionaries/TickerHistory3/)

Let `C_t` be raw close, `P_t=closePr`, `U_t=closeUnadjPr`, `q_t=returnFactor`, `F_t=cumulReturnFactor`, and `r_t=totalReturn`. For an unambiguous adjacent original pair `(p,t)`, report the following residuals separately:

| Comparison | Residual | Interpretation |
| --- | --- | --- |
| Unadjusted prior close | `U_t - C_p`, plus a relative residual | Prior-close continuity |
| Daily adjustment | `P_t / U_t - q_t` | Internal daily-factor agreement |
| Reported daily return | `C_t / P_t - 1 - r_t` | Internal price/return agreement |
| Cumulative recurrence | `(F_t / F_p) * q_t - 1` | Proposed cumulative/daily-factor consistency |
| Implemented adjusted return | `(C_t * F_t) / (C_p * F_p) - 1 - r_t` | Native price transformation versus reported return |

The fourth relationship is an **inference**, not a verbatim vendor formula: if `U_t=C_p`, `P_t=U_t*q_t`, and adjusted prices are `C*F`, equality of the two return constructions implies `F_t/F_p=1/q_t`. The earlier [input audit](2026-09-19-tbltickerhistory-input-audit.md) measured support for this direction; that finding does not establish every source row as correct. The separate [ReturnFactorsHist dictionary](https://docs.spiderrockconnect.com/docs/next/HistoricalData/Data%20Dictionaries/ReturnFactorsHist/) repeats cumulative-factor semantics without resolving every adjustment convention.

Require unique positive identity, increasing dates, valid integral `dn` and `dn_t=dn_p+1` before treating a pair as consecutive. A previous *accepted* row is insufficient: quarantine may have removed an intervening original session. Missing/duplicate predecessors, absent ordinals, nonpositive denominators or nonfinite arithmetic yield `not_comparable` with reasons, never a zero residual or a pass. Confirm the raw source session pair rather than inferring adjacency from civil-day distance. Record first/last available evidence dates and unresolved calendar/coverage uncertainty.

Preserve original numeric text and report absolute/relative residuals, distributions and diagnostic bins fixed before inspection, for example `1e-8`, `1e-6`, `1e-4`, `1e-3`. These are severity summaries, **not automatic economic acceptance tolerances**. Source float rounding and vendor conventions require separate interpretation. An all-consistent row remains economically `unverified`; an independently established contradiction is `failed`; missing evidence remains `unknown`.

## Independence and close-only exceptions

Internal agreement cannot certify an economic return. The earlier input audit documents PGN, ID `41613`, on 2012-03-27: `close/closePr-1` agrees with an implausible reported return while prior observed close disagrees. The [KLAC exception](2026-09-19-klac-source-adjustment-audit.md) records ID `38946` on 2026-06-12: defective OHLC triggered quarantine, but reinstating its close would still retain a split omitted by the supplied cumulative factor. KLA's [issuer announcement](https://ir.kla.com/sec-filings/all-sec-filings/content/0001193125-26-212093/d116682dex991.htm) and [June 12 filing](https://www.sec.gov/Archives/edgar/data/319201/000119312526269375/d144278d8k.htm) establish the ten-for-one event. These prior observations motivate the audit; they do not diagnose ID `150340`.

A field-specific close-only policy is potentially defensible when the defect is confined to unused O/H/L, unique historical identity is resolved, and a separately evidenced close **and** corporate-action treatment establish a usable valuation mark. Keep the raw row and existing exclusion; a reconciled mark is a distinct versioned derivative with evidence, affected fields, effective date, availability uncertainty and decision rationale. Never infer split ratios from jump size, adjust volume/shares with a dividend-inclusive cumulative factor, or accept duplicate identity by choosing a convenient row.

An official-close record may help distinguish a bad range field from a bad mark. SpiderRock's [StockCloseMarkHist dictionary](https://docs.spiderrockconnect.com/docs/next/HistoricalData/Data%20Dictionaries/StockCloseMarkHist/) distinguishes exchange closing price, vendor mark and mark state. Another table from the same provider remains shared-lineage evidence, not automatically an independent oracle; access to its historical records is not currently established. Issuer/exchange corporate-action evidence must be matched to historical security identity and effective date, rather than today's ticker.

For an eventual **valuation-only** exception, preserve the original decision exclusion and feature observation status. Replacing canonical close can otherwise alter delayed momentum even while that day's decision mask is false: the baseline feature view intentionally uses observed history outside historical universe membership. A broader close-only observation policy changes signals/universe and is a new declared data recipe, not a transparent repair of this frozen trial. True cessation/merger/bankruptcy may require terminal cash/event accounting; a manufactured positive closing mark is not an adequate substitute. Publication vintage, security type, shares timing, locates and liquidity remain separate unknowns.

## Smallest next implementation

Add a **read-only** `tools/audit_tickerhistory_reconciliation.py`, sharing or reusing the bounded row framing, numeric/identity parsing and exact QA predicates in `tools/prepare_tickerhistory.py::prepare/flush`. Initially it emits evidence and dispositions only; it does not write accepted input, patch factors, or rerun strategy selection. Its request is the full required-gap inventory plus the bound training manifests, not hand-selected favorable securities.

The native comparison seams already exist: `include/atx/engine/data/orats_history.hpp::kOratsFields` retains all return-check columns in segments; `src/data/history_panel.cpp::adjusted_history_prices` applies the pointwise factor; `atx-impl/src/equity_baseline_views.cpp::feature_view` distinguishes observation validity; and `src/book/replay.cpp` rejects missing required marks. The final research panel does not retain every reconciliation input, so audit the source/segment pair rather than reverse-engineering factors from adjusted prices.

Focused acceptance cases: excluded bad low with independently reconciled close; the same case with a bad split factor remaining unresolved; conflicting duplicate identity; an intervening quarantined session preventing false adjacency; small rounding disagreement reported without repair; and a true source-absent held key retaining failure/unknown status. Original bytes/exclusions, failed-run identity and evaluation boundaries must remain unchanged in every case.
