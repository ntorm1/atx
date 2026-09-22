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

No market valuation conclusion follows from this surface.  Current directory
evidence may identify a present market-security candidate, but remains
unqualified for historical market cap, valuation multiples, panels, backtests,
or certified historical output until dated class-specific identifier evidence
covers the relevant fact or bar time.
