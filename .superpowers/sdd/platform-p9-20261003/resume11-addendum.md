# P9 RESUME 11: addendum to the standing contract (PM, 2026-10-15)

Read `resume4-dispatch.md`, `resume6-addendum.md`, (lanes) `r6-wave3-common.md`, `resume7-addendum.md`,
`resume8-addendum.md`, `resume9-addendum.md` (TEST-1), `resume10-addendum.md` (its "Owner rules" section is
carried verbatim and stands; its rulings LEASE-DEAD, HOST-4, HOST-5, REV-FRESH, FU-REVIEWS, TEETH-BIN-23 stand),
then this file. This file wins where they differ; a ledger "Ruling" line wins over everything. Then read your
section of `r11-dispatch.md`, your lane's handoff / review file in its pool, and grep your lane ID and every ruling
id your section names in the ledger `progress.md` (the blocks from the lines starting "- RESUME 10" and
"- RESUME 11" hold the newest rulings). All files are in `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/`
unless a pool is named.

## Owner rules (unchanged: `resume10-addendum.md` "Owner rules" is the text; re-read it)

Reminders for the traps agents hit in RESUME 10:

- Your shell starts in `C:/atx`. NEVER run git, a build, a test or an edit there: `cd` to your pool (or use
  `git -C <pool>`) first. Never any git write in pools 8, 24 (frozen, TEETH-POOL) or 2 (root agents excepted).
  Never touch a lease (LEASE-DEAD: `owner=dead` everywhere is expected; pool-32 prints `owner=alive`: also untouched).
- `core.autocrlf=true` on this host. TEETH-CRLF: a TEETH-RESTORE tool compares `git hash-object <path>` against
  `git rev-parse HEAD:<path>`, and the raw SHA-256 of the working bytes against its own saved copy; never the raw
  bytes against `git cat-file blob`.
- HOST-4: ONE C++ build among our pools at a time; wait (60 s quiet polls) while another of OUR builds runs; bound
  180 minutes, then hand back BLOCKED-HOST with the free-RAM figure. HOST-5: a `ninja` / `cl` / `clang-cl` whose
  command line names a path outside `C:\atx-wt\` is a foreign session's: ignore it in the wait loop (the
  memory gate of `research-build.ps1` still decides admission).
- A4-BUILD: a lane that deleted a configure-generated file makes `research-build.ps1` fail at ninja start with 0
  files compiled: re-configure (`powershell scripts\atx-build.ps1 configure -Preset equity-dev`) first.
- TEETH-BIN-23: pool-23's `build-equity/` holds binaries built from a MUTATED `fit.cpp`; nobody uses its exes,
  nobody deletes it. EXE-CUR: every exe you rely on is named by its receipt (`build-equity/mega-<tag>-receipt.json`)
  and its SHA-256 is logged beside the run.
- PERM-1: a refused command is never retried, never substituted; the exact command goes in your final message.
- Records in the pool's sprint dir (`git add -f`), never under the session scratchpad. Long record files: the
  editor tool, not a Bash heredoc. Ledger `progress.md` is the PM's: never edit it.
- Blind: no 2020-2023 statistic in any record or message; root history as one-line subjects only.
- Commit trailer exactly `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## State every agent needs (PM check, read-only, 2026-10-15)

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`: code head `5c811b56`, docs head `e3b6c111` (root M5
stopped at STOP 10 before any merge: nothing of M5 landed). Tracked changes in pool-2: `progress.md` (lines after
root M5's docs commit) and `next-goal-prompt.md`; untracked, never added: five PNGs under `docs/plans/`. Root M5's
step 0 commits the ledger as it stands plus `status-11.md`, `resume11-addendum.md`, `r11-dispatch.md`
(`git add -f`). Known-red set: exactly `atx-impl/tools/test_nav_summ_v8.py::test_legacy_n37_numbers_reproduced`
(root-only; B2-N37) until B2 fix 2 `101f695a` merges. Release
`AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` is a named skip. Next free root build tag `p9-2z`.
Trial ledger `build-equity/trials.jsonl`: 133 lines, sha256 prefix `27e40f9f`. `build-equity/research-store/` absent
in pool-2 (SQL3-2). Current root exes: the four `p9-2y` exes (`build-equity/mega-p9-2y-receipt.json`, source
`5c811b56`). Host at dispatch: free RAM 2.47 of 15.69 GB; C: 56.8 GB free; no build running; three small foreign
python processes.

| lane | pool / branch | head (verified 2026-10-15) | state | R11 step |
|---|---|---|---|---|
| waves 2 + 3 core | merged | root `5c811b56` | complete | gate after M5 / M6 |
| B2 fix 2 | 14 `feat/p9-b2-20261003` | `101f695a` APPROVE | re-review 3 done | root M5 (1) |
| OD3P follow-up | 21 `feat/p9-od3p-20261004` | `4a7372e2` APPROVE | re-review 2 done | root M5 (2) |
| D3W-PB (stretch) | 23 `feat/p9-d3w-20261005` | `44a331e6` APPROVE (chain `c044f5f6` -> `460f5ba2` -> `44a331e6`) | review done | root M5 (3) |
| SIGN (stretch) | 29 `feat/p9-sign-20261006` | `e2844926` APPROVE (chain `c2010ed1` -> `50a43599` -> `e2844926`) | review done | root M5 (4) |
| A4 (stretch) | 27 `feat/p9-a4-20261005` | `fc3e414e` APPROVE of record; `e982afef` = fix-1 handoff stub above it | fix round 1 (A4-S, tests only) not started | root M5 (5) merges `fc3e414e`; lane A4-FIX1 in parallel; scoped re-review; fix head at M6 iff APPROVE |
| AL-CLOCK fix 1 | 30 `feat/p9-alclock2-20261007` | `364391e0` = re-review 1 PARTIAL above lane head `f892eab1` | no verdict | fresh reviewer resumes (ALCLOCK-REV-RESUME); root M6 iff APPROVE; else `e9b3a323` (merged) stays of record |
| SQL3 item 5 | 13 `feat/p9-sql3-20261004` | `d43b3a8b` (handoff) over `d91ce86c` (merge of root `5c811b56`) over code `b55c0d11` | call lines fixed and tested; teeth / report / SQL3-WQ / SQL3-TREE pending | lane SQL3-5 finishes; review 2 (BASE `69b8c162`); root M6 |
| SQL4 (stretch; SQL4-GO) | 32 `feat/p9-sql4-20261008` | `73a871fb` (handoff) over WIP `a1955a71` | tasks 1-4 coded, NOT compiled | lane SQL4 finishes; first review; root M6 LAST, before the gate's P9-B0 re-pin, else P10 untouched (SQL-12) |
| PRE | sprint dir `p9-prereg.md` | round 2b-i `685e238b` | 2b-ii after the gate | the ledger's three "PRE 2b-ii list" lines |

Frozen pools 8 and 24: read-only (TEETH-POOL). All other pools above: trees clean at dispatch.

## Rulings of RESUME 11 (full text in the ledger; binding)

- **HOST-4 (R11 order).** Build priority this wave: root (M5, then M6, then the gate) > A4 fix 1 > AL-CLOCK
  re-review 1 (its one build only where the lane's logs and receipts do not already prove the item; EXE-CUR) >
  SQL4 > the scoped reviewers (SQL4 first review, A4 re-review 1). SQL3 item 5 and its review 2 are Python only.
- **R11-RESUME.** Every lane resumes from its own handoff / review file by a fresh agent; nothing the ledger marks
  APPROVE or complete is re-dispatched; a PARTIAL review is resumed into the same file (REV-FRESH).
- **R11-GOAL.** The owner's 2026-10-15 goal text re-states the sprint and changes no ruling: priorities Sharpe >
  gross return > capacity > significance; the new-signal cell stays dropped (P9-S lapsed); SQL4 moves to P10 whole
  if it misses the gate's P9-B0 re-pin (SQL-12); "done" = the single gate (GATE-1) green, the registered cells run
  with explicit adopt / reject, OD-3 read once, the freeze verdict and the final ruling list.

## Context and hand-back

If you run low on context: commit what is done (`WIP(<lane>): ...` for unfinished work), write the handoff (done /
not done / next step, file:line), report DONE_WITH_CONCERNS. Do not start a build you cannot wait for.

Final message to the PM (under 200 words; details go in files): STATUS (DONE / DONE_WITH_CONCERNS / BLOCKED /
BLOCKED-HOST), head SHA, root SHA merged, build tags, tests with pass / fail counts, items pending, files written
(paths), disk freed, any permission refusal (the exact command), open questions one line each. No logs, no 2020-2023
statistic.
