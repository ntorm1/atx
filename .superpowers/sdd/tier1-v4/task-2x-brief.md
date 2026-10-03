# Task 2x: Repair SC/BZX exchange mapping and rebuild affected stages

Owner-requested follow-up to Tasks 1/2. Read IMPLEMENTER.md and resume-contract.md. Ruling R4: execute within Task 2 fix round 1, before that task's scoped re-review, because this repair closes its I3 finding; one agent owns both tasks, with no other writer in the market lane. Own security_master.py, identity_v3.py, market_index.py, indexes.py, universe.py only where necessary and only if not foreign-dirty; corresponding tests/docs/stubs; task-2x-report.md. Never modify/rebuild foreign panel code/stage.

Problem: FINRA class SC (Nasdaq Capital Market) and BZX are missing from EXCHANGE_BY_CLASS. NULL panel exchange values propagate into share_exchange_history and market/index proxy eligibility, excluding valid lines; universe currently patches locally.

Deliverables:
1. Verify canonical FINRA/exchange meanings from existing source contracts or authoritative documentation; correct clean shared mapping.
2. Apply repaired mapping in the codes/eligible-line builders when the frozen panel has NULL exchange. Preserve GC1 clocks and documented T1-b qualification; use source evidence, never an unconditional default. Ensure market_index/indexes consume corrected mappings without requiring a panel rebuild. Keep one authoritative mapping where feasible.
3. Add offline regressions for SC/BZX inclusion, unsupported classes remaining unmapped, and index/universe alignment. Rebuild identity_v3 codes, market_index, indexes, then universe under memory guard <=1 GiB, sequentially within the combined Task 2/2x lane.
4. Before/after evidence: SC/BZX line and cell counts; NULL exchange counts by source class; all_common membership and index counts; SC representation in R2000p; unique keys; PIT/cutoff checks; inherited market/index/universe gates. No return checks >=2023-01-01.
5. Publish accurate docs/manifests and input hashes. Existing FAIL gates remain FAIL unless measured fixed. Record residual coverage/data-source limits and exact commands in report.

Review package must list specific commits, never BASE..HEAD. No shared registry edits before Task 17. Explicit-pathspec commits. Report all original Task 1/2 artifact provenance relevant to rebuilds.
