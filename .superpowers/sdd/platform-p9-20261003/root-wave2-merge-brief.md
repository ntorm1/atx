# Root brief: wave-2 merges (P9)

Root = `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Wave-1 gate is met (known-reds only) at `3c9eeb01`.
The ledger `progress.md` (same dir) is authoritative: grep the lane ID for every ruling and merge note before a merge.
Per-merge recipe and standing rules are the wave-1 ones: `root-wave1-merge-brief.md` section "Per merge"; standing
rules in `status-3.md` "How the owner wants it run"; rulings in `resume-rulings.md`.

## Order (Ruling W2-ORDER)

Merge by readiness, not by fixed slot, honouring only the real dependencies:
SQL1 before SQL2; A3 (without its flip commit) before AL-SIG; D2 before AL-COMB (AL-COMB rebases first); T2 last and
as a unit; C2 and COV are based on `ad406718` (C1 in base); all others on `1239a5ff` and meet C1 at merge.
A lane is mergeable only when the ledger has `Task <LANE>: complete` (review or re-review APPROVE). Merge by the SHA
the ledger names, `git merge --no-ff <sha>`.

## Per merge

1. Read the lane's `task-<LANE>-report.md` (+ fix rounds) and review files in the lane's pool sprint dir; copy them
   into the root sprint dir (they are git-ignored in the lane; `git add -f` here).
2. Merge; resolve shared-tail conflicts (CMake lists, registries, kind tables) by keeping both sides.
3. One target-scoped build per C++ lane via `scripts/research-build.ps1` (tags continue `p9-1s`, `p9-1t`, ... then
   `p9-2a`...), exactly the targets the lane report names, 0 warnings under `/W4 /WX`. Lanes deleted their build trees:
   every lane's claims are re-proved here from the committed head.
4. The lane's named gtests, then pytest on touched suites (explicit paths; `scripts/tests` and
   `atx-engine/tools/test_stage_chain.py` in SEPARATE invocations).
5. SQL2-CLS: add the lane's new `atx.<name>/v<n>` literals to `classes.json` (one commit per merge) once SQL2 is in;
   re-run the fixture chain (digest `d32f7655...4e69` unless a literal row legitimately changes nothing digested).
6. Lane-specific root items from the ledger (identity runs, flips, deletions after identity, MR items).
7. Append a section to `root-wave2-merge-report.md` and a line to `integration-log.md`; commit code and docs
   separately; trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Do not commit `progress.md` (PM's).
8. After a lane is merged and verified, its pool's leftover build tree may be deleted (DISK-2); keep receipts.

A slip (typo, missing include, trivial conflict) is fixed in place and logged. A design error, an identity miss or a
new red outside the known-red set stops that merge: report to the PM, continue with independent lanes if any.
0 trials throughout: `build-equity/trials.jsonl` stays 133 lines sha `27e40f9f`. Never edit an expected hash.

## Known-red set during wave 2

M1a-RED x3 (two gtests resolved by A3's merge; the pytest `test_fields_are_the_rule_applied_to_the_registry` per its
ledger entry) and M1c-RED x2 Release (resolved by C2's merge). Wave-2 gate after the last merge: 0 failed, no
known-red exception.

## After the last merge

Suites as in wave 1 (Debug + Release where named), golden at 1 and 4 workers, canary goldens, P9-B0 again
(substitution list written first; cache hit/miss strings are volatile per M1d-NOTE-3), then W1-I2(b): re-pin lineage
parent specs to the wave-2 P9-B0 references in one ruled commit; D-COV (Ruling COV-7: Release tree, after Debug =
Release identity on the fixture); scoreboard; tick G-P rows; whole-wave-2 review follows.
