# R19: bounded, independently pinned extraction validation

Actual native source freeze v2 stopped after 1502.7 seconds because host commit
headroom fell to 0.3095 GiB. The child peaked at 647,417,856 bytes, below its
0.8 GiB limit; no cap event occurred. Full extraction validation never completed.
An identical retry would discard all progress and replay all 85 source batches.
The current implementation has no durable, independently admitted per-batch
source-validation boundary. This is an operational release defect, not evidence
that the extracted source or financial economics is correct or incorrect.

Ruling: use Task16's final cumulative fix round5/5 for bounded extraction
validation that survives process interruption. Each source batch must undergo
the same complete ZIP reconstruction and exact typed multiset/schema, member,
error and counter comparisons. Pin the actual archive, extraction plan, exact
batch payload/stats/receipt, relevant code and frozen validation contract before
work and recheck after it. An interrupted or partial attempt is never reusable.
Publish a receipt only after all checks succeed, atomically and without replacing
an existing immutable receipt. No whole-source PASS follows from one batch.

Reuse must require caller-supplied, independently reviewed expected receipt
identities, bound outside the mutable candidate directory. The controller will
admit receipts from successful guarded executions and pin the complete inventory
before consumption. A candidate's own hashes, marker files, output existence,
size/mtime or self-asserted success cannot authorize reuse. Recheck current actual
source/code/product bytes against the independently admitted validation identity.
Missing, duplicate, stale, abbreviated, tampered, or unpinned receipts refuse.
Do not create a circular binding digest between the source contract and its
validation receipt; explicitly separate source identity from admission identity.

All 85 extraction batches and 144 preparation units remain mandatory. Native
financial batch admission and full original financial gates remain unchanged.
No legacy financial batch import, source-economic certification, denominator
change, host guard change or return measurement is authorized by this ruling.
Unqualified candidates stay NOT_PRODUCTION_READY. Preserve an explicit forced
replay path for independent verification of receipt semantics.

Cost: additional immutable proof records and controller admission work, plus
source/code rehashing for each bounded command. This avoids repeating completed
expensive ZIP reconstruction after unrelated host pressure. The final independent
review must inspect the new trust boundary and operational interruption tests.
This is cumulative Task16 round5, not a new component cap. If load-bearing
findings survive that review, apply the existing cap rule; do not dispatch round6.
