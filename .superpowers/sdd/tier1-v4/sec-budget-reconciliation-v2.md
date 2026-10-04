# Current SEC ledger chronology and unresolved shared budget

Root audited the exact20 ledger/receipt paths in the earlier Task9 inventory,
105,599,673bytes, with streaming bounded reads and two-pass file identities.
No source bodies, financial/return rows or network requests were read/issued.
Auditv1 passed11.1s/27,258,880bytes under0.15GiBguard. Observed alternate attempt
clock fields required a narrow revision: auditv2 additionally recognizes
granted_at/time and sums explicit per-receipt requests. It passed12.1s/
27,267,072bytes under the same guard. Both receipts ended0/no cap. Use
sec-budget-chronology-audit-v2.json as the latest record; v1 remains historical.

Current source declarations, not interchangeable request totals:

| Source | Current verified records | UTC interval | Meaning and limit |
| --- | ---: | --- | --- |
| earnings-release fetch ledger |108,494|2026-09-26 to09-27|Outcome records, no per-attempt fields; older than this goal's implementation work.|
| sec_events fetch ledger |17,103|2026-09-29 to09-30|Outcome records, hidden retries/redirects not counted.|
| sec_text receipts |38,433|2026-09-29 to09-30|All declare requests; sum38,678. Prior3,303 checkpoint is stale. Interrupted requests before receipt writes are not proved absent.|
| issuer-history attempt ledger |2,998|2026-10-03|Includes later attempts beyond2,921 issuer-history fetch outcomes; do not add the77 handcheck outcomes again.|
| notes pilot receipts |398|2026-10-03|Explicit requests sum398; complete producer/interrupt accounting is separate.|
| fundamentals depth |2|2026-10-03|Two attempt records, two response receipts and durable counter2 agree.|
| ratios handcheck receipts |40initial+240later|2026-10-03|Outcome rows; retained prior elapsed-time upper bound1,127 applies only to the named guarded jobs.|

The known paths had stable identities throughout each audit. This does not
certify unlisted stores, missing pre-receipt requests, lifetime activity or the
complete objective interval. The original25,762 plan baseline and conservative
30,352 arithmetic must not be treated as an authenticated current all-attempt
balance; text activity alone has grown substantially since its old checkpoint.
Nor should the108,494 historical earnings outcomes automatically be charged as
new requests in this goal. The exact intended baseline interval and complete
retry/redirect coverage remain unresolved. No remaining capacity is granted by
subtracting either figure from60,000; SEC acquisition remains held.

Next accounting work must establish the baseline's time/scope and closed
producer-specific all-attempt totals or conservative bounds, including interrupted
text workers, before a named new allocation. All source acquisition plans retain
their limits; this audit does not reset the cap or silently authorize a fetch.
The current text receipt volume is also a lead for later guarded landing
verification, not proof of38,433 unique complete filings or retained originals.
