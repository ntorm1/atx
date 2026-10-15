# Handoff: root after M3 (RESUME 10: C3b, D3W, ABS merges), 2026-10-09

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Started from `handoff-root-m3.md` ("Exact next step") and
`handoff-root-m2.md`. Rows: `integration-log.md` "M3 (RESUME 10)" (under "P9 wave-3 merges"); detail:
`root-wave3-merge-report.md` "Step group M3 (RESUME 10)" and sections "C3b", "D3W", "ABS"; evidence `m3-logs/` (tools
`m3-logs/tools/`).

- **Status: DONE.** The three merges landed with 0 conflicts. Every root item the dispatch and the three reviews list
  holds or is carried by rule: the gate items (TEST-1) go to the gate, the D3 keep-both items to M4, and the od3 plan on
  fakes to M4 because OD3P is not in root. No stop, no unattributed red, no identity miss.
- **Code head: `094bcd3b`** = step 0 `8346fc2e` (docs) + merge `b182dfdd` (C3b at `76b9f256`) + merge `08f04e07` (D3W at
  `f0726acb`) + merge `898bf120` (ABS at `6a3f5043`) + SQL2-CLS `094bcd3b` (`classes.json`).
- **Docs head:** the commit that adds this file. It also commits `progress.md` and `r10-dispatch.md` exactly as the PM
  left them in the working tree (no ledger line edited by root), the merge report, the integration log and `m3-logs/`.
- **Trial ledger** `build-equity/trials.jsonl`: 133 lines, sha256 prefix `27e40f9f` at start and end (0 trials).
- **Build tags used: `p9-2u`, `p9-2v`, `p9-2w`** (all Debug, `equity-dev`, 0 warning lines). **Next free root tag:
  `p9-2x`.**
- **Known-red set:** exactly `atx-impl/tools/test_nav_summ_v8.py::test_legacy_n37_numbers_reproduced` (root-only; runs
  only with `ATX_EQUITY_ROOT`; B2-N37, attributed to merge `5dfb0ba1`) until B2 fix round 2 lands. No new red in M3.
  Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` stays a named skip.
- **SQL3-2** holds: `build-equity/research-store/` absent in pool-2 at start and end.
- **SQL2-CLS:** C3b none; D3W none; ABS `094bcd3b`: `atx.od3-flagged-members/v1` in a new class `od3-drop-list`
  (`spec_doc` / `spec_kind` `other`, after `diagnostic-spec`; root's S-6 choice, reason in the merge report) and
  `atx.carry-weights/v1` in `fit-output`. OD3P's merge (M4) needs no further row for these two.
- No lease touched (LEASE-DEAD). No permission refusal. No data dated 2024 or later opened; no statistic read (every
  comparison by SHA, JSON path, count or boolean); `holdout_gate.py` not run.

## Exes (EXE-CUR)

New this group (Debug `build-equity/bin`; on-disk SHA-256 = the receipt's at the time of each test run; SPDLOG-RELINK:
re-hash against these receipts before using one as evidence after another pool's build):

| tag | source | receipt | exe | SHA-256 |
|---|---|---|---|---|
| p9-2u | `b182dfdd` | `build-equity/mega-p9-2u-receipt.json` (copy in `m3-logs/`) | atx-impl-strategy-target-tests | `7a427e2c5948ab45e06f7f4ff0aed0995694d71dc117be5e1326056c37e27a7a` |
| p9-2v | `08f04e07` | `mega-p9-2v-receipt.json` | atx-research-eval | `6962966c4b9d419996e24df55ac825e6f6ccd7672956986ad0a3d5b1b5b68778` |
| p9-2v | `08f04e07` | same | atx-engine-research-eval-tests | `dd8a3103828b4a671c048d569eba4d817d0e91acf49bd2fb61983e5777ddb7ca` |
| p9-2v | `08f04e07` | same | atx-engine-research-composition-tests | `4e4ea8850e4ea7ef0f3b2b2d0ee6a06c4e1f615efe70fec5fb77c38dd1f333f4` |
| p9-2v | `08f04e07` | same | atx-research-composition | `5e81e9c2d965de28dac83f10c4906aeb48d39113000ad843540768ba787884b1` |
| p9-2v | `08f04e07` | same | atx-research-admission | `411af33a0def5a6c28983dbf1ffc2fbc324694e8ac2e6b24401b27b693c9fa26` |
| p9-2w | `094bcd3b` | `mega-p9-2w-receipt.json` | atx-equity-strategy-ic | `8608b8249258da827166454bd3b95c5be61625fcd2c4300e590d94e15723f857` |
| p9-2w | `094bcd3b` | same | atx-impl-strategy-ic-tests | `fa383e75c7fb4aca66fe53117e419c4fc018e94c4032378f26ce3aff0254af52` |

The p9-2v IC exe `a22c935d...` and IC tests `0a2d0d99...` were overwritten by p9-2w; their D3W runs are recorded in
`m3-logs/gt-exits.txt`, and the D3W IC filter was re-run on p9-2w (18 / 18).

Current by source and unchanged: Debug store exe p9-2r `d8a500d8...` (= its receipt, re-hashed this group) and catalog
tests p9-2r `f1134aaa...` (no catalog source changed since `fd084e78`).

**Stale by source after M3** (`atx-impl-strategy` now holds C3b's `book/nav_*`, D3W's IC runner and ABS's
`strategy_ic_absent`, and links `atx-engine-research-composition`, which D3W changed): every Debug exe outside the
three receipts above that links `atx-impl-strategy`, `atx-engine-research-composition` or the eval library, in
particular the Debug NAV exe `atx-equity-strategy-targets` (p9-2p `637f7069...`), `atx-equity-strategy`,
`-risk`, `-mine`, and `atx-impl-tests` (p9-2t `13bb6bde...`); and every Release exe (p9-2q / p9-2s) linking those
libraries. Release builds and the canary are the gate's (TEST-1); the first root step that needs a Debug NAV exe
rebuilds it (EXE-CUR).

## Results of this group

| item | result (evidence in `m3-logs/`) |
|---|---|
| 0 housekeeping | `8346fc2e` |
| C3b | `b182dfdd`, 0 conflicts, keep-both with B2 PASS (`c3b-keep-both.txt`); p9-2u; named 34 / 34; wave files 104; kinds + eval verb 118; guard 5 |
| D3W | `08f04e07`, 0 conflicts; B2-EVAL-KEEP PASS (`d3w-keep-both.txt`); p9-2v; eval 27 / 27, PRE-15 + vol 2 / 2, composition 17 / 17, IC 18 / 18; pytest 155 / 1 skip, 47 with exes, `test_research_spec.py` 87; identity 17 / 21 + 4 fingerprint-only; guard 5 |
| ABS | `898bf120` + SQL2-CLS `094bcd3b`; p9-2w; `AbsentMembers.*` 11 / 11, identity 53 / 53; guard 1f -> 5p; store suite 43 x 2 seeds; fixture chain `d32f7655` unchanged; `--list-rules --json` = fixture, `absent-members-v1` first; `exe_capabilities` holds it; fixture readers 138 |
| slip M3-S1 | root edited `m3-logs/tools/build.sh` while p9-2u ran; only the wrapper's epilogue failed; the build is evidenced by its receipt (logged in `builds.txt`) |

## What the next root group (M4) must know

- **D3 (`e197b3b6`)**: keep-both against C3b's eight kind-fixture lines (line 18, `pm6-6-engine`; D3's walk-forward hunk
  is separate) and C3b's three `test_wave_kinds.py` hunks (merged `:613-614`, `:733`, `:759` on root now); B2's
  `F.pin_eval_verb(root)` at `test_wave_kinds.py:171` must survive; run `test_wave_kinds.py` after the merge (D3's
  fixture-to-match tests then run the engine route); the five stale docstrings (C3b's `:543`, `:563`, `:584`, `:734`,
  `:737`) can be fixed then (comments only). D3W review item 7: the fitter flag names, the refused set and the parent fit
  must match D3's `wave_kinds.py:86-90`: re-check at D3's merge. C3-5 probes (a) and (b) as before.
- **OD3P**: SQL2-CLS rows already in root (`094bcd3b`): the guard should stay green at OD3P's merge (re-run it). Then
  ABS review item 6: `scripts/tests/test_research_cycle_od3.py` (fakes); the plan on `scripts/specs/p9/od3/` with a fake
  IC exe lacking `absent-members-v1` must refuse (exit 2, before any step, naming the capability, the history roles,
  OD3P-1 and "not read: no producer"), and with the capability (fake, then the real p9-2w exe or a later one) must plan
  with each history role's u / w pass ending `--absent-members <spec path> --absent-members-sha256 <pin>` after
  `--candidate-cache` / `--theme-registry`; the TRAIN anchor gets nothing. OD3P's identity tests pin root's
  `research_roles.py` blob `313b23b3`: M3 did not change that file.
- **EXE-FREEZE (ABS review item 7):** ABS is in root before the wave-3 P9-B0 re-pin; any later relink of the IC exe
  redoes P9-B0 and the re-pin first.
- **AL-CLOCK, COV-MV**: as `handoff-root-m3.md` "Notes for M4" (AL-CLOCK before COV-MV; `atx-impl/tests/CMakeLists.txt`
  tail keep-both with AL-CLOCK's block first). Note ABS appended 3 lines to `atx-impl/CMakeLists.txt` (not the tests
  list).
- **Gate items carried (TEST-1):** C3b's NAV flag-absent run; ABS's IC flag-absent passes (canary goldens at 1 / 4
  workers, parents' identity runs); D3W's whole targets and the first valid `scripts/tests` seed-1 run; the fixture
  readers in the whole suites; every Release build. D3W review item 4: every mom-volman-v1 IC run now also publishes
  two sleeve files, so a directory digest or file list of such a run differs from a pre-merge one.
- **Freeze (C3B-S):** the `sign_pm7_35` G-P5 row (`scripts/tests/test_no_python_mirror.py:64-65`) still names lane
  `"E2"`: SIGN relabels it in its deletion commit, or root relabels it "P10 (SIGN not merged)" before `P9_FREEZE = True`.
- **PRE (round 2b-ii)**: Appendix IV item 14 = KEPT (sleeve line, ALCOMB-6 in root); item 5 stands from M2j; D3W
  review items 6 (v1 / v2-only readers refuse v3) and 9 (S-6), D3W-STALE beside PRE-40, PRE-40 for root's P9-M / P9-H
  manifests (`pm6-6-engine`), F.1 / F.2: ABS merged (the clean-source line has a producer).
- **Records:** `task-D3W-report.md` is not in `f0726acb`; it arrives with D3W-PB's merge (pool-23 `dcddb01d`).
- **Tools of this group:** `m3-logs/tools/build.sh` (HOST-5 waiter via `busy.ps1`; warnings counted by
  `warn_count.py`: the research-build log is UTF-16, so a plain `grep -c warning` on it counts nothing; the earlier
  groups' "0 warning lines" came from that grep (M2's p9-2t log re-counted with `warn_count.py`: 0), while exit 0 under
  `/WX` is what proves no warning);
  `keep_both.py BASE OURS THEIRS MERGED FILE...` (every hunk of both sides kept); `abs_caps.py EXE`; `gt.sh`,
  `pyt.sh`, `fixture_chain.sh` as before. Never edit `build.sh` while a build runs (M3-S1).
- Interpreter `C:/Program Files/Python312/python.exe` explicitly; create the `--basetemp` parent dir first; long records
  with the editor tool (the Bash heredoc fails on apostrophes).

## Kept and deleted outputs

- Kept: root's Debug object tree `build-equity/` and `build-equity-rel/` (C: about 45 GB free). The three new receipts
  and build logs (`build-equity/mega-p9-2{u,v,w}-*`, copies in `m3-logs/`).
- Lane pools: nothing deleted. Pool-15 and pool-28 leftover build trees may be deleted by name (DISK-2); pool-23's are
  needed by D3W-PB (D3WPB-SEQ).
- Session scratchpad `root-m3/` (merge messages, basetemps, D3W identity archives / outputs, fixture-chain catalogs):
  deleted by literal name at the end of this group.

## Uncommitted state at the end

- None of root's. Five untracked `docs/plans/*.png`: not root's, left. Any ledger lines the PM adds after this commit.

## Permission refusals

- None.

## Open questions for the PM (one line each)

1. SQL2-CLS: root gave `atx.od3-flagged-members/v1` its own class `od3-drop-list` (S-6) instead of the proposed `cycle-spec`; confirm, or rule `cycle-spec` (a one-row move, guard and fixture chain unaffected either way).
2. The Debug NAV exe (p9-2p) and `atx-impl-tests` are stale by source after M3; rebuild them at M4 (one more Debug build) or leave them to the gate?
