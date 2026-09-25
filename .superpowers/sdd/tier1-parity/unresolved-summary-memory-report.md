# SM2: bounded unresolved CompanyFacts summaries

Status: accepted. Root's one independent review has no Critical/Important
findings. Scoped Ruff passed. All five focused cases passed in the serialized
guarded runtime slot, native peak0.593109131GiB under1GiB. The implementer ran
no runtime work and no production memory saving is claimed yet.

## Change and preserved contract

`resolve_company_facts_identifiers` previously allocated one Python dictionary
per unresolved fact, constructed a full unresolved DataFrame, and expanded three
grouped count arrays before immediately reducing that output to one row per CIK.
The summary now accumulates directly in an insertion-ordered dictionary keyed
by CIK, retaining only one representative row plus counters for each CIK.
Auxiliary summary state therefore scales with distinct unresolved CIKs, rather
than unresolved fact rows. The resolved fact frame and PIT lookup remain intact.

Every input fact still resolves at its own filing availability. Unresolved facts
are retained, archive current-identity fallback remains disabled, and non-archive
fallback behavior is unchanged. Summary order follows first unresolved CIK
occurrence; representative security and availability come from that CIK's first
unresolved row, including NaT, rather than an inferred earliest/latest date.
Duplicate facts contribute separately to security, entity, and fact counts.
Output columns and the empty-summary contract are unchanged.

Owned implementation scope is only the resolver's summary block in
`atx-db/src/atx_db/fundamentals.py`, the new focused test module, and this report.
No guard, source universe, complete resume proof, mutation transaction, or
identifier SQL changed. Other source-memory tasks are unaffected.

## Acceptance

The focused module uses the real identifier SQL with minimal 64MB/one-thread
DuckDB lookup tables, avoiding an unrelated warehouse bootstrap. It checks:

- Interleaved CIKs with security-only resolution, an older filing predating
  mapping availability, duplicate rows, a fully resolved row, and a missing clock.
- All seven facts, original input, order, values, clocks, per-fact identifiers,
  exact summary counts, first representative selection, and candidate metadata.
- An initial missing clock remains missing in its candidate summary; archive
  fallback is refused while the existing non-archive fallback policy remains.
- Fully resolved facts and empty input preserve their empty-summary schemas.

Root's focused command, when the single runtime slot is available:

```text
python -m pytest tests/test_companyfacts_unresolved_summary.py -n 0 -o addopts= -q
```

Evidence: unresolved-summary-focused1.log/.err/-memory.json and
unresolved-summary-memory-review.md. No repeat checks or review are due.
A static reduction in auxiliary allocation is not evidence of a lower
full-source native peak or a completed ingestion; production remains pending.
