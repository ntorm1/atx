# Archive5 verified resume checkpoint

Observed2026-09-21 01:15:50UTC. This is a live checkpoint, not run completion.
Sourcecd841f26; documentation-onlyHEADc1d9a948. Worker16900 and parent13004
created01:08:14.655607UTC /01:08:14.631974UTC. Same session35531 remains live.
Production is the sole runtime under3GiB process /1GB DuckDB /1thread limits.

Governed startup created warehouse.duckdb.pre-migrate.20260921-010817.bak,
12,125,220,864bytes, then reached companyfacts_load. Actual migration ledger
inspection waits for writer release; no backup hash was independently recomputed.

The loader's INFO log is STDERR, activation-companyfacts-archive5.err; stdout
.log is empty until the CLI emits its result. Log timestamps are local EDT;
add4hours for UTC. The receipt is activation-companyfacts-archive5-memory.json.

At01:14:35.252UTC the loader reported verified targets5792 /rows28,205,479.
Both fact and point fingerprint scans aggregated10,174identity groups. This
matches3,750ancestorloaded receipts plus2,042archive4loaded receipts, across
two terminal lineage runs. Raw source/point evidence survived FC1 unchanged.
Verification began01:09:04.886UTC (~330seconds to verified result).

At01:15:50.097UTC: processed725/20,390, loaded638 (verified reuse), empty83,
unavailable4, failed0, newly written attemptrows0. Zero here reflects reuse;
it is not zero retained warehouse data or completed source coverage. The new
dataset UUID has not been queried while the writer is active.

Disk free84,994,764,800bytes at01:15:21UTC. Continue watching the same job/logs
and guard. No competing DB/tests/probes, restart, memory escalation, backup
deletion, quality-condition flip, source completeness or parity claim.
