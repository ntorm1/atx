# Reported-quarter EPS source draft

This isolated draft adds the bounded public-SEC source layer only. It does not
claim that source evidence alone completes CVX or activates the downstream
metric; the separately owned core resolver must consume the contract in
`source-contract.md`.

## Draft contents

- `sec_submissions.py` adds deterministic CIK/accession-keyset discovery of
  `8-K` Item `2.02` metadata. It has optional requested-history bounds and no
  production two-year ceiling. The selector preserves CIK ownership and does
  not use ticker linkage.
- The same module retains `acceptance_datetime_raw` on future metadata loads.
  Existing normalized rows remain valid source metadata; they do not have to
  be reloaded to enter the daily path because a known filing date supplies the
  existing `sec_filed_date_plus_46h_v1` conservative eligibility clock. Their
  original-zone status remains unknown, so the draft never calls them exact
  publication timestamps. The bulk-resume keys, archive hash, verified prefix,
  CIK order and existing source receipts are untouched: the column is additive
  and does not enter `_bulk_source_keys` or prefix comparison.
- `press_release.py` adds a project-only SEC client user agent
  `atx-db/0.1 atx-research@example.com`, keyset streaming over candidates,
  SEC archive index / EX-99 selection, request timeouts supplied to the
  session, retries from the existing SEC session, bounded index/document byte
  reads, cache-by-accession-document, SHA-256 receipts, and retriable fetch
  outcomes. No web search, issuer URL, credential, or LLM is involved.
- The deterministic HTML parser only accepts one aligned row in an explicit
  three-month, 13-week or 14-week table, with a labeled fiscal year/quarter and
  exact `period_end`. It retains `duration_evidence` in raw payload so the
  bridge does not calendar-map a 52/53-week issuer; a month-based start is
  derivable only from explicit three-month evidence, while a week-based start
  needs explicit boundary qualification.
  It rejects ambiguity, adjusted/non-GAAP, basic and continuing-operations
  rows. It does not calculate EPS from income/shares or annual/YTD values.
- Migration 0320 adds the immutable `sec_earnings_release_receipts` evidence
  table and only the additive submissions raw-clock field. The body refreshes
  the schema-contract pin and the mirrored registry registers the body.
- The standalone CLI has explicit cache/history/resource controls. The normal
  source default scans the requested history; `candidate_batch_size` only
  bounds working memory, it is not a history cap.

## Availability and source vintages

The receipt retains `acceptance_datetime`, original raw timestamp and offset,
timezone status, retrieval time, URLs and SHA separately. The source fact's
daily `available_at` is `max(valid explicit-offset acceptance, filing date +
46 hours)`. This is the reviewed conservative `sec_filed_date_plus_46h_v1`
daily eligibility policy, not an assertion that EDGAR disseminated the filing
at its acceptance instant. A naïve timestamp without a valid filed date emits
receipt evidence but does not obtain an availability instant.

The downstream resolver must retain the release vintage and the later direct
Company Facts vintage. Once both are visible, an absolute EPS difference over
`0.005` must produce an unavailable conflict state, with both lineages kept;
it must not silently choose Company Facts, average values, or delete the
release. The raw CIK owner stays unresolved evidence until the existing
fact-time identity path authorizes a security link.

## Core resolver handoff

The existing `refresh_fundamental_statement_points` rebuilds only from
`fundamental_fact_revisions`, while `_standardization_set_based` independently
groups upstream source events. A direct SQL union of `press_release_facts`
would lose the release/direct event relation and let ordinary priority ordering
silently hide a visible conflict. The bridge must therefore own a durable
reported-EPS resolver state keyed by `(raw CIK owner, period_end, fiscal_year,
fiscal_period, EPS_DILUTED, GAAP, USD_PER_SHARE)`, with source event identity
`(source, accession, document SHA/CompanyFacts fact revision, available_at)`.
It should:

1. materialize every valid release and direct event without deleting either;
2. apply source-owner-to-market identity only at fact time;
3. emit one visible state for each availability event, including explicit
   `conflict_unavailable` when both values exceed tolerance;
4. give the statement producer only resolved visible rows and retain conflict
   diagnostics for standardization/derived unavailable propagation; and
5. remove/rebuild only resolver rows in the requested raw-source scope, never
   Company Facts raw facts or unrelated issuer data.

## Focused validation to run during integration

No tests, imports, database access or network calls were run under the source
draft fence. The added network-free fixture covers offset preservation,
conservative naïve-clock handling, EX-99 ambiguity, aligned GAAP diluted EPS
selection and adjusted/continuing-operations rejection. Root should run:

```powershell
pytest atx-db/tests/test_reported_quarter_eps_source.py atx-db/tests/test_sec_submissions_bulk.py atx-db/tests/test_press_release.py
pytest atx-db/tests/test_migrations.py atx-db/tests/test_schema_contract_v2.py
```

Then run the core resolver's focused no-lookahead, identity, conflict and
FY-minus-YTD trap fixtures before a guarded small SEC source execution.
