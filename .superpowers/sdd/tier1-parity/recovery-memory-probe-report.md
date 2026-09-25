# LR1 — scalar-binding memory diagnosis

## Result

The hypothesis is supported on the installed DuckDB 1.5.5 runtime. A first
builtin-string parameter binding loaded pandas and NumPy and increased Windows
process private bytes by **548,102,144 bytes (0.510460 GiB)**. Equivalent literal
SQL did not import either library. This is measured scalar-binding overhead;
it is not a full recovery or source-ingestion memory measurement.

One authorized in-memory diagnostic completed at 2026-09-25 00:08:23 UTC under
the existing memory guard: **0.546215 GiB native process-tree peak**, below the
0.75 GiB cap. DuckDB used 64 MB and one thread. The guard's start headroom was
2.818878 GiB physical and 4.367706 GiB commit. No warehouse, source archive,
persistent database, pandas API, tests, or full table was opened. The runtime
slot was returned immediately after the terminal receipt.

## Evidence

| Step | Private bytes | Private GiB | pandas modules | NumPy modules |
|---|---:|---:|---:|---:|
| Before DuckDB import | 10,481,664 | 0.009762 | 0 | 0 |
| After DuckDB import | 21,180,416 | 0.019726 | 0 | 0 |
| Connection and literal session setup | 25,280,512 | 0.023544 | 0 | 0 |
| Literal VARCHAR and TIMESTAMP result | 25,583,616 | 0.023827 | 0 | 0 |
| First builtin-string binding | 573,685,760 | 0.534286 | 293 | 100 |
| Builtin datetime binding | 573,685,760 | 0.534286 | 293 | 100 |
| Combined string/datetime binding | 573,685,760 | 0.534286 | 293 | 100 |
| Connection closed | 573,685,760 | 0.534286 | 293 | 100 |

Windows `GetProcessMemoryInfo` supplied process private/working-set bytes via
stdlib ctypes. The separate guard supplied native process-tree peak memory.
These are different scopes; private-byte snapshots are not peak or physical
resident-memory measurements. The first string bind increased working set from
39,616,512 to 102,359,040 bytes; its private-memory impact was substantially
larger. Closing the DuckDB connection did not release the imported runtime's
private-memory baseline during this short probe.

Literal and bound results were exactly equal for builtin strings (including an
escaped apostrophe), an aware UTC Python datetime cast to TIMESTAMP, and both
values together. The timestamp retained six-digit microsecond precision under
the UTC session setting. No external identifier or email was used.

Artifacts:

- `recovery-memory-probe.py`: the single diagnostic, recording snapshots after
  each operation so partial evidence would survive a later guarded stop.
- `recovery-memory-probe1.json`: completed comparisons, module observations,
  memory samples, installed DuckDB version and timestamps.
- `recovery-memory-probe1-memory.json`: terminal guard receipt, return code 0.
- `recovery-memory-probe1.log` / `.err`: diagnostic output and empty stderr.

## Narrow recovery adaptation supported by the evidence

The archive18 recovery currently binds a source-run UUID for two read queries
and binds UTC time, integer row count, reason text, and UUID for two ledger
updates. It imports no pandas itself. A narrow literal-only adaptation can
preserve its complete row scans, scope checks, counters, exact running-ledger
predicates, one transaction, output evidence and checkpoint while avoiding the
observed first-binding import.

Validate the internally recovered UUID before embedding it. Escape every SQL
string by doubling single quotes, including the generated reason. Validate and
format only the builtin integer count as a numeric literal. Use a typed
TIMESTAMP literal generated from the same aware UTC `now` value at microsecond
precision, retaining the explicit UTC session. Do not replace these narrow
internal values with arbitrary string-built user SQL or weaken predicates.

This recommendation does not establish the live recovery's new peak, and the
0.510460 GiB difference must not simply be subtracted from an earlier native
peak and reported as measured. The adapted script still needs guarded live
acceptance. It also does not promise savings for source loaders that already
require pandas; this result concerns narrow operational recovery scripts.

No production or recovery implementation was edited during LR1.
