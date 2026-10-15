# Handoff: root M6b (RESUME 11: SQL4 merge) + M6B-FINISH (RESUME 12: its checks), 2026-10-15

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Group M6b: the GATE-0 root agent, continued (ledger
"Dispatch root M6b"; rulings SQL4-S, SQL4-ADD, STORE-JSON-P10, DISPATCH-COMMIT), started from `handoff-root-gate0.md`
(code head `7320e1b9`, docs head `fb566b44`, next free tag `p9-3e`), STOPPED at OWNER STOP 11 after the merge and its
build. Continued by a fresh root agent as **M6B-FINISH** (R12-RESUME; `r12-dispatch.md` "ROOT M6B-FINISH"; rulings
SQL4-CUTOFF-2, M6B-SCOPE), which ran step 0 and items 1-9 below. This file is updated in place. Evidence and tools:
`m6b-logs/` (tools in `m6b-logs/tools/`). Detail: `root-wave3-merge-report.md` section "SQL4"; row:
`integration-log.md` "M6b (RESUME 11) + M6B-FINISH (RESUME 12)".

- **Status: DONE_WITH_CONCERNS.** Every check of the SQL4 merge holds. One clean-up file could not be deleted (the
  permission classifier refused it; PERM-1, not retried): `build-equity/mega-candidate-cache-v8-lo3/index.sqlite`. It
  must go before the gate's P9-B0 (below, "What the gate root must know").
- **SQL4 verdict of this group (SQL4-CUTOFF-2): checks green: SQL4 in the gate head.**
- **Code head: `06dc9e5a`** = `Merge lane SQL4 at 44a76937 (first review APPROVE; code c5df575d)` (parents `fb566b44`,
  `44a76937`; MERGE-SHA; 0 conflicts; tree `b5b4eec9` = `git merge-tree --write-tree fb566b44 44a76937`). No slip, so
  no code commit in M6B-FINISH. This is the gate head.
- **Docs head:** the records commit that adds this version of this file (`git log -1 --format=%h -- <this file>`),
  above step 0's `6618abe1` (`docs(p9): RESUME 12 dispatch records and the STOP 11 / RESUME 12 ledger lines
  (DISPATCH-COMMIT)`: `progress.md`, `next-goal-prompt.md`, `status-12.md`, `resume12-addendum.md`,
  `r12-dispatch.md` as the PM left them). The records commit carries this file, the SQL4 section of
  `root-wave3-merge-report.md`, the integration-log row, `m6b-logs/` and `progress.md` as it stands (no ledger line
  edited by root). Docs agents of PAR-1 (PRE part A, IDX) commit their own files by pathspec beside root; root's
  commits add only root's paths.
- **Build:** M6b used **`p9-3e`** (Debug `equity-dev`, source `06dc9e5a`, the four SQL4 targets; exit 0; 0 warnings;
  48 TUs / 7 links). M6B-FINISH built nothing. **Next free root tag: `p9-3f`** (unused).
- **Trial ledger** `build-equity/trials.jsonl`: 133 lines, `27e40f9f` at every check (start, before / after the
  real-tree catalog, end).
- **SQL3-2:** `build-equity/research-store/` absent throughout; the real-tree catalog of item 7 was written to the
  session scratchpad `root-m6bf/real.sqlite`, the fixture catalogs to `root-m6bf/post-cat.sqlite`.
- **Known-red set: EMPTY.** Targeted checks only (TEST-1); the whole suites, Release, canaries, golden and the stage
  chain are the gate's. ENV-1: 0 hits. Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` stays a
  named skip.

## Results, items 1-9 (`pool-32 task-SQL4-review.md:274-350`)

| item | result | evidence (`m6b-logs/`) |
|---|---|---|
| step 0 | **done**: docs commit `6618abe1`; then code head `06dc9e5a`, `git status --porcelain` = the five PNGs, ledger 133 / `27e40f9f`, research-store absent | `git log` |
| 1 exe re-hash | **holds**: the four p9-3e exes `disk = receipt`; all 20 Debug exes of `build-equity/bin` = their newest receipt | `exe-rehash.txt`, `exe-sha-end.txt` |
| 2 gtests | **holds**: IC named filter **79 / 79** (tripwires `StrategyIcRunner.VmSourcesPinnedToSemanticsVersion`, `.IcSourcesPinnedToSemanticsVersion`, `MarginalIc.PairCacheSourcesPinned`; fakes `IcCacheIndex.FlagAbsentKeepsFileLayoutAndSummary`, `IcCacheIndex.SummaryRecordsIndex`, `MarginalPairIndex.FlagAbsentBlockUnchanged`: all OK = item 7 of the review); catalog `ResearchCatalog*:SealGuard*:StoreCli*` **30 / 0 failed**; `ResearchCatalog.*` **17 / 17**; SQL2's full `StoreCli.*` **6 / 6** | `gt-exits.txt`, `gt-*.log` |
| 3 Python | **holds**: store suite **43** (137 subtests) at seeds 0 and 1, `FixtureChain` PASSED both ("total checked 12 mismatches 0" asserted); `test_marginal_argv_is_the_verbs_full_cli` **1**; `test_research_store_hook.py` **14** (real-exe case ran); classes guard: no new literal (scan 932 -> 934 files = the two new `strategy_ic_cache_index` C++ files; literals 135 -> 135; unregistered 0 -> 0) | `pyt-exits.txt`, `pyt-*.log`, `guard-scan-after.txt` |
| 4 fixture digest | **holds**: only `ledger_state.head_sha256` / `head_rule` (NULL -> `8b1e9fe0...f008` = B2's `chain_head` of the fixture ledger / `atx-ledger-chain-fold/v1` = kChainRule) differ among non-volatile rows; the volatile tables `artifact_seen`, `catalog_run`, `skipped_path`, `store_info` differ only in run id / timestamp / exe SHA / seconds. **New fixture `catalog_digest`: `a50fdcf9c9c83bdac0d41ff368666da31578ebb4407bbc4498175a63014f100c`** (was `d32f7655...4e69`); cause: SQL4's `ingest_ledger.cpp` fills the ledger head. No expected value edited (none pins either digest) | `fixture-dump-post.txt`, `post-dump/`, `fixture-dump-cmp.txt` |
| 5 X-5 1a | **holds**: w 10 / u 3 files byte-identical; the two JSON files differ on `seconds` paths only; 0 outside | `cmp-1a-{w,u}.txt`, `counters-1a.txt` |
| 5 X-5 1b | **holds** (identity): `index.sqlite` `3a177dfd...c6f0` unchanged by the no-flag w and u runs, no `-wal` / `-shm`, outputs = 1a; **its deletion REFUSED** (PERM-1 #2) | `x5-1b.txt`, `cmp-1b-*.txt` |
| 5 X-5 case 2 | **holds**: D fresh (`cache init`); u cold 133 s in bounds, hits 0 / `vm_evaluations` 58; u warm hits 58, `ic_results` hits 58; w hits 58 / 58; no `*.json` in D; u warm and w 0 outside the review's list; u cold 3 outside (`/roles/*/research_fields/{field_loads,loaded_bytes,peak_resident_fields}`), attributed by a no-flag cold control on a fresh empty cache E: the same 3 vs 1a, and control vs flagged cold 0 outside, so they come from cold vs warm field loads, not the index. Warm u wall: 6.6 s (no flag) / 6.9 s (index). D and E deleted | `x5-c2.txt`, `cmp-c2*.txt` |
| 6 marginal shard | **holds**: (a) P1 shards cold / warm, (b) P2 `cache init` + `--pair-cache-index sqlite` cold / warm, all in bounds (233-249 s); cold vs cold and warm vs warm 0 outside `/inputs/pair_cache`, `/stage_seconds`; `working_bytes` equal, candidates 73 = 73; (b) cold rows_read 0, rows_written 2628 = computed; (b) warm computed 0, hits 2628 = the pair count; P2 held only `index.sqlite`; P1, P2 deleted | `ms.txt`, `cmp-ms-*.txt` |
| 7 ledger | **holds**: reparse points 0; real-tree catalog (bounded runner, 95 s) outside the default path; `verify --ledger build-equity/trials.jsonl` exit 0, `index ok`, 21 recorded heads all `prefix <k>`, `not-a-prefix-head 0`, `other-ledger 0`; ledger 133 / `27e40f9f` before and after | `reparse-points.txt`, `realcat.txt`, `realcat-verify.txt` |
| 8 clean-up | **fails on one file (PERM-1, not a code red)**: no D / E / P1 / P2 / copy left; pre-merge references deleted after 1a; the real cache dir holds the extra `index.sqlite` (352 files, listing `fb444f7d316a1a4c`; without that row = `155872de50833fca`, every original file unchanged) | `cleanup.txt`, `cache-lo3-listing-end.txt` |
| 9 records | **done**: `root-wave3-merge-report.md` "SQL4" (ROOT-1 attribution row: the 24 code files by merge commit `06dc9e5a`), `integration-log.md` row, this file | - |

"Not run in P9 (RUN-1 bound / host)" (M6B-SCOPE): **none**; every case completed within `--seconds 600 --max-rss-mib
2048`. The one extra run (item 5's no-flag cold control) was needed to attribute the 3 leaves; it is in the records.

## Exes (EXE-CUR; Debug `build-equity/bin`; on-disk SHA-256 re-hashed in M6B-FINISH: all `disk = receipt`, `exe-sha-end.txt`)

| tag | source | exe | SHA-256 | current by source at `06dc9e5a` |
|---|---|---|---|---|
| **p9-3e** | `06dc9e5a` | atx-equity-strategy-ic (IC exe) | `adffe7421781145a9718e67be05b46038b89a46311f26390eeb3312da236d56d` | yes |
| **p9-3e** | `06dc9e5a` | atx-impl-strategy-ic-tests | `55db8666c03533439ecadd10d9412f4c65167581e0a11a806de2572ea69ebd6b` | yes |
| **p9-3e** | `06dc9e5a` | atx-research-store | `a54955e965ee2aa035e66cc9ed668a16246c089684a53515431dea428b605c73` | yes |
| **p9-3e** | `06dc9e5a` | atx-engine-research-catalog-tests | `b73b2199db7fd1bc451bd12872a5eb0851453db4299ed29e812dbbd151d9835a` | yes |
| p9-3d | `1d003241` | atx-equity-strategy-targets (NAV) | `7e101b564d800c9e4b57541e11a30d17d8f32d5f74fd2c5ccc95d3b2e5b5f0dc` | **no** (newly: SQL4 changed `atx-impl-strategy`, which it links) |
| p9-3d | `1d003241` | atx-impl-strategy-target-tests | `685b8ba2947f878a9aa0a6a77cd7e4639c83588efc1cb398519be735efb13454` | **no** (same) |
| p9-2z | `138e1d2c` | atx-research-eval | `4791019131c0d74603b83cb772ef027ec8641167fd95844c4674e75bdf39893b` | yes |
| p9-3a | `6088bfe9` | atx-research-composition / atx-research-admission / composition tests | `212f8b99...e98e` / `411af33a...fa26` / `76b664b7...23a9` | yes |
| p9-3c | `bba4c06f` | atx-research-fields / fields tests | `d14346e8...5cde` / `39be2b3d...ebdf` | yes |
| p9-2y | `5c811b56` | atx-engine-book-tests | `6977a5a7...8360` | yes |
| p9-2t | `fcb0200e` | atx-engine-research-ledger-tests / atx-impl-tests | `8c5a6065...22cd` / `13bb6bde...67fe` | ledger tests: its C++ sources unchanged since `fcb0200e` (only the shared `atx-engine/CMakeLists.txt` / `tests/CMakeLists.txt` changed; not checked block by block); atx-impl-tests **no** |
| p9-2v | `08f04e07` | atx-engine-research-eval-tests | `dd8a3103...b7ca` | **no** (M4) |
| p9-2p | `327c86b2` | atx-equity-strategy / -risk / -mine / atx-impl | `3dc162d9...86ba` / `a50bb015...465b` / `1e9e0cda...83bb` / `fbdcf2c2...c818` | **no** |

Superseded by p9-3e: the IC exe / IC tests p9-3b and the store / catalog tests p9-2r. Every Release exe is the gate's.
EXE-FREEZE: the gate's builds relink the NAV and IC exes before P9-B0 (p9-3e relinked the IC exe before the re-pin:
holds).

## What the gate root must know

- **SQL4 verdict: checks green: SQL4 in the gate head** (`06dc9e5a`). A red at the gate in any file of SQL4's range is
  attributed to merge commit `06dc9e5a` (the 24 files are listed in the merge report's ROOT-1 attribution row).
- **BEFORE step 1 / P9-B0: `C:/atx-wt/pool-2/build-equity/mega-candidate-cache-v8-lo3/index.sqlite` must not exist.**
  It is an empty SQL4 cache index (73,728 bytes, SHA-256 `3a177dfddbd86dcc43caf0fca92339faad5e051bf4012b90effdb95cff6bc6f0`)
  that item 5 case 1b's `cache init` made; its deletion by literal name was refused (PERM-1 #2, `m6b-logs/perm-refusals.txt`
  has the exact command). The IC exe without the flag does not read it (1b), but `scripts/run_bounded_research.py`
  (SQL3's hook) records it: every bounded run naming `--candidate-cache build-equity/mega-candidate-cache-v8-lo3` gets
  a `store` block `{"catalog": null, "indexes": [".../index.sqlite"]}` in `start.json` / `receipt.json`, against SQL3-2
  (no `store` block in any receipt of a run of record). The owner / PM deletes that one file by hand; then the real
  cache's listing is `155872de50833fca` again (`m6b-logs/tools/dir_listing.py build-equity/mega-candidate-cache-v8-lo3
  <out>`; 351 files, 7,799,539,072 bytes). The gate root checks it is absent at step 0.
- **The fixture catalog digest of record is now `a50fdcf9c9c83bdac0d41ff368666da31578ebb4407bbc4498175a63014f100c`**
  (SQL4-S S-1, for the gate's store step 7): the cause is `ledger_state`'s head only (item 4).
- **SQL-11:** no cache-index flag on any cell argv, spec or prereg (`m6b-logs/sql11-recheck.txt`); the index paths are
  proven opt-in (case 2, item 6) and the no-flag paths identical (1a, 1b, the fakes). The gate's X-5 opt-in cache
  identity (SQL4-CUTOFF-2) can reuse `m6b-logs/tools/rerun.py`, `cmp.py` (call it with `MSYS_NO_PATHCONV=1` when an
  `--allow` regex starts `^/`: Git Bash rewrites it otherwise), `counters.py`; for a cold-vs-warm comparison add
  `/roles/*/research_fields/{field_loads,loaded_bytes,peak_resident_fields}` to the allowed list (item 5's control).
- **Exes:** the NAV exe and target-tests p9-3d are stale by source after SQL4 (they link `atx-impl-strategy`); the gate
  builds them anyway.
- **References kept:** `build-equity/p9-m6b-x5-{w,u}-post` (+ `-post-run`): the p9-3e no-flag X-5 w / u outputs on the
  real cache (1a's post side; regenerable; delete by name when not needed).
- **Host:** a foreign atx-vol build in `C:/atx-wt/pool-33` (`cmake --build ...\build-vol-rel-avx2`) held the bounded
  runner's no-compiler admission for 1,014 s at 19:18-19:35 UTC; free RAM was 2.5-5.3 GB; C: 72 GB free at the end.
- PAR-1: docs agents (PRE part A, IDX) may be committing their own files by pathspec in pool-2's sprint dir; root adds
  only its own paths and never sweeps theirs.

## Kept and deleted outputs

- Deleted by literal name: `build-equity/p9-m6b-x5-{w,u}-pre` and `-pre-run` (M6b's pre-merge references, after 1a);
  D `build-equity/p9-m6bf-x5-idx-D`, E `build-equity/p9-m6bf-x5-noidx-E` (about 7.7 GB); P1 / P2
  `build-equity/p9-m6bf-ms-P{1,2}`; the 1b, case-2, control and marginal output dirs and their `-run` dirs; the catalog
  runner dir `build-equity/p9-m6bf-realcat-run`. Every runner receipt survives inside its `m6b-logs/run-*.log` /
  `realcat-runner.log`. About 8.1 GB freed.
- Kept: `build-equity/p9-m6b-x5-{w,u}-post{,-run}`; `build-equity/` and `build-equity-rel/` object trees (the gate's
  builds); the p9-3e receipt and logs; every earlier receipt.
- Session scratchpad `root-m6bf/` (this agent's: pytest basetemps, `tmp/`, `post-cat.sqlite`, `post-verify.txt`,
  `real.sqlite`): deleted by literal name after the records commit (see "Permission refusals" if that was refused).
- Left by refusal #1: the previous M6b agent's scratch `C:/Users/natha/AppData/Local/Temp/claude/c--atx/
  2a54b18d-50af-4385-9bb7-113c392a6360/scratchpad/root-m6b/pre-cat.sqlite`, `pre-verify.txt`, empty `pre-dump/`
  (harmless; nothing reads them).
- Left by refusal #2: `build-equity/mega-candidate-cache-v8-lo3/index.sqlite` (above).

## Uncommitted state at the end

- None of root's code. Five untracked `docs/plans/*.png` (not root's). `gate0-logs/.mypy_cache/` (editor cache,
  git-ignored, never added). Files of the PAR-1 docs agents (theirs to commit). Ledger lines the PM adds after the
  records commit.

## Permission refusals (PERM-1; never retried; exact commands in `m6b-logs/perm-refusals.txt`)

1. Deleting the previous M6b agent's scratch catalog (`rm .../2a54b18d-.../scratchpad/root-m6b/pre-cat.sqlite
   .../pre-verify.txt && rmdir .../pre-dump`): refused ("Irreversible Local Destruction").
2. Deleting `build-equity/mega-candidate-cache-v8-lo3/index.sqlite` by literal name after case 1b: refused (same
   reason). Gate-blocking until removed by hand (above).

## Exact next step

The PM (or the owner) deletes `C:/atx-wt/pool-2/build-equity/mega-candidate-cache-v8-lo3/index.sqlite` and checks the
listing `155872de50833fca`; then the gate (GATE-DISPATCH-12) at gate head `06dc9e5a`, next free tag `p9-3f`, SQL4
verdict "checks green: SQL4 in the gate head".

## Open questions for the PM (one line each)

- Who deletes the real cache's `index.sqlite` (refusal #2) before the gate: the PM by hand, or the owner?
- Should the gate root re-verify the real cache listing (`155872de50833fca`) at its step 0? (Recommended.)
