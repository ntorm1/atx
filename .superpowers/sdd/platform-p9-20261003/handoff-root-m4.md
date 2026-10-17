# Handoff: root after M4 (RESUME 10: D3, OD3P, AL-CLOCK, COV-MV merges), 2026-10-09

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Started from `handoff-root-m3b.md`. Rows:
`integration-log.md` "M4 (RESUME 10)"; detail: `root-wave3-merge-report.md` "Step group M4 (RESUME 10)" and sections
"D3", "OD3P", "AL-CLOCK", "COV-MV"; evidence `m4-logs/` (tools `m4-logs/tools/`).

- **Status: DONE.** Four merges, in the PM's order, each at the APPROVE SHA the ledger named at its slot (MERGE-SHA).
  One conflict in all (COV-MV: the `atx-impl/tests/CMakeLists.txt` tail, keep both, AL-CLOCK's block first). Every
  root item of the four reviews holds or is carried by rule (gate items under TEST-1; P0 / P9-B0 / PRE items to their
  owners). No stop, no unattributed red, no identity miss.
- **Code head: `5c811b56`** = merge `0a2ceb2b` (D3 at `e197b3b6`) + merge `7f6f1c75` (OD3P at `dc11c977`) + merge
  `858a4fca` (AL-CLOCK at `e9b3a323`) + merge `5c811b56` (COV-MV at `25e11b32`). No SQL2-CLS commit (no new literal).
- **Docs head:** the commit that adds this file. It also commits `progress.md` exactly as the PM left it in the working
  tree (no ledger line edited by root), the merge report, the integration log and `m4-logs/`.
- **Which heads were merged:**
  - D3 `e197b3b6` (review of `699440bb`).
  - OD3P `dc11c977` (re-review 1 of `97a8157f`). Its follow-up round (pool-21, T2 / S-a / S-d / OD3P-T2V, re-review 2,
    BASE `dc11c977`) is M5's second OD3P merge.
  - AL-CLOCK `e9b3a323` (first review of `7ebd41ff`; code `1cbe5233`): at its slot the ledger had no fix-1 APPROVE line
    (newest `Task AL-CLOCK: APPROVE at 7ebd41ff`). **Fix round 1 (ALCLOCK-S; pool-30 `feat/p9-alclock2-20261007`,
    working when M4 merged) remains for M5** as a second AL-CLOCK merge at its own APPROVE SHA, after
    `task-AL-CLOCK-rereview-1.md` (BASE `e9b3a323`).
  - COV-MV `25e11b32` (re-review 1 of fix round 1 `942f499b`; the ledger's `Task COV-MV: APPROVE at 25e11b32` and the
    PM's message): fix round 1 is IN; **no COV-MV merge remains for M5**.
- **Trial ledger** `build-equity/trials.jsonl`: 133 lines, sha256 prefix `27e40f9f` at start and end (0 trials).
- **Build tags used: `p9-2x`, `p9-2y`** (both Debug, `equity-dev`, exit 0, 0 warning lines). **Next free root tag:
  `p9-2z`.**
- **Known-red set:** exactly `atx-impl/tools/test_nav_summ_v8.py::test_legacy_n37_numbers_reproduced` (root-only; runs
  only with `ATX_EQUITY_ROOT`; B2-N37, merge `5dfb0ba1`) until B2 fix round 2 lands. No new red in M4. Release
  `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` stays a named skip.
- **SQL3-2** holds: `build-equity/research-store/` absent at start and end. **SQL2-CLS:** none of the four lanes adds a
  literal; classes guard 5 passed after each of D3, OD3P and COV-MV (AL-CLOCK's tree is inside the last run).
- No lease touched (LEASE-DEAD). No permission refusal. No data dated 2024 or later opened; no statistic read (every
  comparison by SHA, argv token, count, exit code or boolean); `holdout_gate.py` not run. No host wait: no other build
  of `C:\atx-wt\` ran at either admission (HOST-4 / HOST-5; free RAM about 4 GB).

## Exes (EXE-CUR)

New this group (Debug `build-equity/bin`; on-disk SHA-256 = the receipt's at every test run and again at the end of the
group, `m4-logs/exe-sha-end.txt`; SPDLOG-RELINK: re-hash against these receipts before using one as evidence):

| tag | source | receipt | exe | SHA-256 |
|---|---|---|---|---|
| p9-2x | `858a4fca` | `build-equity/mega-p9-2x-receipt.json` (copy in `m4-logs/`) | atx-impl-strategy-target-tests | `68a26abd619d0f15987788d84f4c40193ee2c2c244cea58effa69be3b018eba0` (overwritten by p9-2y) |
| p9-2x | `858a4fca` | same | atx-equity-strategy-targets (NAV) | `d255a0c3b6914ddca792b5ab748fe796162472e191b769664b90a1f088b25b7f` (overwritten by p9-2y) |
| p9-2x | `858a4fca` | same | atx-equity-strategy-ic | `84fdbae1e732fb1485cc88f6de38082a6380addb46ed71115104093e986bb44e` (overwritten by p9-2y) |
| **p9-2y** | `5c811b56` | `build-equity/mega-p9-2y-receipt.json` (copy in `m4-logs/`) | atx-impl-strategy-target-tests | `ee69e2fb06db6702b78a9e8cbb68a9a13ca25ccc5fa578ada0c3d90b9afd8b8e` |
| **p9-2y** | `5c811b56` | same | atx-engine-book-tests | `6977a5a75b386d283c81e148c0fec31a2da1f95536dc98cab2366d424d1f8360` |
| **p9-2y** | `5c811b56` | same | atx-equity-strategy-targets (NAV) | `e6b21410987a8946dcd9a5d0b5631a15fee76d682d2be987c46f1d05be4bd219` |
| **p9-2y** | `5c811b56` | same | atx-equity-strategy-ic | `fbb5b3928ebb994ed14d87efe8d28c9929af94636d666433151d33cc5e40a314` |

The p9-2x runs (AL-CLOCK's tests, the C3-5 probes, the live-table test) are recorded with their SHAs in
`m4-logs/gt-exits.txt`, `pyt-exits.txt` and `c3-5-probe.txt`.

**Current by source at `5c811b56` and unchanged:** the four p9-2y exes; Debug p9-2v `atx-research-composition`
`5e81e9c2...` and `atx-research-admission` `411af33a...` (their libraries link `atx::core` only; COV-MV's one line in
`atx-engine/CMakeLists.txt` is in the `atx-engine` library's source list); Debug store exe p9-2r `d8a500d8...` and
catalog tests p9-2r `f1134aaa...` (catalog / store libraries link `atx::core` only; no store / catalog / core source
changed).

**Stale by source after M4:** the Debug eval verb p9-2v `6962966c...` (`atx-engine-research-eval` links `atx::engine`,
which gained `book/mv_target.cpp`) and the eval / composition test exes of p9-2v if they link it; the IC tests p9-2w
`fa383e75...` (link `atx-impl-strategy`, changed by AL-CLOCK and COV-MV); `atx-impl-tests` (p9-2t); every other Debug
exe linking `atx-impl-strategy` or `atx::engine` (`atx-equity-strategy`, `-risk`, `-mine`, ...); every Release exe
(p9-2q / p9-2s). Release builds and the canary are the gate's (TEST-1); the first root step that needs one of these
rebuilds it (EXE-CUR).

**EXE-FREEZE:** the IC exe was relinked twice in M4 (p9-2x, p9-2y), before the wave-3 P9-B0 re-pin; any later relink
redoes P9-B0 and the re-pin first.

## Results of this group

| item | result (evidence in `m4-logs/`) |
|---|---|
| D3 | `0a2ceb2b`, 0 conflicts, keep-both PASS (`d3-keep-both.txt`, `d3-c3b-b2-lines.txt`); no build; `test_wave_kinds.py` 191 / 1 skipped (= the reviewer's three-way preview with C3b; the dispatch's 192 is D3's own head), C3-13 10, W3-8a 12, `test_wave_eval_verb.py` 7, guard 5; C3-5 (a) / (b) hold (`c3-5-probe.txt`); live-table Debug 1 / 1 twice; walk-forward constants = the fitter's (`d3-wf-flags.txt`) |
| OD3P | `7f6f1c75`, 0 conflicts (`cli.py` keep-both PASS); no build; lane files 74; guard 5 (class `od3-drop-list`, M3-Q1); od3 plan refuse / plan on fakes and on the real IC exe p9-2w: PASS (`od3p-plan-probe.txt`) |
| AL-CLOCK | `858a4fca`, 0 conflicts (keep-both with C3b PASS); p9-2x; named 25 / 25 (24 + C3b's case), C3b named 34 / 34; `test_info_clock_rule.py` 5; live-table 1; W3-9 tails hold; C3-5 with `--rate info-clock-rate-v1` to the spec stage: one `--rule`, one `--rate` |
| COV-MV | `5c811b56`, 1 conflict (tests CMake tail; AL-CLOCK first); keep-both PASS; reserve body both terms; p9-2y; `MvTarget.*` 12 / 12, `MvAimRule.*` 16 / 16, named 100 / 100 (1 disabled), AL-CLOCK named 25 / 25; `--list-rules --json`: only `mv-aim-v1` added, last and only target row, incompatible with `two-speed-v1`; `validate_specs.py` 0; live-table 1; guard 5; pins hold |

## What the next root group (M5) must know

- **AL-CLOCK fix round 1** (second AL-CLOCK merge, MERGE-SHA = the re-review's APPROVE commit): the COV-MV re-review
  states the fix head adds no line to either CMake tail. ROOT-1a: ONE Debug build (`p9-2z` if first) of
  `atx-impl-strategy-target-tests` + the NAV exe; `InfoClock*` + S-3's calibrate gtest; `test_info_clock_rule.py` (S-4
  reads a row fixture); the ALCLOCK-S1a text hunks (`nav_cli.cpp:95`, the `--book-workers` help,
  `nav_lockstep.cpp:216`) are help / refusal text only; S-2 changes `scripts/specs/p9/rules/info-clock-rate-v1.json`, so
  record its new SHA-256 for Appendix IV item 16 (M4 recorded the pre-fix `51b8b754...`). Re-run the live-table test
  if the NAV table moves (it should not).
- **OD3P follow-up** (re-review 2, BASE `dc11c977`): T2 on the verb, S-a / S-d with teeth, OD3P-T2V label, OD3P-SKIP
  (the real-verb case must run, not skip, in root: the verb exe is p9-2v, now stale by source after COV-MV, so the
  merge that runs it rebuilds `atx-research-eval` first, EXE-CUR).
- **OD3P P0 (before the OD-3 read, root's):** `scripts/specs/p9/od3/flagged_members.json` `rules_pending` still holds
  `mv-aim-v1`; COV-MV's registration says `source_sample_end` 2010 (< 2015), so the entry is removed and the file
  re-pinned (current sha256 `9aa6fb596a4edd0b2a8028a337678161efdfa50535d61dbe9c18fdff9f56f5ff`); until then line 4's
  plan is refused at plan time. Not edited in M4 (a pinned input of the read; P0 is the runbook's step).
- **D3W-PB** (`44a331e6`, ledger "Task D3W-PB"): `c044f5f6` with the TRAIN identity procedure, then `460f5ba2`, then
  `44a331e6`; S-g keep-both in `scripts/tests/test_no_python_mirror.py` comment lines (root's B2 line, then the two
  D3WPB-Q1 lines). SIGN (`50a43599`, SIGN-CLS: `atx.sign-receipt/v1` under `stage-receipt`), B2 fix 2, SQL3 item 5,
  A4 iff APPROVE, SQL4 last (SQL4-GO) as the PM orders.
- **Probe tool:** `m4-logs/tools/m4_c3_probe.py` is the working shape for a tiny-world v2 wave on real exes after B2:
  the base must name and pin the eval verb and be `lock --exes`-ed (bases `v5p` / `v6p`), else verify stops on
  B2-10 (c) / W1-I2 (attempts `d3a`, `d3a2`, `d3b2` show both stops). SEAL-ALLOW-3's per-hit check passes on the
  mechanics readers' `started_utc` lines only.
- **Gate items carried (TEST-1):** whole `scripts/tests` seeds 0 / 1 and the stage chain (D3, OD3P: seed 1 never ran on
  OD3P's fix code); D3-Q2 / D3-S1 greps; the live-table test on the Release build; AL-CLOCK's flag-absent X-5 / Y-F0
  identity and the `--info-clock-print` opt-in check; COV-MV's rule-absent identity (COVMV-BYTES (c)) and gold-run 1 /
  4 workers; Debug whole target-tests; every Release build and both canaries.
- **Before the first P9 cell (P9-B0 / PRE):** W1-I2 re-pin of the lineage parents (the NAV exe changed);
  ALCLOCK-1 `--info-clock-print` on the Y-F0 reference; Y-F0's fields manifest must list `ea_days_since`; ALCLOCK-4's
  prereg line (2b-ii); COV-MV's template `parent` and re-pin (PRE-29); round 2b-ii items 16 / 17 / 18 as recorded in
  the merge report's "For PRE" sections.
- **Freeze (C3B-S):** the `sign_pm7_35` G-P5 row still names lane `"E2"` until SIGN's deletion commit merges.
- **Tools of this group:** as `handoff-root-m3b.md` plus `od3_plan_probe.py REAL_IC_EXE TMPDIR`,
  `m4_c3_probe.py {base BASE | wave LABEL | report LABEL}`, `nav_rules_cmp.py BEFORE AFTER [ID]`. Pre-COV-MV NAV table
  saved as `m4-logs/nav-list-rules-p9-2x.json` (the p9-2x exe is overwritten). Interpreter
  `C:/Program Files/Python312/python.exe`; create the `--basetemp` parent dir first; long records with the editor tool.

## Kept and deleted outputs

- Kept: root's object trees `build-equity/` and `build-equity-rel/`; the two receipts and build logs
  (`build-equity/mega-p9-2{x,y}-*`, copies in `m4-logs/`); the twelve probe worlds `build-equity/p9-m4-tw-*`
  (synthetic, about 25 MB each, regenerable; the C3-5 evidence beside M2j's `p9-m2j-tw*`).
- Lane pools: nothing deleted (DISK-2 not in this dispatch).
- Session scratchpad `root-m4/` (merge messages, basetemps, the od3 plan probe's fake worlds): deleted by literal name
  at the end of this group.

## Uncommitted state at the end

- None of root's. Five untracked `docs/plans/*.png`: not root's, left. Any ledger lines the PM adds after this commit.

## Permission refusals

- None.

## Open questions for the PM (one line each)

1. Do `mv-aim-v1` and `info-clock-rate-v1` compose (neither row's `incompatible[]` names the other; no P9 cell names both): rule "compatible", or add a refusal in P10?
2. Who removes `mv-aim-v1` from `flagged_members.json` `rules_pending` and re-pins it (2010 < 2015): root at the gate before the OD-3 read (P0), as the runbook says?
3. C3b's five stale docstrings in `scripts/tests/test_wave_kinds.py` (comments only, a W3-8a kind file): leave to P10, or let lane D3's next touch fix them?
