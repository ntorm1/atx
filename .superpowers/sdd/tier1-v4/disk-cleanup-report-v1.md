# Owned scratch cleanup, 2026-10-04

Owner requested removal of unnecessary artifacts/caches to free disk space.
Exact path audit and native PowerShell deletion removed3,175,142,016 logical
bytes from four obsolete derived scratch targets. C: free space rose from
43,656,728,576 to46,237,425,664bytes during deletion: observed gain2,580,697,088.
Logical size and observed free-space change are distinct; the difference's cause
was not established.

Removed only the ended candidate runs' private UUID scratch databases/spills,
two completed universe diagnostic databases, and the obsolete unauthenticated
fundamentals_v11/_interrupted_work-r2-20261003T211353Z preparation. Current
authenticated preparation superseded the latter. Raw sources, extracts,
completed batches, candidate outputs/receipts, logs and foreign caches remained.

Audit bound the exact files/sizes/mtimes, checked no reparse points or current
preparation/candidate dependency references, and confirmed ended owner jobs.
Deletion rechecked the absolute owned paths/inventory and used Remove-Item with
LiteralPath. See disk-cleanup-audit-v1.json and logs/disk-cleanup-applied-v1.json.

Post-cleanup guard completed17.1s, peak115,576,832bytes, exit0. All144 current
preparation units reverified. Candidate2018 and2019 payload SHA/size match their
receipts, no unsealed year directories, no manifest or publication. The2019
receipt appeared before the second continuation stopped for host commit
headroom; byte verification does NOT prove normal native admission completed.
See logs/disk-cleanup-verification-v1.json and all audit/apply/verify receipts.

No blanket cache sweep. Future cleanup must again establish ownership, ended
processes, absence of live dependencies and preservation of source/audit evidence.
