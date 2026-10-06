# P9 RESUME 7: root merge contract for the remaining wave-2 and wave-3 lanes (PM, 2026-10-05)

Root = `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. One root agent at a time; each step group is one fresh
agent, named in the PM's prompt with the lane(s), the approved SHA(s) and the next free build tag. Read first:
`resume4-dispatch.md`, `resume6-addendum.md`, `resume7-addendum.md`, this file, the newest `handoff-root-*.md` (the
state you start from), then for each lane: its report, review and re-reviews in the lane's pool sprint dir, its
section of `r7-dispatch.md`, and the rulings by grep of the lane id in `progress.md` (the ledger is the PM's: commit
it as it stands with your docs commits, never edit its lines).

## Order (as lanes become APPROVE; honour only these dependencies)

B2 redo before the "last items" rounds of C3b / SQL3 / OD3P; AL-CLOCK before COV-MV (W3-9); D3 (guards, kinds) before
D3W; T2 as a unit after B2's redo; everything else by readiness. A lane is mergeable only when the ledger has
`Task <LANE>: APPROVE` at a named SHA. Merge by that SHA: `git merge --no-ff <sha>`.

## Per merge

1. Start state: head, clean tree (only the PM's files and the four untracked PNGs), trial ledger 133 lines
   `27e40f9f`. Copy nothing by hand: the lane's records arrive with the merge.
2. Merge. Shared-tail conflicts (CMake lists, registries, `nav_rules.cpp` rows, parser entries, kind tables, kind
   fixtures, `SCHEMA_ONLY_FIXTURES`, test-file hunks of two lanes) are resolved by keeping both sides; list each. A
   conflict that needs a design choice stops that merge (report; go on with an independent lane if the prompt names
   one). Never `git checkout --theirs / --ours`, `git restore` or a copy-over to resolve (B2-14): edit the region.
3. EXE-CUR: one target-scoped build per C++ lane and preset through `scripts/research-build.ps1` (tags continue from
   the handoff's next free tag; Debug `equity-dev`, Release `equity-rel`; `-Canary` where the NAV or IC exe is
   built), exactly the targets the lane report names, 0 warnings. A Python-only lane needs no build.
4. The lane's named gtests on both presets; then pytest on the touched suites; then ROOT-1: the whole
   `scripts/tests` (seed 0; seed 1 too if the lane changed Python) and `atx-engine/tools/test_stage_chain.py`, in
   separate invocations. Known-red set: EMPTY. ENV-1 for exit 3221225794.
5. SQL2-CLS: the lane's new `atx.<name>/v<n>` literals into `classes.json` (one commit per merge), guard before /
   after, the store suite, the fixture chain digest.
6. The lane's root items ("What root must check at merge" in its review; "what root re-proves" in its report; the
   ledger's rulings for that lane): identity runs (flag absent: the parent's u / fit / w / NAV byte-identical to
   the references outside `producer`; bounded runner, `--seconds` <= 600, `--max-rss-mib 2048`; SEAL-ALLOW-3; compare
   by SHA and JSON path only, read no statistic), probes (C3-5 at AL-CLOCK's merge), flips, deletions after identity.
7. Records: a section in `root-wave2-merge-report.md` (wave-3 lanes: `root-wave3-merge-report.md`, created at the
   first of them), a row in `integration-log.md`, the round-2b facts the prereg needs from this lane (Appendix IV of
   `p9-prereg.md` names them), and a new `handoff-root-<group>.md` (code head, docs head, exes with tag / SHA-256 /
   source commit, known-red set, next free build tag, kept and deleted outputs, open questions). Code and docs in
   separate commits; trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
8. After a lane is merged and verified its pool's leftover build tree may be deleted by name (DISK-2); keep
   receipts and logs; never touch a lease.

A slip (typo, missing include, trivial conflict, a one-line doc fix a review asked root to make) is fixed in place
and logged. A design error, an identity miss, or a red you cannot attribute to a slip stops that merge: the merge
commit is NOT reverted or reset by you; write the handoff, report BLOCKED with the evidence; the PM returns the lane.
0 trials throughout. Never edit an expected hash.

Final message to the PM, under 200 words: STATUS; per lane merged: merge SHA, conflicts (count), build tags, gtest
and pytest counts, each root item holds / fails; code head, docs head, known-red set, next free build tag; any
permission refusal (exact command); open questions one line each. No logs, no statistic.
