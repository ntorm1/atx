# SM2 independent review

Reviewer: root Codex,2026-09-25. One review; no Critical or Important findings.

The implementer owns only the resolver's unresolved-summary block and focused
tests/report. The SQL mapping lookup, filing clocks, input copy, row ordering
and all fact mutations remain unchanged. The same branch conditions retain
fully unresolved rows and archive entity-only failures; the existing
non-archive entity-only behavior remains. Every unresolved row increments the
same three counts. Dictionary insertion order preserves first CIK occurrence,
and its representative retains the first unresolved security/date, including
NaT. Column order and empty output shape remain compatible with the candidate
consumer. No new skip, source narrowing or relaxed source proof is present.

The focused real-DuckDB acceptance covers mixed PIT states, duplicates,
interleaved issuers, exact candidate metadata, missing availability, fallback
policy and empty outputs. Scoped Ruff and diff checks passed. Runtime results
belong in unresolved-summary-focused1 artifacts and the implementer report.

The allocation improvement is structural: one summary per CIK replaces one
record per unresolved fact plus groupby arrays. No measured full-source peak
reduction or reduced launch profile is inferred from this static review.
