# Fundamental effective filing clock (FC1)

Policy version: `sec_filed_date_plus_46h_v1`.

For `source = 'SEC companyfacts'`, effective eligibility is no earlier than both
the stored `available_at` and the source filing date plus 46 hours (the following
calendar day's 22:00 UTC). For `fundamental_points`, the source filing date is
stored in `as_of_date`. Daily consumers continue using their existing inclusive
22:00 UTC trading-day cutoff. A Friday filing is eligible on Saturday at 22:00;
without a weekend trading decision it first enters the next trading session.

This is a conservative date-based policy, not an exact SEC acceptance timestamp
or measured first public/API/local arrival. It closes the ordinary winter case
in which a same-day filing can be accepted at 22:15 UTC after a 22:00 decision.
It does not establish a universal upper bound on acceptance or dissemination.
Official filing-date corrections and unbounded delivery delays remain limits.
Neither unverified submissions `acceptanceDateTime` nor an earlier earnings
release (`rdq`/`pdate`) accelerates a fact. RDQ stays descriptive.

Null, malformed, or non-finite SEC filing dates have unresolved eligibility. The
shared effective projection excludes those rows; raw storage is retained for
diagnosis. It does not substitute a load clock, period end, acceptance guess or
current date. If the source filing date is valid but the stored timestamp is
NULL, only the documented date-policy floor is assigned. Non-SEC source clocks,
including their existing NULL behavior, remain unchanged. Typed raw DATE columns
already reject most malformed dates at ingestion.

`_fundamental_clock.py` supplies an inline SQL projection with no additional
fact/submissions join. Revision construction reads that projection before its
base IDs and revision windows. Existing statement, period, TTM, calendarized,
standardized, P1 event and AF1 annual arithmetic propagate the resulting clock.
Accession-based IDs that do not include a clock remain accession-based;
clock-dependent IDs/hashes downstream use the corrected input. No identities
are remapped. Historical CIK/security/entity assignments retain their original
resolution assumptions, and reconstructed source revisions still do not certify
warehouse observation vintages or completeness.

The raw `fundamentals_asof` reader returns and filters projected `available_at`.
SEC fundamental feature construction and the price/fundamental overlap quality
check use the same projection. Migration 0319 updates the existing overlap view
without changing its columns/grain and appends policy notes to existing catalog
entries. Feature build metadata and the activation statement-stage result record
the policy version. Applying the migration alone does not correct existing
materializations.

The Company Facts branches of `fundamental_xbrl_metrics`, `estimates.measure_actuals`
and `expected_growth` also read the same effective projection. Inline-XBRL
acceptance-based inputs retain their existing clocks. These three builders are
outside the activation ladder; existing outputs need a separate normal refresh.
In particular, standardization can consume a preexisting `fundamental_xbrl_metric`
table, whose legacy rows mix inline and Company Facts origins and do not retain
the source filed date. Root must inventory that table before run5: an empty table
satisfies this continuation's prerequisite; any existing rows require a concrete
bounded refresh before core standardization may proceed. The current XBRL builder
collects candidates in pandas and must not be invoked over the full universe
merely to clear that prerequisite. There is no inferred policy stamp or automatic
gate on those legacy rows, and FC1 does not claim they have been rebuilt.

Raw `sec_company_facts`, `fundamental_points`, source files, and CF5 fingerprints
and receipts are unchanged. Direct SQL, raw lake exports, raw watermarks, and
`v_fundamental_points_latest` remain raw/current provenance surfaces; they are
not effective as-of readers. Consumers requiring historical eligibility must use
the effective readers or rebuilt downstream materializations.

Production transition requires a full-scope rebuild from `statement_points`
(including fact revisions) through periods, TTM, calendarization, standardization,
derived metrics, daily metrics and downstream projected/exported consumers.
`statement_points` now refreshes SEC `shares_outstanding_history` after statement
publication, before any market or factor consumer runs. This uses the existing
idempotent source-wide replacement and records its row count. A failed share
refresh fails the stage; a retry rebuilds it. Existing completed stage receipts
from an older policy must not skip this rebuild.

Share-history consumers include the public shares as-of reader, daily DEI shares,
the shares/market-input compatibility relations used by factor projections,
valuation multiples/market capitalization, short-interest metrics, fundamental
signals, cash profitability, earnings surprise and revenue surprise. Existing
materialized consumers need their normal refresh before policy coverage is
claimed; exports/releases need republication. FC1 adds no unrelated activation
stages or factor arithmetic. Production counts, changed-decision prevalence,
source-date exceptions and full-universe memory/disk behavior require separate
guarded root measurement.
