# Handoff: root GATE, chain agent 1 (GATE-1, RESUME 12), stopped at OWNER STOP 12, 2026-10-15

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Dispatch: `r12-dispatch.md` "ROOT GATE" (gate head
`06dc9e5a`, docs head `0a481348`, next free tag `p9-3f`, steps 0 and 8 DONE, start at step 1; GATE-IDX, GATE-PAR,
PAR-1), with `r11-dispatch.md` "ROOT GATE" + "ROOT GATE: rulings from the whole-wave review" and `root-gate-brief.md`.
Report (incremental, the record of every step): `root-gate-report.md`. Evidence and tools: `gate-logs/`
(`gate-logs/tools/`). Row: `integration-log.md` "GATE (RESUME 12), chain agent 1".

- **Status: DONE_WITH_CONCERNS** (OWNER STOP 12, "stop here and write handoff file"; the PM's message). Stopped at a
  clean point: no build, suite or bounded run of mine in flight at the records commit (the two suite invocations that
  were running when the stop came were let finish and are recorded below; nothing was killed).
- **Code head: `06dc9e5a`** (the gate head; this agent made no code commit: no slip found).
- **Docs head:** the records commit that adds this file (`git log -1 --format=%h -- <this file>`). Below it, beside
  root, the IDX docs agent's `5f48a25d` (`rulings-index.md`, PAR-1). The PRE agent's working-tree `p9-prereg.md` is
  NOT in root's commits (PAR-1: theirs to commit by pathspec).
- **Build tags used: `p9-3f`** (Release `equity-rel`, `-Canary`) and **`p9-3g`** (Debug `equity-dev`, `-Canary`).
  **Next free root tag: `p9-3h`.**
- **Trial ledger** `build-equity/trials.jsonl`: **133 lines, sha256 `27e40f9f...`** at start and at the stop (0 trials).
- **`build-equity/research-store/`**: absent at start and at the stop (SQL3-2).
- **GATE-IDX:** `build-equity/mega-candidate-cache-v8-lo3/index.sqlite` **still present at the stop** (73,728 bytes,
  sha256 `3a177dfddbd86dcc43caf0fca92339faad5e051bf4012b90effdb95cff6bc6f0`, the same file M6b left). Not touched by
  this agent; no run naming that cache was started (none of steps 1-2 names it). Its deletion is the owner's.
- **Known-red set: EMPTY** (see the step-2 results below: no red seen in what ran).
- **Permission refusals:** none.

## Done (evidence paths)

| step | item | result | evidence |
|---|---|---|---|
| 0 | start state re-verified | **holds**: code `06dc9e5a`, tree = five PNGs + the PM's / docs agents' sprint-dir edits, ledger 133 / `27e40f9f`, research-store absent, `ucrtbase.dll` 10.0.26100.7623 `2C08E77F...65EF` (= the CRT-2 record; unchanged), `ucrtbased.dll` 10.0.22000.832 `19E79E91...F48D`, free RAM 5.8 / 16.1 GB, C: 72.8 GB free | `gate-logs/start-state.txt`; `root-gate-report.md` "Step 0" |
| 1 | Release build p9-3f (27 targets: 11 exes incl. GATE-EXES' composition and admission + 16 test targets) | **holds**: exit 0, 244 TUs / 34 links, **0 warning lines**, canary (Release goldens) **15 passed** | `gate-logs/mega-p9-3f-*`, `builds.txt`, `research-build-p9-3f.out` |
| 1 | Debug build p9-3g (same 27 targets) | **holds**: exit 0, 68 TUs / 19 links, **0 warning lines**, canary (Debug goldens) **15 passed** | `gate-logs/mega-p9-3g-*`, `porcelain-before-p9-3g.txt` |
| 1 | SPDLOG-RELINK re-hash after the last link | **holds**: 27 / 27 Release and 27 / 27 Debug exes `disk = receipt` | `gate-logs/exe-sha-rel.txt`, `exe-sha-dbg.txt` |
| 2 | GATE-COVDIFF S-7 hand check (read-only; manifest `files` + container vs bytes on disk) | **holds** on all three container stores: `p9-m2i-dcov-v11` (manifest `39736647...` = pin), `p9-m2i-dcov-covv1` (`ea58691f...` = pin), `p9-m2d-cov-risk-container` (`9c12852e...` = the pin its M2i Y-1 NAV receipt names): every listed file and the container `model.atxcov` equal to the manifest; no unlisted file | `gate-logs/covdiff-s7-files.txt`, `tools/store_files_check.py` |
| 2 | Release gtests, whole targets (`tools/suite_gt.sh rel`) | **holds**: 16 / 16 targets exit 0; 4,098 run, 0 failed, 9 skipped, all named and pre-existing: `atx-impl-tests` 5 (`Alpha101Orats.RankByOptimizerSharpe`, `Alpha101Orats.RankBySharpeRiskParity`, `AtxImplDiscover.W6_RediscoverLowVolCapacityAlpha`, `FundamentalZoo.RealDataIcReport`, `SingleAlphaCapacity.SweepAndVerify`), `atx-engine-alpha-tests` 2 (`AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` OD5-2, `AlphaVm_ZeroAlloc.WarmDispatchLoop_BoundedByOutputsAndSizeIndependent`), `atx-engine-risk-tests` 2 (`RiskQpAugmentNightly.MatchesDenseOracleAcrossLargeBattery`, `RiskSectorColumnsById.CheckedIndexingCatchesThePreW0Read`); ENV-1 0 hits | `gate-logs/gt-exits.txt`, `gt-rel-*.log` |
| 2 | `scripts/tests` whole, PYTHONHASHSEED 0, Release exes (`GATE_BUILD=rel`) | **exit 1**: 1 failed, 702 passed, 3 skipped (wall 36,486 s from 20:20Z, spanning OWNER STOP 12; ENV-1 0 hits); the one red is `test_cycle_e2e.py::test_cycle_e2e_goldens_ic_reference_redundant_copy_and_idempotent_rerun`, refused at plan time by the W1-I1 guard (IC cache path 327 > 250 under the tool's TMP root in the session scratchpad) = GATE-TMP's cause; re-run pending (GATE-TMP) | `gate-logs/pyt-exits.txt`, `pyt-scripts-tests-rel-s0.log` |

## Exe table (step 1; source = gate code head `06dc9e5a`, receipt Source `5f48a25d`, the docs head then: no code path differs)

The full table with every test target is in `root-gate-report.md` "Step 1" (same values). The exes of record:

| exe | Release p9-3f (`build-equity-rel/bin`) | Debug p9-3g (`build-equity/bin`) |
|---|---|---|
| atx-equity-strategy-targets (NAV) | `255b58acedf7df82a1caa672ff30a58c317a8f0dccf7dbe7d3b7393b91063565` | `5dced610e204ba860bd2c8c8da5c3c6bd58a1ef98c242bec352a6b64d8762a8f` |
| atx-equity-strategy-ic (IC) | `ccd54db605f67fef1aa480050a6c785ffe179dae2f77e978dd6d01a5c117d705` | `adffe7421781145a9718e67be05b46038b89a46311f26390eeb3312da236d56d` |
| atx-research-eval | `e2b48c46055c6aefcda0b992f8c0341f7275274793bc30fbb07791f7a58d962c` | `4791019131c0d74603b83cb772ef027ec8641167fd95844c4674e75bdf39893b` |
| atx-research-composition | `04db0d67c1563184edd6fe8ff66796877c8db20239d5cfb423fa78e047df16f2` | `212f8b9941f0be705ebac138c03e83279fc863ab12051f00dd09501db7eed98e` |
| atx-research-admission | `f53d9277e4ad2e3915343ccca032df3f0098a1a1cd7b4d832b640f5f7437063f` | `411af33a0def5a6c28983dbf1ffc2fbc324694e8ac2e6b24401b27b693c9fa26` |
| atx-research-store | `0eeac21bb95677d8da5268ca829b88dc668521a4c9f625e0fbbde92f96fb3818` | `a54955e965ee2aa035e66cc9ed668a16246c089684a53515431dea428b605c73` |
| atx-research-fields | `883c5b74a95731eb310cfd30cb1ab39ee102e077ab2d7318bd5db941f486584a` | `1c670d86a0fdcae6a86b0eff56a184902d1eed8b244f8af6ab31425e18185fac` |
| atx-equity-strategy | `f883f81f0fbb928cb8fe2e8b1931841e0eef058e7b6398810e10de68cb0495a4` | `47d341c2541e792873ec5d3db0b739af6de61e9962ba46667c755a1ca56ca9a4` |
| atx-equity-strategy-risk | `0c110534dd2df43e995040a07e5368eac788166b5e55cc2e0e2fa5e088be586b` | `6696da719374777aefb49ce9e6b4707187549dff9b838e0c2338d21e852b47a1` |
| atx-equity-strategy-mine | `daf889437983628ccbc375e76a078d3decf079a9d3a79b6cca79cbcc4a063eba` | `4dee20571d4c4481bcf175d8bb688574ba95c60ae29d08b4bb474849f7945412` |
| atx-impl | `71f0c6311aaffffaa48f4c221c18c94e009a3b7ebc5a45798baff009d75bbdb9` | `7095d4477dd7fee80ead2d9bbdb516a726ed3ab943beed95ac1dc5335af63e55` |

Receipts: `build-equity/mega-p9-3f-receipt.json`, `build-equity/mega-p9-3g-receipt.json` (copies in `gate-logs/`).
EXE-FREEZE has NOT started (no re-pin yet). SPDLOG-RELINK: re-hash every exe against these receipts
(`python gate-logs/tools/exe_sha_end.py build-equity build-equity-rel` and `... build-equity build-equity`) before
using one as evidence; a relink after this hand-back needs a new receipt first.

## Reference dirs so far

None created (no P9-B0 run, no identity run). No `build-equity/p9-gate-*` dir exists.

## Not done

- **Step 2, the rest:** Debug gtests whole (`tools/suite_gt.sh dbg`); `ctest -L atx_research` and
  `-L atx_equity_strategy` on both trees (`tools/ctest_label.sh rel|dbg LABEL`); `scripts/tests` seed 1;
  `atx-engine/tools/test_stage_chain.py` alone; `atx-engine/tools` (the other test files, explicit paths);
  `atx-impl/tools`; `atx-impl/strategies`; the fields fixture pytests the fields review names
  (`atx-engine/tests/fixtures/research_fields/test_*.py`, `research_fields_holdings/test_*.py`, then
  `atx-engine/tools/test_research_fields_holdings.py` and `test_field_registry.py` separately);
  `scripts/tests/test_no_python_mirror.py` in its own invocation (IC review item 2); the three `cov-diff --legacy`
  bounded runs (GATE-COVDIFF; COV-9 role check first, `m2g-logs/tools/s9_check.py`); `v8-risk-lo3-v10` cited from
  M2d (`handoff-root-m2d.md:23`, `m2d-logs/cov-diff-legacy.log`: not re-run, as ruled).
- **Steps 3-7 and 9:** not reached. Step 8 DONE before this chain.

## Exact next step

**Step 2, next item:** the rest of step 2 in the order of "Not done"; GATE-TMP short roots. Then the rest of step 2 in the order of "Not done" (GATE-PAR: two low-RAM suites at
a time, e.g. a gtest target list beside one pytest invocation; the cov-diff bounded runs alone), then step 3.

Commands (cwd `C:/atx-wt/pool-2`, Git Bash; `L=.superpowers/sdd/platform-p9-20261003/gate-logs`;
`S=<your session scratchpad>/gate`; create `$S` first):

```
bash $L/tools/suite_gt.sh dbg                                   # Debug gtests whole (impl-tests about 20 min)
GATE_BUILD=rel bash $L/tools/pyt.sh scripts-tests-s1 1 $S/bt-s1 $L/pyt-scripts-tests-rel-s1.log -- scripts/tests
GATE_BUILD=rel bash $L/tools/pyt.sh stage-chain 0 $S/bt-chain $L/pyt-stage-chain-rel-s0.log -- atx-engine/tools/test_stage_chain.py
GATE_BUILD=rel bash $L/tools/pyt.sh engine-tools 0 $S/bt-et $L/pyt-engine-tools-rel-s0.log -- $(ls atx-engine/tools/test_*.py | grep -v test_stage_chain.py)
GATE_BUILD=rel bash $L/tools/pyt.sh impl-tools 0 $S/bt-it $L/pyt-impl-tools-rel-s0.log -- atx-impl/tools
GATE_BUILD=rel bash $L/tools/pyt.sh impl-strategies 0 $S/bt-is $L/pyt-impl-strategies-rel-s0.log -- atx-impl/strategies
bash $L/tools/ctest_label.sh rel atx_research ; bash $L/tools/ctest_label.sh rel atx_equity_strategy   # then dbg
```

`tools/pyt.sh` with `GATE_BUILD=rel` points `ATX_EQUITY_BIN`, `ATX_EQUITY_BUILD_TYPE=Release`, `ATX_EQUITY_IC_EXE`,
`ATX_EQUITY_TARGETS_EXE`, `ATX_RESEARCH_{STORE,ADMISSION,COMPOSITION,FIELDS}_EXE` at the Release p9-3f exes
(`ATX_EQUITY_ROOT` = `build-equity`), so the fitter `RealExecutables`, the sign-verb file, the n37 test and the
canary inside `scripts/tests` run on the producer of record (IC review item 2, Python review item 1, fields review
item 2); TMP / TEMP go under the basetemp. `tools/gt.sh` / `suite_gt.sh` log one line per target to
`gt-exits.txt`; `tools/ctest_label.sh` to `ctest-exits.txt`. A red under load is re-run alone once (GATE-PAR, ENV-1)
before it is attributed (ROOT-1 rows by merge commit: `git log --first-parent --merges` over the file).

## What the next root agent must know

- **GATE-IDX (binding):** before the FIRST run naming `--candidate-cache build-equity/mega-candidate-cache-v8-lo3`
  (step 3's X-5 / Y-F0 u and w identity; step 4's Y-F0 u / w; step 7's opt-in cache identity) check that
  `index.sqlite` is absent and that `python m6b-logs/tools/dir_listing.py build-equity/mega-candidate-cache-v8-lo3
  <out>` prints listing `155872de50833fca` (copy in `gate-logs/tools/dir_listing.py`); if still present: finish every
  step that does not name the cache, commit, hand back BLOCKED-OWNER naming the file.
- **Step-3 parts that do NOT name the cache** (they can run while the file exists): the NAV flag-absent identity of
  X-5 (`p9-c1-s2-x5-nav`, M2J-2) and Y-F0 (`p9-b0-yf0-nav`) through argv and through `--spec` on the Release NAV exe,
  and ALCLOCK-1's `--info-clock-print` opt-in on Y-F0 (no NAV reference argv names the candidate cache: checked on
  the receipts); tool `gate-logs/tools/nav_run.py` (`printspec` first: re-print the NavSpec with the gate's exe), and
  `m2g-logs/tools/nav_cmp.py` (allow `summary.json:/producer/build_type`, `/producer/engine_git_sha`); the receipts
  must carry no `store` block (`nav_run.py` prints it). The golden `0x889874a3b9b29c55` at 1 and 4 workers is the
  mine-tests `SignalFitnessDefaults.*` and factory-tests `NsgaSearch.ScalarRaw_*` cases inside the whole-target runs
  (read their names out of `gt-{rel,dbg}-atx-impl-strategy-mine-tests.log` / `-atx-engine-factory-tests.log`).
  The canaries ran inside both builds (15 / 15 each). D3-Q2 / D3-S1 greps over `scripts/specs/p9/`.
- **Step-3 items that need preparation:** D3-Q1a needs `--engine-fit --engine-factors F --admission-exe A
  --composition-exe C` on the Y-F0 TRAIN fit argv (`p9-b0-yf0-fit-run/receipt.json`), with F = an
  `atx-equity-strategy-targets factors` output of Y-F0's TRAIN role and library (roster order) made by the Release NAV
  exe (`wave-review-ic.md` section 4 and M-2: record F's manifest SHA = `provenance.engine_fit.factors_manifest_sha256`);
  compare `admission.{json,csv}` whole and `composition_weights.json` with `$.provenance.engine_fit` deleted
  (canonical JSON SHA-256); commit the tool and its output. COMPOSE-1 needs a store that opens against the tiny
  world's role (the C++ fakes live only in `strategy_spo_fixture.hpp` / `strategy_cov_fixture.hpp`); no ready Python
  tool exists: the parse stage alone was already probed by the NAV reviewer (`wave-review-nav.md` 3.2).
- **Release IC exe and the real cache:** `strategy_ic_signal_cache.cpp:108-157`: the Release build flavour has its own
  VM identity (`dslvm1_clang18.1` + the Release suffix) and its own cache root `DIR/<identity>/`; the real cache
  `mega-candidate-cache-v8-lo3` holds only the Debug (legacy-layout) root. So the first Release u / w on that cache
  (step 3 identity, step 4 P9-B0, every later cell) is a COLD pass that writes a new Release subtree of about the
  real cache's size (about 7 GB) INTO that pinned cache dir, and its listing will no longer be `155872de50833fca`
  afterwards (only additions; the Debug files untouched). See the open question below.
- Tools of this agent (`gate-logs/tools/`): `build.sh TAG PRESET TARGETS [canary]`, `busy.ps1`, `warn_count.py`,
  `exe_sha_end.py`, `gt.sh`, `suite_gt.sh rel|dbg [TARGET ...]`, `ctest_label.sh rel|dbg LABEL`, `pyt.sh` (GATE_BUILD),
  `store_files_check.py STORE [MANIFEST_PIN]`, `nav_run.py`, and M6b's `dir_listing.py`, `cmp.py`, `rerun.py`,
  `counters.py` (copies). Interpreter `C:/Program Files/Python312/python.exe`.

## Kept and deleted outputs

- Created: the two build receipts / logs under `build-equity/` (`mega-p9-3f-*`, `mega-p9-3g-*`); the rebuilt exes in
  both bins; `gate-logs/`; the session scratchpad `gate/` (pytest basetemps).
- Deleted: nothing (no regenerable output of mine besides the basetemps, which the next agent may delete by literal
  name: `<this session's scratchpad>/gate`).

## Uncommitted state at the stop

- Not root's: the five untracked `docs/plans/*.png`; the PRE agent's `p9-prereg.md` edits (PAR-1); any ledger lines
  the PM adds after the records commit.

## Open questions for the PM (one line each)

- The first Release u / w on `mega-candidate-cache-v8-lo3` writes a new Release cache subtree (~7 GB) into that pinned real cache dir (a cold pass; the Debug entries untouched): accepted as the cells' normal cache use, or should P9-B0 name a separate Release cache dir (an argv change, a substitution-list item)?
- COMPOSE-1's "fake store on the tiny world" has no ready tool (the fakes are C++ test fixtures only): build a tiny store with the Release risk exe on the tiny world role, or accept the NAV reviewer's parse-stage probe (`wave-review-nav.md` 3.2) as the record?
