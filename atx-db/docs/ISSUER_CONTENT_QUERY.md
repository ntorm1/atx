# Issuer-content query surface

`ATX.US.ISSUER_CONTENT` is a bounded, read-only accounting surface keyed by a
normalized SEC CIK and the visible Company Facts `issuer_owner_id`.  The owner
is a source partition, not a tradable security, share class, or proof that a
ticker was valid at an earlier filing or bar date.

Use `WarehouseReadService.resolve_issuer_ticker(ticker=..., issuer_lookup_as_of=...)`
to select a CIK from co-visible `TICKER` and `CIK` identifier-history rows.
The result includes the identifier interval, availability, as-of date, source,
and an advisory current-directory cross-check.  Current directory contents do
not authorize or veto the historical lookup.

The returned `issuer_market_association` always sets
`historical_security_qualified` to `false`.  If more than one co-visible
directory row exists for the same resolved CIK, it retains every candidate,
leaves `market_security_id` null, and reports
`multiple_current_directory_security_candidates`; it never selects an
arbitrary directory owner.

Use `issuer_content_range(..., content_as_of=...)` to read statements,
standardized values, TTM, ratios, shares, or derived metrics.  Direct `cik=`
selection does not require a ticker directory.  With `ticker=`,
`issuer_lookup_as_of` is required and remains distinct from `content_as_of`.

The content query discovers actual visible owner IDs from
`fundamental_fact_revisions` for the exact normalized CIK.  It returns every
visible owner and labels more than one as
`ambiguous_multiple_visible_owners`; it never fabricates an unresolved-owner
identifier or combines different CIKs.  Derived values are selected by those
owners because that table has no CIK.  Its latest state is ranked before the
requested period range is filtered, so a later NULL/unavailable invalidation
does not resurrect an older valid value.

Before a derived owner is used, the surface checks all raw CIKs whose
availability and as-of date are visible at `content_as_of`. An owner shared
with another normalized CIK is excluded and reported as
`ambiguous_owner_cik_collision`. This owner check is coarse: each visible
derived revision is also checked against its exact retained selected operands.
Every selected standardized leaf must carry the requested CIK, and the
canonical source, registered definition hashes, and operand clocks must verify.
The proof uses the selected revision's own event clock, so a valid first
reported state may be returned after its `valid_to` date.

A numeric metric is returned only after full selected-lineage qualification.
An unavailable NULL state is returned only if its selected source ownership is
verified. Legacy NULL refs, foreign leaves, missing operands, and invalid
numeric states are excluded with `derived_lineage_diagnostics` containing a
bounded state ID, period, status, and fixed reason. The metadata also gives
`derived_lineage_rejected_count`, `derived_lineage_scanned_count`, and explicit
diagnostic and scan truncation flags. Pages are qualified before the requested
row limit is applied. Large or unavailable lineage produces a controlled
query error. The as-of DataFrame exposes equivalent diagnostics in its attrs.

CIK-bearing statement, standardized, TTM, ratio, and share tables use an exact
normalized-CIK filter independently of owner discovery.

For a CVX quarterly diluted-EPS growth read, first resolve the current lookup
clock and then use the accounting-content clock separately:

```python
service.resolve_issuer_ticker(
    ticker="CVX", issuer_lookup_as_of=datetime(2026, 9, 20, 22, tzinfo=UTC)
)
service.issuer_content_range(
    schema_name="derived-metrics", cik="0000093410",
    start=date(2025, 10, 1), end=date(2026, 10, 1),
    content_as_of=datetime(2026, 9, 20, 22, tzinfo=UTC),
    items=["eps_diluted_q_growth_yoy"], basis=["q"],
    fields=["period_end", "value", "value_status", "issuer_owner_id", "cik"],
)
```

The caller should inspect both `issuer_owner_status` and the derived-lineage
diagnostics. A coarse owner collision excludes the shared owner; selected
lineage excludes foreign or unverified states within an otherwise eligible
owner.

No market valuation conclusion follows from this surface.  Current directory
evidence may identify a present market-security candidate, but remains
unqualified for historical market cap, valuation multiples, panels, backtests,
or certified historical output until dated class-specific identifier evidence
covers the relevant fact or bar time.
