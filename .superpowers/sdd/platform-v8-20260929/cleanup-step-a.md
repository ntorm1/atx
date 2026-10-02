# Disk cleanup step A: record (2026-10-02)

Free space on C: before **61,597,388,800 B (57.37 GiB)**, after **67,071,619,072 B (62.47 GiB)**, so **5.10 GiB freed**. The listed paths added up to 4.99 GiB logical; the rest of the difference is cluster slack.

Checks before deleting: `git status --short` was clean in pools 7, 8 and 10. No process had an executable path or command line under pool-7, pool-8 or pool-10 (checked twice). No target and none of its parents (`C:\atx-wt`, `pool-N`, `C:\atx-cache`) is a reparse point. A full walk found no reparse points inside any target. Each resolved path was checked against its expected prefix. Deletion used `cmd rmdir /s /q` on each full path, which does not follow junctions.

## Listed directories

| Path | Size before | Result |
|---|---|---|
| C:\atx-wt\pool-7\build-equity | 2,611,447,542 B (2.43 GiB) | **SKIPPED**: holds 77 tracked files (e.g. `build-equity/audits/iteration10_compare_startup_proposal.py`) and is not git-ignored |
| C:\atx-wt\pool-7\build-equity-rel | 1,065,504,251 B (0.99 GiB) | deleted |
| C:\atx-wt\pool-7\build-equity-bench | 757,905,216 B (0.71 GiB) | deleted |
| C:\atx-wt\pool-7\deps | 487,263,768 B (0.45 GiB) | deleted |
| C:\atx-wt\pool-7\build | 497,087 B | deleted |
| C:\atx-wt\pool-10\build-equity | 2,089,141,487 B (1.95 GiB) | **SKIPPED**: holds 77 tracked files |
| C:\atx-wt\pool-10\build-equity-rel | 778,446,032 B (0.72 GiB) | deleted |
| C:\atx-wt\pool-10\deps | 232,085,645 B (0.22 GiB) | deleted |
| C:\atx-wt\pool-10\build | 490,537 B | deleted |
| C:\atx-wt\pool-8\build-equity | 1,698,469,004 B (1.58 GiB) | **SKIPPED**: holds 77 tracked files |
| C:\atx-wt\pool-8\build-equity-rel | 716,195,223 B (0.67 GiB) | deleted |
| C:\atx-wt\pool-8\build | 497,087 B | deleted |
| C:\atx-cache\deps-rel-pool5 | 224,781,607 B (0.21 GiB) | deleted |

Deleted subtotal: 4,263,666,453 B (3.97 GiB). Skipped `build-equity` total: 6.40 GiB. These directories mix tracked audit scripts with untracked build output. Removing only the untracked content inside them would need a separate ruling.

## Python cache directories (`__pycache__`, `.pytest_cache`, `.mypy_cache`) in pools 7, 8 and 10

- 48 deleted, 1,098,181,776 B (1.02 GiB) in total: 16 in pool-7, 15 in pool-8 and 17 in pool-10. Every one was checked with `git ls-files` and held no tracked file. The per-directory list with sizes is in the session transcript. The largest were the `.mypy_cache` directories under `atx-engine/tools`, `atx-impl/tools`, `atx-impl/strategies`, `scripts/tests` and `.superpowers/sdd/.../studies`, at 50–95 MB each. `pool-10\build-equity\audits\.mypy_cache` (72 MB, untracked) was also removed.
- 3 skipped, because the hard rule excludes anything under `atx-db/`:
  - `pool-7\atx-db\src\atx_db\__pycache__`
  - `pool-8\atx-db\src\atx_db\__pycache__`
  - `pool-10\atx-db\src\atx_db\__pycache__`

## After

`git -C <pool> status --short` is still empty in pool-7, pool-8 and pool-10. Nothing was touched in pool-2, `C:\atx`, `C:\atx-cache\{ccache,deps,vcpkg_installed}`, any vcpkg directory or the Recycle Bin.

# Step A2: untracked and ignored content of `build-equity` (pools 7, 8, 10)

Free space on C: before **67,061,788,672 B (62.46 GiB)**, after **73,505,411,072 B (68.46 GiB)**, so **6.00 GiB freed**. Free space fell slightly between the end of step A and the start of A2 because of other activity on the disk. Combined, steps A and A2 took free space from 57.37 GiB to 68.46 GiB, which is **11.09 GiB freed**.

Checks before cleaning:
- No process had an executable path or command line under pools 7, 8 or 10.
- `build-equity` is not a reparse point in any pool, and a full walk found 0 reparse points inside it.
- `git status --short` was clean in every pool.
- Each pool tracked 77 files under `build-equity`.

Dry run (`git clean -n -d -x -- build-equity`) gave the same 20 entries in every pool. All of them are under `build-equity/`, and none is a tracked path or contains one. The entries were `.ninja_deps`, `.ninja_log`, `CMakeCache.txt`, `CMakeFiles/`, `CPackConfig.cmake`, `CPackSourceConfig.cmake`, `CTestTestfile.cmake`, `Testing/`, `atx-core/`, `atx-engine/`, `atx-impl/`, `atx-tsdb/`, `bin/`, `build.ninja`, `cmake_install.cmake`, `compile_commands.json`, `lib/`, `spdlog.pc`, `tests/` and `vcpkg-manifest-install.log`.

| Pool | Tracked before | `git clean -f -d -x -- build-equity` | Tracked after | `ls-files -d` | Files left on disk | `status --short` |
|---|---|---|---|---|---|---|
| pool-7 | 77 | 20 entries removed, exit 0 | 77 | empty | 77 | clean |
| pool-8 | 77 | 20 entries removed, exit 0 | 77 | empty | 77 | clean |
| pool-10 | 77 | 20 entries removed, exit 0 | 77 | empty | 77 | clean |

Nothing was skipped.
