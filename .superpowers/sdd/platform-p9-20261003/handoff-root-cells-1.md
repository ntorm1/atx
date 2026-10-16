# Handoff: cells root agent 1 -> the next cells root (OWNER STOP 14)

Cells root agent 1 (`r14-prompts/cells-prompt-1.md`), pool-2 (`C:/atx-wt/pool-2`), branch `feat/platform-p9-20261003`,
2026-10-16. Stopped on the owner's STOP 14 ("stop here and write handoff file for next parent agent"), which arrived
after P9-L had already stopped on its own at the w pass (driver HARD-STOP). No bounded run is in flight; nothing is
half-recorded; the trial ledger was never written.

## Done (every commit, in order)

| commit | kind | what |
|---|---|---|
| `cc1cf759` | docs | DISPATCH-COMMIT (step 0): `progress.md`, `r14-prompts/` (cells-prompt-1, rev-flags, deriv-p9l / -p9v / -p9x) as they stood, `cells/start-state.txt` (start state HOLDS), `cells/tools/start_state.py` |
| `0bba6c89` | code | P9-L registration: `scripts/specs/p9/templates/label-terminal.json` `7483f4b10513d9000cfc89036c3f8f7f49626a9dd9c57999bcce11a258754219` (parent / nominal_parent `../../v8/lib-v8ysb-gm.json`; `lock --write --exes`; inherits `--ann-mean-gross` and `--info-clock-print`, each x1 on the planned NAV argv) |
| `6ea79e5a` | code | the calibrated run recipe `label-terminal-cal.json` `7783967471f5b06dc1407d4425605156f7a3e647f28edd2a3ca5f2fb1a989d37` (`wave_stage_cell.calibrated_doc`; G_parent `0.9862134133265011`, tolerance `0.005`; `lock` dry exit 0) |
| `0950cb57` | docs | `cells/p9l-label-terminal-plan.md` (limits G-B9 unscaled, verdict arithmetic, route, order, checks); `p9-prereg.md` PRE-40 brackets (Appendix IV item 7 filled + B.1 "Shape"; SG4-REF at B.0 PRE-7a and the round-2b-ii record; SG-3 refresh of B.1's citations; header paragraph) `e42d5d96...305c`; S2-6 scan (HOLDS) and the cache listing before; `cells/tools/s2_6_scan.py`, `hand_readers.py` |
| this commit | docs | `cells/p9l-label-terminal-record.md` (STOPPED at w, no verdict), the run-1 logs (filtered console, pre-cell, outputs listing, cache listing after, S2-6 scan after, seal scan), `cells/tools/cycle_filter.py`, `hand_seal.py`, `p9l_match.py`, this file; `progress.md` and `next-goal-prompt.md` as they stand |

P9-L run 1 (`research_cycle.py run label-terminal.json --stop-after w --admission-wait 1800`): fields done (pinned v15);
u, fit, card completed; the gate PASS with 0 admission lines appended (9 already ledgered); **w refused** by the pinned
Release IC exe before payload load: `Unavailable: IC runner: required_bytes=3448251496 max_compiled_slots=8 exceeds
configured memory budget before payload load` (the registered w budget `--max-memory-mib 3072`; `--label-terminal`
adds `label_terminal_bytes`, `atx-impl/src/strategy_ic_admission.cpp:322-340`). Cycle `HARD-STOP [w]`, exit 4. Details:
the record, section 3.

## Not done

- P9-L: w, the calibrated NAV (`-cal`), the cell of record `label-terminal-gm.json`, the mechanics verdict, rho, the
  bundle and the printed-only lines, the hand-back for DERIV, DERIV itself, the trial line. No verdict exists; nothing
  of the cell's statistics was read.
- Every later cell (P9-H .. P9-V), the adoption print, OD-3: not started.

## Exact next step (`r12-post-gate.md` "CELLS")

P9-L is stopped inside **step 2 ("Run")**, at its first item (the hand v1 cycle `run`), phase w. It cannot continue as
registered. Next:

1. **The PM's ruling on record open question 1** (below). Without it nothing of P9-L runs.
2. If the PM allows a w budget change: **step 1 ("Registration commit")** again for that edit only: the template gains
   the w-pass budget (e.g. `change.set` / flags so that the w IC argv carries `--max-memory-mib` >= 3289, and the runner
   phase cap w `max_rss_mib` alike, free RAM stated first), re-`lock --write --exes`, the `-cal` recipe re-derived by
   `calibrated_doc` from the new template (G_parent unchanged), a code commit showing the JSON diff, a PRE-40 bracket in
   `p9-prereg.md` B.1 / Appendix IV item 7 with the new SHA-256s, and a line in the plan file (docs commit).
3. Then **step 2** resumed: `research_cycle.py run scripts/specs/p9/templates/label-terminal.json --stop-after w
   --attempt w=2 --admission-wait 1800` (u, fit, card are done and resolve to the same outputs; the refused
   `-run1` dir is never overwritten); then the plan file's section 3 steps 2-9 unchanged (`-cal` NAV, `p9l_match.py
   check`, mech-matched reader, `p9l_match.py record` + `lock` + code commit, seal scan, cache listing, the mechanics
   verdict committed before any return read, rho, bundle, printed-only, record, STOP for DERIV).
   Re-run step 0's checks first (`cells/tools/start_state.py --refs`, `exe_sha_end.py` both builds, the ledger).

## Trial ledger

`build-equity/trials.jsonl`: **133 lines, sha256 `27e40f9f6314585e5ff002ad5e3eacdcdb34e8960d9dc62111074487b34d8554`**
(= dispatch; unchanged by every run of this agent).

## Parent of P9-L

The re-based Y-F0 of the gate's P9-B0: lineage parent spec `scripts/specs/v8/lib-v8ysb-gm.json`
(`a4ef6c0459f5a208f8efdc3c69a97519fbf1460cd3154793a475f94bd000c06a`), paired parent NAV `build-equity/p9-b0g-yf0-nav`
(listing `e205d6ec...f797`, `summary.json` `826d5ec6...8f38`), parent admission
`build-equity/mega-weights-v8x-theme-erc-v8ysb/admission.json` `121f7046...f255`. P9-H's parent stays the same until
P9-L is ruled (accepted -> `label-terminal-gm.json`).

## Exe table

Unchanged: the re-pin table of `handoff-root-gate.md` (Release p9-3f / Debug p9-3g, source `5f48a25d`); re-hashed at
step 0 and before run 1: 27 / 27 "disk = receipt" in both builds; pinned eval `e2b48c46...d962c`, IC
`ccd54db6...7d705`, NAV `255b58ac...63565`. No build.

## Output dirs created (all under `build-equity/`; state; every file's SHA-256 in `cells/logs/p9l-outputs-run1.txt`)

| dir | state |
|---|---|
| `p9l-label-terminal-readers/` (`mech-parent.json`, `mech-parent-run1/`) | complete: G_parent's reader output (keep: the `-cal` recipe's value comes from it) |
| `mega-p9l-train-u-label-terminal-1` + `-run1` | complete (u, 12 files); reusable |
| `mega-weights-p9l-label-terminal` + `-run1` | complete (fit, 3 files); reusable |
| `mega-cards-p9l-label-terminal` + `-run` | complete (card, 138 files); reusable |
| `mega-p9lw-train-label-terminal-run1` | refused w attempt 1 (receipt, start, logs; no output dir); never overwritten |
| `mega-candidate-cache-v8-lo3` (real cache, not mine) | 552 original rows unchanged; +134 files: scope dirs `ic1_af219b3f80b78243` (imputed-v1) and `ic1_ea0a12a3bb527150` (missing-exit-v1), 67 records each, engine `5f48a25d` (post-`110fd735`) |
| not created | `mega-p9lw-train-label-terminal-1`, `mega-nav-p9l-label-terminal-gm`, monitor, `cycle-p9l-label-terminal-gm` |

Not committed, on disk: `cells/logs/p9l-run1-cycle.log` (the raw cycle console; it holds the gate's per-candidate
admission lines, unread under B.0 void (3); the filtered copy is committed).

## Void checks

B.0 void items 1-7 all held through run 1 (record section 7): inputs = the block; no statistic read; nothing at or
after 2024-01-01 (hand seal scan: 5 sealed tokens, all untracked-PNG file names, OTHER 0); exes and `ucrtbase.dll` =
the record; no cache-index flag, `index.sqlite` absent. S2-6 held before the run. The stop is a driver stop (not a void).

## Open questions (for the PM)

1. P9-L's w pass needs an IC memory budget >= 3,289 MiB under `--label-terminal` (exe admission 3,448,251,496 B) vs
   the registered 3,072 (E-28; C.14 "carried alike"): may the template carry a larger w budget (and runner cap), and is
   a resource budget an "input" under void (2)? (Record, open question 1.)
2. `--seal-allow` has no object on the v1 hand cycle (no seal scan in `research_cycle.py run`); the hand scan
   (`cells/tools/hand_seal.py`) replaced it. Accept as disclosed?
