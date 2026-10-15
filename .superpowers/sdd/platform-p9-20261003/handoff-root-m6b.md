# Handoff: root M6b (RESUME 11: SQL4 merge), STOPPED at OWNER STOP 11, 2026-10-15

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. The GATE-0 root agent, continued (ledger "Dispatch root
M6b"; rulings SQL4-S, SQL4-ADD, STORE-JSON-P10, DISPATCH-COMMIT). Started from `handoff-root-gate0.md` (code head
`7320e1b9`, docs head `fb566b44`, next free tag `p9-3e`). Evidence and tools: `m6b-logs/` (tools in
`m6b-logs/tools/`).

- **Status: STOPPED (OWNER STOP 11, "stop here and write a status file").** The merge and its one build are done;
  every post-build check is NOT done. Nothing was in flight at the stop: the build had ended (exit 0) before the stop
  arrived; no identity run, gtest or pytest was running; none was started after it.
- **Code head: `06dc9e5a`** = `Merge lane SQL4 at 44a76937 (first review APPROVE; code c5df575d)` (parents
  `fb566b44`, `44a76937`; MERGE-SHA). 0 conflicts; the merge tree `b5b4eec93ac2a8701b312867ec302ceab1231a05` equals
  `git merge-tree --write-tree fb566b44 44a76937` (`m6b-logs/merge-checks.txt`). No WIP commit was needed: no code
  edit is uncommitted. This is the intended gate head ONLY once the checks below pass (a red that no slip fixes stops
  the merge: not reverted; BLOCKED).
- **Docs head:** the records commit that adds this file (`git log -1 --format=%h -- <this file>`). It carries this
  file, the M6b row of `integration-log.md`, `m6b-logs/`, `progress.md` and `r11-dispatch.md` as they stand
  (DISPATCH-COMMIT; no line of either edited by root).
- **Build:** tag **`p9-3e`** used (Debug `equity-dev`, source `06dc9e5a`, targets
  `atx-equity-strategy-ic,atx-impl-strategy-ic-tests,atx-research-store,atx-engine-research-catalog-tests`; exit 0;
  0 warning lines; 48 TUs / 7 links; 74 s; no configure needed; DirtyEntries 7 = the two PM files in the sprint dir
  and the five untracked PNGs, no code). **Next free root tag: `p9-3f`.**
- **Trial ledger** `build-equity/trials.jsonl`: 133 lines, `27e40f9f` at the start of M6b (14:17) and at the stop
  (14:32).
- **SQL3-2:** `build-equity/research-store/` absent at the start and at the stop; no catalog was written there (the
  only catalog of this group is under the session scratchpad `root-m6b/`).
- **Known-red set:** EMPTY as of M6 / GATE-0; the merged tree's gtests and pytests have NOT run yet.

## Done (with evidence)

| step | result | evidence |
|---|---|---|
| merge pre-checks | SQL4's code range `475f5d8f..44a76937` touches no `classes.json`, no Python, no tripwire test source | `m6b-logs/merge-checks.txt` |
| merge | `06dc9e5a`, 0 conflicts, tree = recompute | `merge-checks.txt` |
| tripwire sources | `git diff fb566b44 06dc9e5a -- atx-impl/tests/strategy_ic_runner_test.cpp atx-impl/tests/strategy_marginal_ic_test.cpp` empty; 0 added / removed lines naming a pinned source list or a 64-hex pin | `merge-tripwires.txt` |
| SQL2-CLS | no new `atx.*` literal added to production code by the merge (grep of the merge diff); guard not yet re-run | `merge-tripwires.txt` |
| SQL-11 | `--candidate-cache-index` / `--pair-cache-index` appear outside `atx-impl/src`, `atx-impl/tests`, `docs/plans` and the sprint dir only in the `atx-impl/CMakeLists.txt:315` comment; the prereg line `p9-prereg.md:780` forbids them on every cell argv | `merge-tripwires.txt` |
| real cache dir, start | `build-equity/mega-candidate-cache-v8-lo3`: 351 files, 7,799,539,072 bytes, listing digest `155872de50833fca`; the same after the two pre-merge runs | `cache-lo3-listing-start.txt`, `cache-lo3-listing-after-pre.txt` |
| X-5 pre-merge references (item 8, 1a's "pre" side) | w and u through the bounded runner (`--seconds 600 --max-rss-mib 2048`), IC exe p9-3b `ef3974cf...` (source `1dd9b9b1`), real cache dir: both `completed`, exit 0; every candidate a cache hit (58 / 58, ic_results 58 / 58, vm_evaluations 0). Outputs `build-equity/p9-m6b-x5-w-pre` (12 files) and `p9-m6b-x5-u-pre` (5 files: `orientations.json`, `recipe.json`, `summary.json`, `train_candidates.jsonl`, `train_daily_ic.csv`) | `run-x5-w-pre.log`, `run-x5-u-pre.log`, `ic-exe-pre.txt`; runner dirs `build-equity/p9-m6b-x5-{w,u}-pre-run` |
| fixture catalog, pre-merge store exe (S-1 "pre" side) | p9-2r store exe `d8a500d8`: `catalog_digest d32f7655...4e69`, `files_seen 28 verified 25 declared 0 skipped 4`, `verify --pins` 0, identity 12 / 0; all 25 tables dumped; `ledger_state` head_sha256 / head_rule NULL | `fixture-dump-pre.txt`, `pre-dump/` |
| build p9-3e | exit 0, 0 warnings (above) | `builds.txt`, `mega-p9-3e-receipt.json`, `mega-p9-3e-build.log`, `research-build-p9-3e.out` |

The pre-merge IC exe p9-3b is stale by source against `7320e1b9` only in AL-CLOCK's NAV TUs
(`atx-impl/src/book/nav_cli.cpp`, `nav_lockstep.cpp`, `strategy_nav_v7.cpp`) and fields-library TUs; none is on the
IC verb's path (`git diff --stat 1dd9b9b1 7320e1b9 -- atx-core atx-engine/src atx-engine/include atx-impl/src`), so it
is the pre-merge IC reference in substance; state it with the 1a result.

## Exes (EXE-CUR; `build-equity/bin`, receipt `build-equity/mega-p9-3e-receipt.json`, copy in `m6b-logs/`)

| tag | source | exe | SHA-256 (receipt) |
|---|---|---|---|
| **p9-3e** | `06dc9e5a` | atx-equity-strategy-ic (IC exe) | `adffe7421781145a9718e67be05b46038b89a46311f26390eeb3312da236d56d` |
| **p9-3e** | `06dc9e5a` | atx-impl-strategy-ic-tests | `55db8666c03533439ecadd10d9412f4c65167581e0a11a806de2572ea69ebd6b` |
| **p9-3e** | `06dc9e5a` | atx-research-store | `a54955e965ee2aa035e66cc9ed668a16246c089684a53515431dea428b605c73` |
| **p9-3e** | `06dc9e5a` | atx-engine-research-catalog-tests | `b73b2199db7fd1bc451bd12872a5eb0851453db4299ed29e812dbbd151d9835a` |

Superseded by p9-3e: the IC exe / IC tests p9-3b (`ef3974cf`, `9cd667c7`) and the store / catalog tests p9-2r
(`d8a500d8`, `f1134aaa`). The other Debug exes stand as `handoff-root-m6.md` lists them (NAV p9-3d, eval p9-2z,
composition / admission p9-3a, fields p9-3c, book tests p9-2y, eval tests p9-2v stale). The on-disk SHA-256 of the four
p9-3e exes was NOT re-hashed after the build (do it first; SPDLOG-RELINK). EXE-FREEZE: p9-3e relinked the IC exe
before the P9-B0 re-pin (holds).

## NOT done (each from the SQL4 review's "What root must check at merge", `pool-32 task-SQL4-review.md:274-350`)

1. Re-hash the four p9-3e exes against the receipt (trial ledger and SQL3-2 held at the stop: re-check at the start).
2. gtests (review item 3): `build-equity/bin/atx-impl-strategy-ic-tests.exe
   --gtest_filter=IcCacheIndex.*:IcResultIndex.*:MarginalPairIndex.*:StrategyIcRunner.*:MarginalIc.*` = 79 / 79 (the
   three tripwires and the flag-absent fakes `FlagAbsentKeepsFileLayoutAndSummary`, `SummaryRecordsIndex`,
   `MarginalPairIndex.FlagAbsentBlockUnchanged` among them: item 7); `build-equity/bin/atx-engine-research-catalog-tests.exe
   --gtest_filter=ResearchCatalog*:SealGuard*:StoreCli*` = 0 failures (SQL4-S S-3; `ResearchCatalog.*` 17 / 17).
   Tool: `m6b-logs/tools/gt.sh LABEL EXE LOG FILTER`.
3. Python (item 5), `ATX_EQUITY_ROOT` / `ATX_EQUITY_BIN` = pool-2's, `ATX_RESEARCH_STORE_EXE` = the p9-3e store exe,
   TMP / TEMP and `--basetemp` under the session scratchpad: the store suite
   `atx-engine/tools/test_research_store{,_blind,_classes,_fixtures,_identity}.py` at seeds 0 and 1 (43; `FixtureChain`
   must RUN: "total checked 12 mismatches 0"), `scripts/tests/test_research_cycle.py::test_marginal_argv_is_the_verbs_full_cli`,
   `scripts/tests/test_research_store_hook.py` (14 cases since GATE-0). The classes guard (no new literal expected).
   Tool: `m6b-logs/tools/pyt.sh`.
4. Fixture digest (item 6, SQL4-S S-1): `bash m6b-logs/tools/fixture_dump.sh <scratch> post m6b-logs`, then diff
   `m6b-logs/post-dump/` against `m6b-logs/pre-dump/`: expected to differ only in `ledger_state`'s `head_sha256` /
   `head_rule` (NULL -> B2's chain head and `kChainRule`) plus the volatile `catalog_run` / `artifact_seen` run ids;
   record the new `catalog_digest` with that cause (no expected value edited).
5. X-5 identity (item 8), tool `m6b-logs/tools/rerun.py RECEIPT NAME 600 2048 [--u] [--cache DIR] [--append TOK]`
   with RECEIPT = `m2e-logs/receipt-p9-m2e-alcomb-x5-w-post.json`; compare with `m6b-logs/tools/cmp.py A B --allow
   REGEX...`; counters (no statistic) with `m6b-logs/tools/counters.py`:
   - 1a: `p9-m6b-x5-w-post` and `p9-m6b-x5-u-post` (no flag, real cache) vs the `-pre` dirs; allowed
     `--allow '(^|/)[^/]*seconds(/|$)'`; every other file byte-identical.
   - 1b: `build-equity/bin/atx-research-store.exe cache init build-equity/mega-candidate-cache-v8-lo3`, hash the new
     `index.sqlite`, run w and u (no flag), check the index SHA-256 unchanged and no `-wal` / `-shm`, then delete
     `index.sqlite` by literal name; allowed as 1a.
   - 2: `mkdir D; atx-research-store cache init D` (D under `build-equity/`, a fresh name), `--cache D --append
     --candidate-cache-index --append sqlite`, u cold then warm (and w): allowed 1a's plus `/roles/*/candidate_cache`
     (whole block), `/roles/*/hash_seconds`, `/roles/*/verify_bytes`, per candidate `signal_cache`, `ic_result_cache`,
     `ic_missing_exit_v1_result_cache`, `stage_seconds` (summary `/roles/*/candidates/*/...`, jsonl top level); cold
     hits 0, warm hits = 58 and ic_results hits = 58; `find D -name '*.json'` empty; log the warm u wall time; delete D.
     The cold pass is a full VM pass (58 candidates) and writes about 7.3 GB: check the clock and disk first.
6. Marginal shard identity (item 9): RECEIPT = `build-equity/mega-v8-b0b-train-u-v8ys-marginal-poolonly-run/receipt.json`
   (the Y-S pool-only argv of record; 223.5 s); (a) `--pair-cache P1` (empty P1) cold then warm; (b) `cache init P2`,
   `--pair-cache P2 --append --pair-cache-index --append sqlite` cold then warm; `marginal_ic.json` byte-identical cold
   to cold and warm to warm outside `/inputs/pair_cache` and `/stage_seconds`; (b) cold `rows_read` 0, `rows_written` =
   `computed`; warm `computed` 0, `hits` = the pair count; P2 holds only `index.sqlite*`; delete P1 / P2.
7. Ledger (item 10, SQL4-S S-6): first `m6b-logs/tools/reparse_points.py C:/atx-wt/pool-2` (names only; written, NOT
   run: its one invocation got no permission verdict and did not execute); then a fresh real-tree catalog OUTSIDE
   `build-equity/research-store/` through the bounded runner (`catalog --catalog <scratch>/real.sqlite --root
   C:/atx-wt/pool-2 --ledger build-equity/trials.jsonl --classes atx-engine/schemas/research_store/classes.json`), then
   `verify --catalog <that> --ledger build-equity/trials.jsonl`: exit 0, `index ok`, one `prefix <k>` per recorded
   head, `other-ledger 0`, `not-a-prefix-head 0`.
8. Clean-up (item 12): no index, copy, D, P1, P2 left; `cache-lo3-listing` at the end = `155872de50833fca`.
9. Records: `root-wave3-merge-report.md` section "SQL4" (not written yet), the integration-log row completed,
   this file's results.

**Exact next step:** item 1 above (re-hash the p9-3e exes, the ledger, SQL3-2), then item 2's two gtest filters on
the p9-3e exes.

## Kept and deleted outputs

- Kept: `build-equity/` and `build-equity-rel/` object trees (the gate's builds use them); the p9-3e receipt, build and
  cache logs in `build-equity/` (copies in `m6b-logs/`); the pre-merge X-5 references `build-equity/p9-m6b-x5-w-pre`,
  `p9-m6b-x5-u-pre` and their runner dirs `-pre-run` (inputs of item 5's 1a; delete by name once compared).
- Session scratchpad `root-m6b/`: the pre-merge fixture catalog `pre-cat.sqlite` and its verify output (regenerable;
  delete by name when item 4 is done).
- Nothing deleted in this group. No index, D, P1 or P2 was created. The real cache dir is as it began (listing equal).

## Uncommitted state at the end

- None of root's code. Five untracked `docs/plans/*.png` (not root's). `gate0-logs/.mypy_cache/` (an editor-side
  cache, git-ignored, never added).

## Permission refusals

- None. One Bash call (`reparse_points.py` over pool-2, then a `tail` of `builds.txt`) and one read-only call got "no
  verdict" from the auto-mode classifier (a transient check failure, not a refusal); neither executed; the second
  was replaced by reading the two files with the read tool, the first is item 7 above.

## Open questions for the PM (one line each)

- Does SQL4 still meet SQL4-CUTOFF (merge `06dc9e5a` landed 14:20; the checks are pending after STOP 11)?
