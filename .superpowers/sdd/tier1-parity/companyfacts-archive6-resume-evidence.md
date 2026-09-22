# Archive6 verified resume observation

## Terminal update and recovery — 2026-09-21 22:28 UTC

Archive6 is now terminal. Its memory guard stopped the owned job at22:23:30UTC
when free physical memory reached1.4619865GiB; free commit was5.913353GiB.
The same exec session returned terminal and the original guard/worker/redirector
were confirmed absent. This is not evidence of an application memory-cap or
source failure. The prior live observations below remain historical only.

Read-only inspection22:25:59UTC proved43,781,769facts and matching points,
31,959,271prices and31,934,514custom rows. Actual archive6 dataset UUID:
`7da8bd67-de3a-4fe6-a9bc-08a7a7d4cce7`. Its235newly processed issuers retained
699,244attempt rows;1128empty/39unavailable/no source-error receipts. See
companyfacts-archive6-stop-inspection.json, whose fields omit raw params and
private contact information. Counts include replacements, not net additions.

After host headroom recovered, root closed only the two interrupted ledger
rows at22:27:11.401503UTC and checkpointed successfully. Recovery used the
unchanged1GB/one-thread connection under2GiB guard, peak1.391GiB. The raw
source tables were not changed by that operation. See the ledger-recovery JSON.

Archive7 subsequently began from that actual terminal predecessor under the
same guard settings; its6805receipt/four-run proof is in progress. The latest
continuation-queue.md owns its process and session identity. Do not reuse the
old archive6 handle or mistake its predecessor UUID for archive7's new UUID.

## Historical live observation

Observed 2026-09-21 UTC; source snapshot remains 2026-09-20. This is an
in-progress observation, not a completion or coverage claim.

## Progress beyond the interrupted predecessor

At22:16:01.496UTC the same archive6 process passed the previous stop:
7725/20390entries,6576loaded outcomes including6570verified reuse,1110empty,
39unavailable,0failed,11,062new attempt rows. At22:16:46.801UTC it reached
7750entries,6596loaded,1115empty,39unavailable,0failed and65,068attempt rows.
These rows include replacements and do not imply net warehouse growth.

Exec session10431 was confirmed live, and CIM independently confirmed worker
11024 with parent11544 and original21:54:46UTC creation time. Latest collected
guard headroom was3.112GiB physical and8.206GiB commit; limits unchanged.
This observation advances the production load but does not establish terminal
success, full-universe coverage or readiness for downstream publication.

## Recovery and resume verification

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
