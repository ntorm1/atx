# P9 worktree disk audit: `C:\atx-wt\pool-*` (2026-10-03)

This is a read-only audit with one narrowly allowed cleanup: `__pycache__`, `.pytest_cache` and pytest tmp dirs in idle pools. It is not committed.

**Out of scope, never listed or touched:** `C:\atx` and every `atx-db\` directory, including the ones inside pools. All sizes below therefore exclude `atx-db\`. Shared caches in `C:\atx-cache` were not measured, except ccache through `ccache -s`.

## Free space on C:

| When (local) | Free bytes | GiB |
|---|---:|---:|
| Audit start (~17:35) | 50,950,098,944 | 47.45 |
| Immediately before the deletion | 49,948,393,472 | 46.52 |
| Immediately after the deletion | 50,280,427,520 | 46.83 |
| Report time (18:21) | 44,774,858,752 | 41.70 |

My deletion freed **9,867,244 B**. The rest of the drive movement came from twelve agents writing at the same time.

The drive lost about **6.2 GB during the roughly 45 minutes of the audit**. In that window ccache grew from 8.1 to 8.6 GB. The pool trees were not re-measured, so the main consumer is unidentified. It may be outside the audited trees (`atx-db\`, `C:\atx`, `%TEMP%`), but that is unconfirmed.

## Method

- **Apparent sizes:** robocopy `/L /E /BYTES /XJ /XD atx-db`, which lists only and never copies.
- **Physical sizes:** a read-only scan of 160,168 files that reads each file's NTFS file id and link count with `GetFileInformationByHandle`.
  - A hard-linked file is counted once.
  - "Freed" means the bytes of files whose every link lies inside the candidate.
  - This matters: pool-2 is **82.0 GiB apparent but 51.07 GiB physical**. The lo3 fields stores are one hard-linked family.
- **Pins and readers:** I grepped for each candidate's name in:
  - `scripts/specs`, `scripts/tests` and fixtures
  - `docs/plans`
  - the `.superpowers` folders of pool-2 and pool-20
  - the run receipts (`start.json` / `receipt.json` / `summary.json` / `audit.*`) inside `pool-2\build-equity`

  The method follows the precedent in `platform-v8-20260929/cleanup-step-c.md`.

## Per-pool table

Sizes are physical GiB, with hard links counted once and `atx-db\` excluded. They were measured before the deletion.

- **Build trees:** `build\`, `build-equity-rel\`, `deps\`, the object dirs of `build-equity\` (`atx-*`, `bin`, `lib`, `tests`, `CMakeFiles`) and its loose files.
- **Run outputs:** every other `build-equity\` subdirectory.
- **Py caches:** `.mypy_cache`, `__pycache__` and `.ruff_cache`.

Lease state comes from `lease-worktree.ps1 -Status`, run from pool-2.

| Pool | Lease (`-Status`) | Occupancy | Build trees | Run outputs | .superpowers | Py caches | Source/other | Total |
|---|---|---|---:|---:|---:|---:|---:|---:|
| pool-2 | LEASED, owner dead (aes-codex-integ-20260925) | **ACTIVE**: P9 root, `feat/platform-p9-20261003` | 9.29 | 41.13 | 0.050 | 0.528 | 0.072 | 51.07 |
| pool-7 | LEASED, owner dead (mega-alpha-lib-20260926) | idle (`feat/platform-v8-xpre`) | 0.00 | 0 | 0.039 | 0.116 | 0.067 | 0.22 |
| pool-8 | LEASED, owner dead (mega-alpha-fields-20260926) | idle (`feat/platform-v8-minejoin`) | 0.00 | 0 | 0.038 | 0.000 | 0.065 | 0.10 |
| pool-10 | LEASED, owner dead (v8x-ximp-20261002) | idle (`feat/platform-v8-ximp`) | 0.00 | 0 | 0.039 | 0.112 | 0.067 | 0.22 |
| pool-12 | LEASED, owner dead (p9-a1) | **ACTIVE**: p9-a3 | 0.00 | 0 | 0.043 | 0.192 | 0.072 | 0.31 |
| pool-13 | LEASED, owner dead (p9-a2) | **ACTIVE**: p9-al-sig | 0.00 | 0 | 0.043 | 0.298 | 0.072 | 0.42 |
| pool-14 | LEASED, owner dead (p9-b1) | **ACTIVE**: p9-b2 | 0.00 | 0 | 0.043 | 0.239 | 0.072 | 0.36 |
| pool-15 | LEASED, owner dead (p9-c1) | **ACTIVE**: p9-c2 | 0.00 | 0 | 0.043 | 0.254 | 0.072 | 0.37 |
| pool-16 | LEASED, owner dead (v8y-yarch-20261002) | idle (`feat/platform-v8-yarch`) | 0.00 | 0 | 0.040 | 0.185 | 0.070 | 0.30 |
| pool-17 | LEASED, owner dead (p9-e1) | **ACTIVE**: p9-e2 | 0.00 | 0 | 0.043 | 0.184 | 0.072 | 0.30 |
| pool-18 | LEASED, owner dead (p9-s1) | **ACTIVE**: p9-s2 | 0.00 | 0 | 0.043 | 0.053 | 0.072 | 0.17 |
| pool-19 | LEASED, owner dead (p9-t1) | **ACTIVE**: p9-alcomb | 0.00 | 0 | 0.043 | 0.218 | 0.072 | 0.33 |
| pool-20 | LEASED, owner dead (p9-d1) | **ACTIVE**: p9-d2 | 0.00 | 0 | 0.043 | 0.076 | 0.072 | 0.19 |
| pool-21 | LEASED, owner alive (p9-sql1) | finished, unmerged lane | 1.72 | 0 | 0.043 | 0.032 | 0.072 | 1.87 |
| pool-22 | LEASED, owner alive (p9-t2) | **ACTIVE** | 0.00 | 0 | 0.043 | 0.099 | 0.072 | 0.22 |
| pool-23 | LEASED, owner alive (p9-sql2) | **ACTIVE** | 0.40 | 0 | 0.043 | 0.074 | 0.073 | 0.59 |
| pool-24 | LEASED, owner alive (p9-cov) | **ACTIVE** | 1.23 | 0 | 0.043 | 0.008 | 0.072 | 1.36 |
| **All pools** | | | | | | | | **58.40** |

Notes on the table:

- **Where the space is:** pool-2 holds 87% of the pool bytes (51.07 of 58.40 GiB). Pools 12-20 and 22 have only CMakeCache-only `build\` stubs (under 1 MB) and no build trees of their own.
- **Lease records are stale (finding):**
  - The eight wave-2 lanes in pools 12-15 and 17-20 are active, but `-Status` shows their leases under wave-1 run_ids (`p9-a1` … `p9-t1`) with owner=dead. All of those heartbeats stopped at about 11:39 local.
  - pool-2's lease still names the 2026-09-25 aes-codex run.
  - An explicit stale recovery run against `-Status` alone would therefore evict live P9 lanes.
- **Shared caches outside the pools:**
  - **ccache** (`C:\atx-cache\ccache`, `ccache -s`): 8.1 GB at audit start and 8.6 GB at 18:20. Its `max_size` is 100 GB, which is **larger than the free space on C:**. Hit rate is 38%.
  - **vcpkg:** shared at `C:/atx-cache/vcpkg_installed` (preset `VCPKG_INSTALLED_DIR`). No per-pool vcpkg trees exist.
  - **FetchContent:** Debug trees share `C:/atx-cache/deps`. The only per-pool FetchContent dirs are in pool-2: `deps\equity-dev` (236,559,459 B) and `deps\equity-rel` (233,548,761 B).

## What I deleted

I deleted **17 `__pycache__` directories, 9,867,244 B in total**, in pools 7, 10 and 16. pool-8 had none.

| Pool | Dirs | Paths (under the pool) |
|---|---:|---|
| pool-7 | 8 | `.superpowers\sdd\mega-alpha-20260926\studies`, `atx-engine\tools`, `atx-impl\strategies`, `atx-impl\tools`, `atx-impl\tools\mega_report`, `scripts`, `scripts\tests`, `scripts\tests\fixtures` (each `\__pycache__`) |
| pool-10 | 6 | `atx-engine\tools`, `atx-impl\strategies`, `atx-impl\tools`, `scripts`, `scripts\tests`, `scripts\tests\fixtures` |
| pool-16 | 3 | `atx-engine\tests\fixtures\research_fields`, `atx-engine\tools`, `atx-impl\strategies` |

**Which pools counted as idle:** every pool is formally LEASED in `-Status`, so no pool was literally unleased. I treated pools 7, 8, 10 and 16 as idle because:

- they are not in the owner's active list;
- their lease owners are dead and their run_ids are from v8 or mega-alpha;
- their last writes were on 10-02;
- no running process referenced them.

**Checks:**

- `git ls-files` showed 0 tracked `__pycache__` or `*.pyc` entries in each pool.
- Every target was outside `atx-db\` and was not a reparse point.

**Nothing else was deleted:**

- No `.pytest_cache` or pytest tmp directory exists in any pool outside `atx-db\`. The previous ones were removed in v8 step C.
- Nothing was touched in pool-2 or any active pool.
- No build directory, `trials.jsonl`, pinned path or reference output was touched.

## Ranked deletion candidates (none executed)

The ranking weighs bytes freed against risk. All bytes are physical, so a candidate frees exactly this much.

**Shared rules for every pool-2 item:**

- pool-2 is the live P9 root tree, so do not delete there mid-run.
- Before each delete, repeat the step-C checks: 0 tracked files, git-ignored, not a reparse point, and a rename probe.
- Remove trees with `cmd /c rmdir /s /q`, which does not follow junctions.

| # | Candidate | Bytes freed | Why it looks regenerable or stale | Risk | Command |
|---:|---|---:|---|---|---|
| 1 | `.mypy_cache` dirs in all pools (72 dirs). Idle pools 7, 10 and 16 alone hold 433,188,530 B. | **2,793,980,876** (2.60 GiB) | Pure type-checker cache, untracked, rebuilt automatically. pool-2 alone has 542 MB, including `build-equity\.mypy_cache` and `build-equity\audits\.mypy_cache`. | Low. The next mypy run is cold and takes a few minutes longer. Do idle pools now; do active pools only once their lane has ended, since a lane may be mid-mypy. | Idle pools: `foreach ($p in 'pool-7','pool-10','pool-16') { Get-ChildItem "C:\atx-wt\$p" -Directory -Force \| Where-Object Name -ne 'atx-db' \| Get-ChildItem -Directory -Recurse -Force -Filter .mypy_cache \| ForEach-Object { cmd /c rmdir /s /q "$($_.FullName)" } }`. For other pools, use the same command with that pool's name after its lane ends. |
| 2 | `pool-2\build-equity\v8-i16d-cand-cache-empty` (174 files, written 10-02 22:13-22:16) | **3,866,437,787** (3.60 GiB) | IC candidate-signal cache filled by the v8 X-5 cold rerun `v8-i16d-x5-w-run` (outcome `completed`). It is keyed to the v8-era executable: its 174 file names all differ from P9 D-1's cache, so no P9 run reads it. Its only reference is that receipt's `--candidate-cache` argv. No spec or `.superpowers` doc names it. The X-5 outputs `v8-i16d-x5-w` and their receipts are separate and stay. | Medium-low. The cache is a byproduct of a reference run, not the reference output itself. Replaying that receipt would start cold, which is the state the `-empty` name says it started in. Needs ROOT/D-lane sign-off because pool-2 is active. | `cmd /c rmdir /s /q C:\atx-wt\pool-2\build-equity\v8-i16d-cand-cache-empty` |
| 3 | `pool-21\build-equity` object dirs (`atx-core`, `atx-engine`, `atx-impl`, `atx-tsdb`, `bin`, `lib`, `tests`, `CMakeFiles`) | **1,841,869,661** (1.72 GiB) | Debug build tree of the finished SQL1 lane. It can be rebuilt from a warm ccache. | Low **after SQL1 merges**. Before the merge, a review fix would need a rebuild of a few minutes. Keep `mega-p9-sql1-*-receipt.json`, the build/cache-stats logs, `audits\` and `v8-interim3-pitch-render-run2\`. | `foreach ($d in 'atx-core','atx-engine','atx-impl','atx-tsdb','bin','lib','tests','CMakeFiles') { cmd /c rmdir /s /q "C:\atx-wt\pool-21\build-equity\$d" }` |
| 4 | `.f64` payloads of `pool-2\build-equity\recent-fast-validation-2023-2024-v1-fields-v1`, `-v2` and `-v3` (27 files) | **984,602,304** (0.92 GiB) | Pre-v8 (09-27) mega-alpha validation-fields stores, superseded by v6. **No consumer anywhere:** the only references are their own producer receipts (`mega-fields-validation-v1-run`, `mega-fields2-val-run`, `mega-fields3-val-run`). They are not hard-linked. | Low-medium. The producer receipts no longer re-verify, and a rebuild to the same pin may be impossible (sealed manifests, as in step-C 3b). Keep each `manifest.json`, following the step-C pattern of deleting payloads only. | `foreach ($v in 1,2,3) { Remove-Item "C:\atx-wt\pool-2\build-equity\recent-fast-validation-2023-2024-v1-fields-v$v\*.f64" -Force }` |
| 5 | `pool-2\build-equity\p9-d1-x5-cand-cache-empty` (written today 15:16) | **3,866,438,483** (3.60 GiB) | Same kind of cache as #2, filled by D-1's X-5 replay `p9-d1-x5-w-run` (completed). It is referenced only by that receipt's argv. | Medium. This is the current sprint: D-2 (pool-20) may replay D-1. Only after the D lane's verdict is recorded. | `cmd /c rmdir /s /q C:\atx-wt\pool-2\build-equity\p9-d1-x5-cand-cache-empty` |
| 6 | `.f64` payloads of `recent-fast-validation-2023-2024-v1-fields-v4` and `-v5` | **1,750,404,096** (1.63 GiB) | Pre-v8 validation fields. v4 is read by `mega-v3-VAL-run1` and `mega-nav-v3-VAL-run`; v5 by `mega-t25-audit`. All of those are pre-v8 records. | Medium. Those VAL/audit records stop re-verifying. The data is 2023-2024 validation-period, so check the holdout policy first. Keep v6 (1,458,670,080 B): it is read by `mega-v4-VAL-*`, `mega-t25-audit-v6*` and parent-handoff-3. | `foreach ($v in 4,5) { Remove-Item "C:\atx-wt\pool-2\build-equity\recent-fast-validation-2023-2024-v1-fields-v$v\*.f64" -Force }` |
| 7 | `pool-24\build-equity` and `pool-23\build-equity` object dirs | 1,321,067,051 + 423,402,108 | Lane build trees, rebuildable from a warm ccache. | Low **after those lanes merge**. Right now they are mid-build: forbidden. | The #3 command with `pool-24` or `pool-23` |
| 8 | `pool-2\build-equity-rel` (whole tree) | 1,656,057,348 (1.54 GiB) | P9 S1 (DS-1) Release IC tree, built today at 20:17Z. Precedent: step C deleted the unadopted v8-13 Release tree. | Medium. Only after S1's Release-token decision is final and no lane needs `atx-equity-strategy-ic` Release. Its `deps\equity-rel` (233,548,761 B) would go with it. | `cmd /c rmdir /s /q C:\atx-wt\pool-2\build-equity-rel` and `cmd /c rmdir /s /q C:\atx-wt\pool-2\deps\equity-rel` |
| 9 | Prevent regrowth: cap ccache | 0 now (8.6 GB used) | `max_size` is 100 GB, which exceeds the free space on C:. | Low. LRU eviction past the cap lowers hit rates slightly. | `C:\atx-cache\bin\ccache.exe -M 20G` |

**Combined:** #1 through #4 free 9,486,890,628 B (8.84 GiB, with #1 counted in full). All candidates together (#1-#8, including `deps\equity-rel`) free 18,737,808,475 B (17.45 GiB).

### Looked at and NOT recommended

| Item | Physical bytes | Why it stays |
|---|---:|---|
| `mega-candidate-cache-v8-lo3` / `-lo1` | 4.35 GiB exclusive (7.26 apparent; the rest is hard-linked with `mine-pool-v9-c1`) / 2.98 GiB | Caches named by pins: `specs/v8/base-lo1.json`, `base-lo3.json` and every `lib-v8*.json`. P9 S1 parity also reads them warm (`root-wave1-merge-report.md:331`, `task-S1-rereview-1.md:59-60`). |
| lo3 fields family (`train-2020-2023-lo3-fields-v9/v10/v13/v14/v15/v15a`, `p9-a1-identity-fields-v15`, …) | 5.34 GiB for the whole family; 33.3 GiB apparent | Fully hard-linked, so deleting old versions frees **0 B**. v13 is the P6 pin (`e5f7f28c…`) that X-5 and D-1 read. |
| `train-2020-2023-lo1-fields-v9` | 4,193,946,978 | Pinned by `specs/v8/base-lo1.json`, `scripts/tests/fixtures/mine-c1.registered.json`, `test_research_spec.py` and `mega-alpha-v8-pitch.config.json`. |
| Risk dirs: `b0c-risk`, `v8-risk-lo3-v10`, `v8-risk-lo1-v9`, `v7-w1-risk-all` | 1.78 GiB | Pinned by v8 / v8-rerun specs. `v8-risk-lo3-v10` is read by Y-1 (`y-vol-target-y-1.json`). |
| `train-2020-2023-{base,base-v1,base-grp,lo1,lo3,lo3-dlret}` | 1.20 GiB | X-5 / D-1 `--train` manifests (`train-2020-2023-lo3`). |
| `recent-projection-v2` | 370,156,558 | Parent of `train-2020-2023-base-v1` (it appears in that run's receipt). |
| `fit-work`, `mine-pool-v9-c1` | 0.25 GiB / mostly hard-linked | `fit-work` is the record store the SQL lanes index. `mine-pool-v9-c1` is pinned by `specs/v9/mine-c1.json`. |
| Run-dir long tail: `v8-*` (about 1.9 GiB beyond the items above), `mega-v8*` (2.03), `b0c-*` (0.73), `cycle-*` (0.03) | about 4.7 GiB | Ledgered trial and NAV dirs. Step C already took the payloads of rejected cells, and the PBO and effective-N grids read the rest. Only a per-dir ledger check could clear any of them. |
| pool-2 `build-equity` Debug objects and `deps\equity-dev` | 7.18 GiB + 0.22 | ROOT's live `equity-dev` tree, written at 21:1xZ today. |
| The idle worktrees themselves (pools 7, 8, 10, 16) | 0.84 GiB outside `atx-db\` | Their branches have commits not in main (9, 22, 1 and 143), and pools 10 and 16 have 77 and 9 dirty tracked files. Retire them only through `lease-worktree.ps1` after their owners confirm. Their `atx-db\` size was not measured. |

## Evidence files

All are in the session scratchpad, not the repo:

- `sizes.csv`: robocopy apparent sizes
- `fileids.tsv`: per-file NTFS id, link count, size and mtime
- `units.csv`: per-unit freed bytes
