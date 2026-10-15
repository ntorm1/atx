# P9 sprint: PM handoff 12 (owner stop 11, 2026-10-15)

For the next parent (PM) agent. Supersedes `status-11.md`. The goal prompt to start the next session is in
`next-goal-prompt.md` (same dir, refreshed at this stop). Written at the owner's stop ("stop here and write a status
file", 2026-10-15 14:27, while root M6b was running its post-merge checks). The one hand-back that had not arrived when
this file was written is marked (pending) and its handoff file is the truth. Verify every head with `git log` before
dispatch. The ledger `progress.md` block from "- RESUME 11" (line 850) to the end records every hand-back and ruling
of this session.

## Read order

1. This file.
2. The contracts every agent gets: `resume4-dispatch.md`, `resume6-addendum.md`, `r6-wave3-common.md`,
   `resume7-addendum.md`, `resume8-addendum.md`, `resume9-addendum.md` (TEST-1), `resume10-addendum.md` (owner rules,
   LEASE-DEAD, HOST-4, HOST-5, REV-FRESH), `resume11-addendum.md` (wins where they differ). Per-agent text:
   `r11-dispatch.md` (ROOT M5 / M6 form, the lanes of this session, "WAVE REVIEW", "ROOT GATE" + "ROOT GATE: rulings
   from the whole-wave review": the gate's dispatch text is READY there). Reviewers: `r7-review-dispatch.md` +
   `r8-review-notes.md` + `briefs/review-template.md`. Root: `r7-root-merge.md` + `root-gate-brief.md` (GATE-1).
3. The ledger `progress.md` from "- RESUME 11" to the end. Every "Ruling" line is binding; this session's rulings are
   listed at the end of this file.
4. `handoff-root-m6.md` (root after M6: code head `d51d011e`), `handoff-root-gate0.md` (the gate's step-0 slips done:
   code head `7320e1b9`), `handoff-root-m6b.md` (pending at the stop: SQL4 merged at `06dc9e5a`; its checks were
   running; the file says what ran and what is left).
5. The five whole-wave review files `wave-review-{fields,store,nav,ic,python}.md` (committed at `22f99018`) and
   pool-32 `task-SQL4-review.md` (the SQL4 range's review); their "What root must do before any cell" lists are folded
   into `r11-dispatch.md` "ROOT GATE: rulings".
6. `p9-prereg.md` (round 2b-i; 2b-ii after the gate: the ledger's "PRE 2b-ii list" lines, now five), `p9-prereg-flags.md`,
   `wave3-carry.md`, `root-gate-brief.md`.

Plan: `C:/atx-wt/pool-2/docs/plans/2026-10-03-p9-sprint-plan.md`. Root tree `C:/atx-wt/pool-2`, branch
`feat/platform-p9-20261003`. Trial ledger `build-equity/trials.jsonl`: 133 lines, sha256 prefix `27e40f9f`, unchanged
(0 trials this session). `build-equity/research-store/` absent (SQL3-2).

## State in short

EVERY LANE OF WAVES 2 AND 3 IS MERGED OR CLOSED. Root this session: M5 (B2 fix 2 `101f695a`, OD3P follow-up
`4a7372e2`, D3W-PB chain to `44a331e6`, SIGN chain to `e2844926` + SQL2-CLS row, A4 pre-flip `6dab6948`), M5c (A4
flips P10: no copy of their TRAIN inputs outside atx-db), M6 (AL-CLOCK fix 1 `eed512bd`, SQL3 item 5 `22b8fd17`),
GATE-0 (the gate's step-0 slips: STORE-M1 classes guard + row; G-P5 PARTIAL noted; the five review files committed),
M6b (SQL4 `44a76937` merged at `06dc9e5a` 14:17; post-merge checks pending at the stop). Code head at the stop
`06dc9e5a` (M6b's merge commit; read `handoff-root-m6b.md` for whether its checks passed and for any slip after it);
docs head `89b9644d` (M6b stop records: handoff-root-m6b.md; the SQL4 merge and its build p9-3e done, NONE of the post-merge checks run: the next root agent resumes from its "Exact next step"). Known-red set EMPTY since M5 (B2 fix 2). Next free root build tag:
`p9-3e` was M6b's (see its handoff); then `p9-3f`.

The whole-wave review (GATE-1 step 8) RAN EARLY (WAVE-REV-EARLY) on `3c9eeb01..d51d011e`: fields APPROVE, Python
APPROVE, NAV / store / IC CHANGES_REQUIRED with NO code defect in any merged lane; every Required became a gate-procedure
ruling (PRE-MVX, GATE-COVDIFF, D3-Q1a, GATE-EXES) or a root slip done at GATE-0 (STORE-M1). SQL4's range is covered by
its own first review (SQL4-ADD).

NOT in P9 (P10 whole): A4's flips `69141218`, `59d807ea`, the registry rows of `fc3e414e`, and the A4 fix-1 tests
(`916f837f`, reviewed APPROVE at `87df9e00`): A4-DB (atx-db never opened), A4-P10-FLIPS, A4-FIX1-P10 (a permission
refusal blocked the re-apply; the owner can cherry-pick by hand, see "Open with the owner"). Python stays the producer
of record for `ftd_shares_ratio21` and `regsho_threshold_days63`; Y-F0's fields manifest `26fee5ce` is unchanged.

No gate run, no P9 cell run, no OD-3 read. The prereg is at round 2b-i.

Host at the stop: free RAM about 2.5 GB of 15.7; C: about 57 GB free (lanes freed about 6.6 GB of object trees this
session); every pool lease prints `owner=dead` except pool-32 (`owner=alive`, the PM's SQL4 lease of 2026-10-08):
LEASE-DEAD: untouched. The owner had `C:\atx-wt\pool-32\atx-impl\src\strategy_ic_signal_cache.cpp` open in the editor
at the stop (SQL4's signal cache TU; merged in root at `06dc9e5a`).

## Lanes at the stop

| lane | pool / branch | head | state | next |
|---|---|---|---|---|
| waves 2 + 3 (all) | merged | root `06dc9e5a` (M6b merge) | complete except M6b's checks | gate |
| SQL4 | pool-32 `feat/p9-sql4-20261008` | `44a76937` APPROVE (code `c5df575d`) | merged at `06dc9e5a`; M6b checks (pending) | root finishes M6b's checks from `handoff-root-m6b.md` if any is left (named filters 79, catalog 17, SQL2 full filter, tripwires, fake identity, the real X-5 u / w + marginal shard identity, `verify --ledger other-ledger 0`, SQL2-CLS); a red = that merge BLOCKED (not reverted) and SQL4 P10 at the gate |
| AL-CLOCK fix 1 | pool-30 | `eed512bd` APPROVE | merged at M6 (`1d003241`) | gate items in `handoff-root-m6.md` |
| SQL3 item 5 | pool-13 | `22b8fd17` APPROVE | merged at M6 (`d51d011e`) | - |
| A4 | pool-27 `feat/p9-a4-20261005` | `87df9e00` (fix-1 re-review APPROVE on `528beddf`) | pre-flip `6dab6948` merged at M5; flips + fix 1 P10 | owner: cherry-pick by hand if wanted (P10) |
| B2 fix 2, OD3P follow-up, D3W-PB, SIGN | pools 14, 21, 23, 29 | `101f695a`, `4a7372e2`, `44a331e6`, `e2844926` | merged at M5 | - |
| whole-wave review | ROOT-SDD `wave-review-*.md` | committed `22f99018` | done | the gate root agent acts on "ROOT GATE: rulings" |
| PRE | sprint dir `p9-prereg.md` | round 2b-i | 2b-ii after the gate: the ledger's five "PRE 2b-ii list" lines + the round-2b-ii facts of the gate report | review by a fresh agent, then the PM rules the file |

Frozen pools (TEETH-POOL): pool-8 and pool-24 unchanged, read-only. Pool-23 `build-equity/` (mutated teeth
binaries, TEETH-BIN-23) unchanged.

## Open with the owner (surfaced; none blocks engineering)

1. **Permission refusal (R11, #1; PERM-1 held, not retried, not substituted):** the A4 fix-1 re-apply, exact
   command: `cd C:/atx-wt/pool-27 && rtk proxy git status --porcelain | wc -l && git checkout -b feat/p9-a4-fix1-20261015 bba4c06f 2>&1 | tail -2 && git log --oneline -1 && git cherry-pick 916f837f 2>&1 | tail -3; echo "cp1=$?"; git log --oneline -2`.
   The re-apply by hand (if wanted; P10 otherwise): a branch from `bba4c06f`, cherry-pick `916f837f`, `528beddf`
   (conflict in `handoff-A4.md`: take the lane's file whole), `87df9e00`; then a scoped root merge with one Debug build
   of `atx-engine-research-fields-tests` and the filter 32. Carried refusals of earlier sessions: unchanged (status-11).
2. **A4 flips (P10):** the engine builders for `ftd_shares_ratio21` / `regsho_threshold_days63` are merged but
   unproven on real TRAIN inputs because those inputs live under `C:\atx\atx-db\data\alpha_panel\v1\{ftd,regsho}\`
   (paths and SHAs in `m5-logs/a4-provenance.txt`). If the owner allows a read-only identity run against atx-db, the
   flips can land in P10's first root group.
3. **Frozen pools 8 and 24, leases `owner=dead`, `C:\atx\data` (DATA-3), `atx-db-request.md` (AL-DATA):** unchanged.
4. **Rulings the owner may want to look at:** PRE-MVX (if P9-MV is accepted, P9-X runs on P9-F0h, the last accepted
   book without `--risk-model`; a two-store NAV is P10), SQL4-CUTOFF (met: SQL4 is in), WAVE-REV-EARLY (the whole-wave
   review ran before the gate head was final; SQL4's range covered by its own review), A4-DB / A4-P10-FLIPS /
   A4-FIX1-P10, D3-Q1a (the engine-fit identity strips `provenance.engine_fit`), GATE-EXES, GATE-COVDIFF, STORE-M1,
   IC-S (G-P5 prints PARTIAL: four Python / C++ rule pairs unlisted, P10 retirement).
5. **Owner items for the final report (unchanged + new):** OD5-2 / OD5-3 / OD5-5 / OD5-7, AL-DATA, SCRATCH-2, the
   broken `dev` preset, SPDLOG-RELINK (a SQL4 test-TU check relinked the shared spdlog objects), the P10 list (now
   also: ALCLOCK-S2, SQL3-S2, NAV-S, STORE-S, IC-S, PY-S, FIELDS-S, SQL4-S, A4-FIX1-S, A4-RR1-S, NAV-2STORE,
   STORE-JSON-P10).

## Gates and rules the next PM must hold (new this session; status-11 lists the earlier ones)

- HOST-4 (R11 order: root > lanes > reviewers), R11-RESUME, R11-GOAL, SQL4-CUTOFF (met), WAVE-REV-EARLY, SQL4-ADD,
  DISPATCH-COMMIT (root commits the PM's dispatch-doc edits; the PM never commits in pool-2).
- The gate (GATE-1) runs on the M6b head with every merge in; its step 0 is DONE (GATE-0); `r11-dispatch.md`
  "ROOT GATE" + "ROOT GATE: rulings from the whole-wave review" is the dispatch text: fill in the gate head and the
  next free tag. EXE-FREEZE from the re-pin; GATE-EXES adds the two research exes to the frozen set.
- PRE-MVX, D3-Q1a, GATE-COVDIFF, COVMV-S2 S-b + NAV-S S-2 (the `mv-aim-v1.json` sentence at the re-pin iff not pinned),
  STORE-M1 (done), IC-S (G-P5 PARTIAL line), PY-S S-6 (the `ledger_n` vs verb N hand comparison at the scoreboard
  step), SQL4-S S-1 / S-2 / S-3 / S-6 (root's checks at M6b / the gate), SQL-11 (no cache flag on any cell argv).
- Known-red set EMPTY; the Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` named skip stays.
- Everything of status-11 and earlier: LEASE-DEAD, HOST-5, REV-FRESH, MERGE-SHA, TEETH-BIN-23, TEETH-POOL, PERM-1,
  TEST-1 / ROOT-1a, COMPOSE-1, MV-FLAG-P0, B2-N37, TEETH-CRLF, ledger appends by scratch file + `cat >>`, never edit
  earlier lines.

## Next steps, in order

1. Verify the stop state: `git log` / `git status` in pools 2, 32 (and 13, 14, 21, 23, 27, 29, 30 unchanged; 8 and 24
   frozen); read `handoff-root-m6b.md`; trial ledger 133 `27e40f9f`; `lease-worktree.ps1 -Status` read-only.
2. Finish M6b's checks if its handoff leaves any (a fresh root agent from "Exact next step"); a red attributes to the
   SQL4 merge: BLOCKED, SQL4 P10 at the gate (the merge is not reverted; the gate head is then taken as it stands with
   the finding stated), per MERGE-SHA / GATE-1.
3. The gate (GATE-1): one fresh root agent (or a chain through `handoff-root-gate-<n>.md`) with `root-gate-brief.md`
   and `r11-dispatch.md` "ROOT GATE" sections; the gate head = root's code head after step 2; tags from the next free
   tag. Step 8 is done (the five files + SQL4's review); root acts on the rulings list at step 0 (already done except
   what M6b adds).
4. PRE round 2b-ii (Appendix IV values from the gate report + the ledger's five "PRE 2b-ii list" lines + PRE-MVX's
   B.7 / B.8 / E lines + PY-S S-4's note), a review by a fresh agent, the PM rules the file.
5. Cells in order: P9-L, P9-H, P9-M, P9-C, P9-MV, P9-W, P9-X (parent P9-F0h, PRE-MVX), P9-V (lapses per PRE-14 /
   PRE-35 / D3-Q1a / COMPOSE-1; P9-X per PRE-30 / PRE-34); adoption print; OD-3 once on P9-F0h (MV-FLAG-P0 at P0);
   freeze gate (plan 4.4 / prereg G); the final report with every ruling.

## Rulings made this session (full text in the ledger from "- RESUME 11")

Process: HOST-4 (R11 order), R11-RESUME, R11-GOAL, SQL4-CUTOFF, WAVE-REV-EARLY, SQL4-ADD, DISPATCH-COMMIT,
STORE-JSON-P10. Lanes: ALCLOCK-S2, SQL3-WQ-2, SQL3-S2, A4-DB, A4-FIX1-CHAIN, A4-FIX1-S, A4-FIX1-REAPPLY, A4-RR1-S,
A4-FIX1-P10, SQL4-XLANE, SQL4-M8, SQL4-S. Gate: PRE-MVX, NAV-S, NAV-COV-ADD, GATE-COVDIFF, STORE-M1, STORE-S, D3-Q1a,
GATE-EXES, IC-S, FIELDS-S, PY-S. Verdicts recorded: AL-CLOCK fix 1 APPROVE `eed512bd`, SQL3 APPROVE `22b8fd17`,
A4 fix 1 APPROVE `87df9e00` (not merged: P10), SQL4 APPROVE `44a76937`; whole-wave review complete (no lane return).
Permission refusals this session: one (R11 #1, the A4 re-apply). Owner's words recorded: the /goal text of 2026-10-15,
STOP 11.

## Standing warnings (in addition to status-11)

- The gate's P9-B0 runs read real data: every argv through the bounded runner; READ NO STATISTIC; the PM reads only the
  registered prints after the cells.
- A root agent's context fills during the gate: chain fresh root agents through `handoff-root-gate-<n>.md`; never leave
  a merge or a run half-recorded.
- Reviewers found NO code defect in the merged waves; the Required items were all procedure. Do not re-review the
  range; the next review is the gate report itself (a fresh reader of `root-gate-report.md` before the cells).
