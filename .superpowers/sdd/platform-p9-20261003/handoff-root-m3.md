# Handoff: root M3 (RESUME 9), stopped at OWNER STOP 9 before its first merge, 2026-10-07

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Started from `handoff-root-m2.md`.

- **Status: STOPPED (STOP 9, relayed by the PM) during the reading phase. No merge was started, none is open, nothing
  was built, no test was run.** No `MERGE_HEAD`; nothing to abort.
- **Code head: `fcb0200e`** (unchanged from M2: B2 `5dfb0ba1`, T2 `27e69387`, slips `6c201a1d`, `fcb0200e`).
- **Docs head:** the commit that adds this file. It also commits `progress.md` exactly as the PM left it in the working
  tree (the ledger lines from "Root M2 hand-back" to "Dispatch AL-CLOCK fix round 1"; no line edited by root).
- **Trial ledger** `build-equity/trials.jsonl`: 133 lines, sha256 prefix `27e40f9f` at the start and at the stop.
- **Merges of M3: none landed, none re-proved.**

| step | lane | APPROVE SHA to merge | state |
|---|---|---|---|
| 1 | C3b | `76b9f256` | NOT started |
| 2 | D3W | `f0726acb` | NOT started |
| 3 | ABS | `6a3f5043` | NOT started |

## Build tags, exes, known-red

- Build tags used this group: none. **Next free root tag: `p9-2u`.**
- Exes of record: unchanged, exactly as `handoff-root-m2.md` "Exes (EXE-CUR)" lists them (Debug `p9-2t`, receipt
  `build-equity/mega-p9-2t-receipt.json`; the p9-2p / p9-2q / p9-2r / p9-2s exes as named there). Nothing relinked by
  root this group (no build ran in pool-2); SPDLOG-RELINK: re-hash against the `p9-2t` receipt before using any of them
  as evidence after another pool's build.
- **Known-red set:** exactly `atx-impl/tools/test_nav_summ_v8.py::test_legacy_n37_numbers_reproduced` (root-only, runs
  only with `ATX_EQUITY_ROOT`), attributed to merge `5dfb0ba1` (B2-N37), until B2 fix round 2 lands. Release
  `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` stays a named skip.
- SQL2-CLS rows added: none. SQL3-2: not re-checked this group (no run happened).

## Read before the stop (the next root agent re-reads from the same list)

`resume4-dispatch.md`, `resume6-addendum.md`, `resume7-addendum.md`, `resume8-addendum.md`, `resume9-addendum.md`
(TEST-1, ROOT-1a), `r7-root-merge.md`, `r9-dispatch.md` "ROOT", `handoff-root-m2.md`, `progress.md` from "- RESUME 9"
to the end (lines 667-746 at the stop). NOT yet read: the three reviews' "What root must check at merge" sections
(pool-15 `task-C3b-review.md` + `task-C3-report.md` "## Part B (C3b)"; pool-23 `task-D3W-review.md` +
`task-D3W-report.md`; pool-28 `task-ABS-review.md` + `task-ABS-report.md`).

## Exact next step (a fresh root agent, pool-2, after the owner lifts STOP 9)

1. Start state: head = this docs commit, code `fcb0200e`, tree clean except the five untracked `docs/plans/*.png`
   (not root's) and any PM ledger lines; trial ledger 133 `27e40f9f`; C: about 58 GB free at the stop.
2. Read the three reviews' root sections above, then run M3 as the PM's "Dispatch root M3" line (`progress.md:740`)
   and the M3 prompt state: `git merge --no-ff 76b9f256` (C3b) -> its re-proof (`test_wave_kinds.py`,
   `test_wave_eval_verb.py`, the wave suites + `test_wave_gross_match.py`, one Debug `p9-2u` of
   `atx-impl-strategy-target-tests`, named 34 incl. `NavSpec.GridFileRefusesADuplicateKey`; C3B-S tools listed, not
   rewritten); then `git merge --no-ff f0726acb` (D3W; B2-EVAL-KEEP keep-both on `eval_report.cpp`; one Debug build;
   eval 26 + `EvalVerb.HandRowAtNTotMinusMLeavesOutTheCampaigns`, `WalkForward.*`, `WeightsV3.*`, `MomVolman.*`,
   `ResearchComposition.*`; fitter pytest files; `d3w-logs/tools/ident.py` 17 / 21 + 4 fingerprint-only); then
   `git merge --no-ff 6a3f5043` (ABS; SQL2-CLS rows `atx.od3-flagged-members/v1`, `atx.carry-weights/v1`; one Debug
   build of `atx-equity-strategy-ic,atx-impl-strategy-ic-tests`; `AbsentMembers.*` 11, identity names 53;
   `--list-rules --json` capability `absent-members-v1`; the od3 plan refuses without / plans with the capability on
   fakes). Tags continue from `p9-2u`.
3. Records: create `root-wave3-merge-report.md` sections "C3b" / "D3W" / "ABS" (note: a file of that name already
   exists in the sprint dir, 11.9 KB, from an earlier group; read it first and append, do not overwrite),
   `integration-log.md` rows, `handoff-root-m3b.md` (or the next group name the PM gives).

## Notes for M4 (unchanged from the dispatch; recorded so they survive the stop)

- D3: keep-both against C3b's kind-fixture lines (8) and its 3 `test_wave_kinds.py` hunks; B2's
  `scripts/tests/test_wave_kinds.py:171` `F.pin_eval_verb(root)` line must survive; C3-5 probes (a) and (b).
- AL-CLOCK: now APPROVE at `7ebd41ff` (review `e9b3a323`), with fix round 1 ordered (ALCLOCK-S, `progress.md:745`):
  merge the round's head if APPROVE by its slot, else `e9b3a323` and the round as a second merge. AL-CLOCK before
  COV-MV; `atx-impl/tests/CMakeLists.txt` tail keep-both with AL-CLOCK's block first.
- COV-MV: fix round 1 head if APPROVE, else `bda51b98` (COVMV-S). OD3P: follow-up head or `dc11c977`. SQL3 item 5,
  B2 fix round 2, D3W-PB if APPROVE.

## Kept and deleted outputs

- Kept: Debug object tree `build-equity/` and `build-equity-rel/` (untouched; C: about 58 GB free). Deleted: nothing.
- No session scratch files written by this group.

## Uncommitted state at the end

- None of root's. Five untracked `docs/plans/*.png`: not root's, left.

## Permission refusals

- None.

## Open questions for the PM

- None from this group (no merge reached a decision point).
