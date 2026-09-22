# Reported quarterly EPS pipeline

The `earnings_release_facts` activation stage reads SEC Item 2.02 filing
metadata, discovers EX-99 documents from the filing index's explicit Type
column, and stores qualified GAAP diluted quarterly EPS with immutable source
receipts. It retains the document's fiscal period and statement-table evidence.
Raw acceptance strings remain available for audit; daily eligibility uses the
conservative filing-date-plus-46-hour floor and any later qualified timestamp.

Statement construction maps accepted release evidence to a unique existing
CompanyFacts owner by CIK. Standardization publishes item1035, `eps_diluted`,
which feeds the ordinary quarterly growth engine. Annual EPS minus nine-month
EPS is not used to manufacture a fourth-quarter value. If visible direct and
reported EPS differ by more than0.005, the standardized state becomes NULL,
retains both source lineages and a conflict exception, and invalidates dependent
growth values at that availability time. Earlier valid states remain queryable
at earlier information cutoffs.

The public `ATX.US.FUNDAMENTALS/standardized` record contract is now3.0.0:
`value` can be NULL. Migration0321 retains the old2.0.0 catalog fields and hash
as inactive history, without rewriting prices or unrelated API metadata.
Queued batch jobs pinned to the old contract must be resubmitted; completed
artifacts retain their original schema. The overall dataset version is unchanged.

Issuer content can be queried without inventing historical ticker ownership;
see [the issuer query guide](ISSUER_CONTENT_QUERY.md). A CIK-owned accounting
result does not itself establish a historical market-security association.

Focused source, migrated-schema, growth-conflict, API, Arrow and upgrade/replay
checks pass. Actual Chevron Q4 2025 exhibit discovery/extraction also passes.
Full-universe materialization and coverage are still pending; see
[activation measurements](TIER1_ACTIVATION_STATUS.md) and
[the CVX acceptance case](CVX_EPS_ACCEPTANCE.md).
