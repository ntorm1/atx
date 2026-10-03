# Task5 bounded recovery checkpoint v4

Supersedes v3 scheduling only. Root has no live data job. Preparation remains
144 authenticated units; legacy parts now have completed batches0000 and0001
of the full85-batch inventory. No final v11 publication or real financial gate
measurement exists; legacy cached-batch admission remains unqualified.

After fresh host headroom evidence, root attempted one new-only batch0001.
First wrapper refused the existing history-0001.parquet.partial before computing
or overwriting it (guard2s/.0566GiB). It was preserved by native PowerShell
Move-Item after exact resolved source/destination checks inside fundamentals_v11;
before/afterSHA was identical. Destination:
fundamentals_v11/_interrupted_batch_0001-20261003T2317Z/history-0001.parquet.partial.
No existing receipt or published stage was overwritten.

The preservation receipt's original size/timestamp fields are invalid: its
FileInfo was accessed after moving the source path. Original receipt is retained.
Independent guarded recheck logs/t5-preserved-batch-0001-audit-v2.json confirms
4bytes, SHA fbc62d3b511368ee275ddc74117d8689b430e1427220e25d30816201d89ca7b6,
observed preserved mtime2026-10-03T22:51:16.381862Z. This is filesystem evidence,
not a financial publication clock. The audit's first attempt rejected the
PowerShell UTF8 BOM; a utf-8-sig retry completed, both logs preserved.

New-only compute wrapper t5-new-batch-0001-v1.py verified all retained source/
target extract identities via reviewed reuse_extract and verified preparation
before and after native F.run_batches((1,1)). It refused any prior0001receipt or
partial and any stage-root publication. Guard logs/t5-new-batch-0001-v2.*:
97.4s, peak.5538GiB under.8cap, exit0; minfreecommit.8014GiB, no cap/host stop.
Native compute53.45s:99CIKs,690783factrows,4048events,141419historyrows.

New output identities in logs/t5-new-batch-0001-v1.json:
- events5931db2489bd3550f9c21ef14fcbc968dd0bd69e9c3f8ef092f62c7621652e18
- cataloge09e58058e6ff47e05697d9fd8baea58f295b29c825767c39989e7b34dc00acd
- historya0865066549b896fe2bbf0eab73c565d15bfab113db470309da731da10986749
- sic2b7d9c8e4cc0b696b7fd935565c3d47d7dc09517f2343660360b6bcd260357ae
- native receiptb1300df1bc3528e6c23bc7b3819750ac145523236ab1f7f9701d79d7dbd67fb8

Status NEW_BATCH_COMPUTED_NOT_ADMITTED. This operational evidence does not repair
legacy _batch_done or independently certify row keys/source/clock semantics.
Task16 independent batch-generation admission design is assigned to
/root/t6_factors under R15 after its own Task6 scoped code approval. No Task5
producer edits/sixth local fix. Existing completed parts cannot be retroactively
blessed by hashing; proposed new immutable generations must compute with full
source/code/preparation provenance and preserve source-backed v11 semantics,
including both logical stage name and physical leaf-name checks. No new native
finalize/cache reuse is granted pending reviewed integration.
