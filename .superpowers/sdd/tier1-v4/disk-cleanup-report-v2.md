# Ended-job scratch cleanup

Removed 1,506,857,694 logical bytes from three exact, audited owned scratch paths.
Observed C: free-space gain during deletion was 1,506,844,672 bytes; free space
after deletion was 57,669,140,480 bytes. This is separate from the earlier cleanup
(3,175,142,016 logical bytes; 2,580,697,088 observed free-space gain). Together the
two operations removed 4,681,999,710 logical bytes and observed 4,087,541,760 bytes
of free-space gain. Other concurrent disk changes are not attributed to this work.

The targets were the ended universe admission's private DuckDB/spill directory
`_tmp/universe-release-r15_20261003_001` (1,505,532,113 bytes), the interrupted ZIP
reconstruction directory `_tmp/fund-release-extract-verify` (1,301,005 bytes), and
the ended fundamental source freeze's private `_spill` (24,576 bytes). They are
recreated scratch, not inputs or resumable admitted units. The cleanup audit found
no dependency references or reparse points and verified all144 current preparation
units and the candidate file identities before deletion.

Native PowerShell checked exact resolved whitelist paths, unchanged bounded file
inventories, ended guard receipts, absent writer processes and candidate hashes.
It preserved the two dead writer locks as immutable forensic files before removing
scratch: universe PID2676 and fundamentals PID23584. Their exact paths, sizes and
hashes are in logs/disk-cleanup-applied-v2.json. No original, extraction file,
admitted candidate, source archive, ledger or foreign cache was removed.

Guarded verification after cleanup passed in10.0s at116,121,600 peak bytes. All144
preparation units, all85 essential restored extraction statistics, both universe
year payloads/receipts and all candidate file inventories still match. The fund
candidate remains empty, with no snapshot or admitted financial unit. Neither
release has a final manifest. This is cleanup safety evidence, not source replay
admission or economic acceptance.

Evidence: disk-cleanup-audit-v2.py/.json; disk-cleanup-apply-v2.ps1;
disk-cleanup-verify-v2.py; logs/disk-cleanup-{audit-v2b,apply-v2b,verify-v2} receipts
and logs; logs/disk-cleanup-verification-v2.json. Audit9.0s/116,305,920bytes;
apply5.0s/97,071,104bytes, both exit0. The initial audit safely refused because
its ended-status allowlist omitted the failed first source attempt. The first
apply safely refused an active reused/temporary PID; a subsequent process check
found all original PIDs absent and unchanged checks succeeded. The transient PID
identity/cause is unproven. Both refused attempts changed no cleanup target.

User authorization is the explicit request to remove unnecessary artifacts and
temporary caches. Current sources and failure evidence remain protected.
