# LR1 independent root review

No Critical or Important findings. The probe opens only an in-memory DuckDB at
64MB/one thread, compares escaped builtin strings and microsecond UTC timestamps
within one connection, and records Windows private bytes plus module imports at
each boundary. Literal execution precedes the first binding. All equivalent
results match; the first bound string loads pandas/NumPy and adds 548,102,144
private bytes. Later binding and close do not release that baseline in the probe.
The guard independently records 0.546215GiB native peak under 0.75GiB.

The report correctly separates private bytes, working set and process-tree peak.
It does not subtract this delta from earlier peaks and call that a measured live
result. The evidence supports a narrow, validated literal-only recovery change;
it does not imply savings for source loaders that already need pandas. No
warehouse, source archive or production mutation was involved in this diagnosis.
LR2 live recovery still requires its own focused contract and actual acceptance.
