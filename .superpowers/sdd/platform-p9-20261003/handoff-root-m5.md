# Handoff: root M5 (RESUME 10), stopped at STOP 10 before any merge, 2026-10-08

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Started from `handoff-root-m4.md`.

- **Status: stopped at STOP 10 before any merge.** The owner's STOP 10 ("stop here", relayed by the PM) arrived while
  root M5 was reading the dispatch documents (`resume4-dispatch.md`, `handoff-root-m4.md`). No merge was started, so
  none was open and no `git merge --abort` was needed. No build ran, no test ran, no real-data run happened, and no
  source file was edited.
- **Code head: `5c811b56`** (unchanged from M4).
- **Docs head:** the commit that adds this file. It also commits `progress.md` exactly as the PM left it in the
  working tree (6 added lines, none edited by root).
- **Exe table:** unchanged from `handoff-root-m4.md` "Exes (EXE-CUR)". Current: the four p9-2y exes
  (`build-equity/mega-p9-2y-receipt.json`, source `5c811b56`). Stale by source: as M4 lists. No relink in M5, so
  EXE-FREEZE is unchanged.
- **Known-red set:** unchanged. Exactly `atx-impl/tools/test_nav_summ_v8.py::test_legacy_n37_numbers_reproduced`
  (root-only; B2-N37) until B2 fix round 2 merges. Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned`
  stays a named skip.
- **Next free root build tag: `p9-2z`** (no `build-equity/mega-p9-2z*` file exists).
- **Trial ledger** `build-equity/trials.jsonl`: 133 lines, sha256 prefix `27e40f9f` (checked at the stop).
- **SQL3-2:** `build-equity/research-store/` is absent (checked at the stop).
- No lease touched. No permission refusal. No data opened. No statistic read. `holdout_gate.py` not run.

## What is left for the next root agent (M5's whole task list, untouched)

Order and details are in the ledger line "Dispatch root M5" (`progress.md`, RESUME 10 block). Each merge happens at its
MERGE-SHA:

1. B2 fix 2 at `101f695a`. After it the known-red set must be empty (the n37 test passes on root's current Debug verb
   with the blind flags). If it does not pass, stop that chain.
2. OD3P follow-up at `4a7372e2`. The real-verb case must run, not skip (OD3P-SKIP). The eval verb p9-2v is stale by
   source, so rebuild `atx-research-eval` first (EXE-CUR).
3. D3W-PB: `c044f5f6` with the TRAIN identity procedure (`task-D3W-PB-report.md`, `d3w-logs/tools/pb-ident.py`,
   `cmp.py`), then `460f5ba2`, then `44a331e6`. The S-g keep-both conflict in `scripts/tests/test_no_python_mirror.py`
   comes at the fix commit (root's B2 line, then the two D3WPB-Q1 lines).
4. SIGN: `c2010ed1`, then `50a43599`, then `e2844926`, plus the SQL2-CLS row `atx.sign-receipt/v1` with class
   `ic-output`, and the ledgered wave results' screen rows checked by path (SIGN-SUBST receipt note).
5. A4: `6dab6948`, then the TRAIN identity on both fields (A4-Q2; a miss stops A4 and the flips are not merged), then
   the flips `69141218` and `59d807ea`, then the newest `Task A4: APPROVE at <sha>` head (`fc3e414e` unless a fix-1
   APPROVE line exists).
6. Still for M6 or later: AL-CLOCK fix 1, SQL3 item 5, SQL4 (last, SQL4-GO), the A4 fix head, and any lane not
   APPROVE in time. Also the P0 / PRE items in `handoff-root-m4.md` "What the next root group (M5) must know".

## Kept and deleted outputs

- Nothing created and nothing deleted. No scratchpad files.

## Uncommitted state at the end

- None of root's. The five untracked `docs/plans/*.png` files (not root's) are left, as in M4.

## Open questions for the PM

- The three questions in `handoff-root-m4.md` are still open; M5 adds none.
