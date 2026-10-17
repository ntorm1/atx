# Handoff: root M2e (C2 merge, AL-COMB merge), 2026-10-04: stopped by the owner before either merge

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`.
- **Code head: `694b4b95`** (the COV merge, unchanged from M2d; M2e changed no code).
- **Docs head:** the commit that adds this file. It sits on `081bf8cb` (housekeeping), which sits on `4902d010` (M2d docs).

**Status: STOPPED (owner stop), clean point.** No merge is in progress (`MERGE_HEAD` absent). No build, test or bounded run
of mine was started, so nothing of mine is running. C2 `4d77d3db` and AL-COMB `efb7ab2e` both resolve in the repo and
neither is an ancestor of HEAD.

Next free build tag: **`p9-2d`** (none used in M2e).

Trial ledger `build-equity/trials.jsonl`: **133 lines, sha256 prefix `27e40f9f`** (checked at the stop; 0 trials).

## Steps of the M2e brief

| step | state |
|---|---|
| Read contract / briefs / M2d handoff / ledger rulings (C2, ALCOMB, CLS-1, SQL2-CLS, RUN-1, B0-W2-1, W1-MIN, E2-12) | done |
| Housekeeping docs commit (`wave3-carry.md`, `al-data/*.md` incl. new `task-AL-DATA-review.md`, unedited) | **done**, `081bf8cb` |
| MERGE 1 C2: `git merge --no-ff 4d77d3db` | **not started** |
| C2: copy report / review / rereview-1 / rereview-2 / `c2-fix*` logs and receipts from pool-15 | not started |
| C2: Debug (`equity-dev`) build of the report's targets | not started |
| C2: Release (`equity-rel`) build | not started |
| C2 gtests (`CalibrateGross.*:NavBookRule.*:BookNormalScore.*` 19; whole `atx-impl-strategy-target-tests`, both builds); Release M1c-RED check | not started (0 run) |
| C2 pytest (`test_research_wave.py` 20; eight wave suites 77) | not started (0 run) |
| C2 (i) gm-pair identity on X-5: substitution list written first | **not written**, not run |
| C2 (ii) delete `scripts/wave_rules.py:110-119` (+ C2-9 refusal if trivial) | not started (depends on (i)) |
| C2 (iii) S13 | listed for E2 (below), as briefed |
| C2 (iv) no P9 reference under the `rel` preset | not checked |
| C2 (v) SQL2-CLS for C2's literals + fixture chain (`d32f7655...4e69`) | not started |
| MERGE 2 AL-COMB `efb7ab2e` (+ all its root items) | **not started** |
| `root-wave2-merge-report.md` sections "M2e C2" / "M2e AL-COMB", `integration-log.md` rows | not written (nothing to report) |
| DISK-2 for pool-15 / pool-19 build trees | not done (lanes not merged yet: keep them) |

## Known-red set now (unchanged from M2d)

- pytest `scripts/tests/test_research_mine.py::test_fields_are_the_rule_applied_to_the_registry` (E2).
- Release `BookNormalScore.*` x2 (M1c-RED; clears only with C2's merge and a Release pass).
- Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` (M2c-RED-2; lane S2R; do not touch).

## Resume commands (all in `C:/atx-wt/pool-2`; follow the M2e brief in order)

```
# MERGE 1 (C2)
git merge --no-ff 4d77d3db          # expect E2-file hunks scripts/wave_result.py + 2 lines scripts/wave_stage_record.py; COV's Cov* tail in atx-impl-strategy-target-tests: keep both
#   then copy from C:/atx-wt/pool-15/.superpowers/sdd/platform-p9-20261003/: task-C2-report.md, task-C2-review.md,
#   task-C2-rereview-1.md, task-C2-rereview-2.md, c2-fix* logs / receipts  (git add -f)
powershell scripts\atx-build.ps1 configure -Preset equity-dev      # only if the tree needs it (plain documented form)
powershell scripts\research-build.ps1 ...  -Tag p9-2d               # Debug, targets named in task-C2-report.md
powershell scripts\research-build.ps1 ...  -Tag p9-2e               # Release (equity-rel), same targets, one build at a time
#   gtests: --gtest_filter=CalibrateGross.*:NavBookRule.*:BookNormalScore.*  (19) + whole atx-impl-strategy-target-tests, both trees
#   pytest (separate sessions, -p no:cacheprovider, own --basetemp, PYTHONDONTWRITEBYTECODE=1):
#     scripts/tests/test_research_wave.py (20); the eight wave suites (77), explicit paths
#   (i) write the gm-pair substitution list into root-wave2-merge-report.md BEFORE the X-5 engine vs Python gm run
#       (0 trials, bounded runner, --max-rss-mib 2048 per RUN-1); compare by SHA / JSON path, L-prime within 1e-12 rel
#   (ii) only if (i) holds: delete scripts/wave_rules.py:110-119 in its own commit; re-run the wave pytests
#   (iv) grep C2 receipts / logs for preset `rel` references; (v) SQL2-CLS commit + fixture chain (d32f7655...4e69)
# MERGE 2 (AL-COMB), after MERGE 1 is closed
git merge --no-ff efb7ab2e          # expect exe_capabilities (sorted, size = count) and strategy_ic_runner*.cpp tails: keep both
#   copy from C:/atx-wt/pool-19/...: task-AL-COMB-report.md, task-AL-COMB-review.md, task-AL-COMB-rereview-1.md, evidence-AL-COMB/
```

The `research-build.ps1` arguments are elided above on purpose: use the exact target list from each lane report and the
argument form of the M2d receipts (`build-equity/mega-p9-2a-*`).

## Items listed for the E2 merge (not for M2e)

- MR-4: AL-COMB's two flags into E2's `wave_kinds.COMPOSITION_FLAGS` (E2-12: E2's test example no longer collides).
- S13 from the C2 report ("left to root at the E2 merge").
- The positive live-table check: frozen `D1_SCHEMAS` = merged exe's `--list-rules --json`.
- C2's two cross-lane lines in E2's `scripts/wave_stage_record.py` (and the `scripts/wave_result.py` edit; Ruling C2-8,
  R4 pin): reconcile with E2's version at its merge.

## Uncommitted state at the stop

- `progress.md` (PM's) modified: not touched.
- Four untracked PNGs under `docs/plans/`: not mine, left alone.
- `disk-audit.md` (git-ignored, untracked, 2026-10-03, not mine): left alone.
- No `status-5.md` or `next-goal-prompt.md` in the sprint dir at the stop.

## Scratch / build outputs

None created in M2e. M2d's kept outputs are unchanged (`build-equity/p9-m2d-cov-risk-container` (+ `-run`),
`p9-m2d-cov-risk-legacy-run` receipt, receipts `mega-p9-2{a,b,c}-*`). Lane pools 15 (C2) and 19 (AL-COMB) are
unmerged: do not DISK-2 them yet.
