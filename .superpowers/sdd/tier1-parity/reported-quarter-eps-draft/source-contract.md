# SEC reported-quarter EPS source contract

Accepted `press_release_facts` row:

- `source = 'SEC 8-K Item 2.02 reported earnings release'`
- immutable source owner: `cik` is zero-padded and `security_id = cik_security_id(cik)`; no ticker/current-security fallback
- `form = '8-K'`, `source_item = '2.02 / EX-99'`, accession, SEC archive URL, EX-99 filename/SHA, table/row/column evidence and original clock evidence are retained in `raw_payload_json`
- `measure_code = 'EPS_DILUTED'`, `basis = 'GAAP'`, `unit = 'USD_PER_SHARE'`, `is_preliminary = true`, `extraction_confidence = 1.0`
- both `period_end` and a document-labeled `(fiscal_year, fiscal_period)` are required. `raw_payload_json.duration_evidence` records an explicit `three_months_explicit`, `thirteen_weeks_explicit` or `fourteen_weeks_explicit` table header. The downstream producer must retain an exact period start/end: it may derive a month-based start only from the explicit three-month evidence and must require an explicit start/period qualification for 13/14-week quarters. It must not calendar-quarter-map a 52/53-week issuer. No FY-minus-YTD EPS, net-income/share construction, basic, adjusted/non-GAAP or continuing-operations values
- `available_at` is `max(valid offset-bearing SEC acceptance instant, filing_date + 46h)` when filed date is known. The `timestamp_zone_status` states `sec_filed_date_plus_46h_v1`; this is conservative daily eligibility, not exact dissemination or certified historical intraday availability. A naïve source timestamp remains `timestamp_zone_unknown`; without a filing date it has no `available_at`.

`sec_earnings_release_receipts` is append-only by receipt id. It records both accepted and rejected/`fetch_failed` outcomes and CIK/accession/document/SHA provenance. `accepted` needs document name, SHA and `available_at`; terminal accepted/rejected records make an accession resumable, while `fetch_failed` remains retryable.

Bridge requirements: release and direct Company Facts rows are separate source vintages. At each visible event retain the earlier release and later direct event. When both are visible and their EPS values differ by more than `0.005`, emit an explicit unavailable/conflict state; do not prefer, average or delete either value. An unresolved CIK source owner remains evidence only until the existing fact-time CIK identity route provides a valid market-security link.
