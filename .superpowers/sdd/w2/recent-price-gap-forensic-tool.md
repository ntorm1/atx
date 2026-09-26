# Bounded recent price-gap source audit

New standalone `atx-engine/tools/audit_recent_price_gap.py` implements the
root-requested forensic extraction. It accepts an external full-source SHA256,
positive securityID, explicit inclusive/exclusive date bounds (maximum366 days),
and a fresh output JSON path in an existing directory. Only that output file is
written; no warehouse, cache, projection or role mutation occurs.

The exact captured source handle is hashed once, then decoded. Parquet footer
min/max statistics prune row groups by ID and date where available; otherwise
only eight columns are decoded in batches of at most65,536 rows:
`tradingDate`, `securityID`, historical `ticker_tk`, `close`, `volume`,
`cumulReturnFactor`, `returnFactor`, `totalReturn`. The ID/date mask precedes
record extraction. No QA, deduplication, terminal-event attribution, return
calculation, aggregate price statistic or strategy selection is performed.

JSON preserves source row-group/row offsets, field types, exact values and original
floating-point IEEE754 little-endian bits. Nonfinite and null values remain
explicit, without invalid JSON NaN literals. Row-group pruning decisions, source
hash, footer+trailer hash and canonical records hash are retained. A source extent,
mtime or file-identity change before publication refuses the audit. The external
pin authenticates the captured bytes against the caller's expected artifact, not
the vendor's historical economic truth.

Resource limits: finite max_seconds in (0,120], cooperative checks between1MiB hash
chunks and Arrow batches;16GiB source,32MiB footer,4,096 groups/100m source rows,
128MiB declared projected-uncompressed bytes per decoded group,10,000 matching
records and16MiB serialized output. Oversized inputs/results refuse without
truncation. Native Arrow decode/fsync are not independently interruptible, so this
is not an OS hard deadline or measured peak-RSS guarantee. Root retains process
supervision. Publication uses exclusive create and checked flush/fsync/close;
write failure is not reported as success and an existing output is never replaced.

After implementation, four synthetic owning checks passed in0.833s (Python3.12,
`-B -m unittest test_audit_recent_price_gap -v`): exact duplicate/null/nonfinite
and bit preservation; footer pruning and no-stat fallback; bad external pin,
record-budget refusal and no overwrite; empty requested window and bounded
deadline arguments. No real source payload was opened by this agent.

Example for root to run after import, using a **new** chosen output path:

```powershell
python -B atx-engine/tools/audit_recent_price_gap.py `
  --source C:/Users/natha/Downloads/TickerHistory3.parquet `
  --source-sha256 0ed96b2696f194deee0d297b51425d3daf96bbaf3b28030b614a34a6943abbae `
  --id 39621 --start 2020-01-02 --end 2020-01-11 `
  --out build-equity/recent-gap-39621-source-v1.json --max-seconds 120
```

The already observed missing row was reported by root, not reproduced here. This
tool supplies the next exact source-record receipt; it does not change the
duplicate quarantine rule or permit a fabricated liquidation mark.
