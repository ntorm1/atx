# Issuer-content production-query draft review

Review basis: static comparison of the draft patch, report, production brief,
current source schemas, and the focused test patch.  No imports, database
reads, tests, network operations, or production-source edits were performed.

## Critical

### Derived rows can cross a CIK boundary when an owner ID is reused

`WarehouseReadService._issuer_content_rows` deliberately omits the CIK
predicate for `derived_metric_values` and selects every row whose
`security_id` is in the owners discovered for the requested CIK:

```python
is_derived = schema.source_table == "derived_metric_values"
cik_condition = "" if is_derived else "AND lpad(trim(b.cik), 10, '0') = ?"
conditions = [f"b.security_id IN ({owner_marks})", cik_condition.removeprefix("AND "), ...]
```

That is safe only while each owner is demonstrably exclusive to one CIK.  The
owner discovery query establishes that an owner has a fact for the requested
CIK, but never establishes that it has no visible facts for another CIK.  If a
resolved source-owner collision occurs, a CIK-A request can therefore return
CIK-B derived values.  This violates the requested exact-CIK isolation and
does not report the collision as ambiguous.  The focused test exercises two
owners for one CIK, but not one owner for two CIKs.

Remedy: at `content_as_of`, discover the normalized-CIK set for each selected
owner from `fundamental_fact_revisions`.  Continue exact-CIK filtering for
CIK-bearing relations.  For derived metrics, include an owner only if that set
is exactly the requested CIK; otherwise return no derived rows for that owner
and expose an explicit `ambiguous_owner_cik_collision` state (or prove the CIK
through immutable derived input lineage before including it).  Add a focused
collision fixture which proves that CIK-B's derived row never appears in a
CIK-A response.

## Important

### The declared issuer association/provenance result is incomplete

The production brief requires the issuer surface to expose the current
directory candidate separately from a qualified historical market attachment:
`issuer_owner_id`, `market_security_id`, `association_method`,
`association_scope`, `historical_security_qualified`, and an unavailable or
ambiguous reason.  The patch returns only an identifier-history
`directory_security_id` and interval from `resolve_issuer_ticker`; it has no
association projection and no qualification flag.  Consumers can see the
candidate but cannot machine-read that it is unqualified for market cap,
valuation, panels, backtests, or certified historical output.

Remedy: add the small read-only association metadata/result described in the
brief, with `historical_security_qualified=false` for the current-as-of
directory path.  Preserve the strict security APIs and do not modify raw facts
or identifier intervals.

### Multiple current security candidates for one CIK are presented as resolved

The resolver marks a lookup resolved whenever `len(ciks) == 1`, then selects
`evidence[0]` as `directory_security_id`:

```python
if len(ciks) != 1:
    ...
return {"status": "resolved", "directory_security_id": evidence[0]["directory_security_id"], ...}
```

Two co-visible classes/security IDs can share one CIK and ticker evidence.
Their order comes from the query/deduplication path, so the singular primary
security candidate is arbitrary while the response says `resolved`.  This is
not historical-security proof, but it still conceals current directory/class
ambiguity.

Remedy: resolve the issuer CIK if appropriate, while reporting a distinct
directory-candidate status/count.  When more than one security/interval tuple
remains, set the market-association portion to ambiguous and omit a singular
candidate ID (or require an explicit candidate selection).

### Issuer shares ignore the public `first_reported` option

`issuer_content_range` advertises `vintage="latest" | "first_reported"`, but
the shares branch always orders descending:

```python
elif schema.source_table == "shares_outstanding_history":
    revision_order = (
        "b.effective_date DESC, b.as_of_date DESC, "
        "coalesce(b.available_at, b.source_loaded_at) DESC, ..."
    )
```

Consequently a `first_reported` shares request returns the latest row.  Its
schema inherits `supports_vintages=True`, so this is a contract mismatch.

Remedy: derive every shares ordering direction from `vintage`, including
effective/as-of/availability/ID tie-breakers, or set the schema option false
and reject that option deliberately.

### Focused tests do not cover most of the proposed surface or adverse clocks

The new test constructs only `fundamental_fact_revisions`,
`derived_metric_values`, and identifier/current-directory tables.  It never
executes statement, standardized, TTM, ratio, or shares queries, nor the
as-of helpers in `asof/fundamentals.py`.  It also lacks cases for a future or
non-covering identifier interval, a late mapping, direct CIK with no visible
owner, unmaterialized content tables, source-row CIK mismatch, multi-security
same-CIK lookup ambiguity, and `first_reported` shares.

Remedy: add compact fixtures for each source-table schema and the above
negative cases.  In particular assert separate lookup/content clocks: a
current lookup must not make a fact visible before its own `available_at`.

## Minor

### Unresolved ticker responses do not retain the normal issuer-result shape

The early unresolved/ambiguous ticker return contains only a small metadata
subset.  It omits schema version, selected fields, owner status/IDs, owner
semantics, and the normalized lookup CIK field present in resolved/direct
responses.  It is explicit, but makes empty responses harder for typed clients
to consume uniformly.

Remedy: build the standard issuer metadata envelope before the early return,
using explicit null/empty values and the lookup unavailable reason.

### Lineage projection is narrower than the brief's raw-to-derived trace

The catalog exposes a useful accession or input hash for each relation, but
the issuer statements schema omits `fact_revision_id`; standardized omits
`input_item_ids_json`; and TTM omits its accession/period input lists.  Those
existing immutable identifiers are needed to trace a selected issuer value
back through raw facts without guessing from an accession.

Remedy: add the relevant immutable IDs/lineage JSON fields to the new catalog
schemas and allow-list projection.  This is additive and does not alter source
or receipt fingerprints.

## Accepted controls

The patch leaves `_security_ids` untouched, uses a separate
`issuer_lookup_as_of` and `content_as_of`, requires co-visible dated TICKER and
CIK history, and treats `sec_company_tickers` as advisory.  CIK-bearing
content tables are filtered by exact normalized CIK plus owners discovered
from visible facts.  The derived query ranks its visible revision group before
applying the output period interval and thus preserves a newer NULL/unavailable
state.  It also correctly reports multiple discovered owners rather than
constructing an assumed unresolved-owner namespace.  The report honestly
identifies security-ID-partitioned derived growth continuity as a separate
upstream issue; this review does not treat the adapter as a fix for it.
