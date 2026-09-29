# Brief: task R-7

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task R-7: library v8.1, new families from new fields

**Files:** registry entries; `scripts/specs/v8/lib-v81.json`; fields from F-1 and from the SEC module

| id | theme | tier | semantics | data |
|---|---|---|---|---|
| `comp_eq_iss_5y` | investment_issuance | B | 5-year composite equity issuance; long low | `ceq_iss_5y` (F-1) |
| `coskew_60m` | low_risk | B | coskewness on 60 monthly returns; long low | `coskew_60m` (F-1) |
| `tax_book` | profitability_quality | B | current tax expense TTM / (statutory rate x net income TTM); long high | new XBRL item (OD-6) |
| `gscore_lowbm` | profitability_quality | B- | Mohanram G-score, 7 of 8 components, inside the bottom book-to-market tercile | existing fields |
| `night_day` | reversal_seasonality | B- | 63-session overnight return minus intraday return | `ret_overnight`, `ret_intraday` (OD-6) |
| `nt_late` | new theme `filing_events` | B- | short for 126 sessions after a first NT 10-K or NT 10-Q | SEC filings stage form rows (OD-6) |
| `nonreliance_402` | `filing_events` | C+ | short for 63 sessions after an 8-K Item 4.02 | 8-K item codes |
| `earn_consistency` | earnings_momentum | C+ | 4-year mean of year-on-year EPS growth with same-sign filter | `ni_q`, `shrs_q` |

- [ ] **Step 1:** draft lane extends `library-v8-draft.md`; members whose data is absent at the freeze are withdrawn.
- [ ] **Step 2:** fields v11 = v10 + event fields. **Step 3:** `add-alpha` x members, `run`.

**Acceptance:** wave accepted whole: dSR > 0 AND mechanics AND turnover not higher AND at least half of the admitted new
members have marginal IC HAC t above 0 (K6).
**Expected [est]:** net +.03 to +.08. **Cost:** up to 8 admission trials, 1 cell (N 47).

