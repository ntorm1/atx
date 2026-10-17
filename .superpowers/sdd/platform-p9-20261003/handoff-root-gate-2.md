# Handoff: root GATE, chain agent 2 (GATE-1, RESUME 13), BLOCKED-OWNER at step 4, 2026-10-16

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Task file `r13-prompts/gate-prompt-2.md` (binding), the
PM's mid-run Ruling TEST-2 (ledger :972-973). Report (the record of every step): `root-gate-report.md` "Status by step
after chain agent 2", "The rulings: state after chain agent 2", "Chain agent 2 (RESUME 13 ...)", "Steps 4-6", "Step
7, agent 2". Evidence and tools: `gate-logs/` (`gate-logs/tools/`). Row: `integration-log.md` "GATE (RESUME 13), chain
agent 2, 2026-10-16". The previous chain link: `handoff-root-gate-1.md` (its placeholders filled from its logs).

- **Status: BLOCKED-OWNER** (GATE-IDX). `build-equity/mega-candidate-cache-v8-lo3/index.sqlite` still present at
  13:40:01Z and at 13:45:27Z (72 KiB; the same file M6b left; never touched by this agent). Every step that does not
  name the cache was finished; step 4 (P9-B0) was not started. **Second blocker (new, for the PM):** the fields dirs
  `build-equity/train-2020-2023-lo3-fields-v10/` and `-v13/` (also v9, v14, v15a) are absent from disk (only their
  `-run` dirs remain): R-2's NAV argv of record (P9-B0 re-base) needs v10; X-5's NAV / u / w identity needs v13.
  Not recreated (`gate-logs/inputs-check.txt`).
- **Code head: `06dc9e5a`** (unchanged; no code commit). **Docs head:** the records commit that adds this file
  (`git log -1 --format=%h -- <this file>`); below it this agent's `c99a76e4` and `0c8bc8da`, and beside root by
  pathspec the PRE agent's `aef2e033` and the IDX-CHECK `73be0a13` (PAR-1).
- **Build tags used: none.** **Next free root tag: `p9-3h`.** Exes of record unchanged (agent 1's p9-3f / p9-3g;
  re-hashed `disk = receipt` 27 / 27 + 27 / 27 at step 0). EXE-FREEZE not started.
- **Trial ledger** `build-equity/trials.jsonl`: 133 lines, sha256 `27e40f9f...` at start and at hand-back (0 trials).
  `build-equity/research-store/` absent. No statistic read or recorded.
- **Known-red set: EMPTY.**
- **Permission refusals: 1** (`gate-logs/perm-refusals.txt`, exact command there): the unstage of an editor-side
  `gate-logs/tools/.mypy_cache` (133+ paths, git-ignored, STILL STAGED in pool-2's index, never committed). Every
  commit of this agent names explicit pathspecs plus
  `:(exclude).superpowers/sdd/platform-p9-20261003/gate-logs/tools/.mypy_cache`. **A bare `git commit` in pool-2
  would commit them**; the owner / PM may unstage by hand.

## Done (evidence paths, `gate-logs/` unless named)

| step | item | result | evidence |
|---|---|---|---|
| R13 | first act: agent 1's placeholders filled from its logs; one docs commit with the PM's files | DONE `0c8bc8da` | git |
| 0 | re-verification | **holds**: code `06dc9e5a`, tree = five PNGs, ledger 133 / `27e40f9f`, research-store absent, `ucrtbase.dll` 10.0.26100.7623 `2C08E77F...65EF` unchanged, `ucrtbased.dll` `19E79E91...F48D`, 27 / 27 + 27 / 27 `disk = receipt`; cache listing 352 files (`fb444f7d316a1a4c`; without `index.sqlite` = `155872de50833fca`) | `start-state-2.txt`, `cache-lo3-listing-agent2-start.txt` |
| 2 | GATE-TMP | closed: tool TMP root moved to a short root OUTSIDE the tree (`C:/Users/natha/AppData/Local/Temp/g2<inv>`); the ruled `build-equity/pt/<inv>` is refused by K3 (`test_cycle_e2e.py:212-217` `fresh_root`, `research_tree.py:75-81`; commits `d6083b33` / `fa61c67f`, pre-P9) | `tools/CHANGES.txt`, `pyt-tmp-probe-ruled-root-s0.log` |
| 2 | `scripts/tests` seed 0 re-run, seed 1 (Release) | 703 passed / 3 skipped / 0 failed each; skips `test_research_cycle.py:56/:706/:946` "set RESEARCH_CYCLE_LIVE_ROOT" | `pyt-exits.txt` |
| 2 | TEST-2 (a): the GATE-TMP single test | 1 passed (outside-tree short root) | `pyt-gatetmp-single-g2-s0.log` |
| 2 | Debug gtests 16 targets | 4,099 run, 0 failed, 6 named skips | `gt-exits.txt` |
| 2 | ctest `atx_research` / `atx_equity_strategy`, rel + dbg | 291 / 728 each, 0 failed, 1 disabled (`CovStageDigest.DumpPerStageOnTheLaneFixture`) | `ctest-exits.txt` |
| 2 | stage chain; `atx-engine/tools` rest; mirror guard; `atx-impl/strategies`; fields pytests | 10; 391; 7; 14; 4 / 26 / 16 / 37 | `pyt-exits.txt` |
| 2 | `atx-impl/tools` | 770 passed on the OS TMP (`GATE_PT=systmp`); on the g2 TMP one TMP-length-dependent golden failed (`test_mega_report_v8.py::test_v7_config_renders_the_pre_v8_bytes`: test fragility, P10 note) | `tools/CHANGES.txt` |
| 2 | TEST-2 (b) GATE-COVDIFF | COV-9 S9 holds (0 of 990); 3 x `cov-diff --legacy` exit 0, "legacy equal: 1405 sessions", no store block | `cov9-role-check.txt`, `covdiff-runs.txt` |
| 3 | COMPOSE-1a | ACCEPT at the store-backed parse stage (tiny world + Release risk store; deleted after) | `compose-1a.txt` |
| 3 | Y-F0 NAV identity (argv + `--spec`); ALCLOCK-1 opt-in | hold (26 / 27 identical, summary producer stamps only; opt-in adds 70 `/scenarios/*/info_clock` leaves only) | `nav-identity-yf0.txt` |
| 3 | Y-F0 fit flag-absent identity | holds outside code-identity stamps (D2 merge `61378337`) | `fit-identity-yf0.txt` |
| 3 | D3-Q1a | **DIFFERENT** (admission identical; 694 weight float leaves differ): P9-W lapses at 0 (PRE-35); P9-F0h produced flag-absent | `d3q1a.txt` |
| 3 | D3-Q2 / D3-S1; golden; canaries | 0 / 0; OK both builds; 15 / 15 both builds | `d3-greps.txt`, `gt-*.log` |
| 7 | scoreboard `--timings`; PY-S S-6; CRT-2 | lineage reproduced (checks 4 / 4); equal (63 / 62); stores = pins | `scoreboard-gate.txt`, `pys-s6-ledger-n.txt`, `crt-record.txt` |

## Not done

- Step 3: X-5 NAV flag-absent identity (argv + `--spec`; needs fields v13); X-5 and Y-F0 u / w identity (name the
  cache: GATE-IDX; X-5 also v13).
- Step 4 P9-B0 (whole run set; substitution list first; R-2's NAV needs fields v10); step 5 rho; step 6 re-pin
  (EXE-FREEZE starts there; the commit's `git diff` lists pins only).
- Step 7: catalog (M2A-4), `verify --pins`, render identity, second catalog, G-P ticks, X-5 opt-in cache identity.
- Step 9: final records after steps 4-7.
- Cut by TEST-2 and recorded as "not run at the gate (TEST-2, owner 2026-10-16)": nothing; every item on the cut list
  had already run and been logged before TEST-2 arrived (they stand as run).

## Exact next step

1. Owner: delete `C:/atx-wt/pool-2/build-equity/mega-candidate-cache-v8-lo3/index.sqlite`; PM: rule on the missing
   fields v10 / v13 (rebuild from their `-run` receipts' argvs through the bounded runner, or substitute).
2. Next root agent: re-verify step 0 (`tools/exe_sha_end.py` both trees; ledger 133 / `27e40f9f`), then the GATE-IDX
   check: `python gate-logs/tools/dir_listing.py build-equity/mega-candidate-cache-v8-lo3 <out>` must print
   `155872de50833fca` and `index.sqlite` must be absent.
3. Step 3 rest: X-5 NAV identity (`tools/nav_run.py` argv + `--spec`, like the Y-F0 runs in `nav-runs.txt`), then the
   u / w identity runs (Release IC on the real cache: a cold pass that adds a Release subtree, about 7 GB, into the
   pinned cache dir: agent 1's open question, still open).
4. Step 4 P9-B0: commit the substitution list first, then the whole run set, each through
   `scripts/run_bounded_research.py` (`--seconds` <= 600, `--max-rss-mib 2048`; state free RAM: 4,367 / 16,069 MB at
   this hand-back); then steps 5, 6, 7, 9.

## Exe table

Unchanged: agent 1's table in `handoff-root-gate-1.md` "Exe table" (Release p9-3f `build-equity-rel/bin`, Debug p9-3g
`build-equity/bin`; receipts `build-equity/mega-p9-3{f,g}-receipt.json`). Re-hash before use (SPDLOG-RELINK).

## Reference dirs

None created (no P9-B0 run). Kept outputs (regenerable, about 35 MB, deletable by literal name after GATE-REV):
`build-equity/p9-gate-x5-nav-{argv,spec}-run`, `build-equity/p9-gate-yf0-{factors,fit-eng,fit-py,nav-argv,nav-clock,
nav-spec,sig}` and each one's `-run` dir. Deleted (own scratch, by literal name): the pytest roots
`C:/Users/natha/AppData/Local/Temp/g2*`, the GATE-TMP probe root, the COMPOSE-1a tiny world and store, the scoreboard
console / JSON in the scratchpad. Disk freed: those scratch roots only (no figure measured). Host at hand-back: C:
66.7 GB free; no build running.

## Next free tag

`p9-3h`.

## Open questions for the PM (one line each)

- GATE-TMP: the ruled short root `build-equity/pt/<inv>` is inside the work tree and is refused by K3 (`fresh_root`, pre-P9): accept the outside-tree root `C:/Users/natha/AppData/Local/Temp/g2<inv>` as the record?
- Fields v10 / v13 (and v9, v14, v15a) are absent from disk: rebuild them from their `-run` receipts (bounded) as gate inputs, or rule a substitution for R-2's P9-B0 NAV and X-5's identity?
- The editor-side `gate-logs/tools/.mypy_cache` stays staged in pool-2's index (unstage refused, PERM-1): the owner unstages it by hand?
- Agent 1's open question (Release u / w writes a ~7 GB Release subtree into the pinned real cache dir) is still open.
