# Handoff: root after M6 (RESUME 11: AL-CLOCK fix 1, SQL3 item 5), 2026-10-15

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Started from `handoff-root-m5b.md` (code head `bba4c06f`,
docs head `97505c1c`, next free tag `p9-3d`, known-red set EMPTY). Rows: `integration-log.md` "M6 (RESUME 11)"; detail:
`root-wave3-merge-report.md` "Step group M6 (RESUME 11)" and sections "AL-CLOCK fix 1", "SQL3 item 5"; evidence
`m6-logs/` (tools `m6-logs/tools/`).

- **Status: DONE.** Both lanes of the dispatch merged whole at their MERGE-SHAs, 0 conflicts, no slip, no red.
- **Code head: `d51d011e`.** Commits of the group, in order: AL-CLOCK fix 1 merge `1d003241` (at `eed512bd`; code
  `f892eab1`), SQL3 item 5 merge `d51d011e` (at `22b8fd17`; code `00779453`). Both trees equal
  `git merge-tree --write-tree` of their parents (`91d2526a`, `6c73537a`).
- **Docs head:** the records commit that adds this file (`git log -1 --format=%h -- <this file>`). It carries this
  file, the M6 sections of `root-wave3-merge-report.md`, the M6 rows of `integration-log.md`, `m6-logs/`, and
  `progress.md` exactly as the PM left it (no ledger line edited by root).
- **Trial ledger** `build-equity/trials.jsonl`: 133 lines, sha256 prefix `27e40f9f` at start and end (0 trials).
- **SQL3-2** holds: `build-equity/research-store/` absent at start, around the SQL3 runs, and at the end.
- **Build tag used: `p9-3d`** (Debug `equity-dev`, source `1d003241`, `atx-impl-strategy-target-tests,
  atx-equity-strategy-targets`, exit 0, 0 warning lines, 5 TUs / 3 links). **Next free root tag: `p9-3e`.**
- **Known-red set: EMPTY.** Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` stays a named skip.
- No lease touched. No permission refusal. No data opened; no file under `C:/atx/atx-db` opened; `holdout_gate.py` not
  run; no statistic printed or recorded. Host: no other build of `C:\atx-wt\` at the admission; free RAM about 3.5 GB;
  C: 57.7 GB free.

## Results (TEST-1 / ROOT-1a: targeted checks only)

| lane | checks | result |
|---|---|---|
| AL-CLOCK fix 1 | `InfoClock*` on p9-3d target-tests | 14 / 14 (13 + the calibrate gtest `InfoClockRate.CalibrateGrossKeepsTheRatesAndCountsOnce`) |
| | named filter `InfoClock*:NavSpec.*:NavRules.*:TradeRateRegistry.*:NavAnnMeanGross.*:CostLawRegistry.*:TargetRegistry.*` | 26 / 26 |
| | `scripts/tests/test_info_clock_rule.py` seeds 0 / 1 | 5 / 5, 5 / 5 |
| | registration sentence (ALCLOCK-4) | both clauses verbatim at `info-clock-rate-v1.json:51`; SHA-256 `7801c19880a225a2f4c6ecb8756117f03fccafa71b92f440830bfb6fdcb8a152`; template `95d4104a...` unchanged |
| | flag absent (capacity sentence conditional) | `capacity_declaration` text unchanged (`cap_decl.py`), selected whenever the rule is off; tests pass; P9-B0 parents' byte identity at the gate |
| | W3-9, SQL2-CLS | nothing to keep both (no CMake / registry tail touched); no new literal |
| SQL3 item 5 | hook / identity / wave-queue files, seed 0, root's exes | 13 / 0 (real-exe case ran); 16 + 6 subtests / 0 (`FixtureChain` ran); 6 / 0 |
| | the seven writers' files (ROOT-1a, with the p9-2z eval verb) | 98 / 0 / 0 skipped |
| | one ingest per write; SQL3-2; SQL2-CLS | hold; holds; none |

The dispatch's "`InfoClock*` 24 + 1" matches no record; the counts above equal the lane's and the reviewer's.

## Exes (EXE-CUR)

Debug `build-equity/bin`; on-disk SHA-256 = the newest Debug receipt's for every exe at the end of the group
(`m6-logs/exe-sha-end.txt`, 20 exes, all `disk = receipt`; SPDLOG-RELINK: re-hash against these receipts before using one
as evidence).

| tag | source | receipt | exe | SHA-256 | current by source at `d51d011e` |
|---|---|---|---|---|---|
| **p9-3d** | `1d003241` | `build-equity/mega-p9-3d-receipt.json` (copy in `m6-logs/`) | atx-impl-strategy-target-tests | `685b8ba2947f878a9aa0a6a77cd7e4639c83588efc1cb398519be735efb13454` | yes |
| **p9-3d** | `1d003241` | same | atx-equity-strategy-targets (NAV) | `7e101b564d800c9e4b57541e11a30d17d8f32d5f74fd2c5ccc95d3b2e5b5f0dc` | yes |
| p9-2z | `138e1d2c` | `build-equity/mega-p9-2z-receipt.json` | atx-research-eval | `4791019131c0d74603b83cb772ef027ec8641167fd95844c4674e75bdf39893b` | yes |
| p9-3a | `6088bfe9` | `build-equity/mega-p9-3a-receipt.json` | atx-engine-research-composition-tests | `76b664b7483a77a717015a3fdebb38f97dc39fbcfecd04a98e52d3cd016023a9` | yes |
| p9-3a | `6088bfe9` | same | atx-research-composition | `212f8b9941f0be705ebac138c03e83279fc863ab12051f00dd09501db7eed98e` | yes |
| p9-3a | `6088bfe9` | same | atx-research-admission | `411af33a0def5a6c28983dbf1ffc2fbc324694e8ac2e6b24401b27b693c9fa26` | yes |
| p9-3b | `1dd9b9b1` | `build-equity/mega-p9-3b-receipt.json` | atx-equity-strategy-ic (IC exe) | `ef3974cfa82d1cc86d6a8079394e5d0964e8abcea8fd36a068b12f8b8bc99b38` | **no**: links `atx-impl-strategy`, which holds AL-CLOCK's `book/nav_cli.cpp`, `book/nav_lockstep.cpp`, `strategy_nav_v7.cpp` |
| p9-3b | `1dd9b9b1` | same | atx-impl-strategy-ic-tests | `9cd667c75c08a9197f4267c7dc1e407630beb84f9163739227e73f8d28eedf56` | **no** (same) |
| p9-3c | `bba4c06f` | `build-equity/mega-p9-3c-receipt.json` | atx-engine-research-fields-tests | `39be2b3d995b11873b5d667c04419b5b063f1f2f56729bce1816eed1b30febdf` | yes |
| p9-3c | `bba4c06f` | same | atx-research-fields | `d14346e836923cc8dea8ebd05b9769f0449b44e0f9f9150866f31d5a2e875cde` | yes |
| p9-2y | `5c811b56` | `build-equity/mega-p9-2y-receipt.json` | atx-engine-book-tests | `6977a5a75b386d283c81e148c0fec31a2da1f95536dc98cab2366d424d1f8360` | yes (`atx::engine` unchanged since M5) |
| p9-2r | `fd084e78` | `build-equity/mega-p9-2r-receipt.json` | atx-research-store / catalog tests | `d8a500d84dc2a25130c69b226d8f5deec47b8dabb6256522c83c00764d3d9d65` / `f1134aaa72338346fcb642ddd57ff15fe6e59cdeb078f1c20782dca45c0f78ed` | yes |
| p9-2v | `08f04e07` | `build-equity/mega-p9-2v-receipt.json` | atx-engine-research-eval-tests | `dd8a3103828b4a671c048d569eba4d817d0e91acf49bd2fb61983e5777ddb7ca` | **no** (`atx::engine` gained `mv_target.cpp` in M4) |

**Stale by source after M6** (rebuilt by the first step that needs them, EXE-CUR; the gate's builds otherwise): the
Debug IC exe and IC tests (newly stale: AL-CLOCK's NAV sources live in `atx-impl-strategy`), the eval tests,
`atx-impl-tests`, every other Debug exe linking `atx-impl-strategy` (`atx-equity-strategy`, `-risk`, `-mine`,
`atx-impl`) and every Release exe (p9-2q / p9-2s). The Debug NAV exe and target-tests are current again (p9-3d).

**EXE-FREEZE:** M6 relinked the Debug NAV exe (p9-3d) before the wave-3 P9-B0 re-pin. The IC exe is stale and must be
relinked before P9-B0 too (the gate's builds). Any relink of the NAV, IC or eval exe after the re-pin redoes P9-B0
and the re-pin first.

**Which exes the gate still needs:** every Release exe (built at the gate), the Debug whole test targets (whole
`atx-impl-strategy-target-tests` can use p9-3d as is; whole `atx-impl-strategy-ic-tests`, `atx-impl-tests`, eval tests
need a rebuild), the Debug IC exe (rebuild), and the current Debug exes above as they stand (NAV, eval verb, store,
fields, composition, admission). M6b's two merges may relink some of them (A4: the fields tests; SQL4: per its report):
re-check by receipt.

## What the next root group (M6b) must know

- **A4 fix 1 (rulings A4-FIX1-REAPPLY, A4-RR1-S; ledger "Task A4 fix 1: APPROVE at `87df9e00`", NOT a MERGE-SHA):**
  the lane re-applies `916f837f`, `528beddf` and the review commit `87df9e00` on `feat/p9-a4-fix1-20261015` from
  `bba4c06f` (cherry-pick by SHA, the reviewer's `a4-evidence/rr1/` files in one records commit, diff of diffs empty);
  the reviewer (continued) names the re-applied head as the APPROVE SHA. Root merges THAT SHA (MERGE-SHA), one
  Debug build (`atx-engine-research-fields-tests`; tag `p9-3e`), the filter 32. The fix touches only `atx-engine/tests/`;
  M6 touched only `atx-impl/` and Python, so a plain merge is expected (re-check with `git merge-tree --write-tree`).
- **SQL4 iff APPROVE, LAST** (before the gate's P9-B0 re-pin, else P10 whole, SQL-12): the real X-5 u / w identity and
  the marginal shard identity per its report; SQL2-CLS for any new literal.
- **Gate items carried (TEST-1)**, added by M6 to `handoff-root-m5b.md`'s list: AL-CLOCK's flag-absent identity
  procedure (`task-AL-CLOCK-report.md` "Flag-absent identity procedure": X-5 and Y-F0 NAV argv and `--spec`, every
  file byte-identical outside `producer`; the `--capacity-curve` parents at the fixed rate keep "; fixed rate only; x1
  is S2 bit for bit"), its opt-in check (ALCLOCK-1), the Debug whole target-tests, the Release leg and the canary; the
  W1-I2 re-pin reads the registration SHA `7801c198...`; SQL3's G-P10 print from the merged tree ((d) PARTIAL "three
  writers hooked, the stage record's side files not", SQL3-S2 S-2 / S-3; (e) landed for `atx.run-store/v1`) and review
  1's F-1 strip for console byte comparisons; whole `scripts/tests` seeds 0 / 1 (`cycle_resume.py`, `cycle_verdict.py`,
  `wave_stage_record.py`, `wave_queue.py` changed) and the stage chain.
- **P10 list grows by** ALCLOCK-S2 S-A (the rule-on test of "x1 is S2 bit for bit") and S-B (one shared test header for
  the PM6-6 readers), SQL3-S2 S-1 (shared hook-test helper), S-2 (the stage record's side files unhooked), S-4 (review
  1's untaken Suggested).
- **PRE (round 2b-ii):** item 16 (B.4 P9-C) takes the registration SHA `7801c198...` (template `95d4104a...` unchanged)
  and ALCLOCK-4's two clauses (merge report "AL-CLOCK fix 1" "For PRE"); items 31 / 33 unchanged (`source_sample_end`
  2019); SQL3 adds nothing.
- **Tools of this group** (`m6-logs/tools/`): `build.sh`, `busy.ps1`, `gt.sh`, `pyt.sh`, `warn_count.py` (from
  `m5-logs/tools`, logs dir `m6-logs`), `cap_decl.py OLD NEW` (the flag-absent capacity sentence at two commits),
  `exe_sha_end.py RECEIPT_DIR BUILD_DIR` (per exe, the newest receipt of that build dir vs the bytes on disk).
  Interpreter `C:/Program Files/Python312/python.exe`; create the `--basetemp` parent dir first; long records with the
  editor tool.

## Kept and deleted outputs

- Kept: root's object trees `build-equity/` and `build-equity-rel/` (M6b's builds and the gate's Debug and Release builds
  use them; the current Debug exes in the table above are their inputs until rebuilt); the p9-3d receipt, build and
  cache logs in `build-equity/` (copies in `m6-logs/`); every earlier receipt; the M4 probe worlds
  `build-equity/p9-m4-tw-*`.
- Created nothing under `build-equity/` except the p9-3d receipt and logs and the two relinked exes; the test runs wrote
  only into the session scratchpad.
- Deleted after the records commit, by literal name: the session scratchpad `root-m6/` (pytest basetemps) and my four
  extracts of lane records there (`alclock-report.md`, `alclock-rr1.md`, `sql3-report.md`, `sql3-rv2.md`).
- Lane pools: nothing deleted; pool-23's `build-equity/` untouched (TEETH-BIN-23).

## Uncommitted state at the end

- None of root's. Five untracked `docs/plans/*.png` (not root's) left. Any ledger lines the PM adds after this commit.

## Permission refusals

- None.

## Open questions for the PM (one line each)

- None.
