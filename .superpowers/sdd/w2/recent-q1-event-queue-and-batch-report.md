# Q1 missing-row event queue and batch forensic extension

This is **EXPOST diagnostic QA only**, read from the pinned role manifest, session/ID axes, and presence/member masks. No prices, returns, source Parquet, warehouse or performance were read. No eligibility, universe or candidate was changed. The purpose is to prioritize independent corporate-event evidence before another strategy run.

Manifest SHA256: `900839a1ea8e21edc0f5edd5e9cd8f2bc7884a9295a86d5d6f2de19a79aed36b`. All four small payload hashes were reverified. Role score sessions run2020-01-02 through2020-03-31.

For each ID that was both a member and physically present on a scored date, find its first subsequent missing source row inside the score window. Earlier membership exit does not remove possible held-mark risk. This deliberately broad queue does not assert that any strategy actually held the ID.

There are **38 IDs:6 first gaps recover later within Q1;32 have no later Q1 print**. No-later-print is role-end censoring, not proof of delisting, bankruptcy, cash acquisition or zero recovery. A later print also does not prove uninterrupted trading. ID2003601 has a temporary first gap and a separate trailing absence from2020-03-30; all other temporary-first-gap IDs print on the final role date.

| ID | Previous print | First absence | Next role print | Missing sessions in first gap | Member on first absence |
|---|---|---|---|---:|---|
| 33764 | 2020-01-03 | 2020-01-06 | none through03-31 | 60 | yes |
| 39621 | 2020-01-03 | 2020-01-06 | none through03-31 | 60 | yes |
| 2752722 | 2020-01-08 | 2020-01-09 | none through03-31 | 57 | yes |
| 4997008 | 2020-01-09 | 2020-01-10 | none through03-31 | 56 | yes |
| 4677674 | 2020-01-14 | 2020-01-15 | none through03-31 | 53 | yes |
| 33551 | 2020-01-15 | 2020-01-16 | none through03-31 | 52 | yes |
| 5747162 | 2020-01-22 | 2020-01-23 | none through03-31 | 48 | yes |
| 160964 | 2020-01-23 | 2020-01-24 | none through03-31 | 47 | yes |
| 383531 | 2020-01-27 | 2020-01-28 | none through03-31 | 45 | yes |
| 5178122 | 2020-01-30 | 2020-01-31 | none through03-31 | 42 | yes |
| 4166548 | 2020-01-31 | 2020-02-03 | none through03-31 | 41 | yes |
| 228112 | 2020-02-03 | 2020-02-04 | none through03-31 | 40 | yes |
| 3951140 | 2020-02-05 | 2020-02-06 | none through03-31 | 38 | yes |
| 428680 | 2020-02-06 | 2020-02-07 | none through03-31 | 37 | yes |
| 5804722 | 2020-02-07 | 2020-02-10 | 2020-02-11 | 1 | yes |
| 1272388 | 2020-02-10 | 2020-02-11 | 2020-02-12 | 1 | yes |
| 4159499 | 2020-02-10 | 2020-02-11 | none through03-31 | 35 | yes |
| 4438373 | 2020-02-13 | 2020-02-14 | 2020-02-20 | 3 | yes |
| 4335207 | 2020-02-19 | 2020-02-20 | none through03-31 | 29 | yes |
| 5122670 | 2020-02-20 | 2020-02-21 | 2020-02-26 | 3 | yes |
| 3984255 | 2020-02-21 | 2020-02-24 | none through03-31 | 27 | yes |
| 273222 | 2020-02-27 | 2020-02-28 | none through03-31 | 23 | yes |
| 1836262 | 2020-02-28 | 2020-03-02 | none through03-31 | 22 | yes |
| 67851 | 2020-03-04 | 2020-03-05 | none through03-31 | 19 | yes |
| 5884670 | 2020-03-04 | 2020-03-05 | 2020-03-13 | 6 | yes |
| 4354916 | 2020-03-06 | 2020-03-09 | none through03-31 | 17 | yes |
| 2373253 | 2020-03-12 | 2020-03-13 | none through03-31 | 13 | yes |
| 4095961 | 2020-03-13 | 2020-03-16 | none through03-31 | 12 | yes |
| 2003601 | 2020-03-17 | 2020-03-18 | 2020-03-24 | 4 | no |
| 4380194 | 2020-03-18 | 2020-03-19 | none through03-31 | 9 | yes |
| 4623063 | 2020-03-23 | 2020-03-24 | none through03-31 | 6 | yes |
| 4625325 | 2020-03-24 | 2020-03-25 | none through03-31 | 5 | no |
| 398524 | 2020-03-26 | 2020-03-27 | none through03-31 | 3 | yes |
| 33767 | 2020-03-27 | 2020-03-30 | none through03-31 | 2 | yes |
| 4614249 | 2020-03-27 | 2020-03-30 | none through03-31 | 2 | yes |
| 4657753 | 2020-03-27 | 2020-03-30 | none through03-31 | 2 | yes |
| 5094125 | 2020-03-27 | 2020-03-30 | none through03-31 | 2 | no |
| 5094126 | 2020-03-27 | 2020-03-30 | none through03-31 | 2 | yes |

The complete record includes earlier eligibility and trailing-gap dates in `recent-q1-event-queue.json`, canonical LF SHA256 `6ec5e55c73b73ff980b86e2a48ff010d6018c44e86d801572acf2e8b9df79499`. `audit_recent_role_event_queue.py` reproduces the masks-only extraction. The old narrower d+1/d+2 audit counted only near-term gaps after an observed eligible decision; this broader38-ID queue also captures later held-mark risk and score-tail censoring, so the denominators differ.

## Optional batch forensic tool

`audit_recent_price_gap.py` now supports mutually exclusive `--id ID` (unchanged single-ID format) or `--ids ID...` (1..64 unique positive int64 values). Batch output has its own `atx.recent-price-gap-batch-audit/v1` schema, sorted requested IDs and exact `record_counts_by_id`, including zero matches. It still performs one streaming source hash and one projected row-group traversal, preserving original row order, duplicates and IEEE bits. Footer pruning retains a group if its ID range could contain any requested ID. The10,000-record/16MiB budgets apply to the whole batch, not per ID; exhaustion refuses rather than truncates. Existing120-second cooperative checks and exclusive output rules remain.

Implementation preceded two additional synthetic checks. All **6/6 checks passed in0.502s**: batch single-ID record parity, exact per-ID/zero counts, one whole-source digest, invalid/duplicate/>64 IDs, and global record-budget refusal join the original four checks. No real batch source run was performed by this agent.

Suggested root-run selection, with the existing external source pin and a new output file:

```text
--ids 33764 39621 2752722 4997008 4677674 33551 5747162 160964 383531 5178122 4166548 228112 3951140 428680 5804722 1272388 4159499 4438373 4335207 5122670 3984255 273222 1836262 67851 5884670 4354916 2373253 4095961 2003601 4380194 4623063 4625325 398524 33767 4614249 4657753 5094125 5094126
--start 2020-01-01 --end 2020-04-01 --max-seconds 120
```

The forensic tool only supplies source records and historical symbols. Independent event evidence and its actual availability must determine event handling; neither this retrospective queue nor later recovery is a permissible universe filter.
