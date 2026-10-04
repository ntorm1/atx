# Third task-owned scratch cleanup

Removed two exact, verified scratch directories after native source freeze ended:
`fundamentals_release_v1/_spill` (642,822,144 bytes) and
`_tmp/fund-release-extract-verify` (21,296,236 bytes). These held expired DuckDB
scratch and interrupted batch9/12/15 reconstruction peers, not source or admitted
financial units. Source archives, all85 extraction validation receipts, current
144 preparation units, candidate generations and foreign caches were retained.

Native PowerShell validated absolute whitelist paths and every ancestor/member
against reparse points, checked ended source-freeze evidence and absent matching
writers, and rejected source snapshot references to either target. It preserved
the exact snapshot, external admission and binding hashes before/after deletion.
The guarded cleanup completed in3s, peak214,700,032bytes, exit0 under0.3GiB.

Observed free-space gain during deletion was660,836,352bytes. The three cleanup
operations together observed4,748,378,112bytes recovered and removed5,346,118,090
logical bytes. C: had51,772,059,648bytes free immediately after this deletion.
The separate roughly20GB recovery since the earlier disk audit is not attributed
to our cleanup. Temporary aggregate sizes do not establish ownership: generic
spill directories, unfinished panel_goldready and other sessions' caches remain.

The persisted `logs/disk-cleanup-applied-v3.json` contains complete identities,
inventories and counts. The cleanup's final stdout summary showed null fields
because Select-Object did not expose ordered-dictionary keys; the saved record
and deletion checks are valid. No deletion was retried to fix that display.

After cleanup, the complete native fund source freeze passed again in42s at
174,542,848peakbytes. It admitted the same frozen source/144 preparation inventory
and preserved snapshot SHA c73f6293a5785c185567b01c443747aa2c7294614b45bfdaf2b8016ec9807a0c.
Financial batches remain unadmitted; cleanup safety is not economic acceptance.

Evidence: `disk-space-preflight-v3.json`, `disk-cleanup-apply-v3.ps1`,
`logs/disk-cleanup-applied-v3.json`, corresponding guard receipts/logs and
`logs/t16-fund-r19-freeze-after-cleanup-v1.*`.

Automatic approval review previously rejected removal of two interrupted test
cache directories (pytest-233/234) with "blocked by policy", without a more
specific reason. Those directories were left untouched and were not retried.
