# Retained text-loop execution evidence

The exact retained fetch-loop.log now has a bounded two-pass audit. It is945,136B,
SHA2569288277652579c7ffd80de168fba66ccfc75c64e5946e7025ea57e90529ea9f3.
The previously pinned893B wrapper still matches. The audit completed1s with
27,037,696B peak under0.15GiB; no network or source-body file was opened.

There are847physical lines:517 structured guard-state records,6 wrapper-exit
records,302 fetch-progress lines and22 unclassified lines. The output retains
only whitelisted execution metadata; it does not copy unknown log contents.
Guard-state updates must not be counted as separate commands or HTTP attempts.

Eight terminal child records occur in this one interleaved log:

| Guard PID | Child PID | Status | Child seconds |
| --- | --- | --- | ---: |
|6344|18948|stopped_low_commit|189.5|
|17924|21724|stopped_low_commit|644.9|
|12492|22248|stopped_low_commit|148.6|
|9976|20636|stopped_low_commit|9.2|
|9476|2168|stopped_low_commit|39.9|
|10668|15440|stopped_low_commit|79.4|
|13276|22616|stopped_low_commit|854.1|
|16360|7132|completed|73758.9|

All name filing_text --phase fetch --reland. Recorded thread arguments include3
and5. The six wrapper exits report attempts1..6, first five137 then0, spanning
2026-09-30T00:34:58Z through22:02:14Z. They cannot close all eight child records.
Physical log order also interleaves independently admitted executions; it is not
a single producer's chronological attempt ledger. The last child was admitted
at01:33:00Z and its wrapper exit is22:02:14Z, consistent with the retained report
that execution continued long after the earlier3,303text-receipt checkpoint.

This improves named execution recovery but does not authenticate a complete
invocation inventory, missing pre-receipt attempts, original rate-lock identity,
or the25,762baseline's constituent interval. Neither517 state updates,8 terminal
records,6 wrapper exits nor38,678declared receipt requests are interchangeable
all-attempt totals. No remaining budget follows. The60,000ceiling and HTTP hold
remain; no source or release gate is promoted.

Evidence: sec-text-loop-metadata-audit-v1.py/.json and its exact successful
logs/sec-text-loop-metadata-audit-v1.receipt.json. Only the named wrapper and
stdout log were accessed, without scanning other raw objects or ledgers.
