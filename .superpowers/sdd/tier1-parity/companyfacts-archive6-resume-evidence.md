# Archive6 verified resume observation

Observed 2026-09-21 UTC; source snapshot remains 2026-09-20. This is an
in-progress observation, not a completion or coverage claim.

The user explicitly requested continuation after stopping archive5. Root
confirmed no surviving owned load, inspected the warehouse read-only, and
recovered only the two interrupted archive5 ledger rows with a successful
checkpoint at21:53:52UTC. Actual predecessor dataset UUID:
`6400b3c2-f0d1-4f47-bcf0-95aadd9241de`. See the committed archive5 inspection
and recovery JSON files; commit172cba97 includes those receipts and scripts.

Archive6 began21:54:46UTC. Exec session10431, native guard2944, child redirector
11544, native worker11024. CLI run ID is `activation-companyfacts-archive6`;
the new dataset UUID must be read from the actual ledger after terminal exit.
Do not substitute its predecessor's UUID. Only companyfacts_load is selected;
full archive_members, replacement enabled, verified predecessor resume, force,
DuckDB1GB/one thread, backup-keep100, dummy SEC UA. All existing source files,
backups and the user's stash remain intact.

The process cap was reduced to2GiB because available physical memory did not
meet the former3GiB cap's5GiB preflight. Existing cap+2GiB startup and1.5GiB
physical/3GiB commit runtime thresholds are unchanged. One workload only.
This is a tighter memory ceiling, not a measured claim of full-run capacity.

INFO lines in activation-companyfacts-archive6.err use local EDT; add four
hours for UTC. The verified sequence is:

- 21:54:50.973:6570loaded receipts across3ancestor runs.
- 21:57:41.980:fact fingerprint scan completed,10,743identity groups.
- 22:00:39.762:point fingerprint scan completed,10,743identity groups.
- 22:00:42.881:verified6570targets /31,008,510retained rows.
- 22:03:47.954:traversal1775/20390;loaded1514verified reuse, empty246,
  unavailable15, failed0, new attempt rows0.

The last collected guard sample reported4.866GiB free physical memory and
10.275GiB free commit headroom. The native worker was separately confirmed
live by CIM at22:01:26 with its original creation time; the same exec session
remained live in the22:03:47observation. The worker's earlier private-memory
sample was1.62GiB during proof; that sample is not the process-tree peak.

Continue this live job. On actual terminal exit inspect receipts and ledgers,
then follow production-resume-sequence-2026-09-21.md (commit227187a0) for the
verified submissions resume, full run5, separate snapshot-pinned custom-signal
evaluation, measured coverage/quality and eligible release. No full suite,
whole-branch gate review, main merge, release or significant-alpha claim has
been made at this checkpoint.
