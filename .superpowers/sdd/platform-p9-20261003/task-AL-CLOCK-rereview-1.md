# Re-review 1: lane AL-CLOCK, fix round 1 (ALCLOCK-S)

**PARTIAL (OWNER STOP 10, relayed by the PM: "stop here").** No verdict yet. 0 Required, 0 Suggested so far.
I have not read the diff yet, so I have no findings. A resuming reviewer writes the verdict into this same file and
replaces this header (REV-FRESH).

- Lane / pool: AL-CLOCK, `C:/atx-wt/pool-30`, branch `feat/p9-alclock2-20261007`. Pool-8 is FROZEN and I did not
  touch it.
- HEAD to review: `f892eab1`. BASE: `e9b3a323`, the first review's APPROVE commit (its code head is `1cbe5233`).
  REV-BASE and FU-REVIEWS both give this BASE.
- Scope: the diff `e9b3a323..f892eab1` only.

## What I have read

- ROOT-SDD (`C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/`):
  - `resume4-dispatch.md`, `resume6-addendum.md`, `resume7-addendum.md`, `resume8-addendum.md`,
    `resume9-addendum.md` (TEST-1, TEETH-RESTORE, HOST-3) and `resume10-addendum.md` (HOST-4, REV-FRESH,
    FU-REVIEWS);
  - `r7-review-dispatch.md` (the review contract) and `briefs/review-template.md`;
  - `r8-review-notes.md`, section "AL-CLOCK";
  - `r10-dispatch.md`: the sections "ALCLOCK-FIX1", "ROOT M4" item 2, and "Scoped re-reviews of this wave"
    (the AL-CLOCK bullet).
- Ledger `progress.md`, by grep only. These are the lines I read:

  | ruling / record | line |
  |---|---|
  | ALCLOCK-1 | `:625` |
  | ALCLOCK-2 | `:626` |
  | ALCLOCK-4 | `:702` |
  | ALCLOCK-T2 | `:703` |
  | ALCLOCK-S | `:745` |
  | ALCLOCK-S1a | `:760` |
  | ALCLOCK-S3a | `:761` |
  | the lane's hand-back | `:828` |
  | ALCLOCK-M | `:829` |
  | the dispatch of this re-review | `:830` |
  | W3-9 | `:387` |
  | PRE-7a | `:538` |
  | EXE-CUR | `:567` |
  | PERM-1 | `:595` |
  | PRE-40 | `:623` |
  | TEETH-RESTORE | `:674` |
  | TEST-1 | `:688` |
  | MERGE-SHA | `:696` |
  | HOST-4 | `:774` |
  | REV-FRESH | `:775` |
  | FU-REVIEWS | `:780` |
  | HOST-5 | `:788` |

  ALCLOCK-3 has no ledger "Ruling" line. The first review took its text from `status-8.md:157-158`.
- In pool-30's sprint dir: `task-AL-CLOCK-review.md` (the first review: APPROVE, S-1 .. S-5). I also listed
  `alclock-logs/` to see which records exist (contents not read):
  - the `-k`, `-l` and `-m` receipts, build logs and research-build outputs;
  - `gt-k-clock.log`, `gt-k-named.log`, `gt-l-teeth.log`, `gt-m-named.log`;
  - `pyt-clock-{k,m}-s{0,1}.log` and `teeth-s3.log`;
  - `check-fix1-s1.log` and `check-fix1-s3-test.log`.

## Not yet read

- The diff itself.
- In `task-AL-CLOCK-report.md`: the section "Fix round 1 (ALCLOCK-S)" and its "For PRE" block.
- The fix-round sections of `handoff-AL-CLOCK.md`.
- The contents of the `alclock-logs/` files and the teeth tool.
- `.agents/cpp/agent.md` section 10.

## What I have verified myself

- At the start, pool-30 HEAD = `f892eab1` on `feat/p9-alclock2-20261007`, and the tree was clean.
- `git log --first-parent e9b3a323..f892eab1` lists 11 commits. Three of them are root merges:

  | merge | lane-side parent | root-side parent |
  |---|---|---|
  | `65ec16af` | `a662d1c2` | `b182dfdd` |
  | `1d603d23` | `bcade871` | `858a4fca` |
  | `38967882` | `ee70a69f` | `5c811b56` |

  I have not yet checked that these merges are plain (that `merge-tree` recomputes each tree).
- The lane's own commits are listed below. I know them by their subjects only; I have reviewed none of their
  hunks.

  | commit | subject |
  |---|---|
  | `a662d1c2` | stop record |
  | `170ac2b2` | S-3 gtest and S-4 fixture |
  | `80b7ae69` | S-2 registration sentence and S-4 pytest |
  | `bcade871` | S-1 text |
  | `9d3916b8` | build wrapper wait (HOST-4 / HOST-5) |
  | `b96fe073` | `-k` records |
  | `ee70a69f` | `-l` teeth records |
  | `f892eab1` | report, handoff and `-m` records |

- No test has been run and no build has been started by me. Pytest seeds 0 and 1 were not run.

## Findings so far

None. The diff has not been read.

## Exact next step (for a resuming reviewer; fresh under REV-FRESH)

1. Read this file. Then read the ledger lines above. Then read the lane's fix-round records:
   - the report section "Fix round 1 (ALCLOCK-S)" and its "For PRE" block;
   - the fix-round sections of the handoff;
   - `alclock-logs/`;
   - `.agents/cpp/agent.md` section 10.
2. Write the diff to your own scratch file with `rtk proxy git -C C:/atx-wt/pool-30 diff e9b3a323 f892eab1`, and
   read it from there.
3. For each of the three merges above, check that `git merge-tree --write-tree <p1> <p2>` gives the commit's own
   tree (plain merge).
4. Give a spec verdict item by item, as the dispatch prompt names the items:
   - **S-3 (REQUIRED).** `InfoClockRate.CalibrateGrossKeepsTheRatesAndCountsOnce`, in ALCLOCK-S3a's shape.
   - **S-1.** The text in `strategy_nav_v7.cpp`, `nav_cli.cpp` and `nav_lockstep.cpp`. The capacity selector with
     the flag absent must give the old sentence byte for byte. `FlagAbsentLoadsNoClock` must still guard.
   - **S-2.** ALCLOCK-4's two clauses verbatim, with "C3-4" kept. The registration is not pinned (PRE-40 does not
     bind). The new SHA `7801c198...` must be re-quoted in "For PRE", and the pytest must assert the sentence.
   - **S-4.** The fixture must equal the `row_json` row; the gtest asserts equality; the pytest's NAV row is the
     fixture.
5. Classify every hunk as text / test / fixture / registration / selector / wrapper. Confirm that no default
   changed, that no key is output unconditionally, and that the rate's arithmetic is untouched.
6. Check `/W4 /WX` and the 100-column limit. Check helper reuse against `strategy_nav_calibrate_test.cpp`.
7. Evidence (TEST-1 / EXE-CUR):
   - The `p9-alclock-k` and `-m` logs: check that their exe SHAs match the receipts, and that `-m` is the
     merged-tree build (ALCLOCK-M).
   - The `-l` teeth log and tool: the `nav_session.cpp` `if (clock_rule)` mutation, with only the new test red and
     the restore verified.
   - Run `scripts/tests/test_info_clock_rule.py` yourself at seeds 0 and 1 in pool-30. Use the explicit
     interpreter, `-p no:cacheprovider`, your own `--basetemp` (create its parent first), and
     `PYTHONDONTWRITEBYTECODE=1`.
   - Make your own build only if the logs are insufficient, and then only under HOST-4 / HOST-5.
8. Write the verdict, findings, the split between what you verified yourself and what comes from the lane's logs,
   and "What root must check at merge" into THIS file. Root must check:
   - the second AL-CLOCK merge in M5, above `858a4fca`'s first;
   - `InfoClock*` 14 / 14 and the named filter 26 / 26;
   - the registration SHA for the re-pin;
   - the fixture row against the merged registry, including COV-MV's row;
   - that flag-absent identity is unchanged.
9. Commit only this file (`git add -f`, with the standard trailer) and leave the tree clean.

## Housekeeping

- Permission refusals: none.
- Not touched: pool-8, pool-24, pool-2 (read only), `C:/atx`, `atx-db/`, `main`, leases. No push.
- No build or test was started. No scratch file was written.
- This file is the only commit.
