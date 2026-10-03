# Fundamentals v11

Status: implementation in progress; publication and acceptance measurements pending. This is not a certified release.

The producer preserves CompanyFacts taxonomy/concept identity, reporting currency and filing vintage. `events`, `catalog` and `quarterly_history` use the retained v10 source extract, with v11 source-backed missingness rules. Money is USD where verified FX conversion exists; native unsupported currencies remain flagged. Shares and per-share values retain their distinct units and basis.

Keys and clocks follow fundamentals/v2: event/catalog `(cik, accession)`; history `(cik, item, period_end, accession, available_at, currency)`. All vintages retain actual disclosure clocks. Monthly visibility is strictly before month-end 22:00 UTC; daily visibility is strictly before the prior session 22:00 UTC. Subsequent disclosures never suppress earlier visible rows.

Missing facts, PRE omissions and nil omissions remain NULL. Explicit reported zero remains zero. Parent equity and net operating assets require their actual components. No assets-minus-liabilities equity or liability plug is permitted. Debt uses an explicit current aggregate, otherwise both current long-term debt and short-term borrowing; noncurrent debt uses an explicit source or total long-term debt less reported current maturities. Net/gross/lease scope still requires independent source qualification.

Own-filing FSDS IS/CI presentation can select contractual revenue when generic Revenues is absent from that statement. No value sign, size or issuer preference changes concept priority. Comparative annual revenue, COGS, NI, cash flows and catalog tax/debt/retained-equity stocks publish at their actual disclosure clocks.

The standardized consumer retains `source_period_start/end` (DATE), `source_period_days` (INTEGER), `source_full_year` (BOOLEAN), `source_period_available_at` (TIMESTAMP UTC) and `source_period_accession` (VARCHAR). Exact filing evidence has priority over older comparative metadata. A source interval of 350–380 inclusive days supports full-year eligibility; a short transition period does not. Missing source duration cannot be certified using adjacent fiscal endpoints.

Annual ratio history additionally requires visible source duration, matching fiscal calendar and actual adjacent annual history. Missing-history observations remain in coverage denominators. Primary diluted-EPS P/E still requires security-class/ADR/split/date qualification; issuer-cap earnings extensions remain separate. Indicated dividend yield requires Task13. Composite original-model and independent comparison certification remain open.

Coverage FY2015–25, 117-item gates, balance identity, FSDS, cutoff and mega-alpha exit measurements are pending guarded publication. Earlier Task3/4 measurements do not certify this code. Staleness remains the existing producer/consumer policy, with ratio freshness at 400 days for quarterly reporters and separate annual eligibility.
