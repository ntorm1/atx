# Issuer-content query draft report

## Scope and ownership

This isolated draft changes only the requested query surface:

* `atx-db/src/atx_db/asof/fundamentals.py`
* `atx-db/src/atx_db/api/catalog.py`
* `atx-db/src/atx_db/api/service.py`
* `atx-db/tests/test_issuer_content_query.py`
* `atx-db/docs/ISSUER_CONTENT_QUERY.md`

It has no migration, raw-schema, ownership, registry, activation, producer, or
market-valuation change.  No production source file was modified.

## Delivered contract

`ATX.US.ISSUER_CONTENT` adds statement, standardized, TTM, ratio, share, and
derived schemas.  They expose `issuer_owner_id` as an actual source-owner ID
and state that it is not a historical market security.  The service adds a
separate `resolve_issuer_ticker` path and `issuer_content_range`; the existing
strict `WarehouseReadService._security_ids` path is unchanged.

Ticker lookup requires exact upper-case ticker and normalized CIK history rows
for the same security, each covering `issuer_lookup_as_of`, observed no later
than that timestamp.  It returns unresolved or ambiguous rather than falling
back to an undated current directory row.  `sec_company_tickers` is an
advisory output cross-check only, so current directory changes cannot gate
co-visible identifier history or leak a future mapping.

Content selection requires its separate `content_as_of`.  Direct CIK queries
work without any ticker directory.  Visible exact-CIK rows in
`fundamental_fact_revisions` discover all actual source owners.  More than one
visible owner is retained and labelled explicitly; no guessed
`SEC-COMPANYFACTS-UNRESOLVED-CIK-{cik}` ID is constructed.  CIK-bearing tables
are filtered by that exact normalized CIK and discovered owner set.  Derived
rows are filtered by the discovered owners because they lack a CIK column.
Their query ranks the latest derived state, including NULL/unavailable values,
before applying the requested period range.

## Static producer trace: unresolved owner changes can split growth

This adapter cannot mend an upstream owner change.  The present producer code
is explicitly partitioned by `security_id`:

* `atx-db/src/atx_db/_derived_pit.py:82-95` builds item events and changes with
  `PARTITION BY security_id, canonical_code, bucket` and then output event
  ranges with `PARTITION BY security_id, code, bucket`.
* `atx-db/src/atx_db/_derived_pit.py:103-115` builds target buckets with
  `PARTITION BY security_id, bucket`.
* `atx-db/src/atx_db/derived_metrics.py:143-150` enumerates derived work by
  `SELECT security_id FROM fundamental_standardized ... UNION SELECT
  security_id FROM derived_metric_values`.

Relevant source excerpt from `_derived_pit.py`:

```sql
OVER (PARTITION BY security_id, canonical_code, bucket
      ORDER BY available_at RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)
...
QUALIFY lag(state) OVER (PARTITION BY security_id, code, bucket ORDER BY event_at)
```

Relevant source excerpt from `derived_metrics.py`:

```sql
SELECT security_id FROM fundamental_standardized
...
UNION SELECT security_id FROM derived_metric_values WHERE source = ?
```

Therefore, if CIK X has earlier quarterly inputs under owner A and later inputs
under owner B, `eps_diluted_q_growth_yoy` sees separate histories and cannot
match quarter B to the prior-year quarter A.  The result is a genuine derived
metric gap, not a query-selection issue.  This draft reports both owners and
their derived states honestly.  It does not rewrite raw ownership, schemas,
46M facts, or receipt fingerprints, and does not claim the adapter fixes the
split.

Bounded follow-up: after archive materialization, measure CIKs with more than
one visible `fundamental_fact_revisions.security_id` and inspect whether the
affected derived metric windows cross the owner boundary.  If material, design
a separately reviewed producer-level CIK continuity key (or a governed
issuer-owner bridge) with PIT rules and a complete re-materialization plan.
Do not introduce it during receipt-preserving archive work.

## Focused test design

`test_issuer_content_query.py` covers the substantive contract cases:

1. a direct CIK read with two actual owner IDs, including a resolved and an
   unresolved owner;
2. preservation of a newer NULL/unavailable derived state over an older valid
   state; and
3. a ticker lookup resolved from co-visible identifier history while the
   current-directory cross-check is false.

The root integration owner must run focused selectors only after the writer is
terminal.  No test, import, database read, network operation, mypy, or runtime
execution was performed for this draft.

## Static commands used

Only static file discovery/read/copy/edit commands were used:

* `Get-Content` for the production brief, acceptance case, owned source, and
  schema excerpts;
* `rg` for code and test discovery plus the producer partition trace;
* `Copy-Item` into this isolated draft; and
* `apply_patch` for all draft edits.
* `git diff --check --no-index` and `git apply --check integration.patch` to
  validate whitespace and that the integration patch applies; neither executed
  Python, imports, tests, or accessed the warehouse.
