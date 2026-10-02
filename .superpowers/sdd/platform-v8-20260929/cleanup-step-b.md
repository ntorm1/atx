# Disk cleanup step B: `C:\atx-wt\pool-2` (2026-10-02)

The list follows Ruling PM6-7 (`.superpowers/sdd/platform-v8-20260929/progress.md:1528`). I checked it against the coordinator's list and they match.

Free space on C: before **72,409,489,408 B (67.44 GiB)**, after **130,121,891,840 B (121.19 GiB)**, so **53.75 GiB freed**. The deleted directories had an apparent size of 57.52 GiB. The gap is consistent with hard-linked files that are shared with directories that were kept.

Checks before deleting:
- `git -C C:/atx-wt/pool-2 status --short` was clean. Branch `feat/platform-v8-20260929`.
- No process had a command line under `pool-2/`.
- `C:\atx-wt`, `pool-2`, `pool-2\build-equity` and `pool-2\deps` are plain directories, not reparse points.
- For each target, immediately before deleting it:
  - It existed and was not a reparse point.
  - Its resolved path equalled the expected full path.
  - It was git-ignored, and `ls-files` returned no file.
  - A full walk found 0 reparse points and 0 errors inside it.
  - Its newest file was dated before 2026-10-01.
  - No live process referenced it.
- No file contents in the targets were opened.
- Deletion used `cmd rmdir /s /q` on each full path, which does not follow junctions.

Reference check: whole-path-component, case-insensitive match against 18 files. These were 15 `scripts/specs/v8/**/*.json`, `v8-prereg.md`, `docs/plans/mega-alpha-v8-pitch.config.json` and `docs/plans/mega-alpha-v7-pitch.config.json`. Every target got **0 hits**, and so did `deps/hygiene` and a bare `hygiene` component. As a positive control, the same matcher finds `build-equity` 292 times and `mega-candidate-cache-v8-lo3` twice. This confirms `mega-candidate-cache` is matched as an exact name and not as a prefix.

| Path (under `C:\atx-wt\pool-2\`) | Apparent size | Files | Newest file | Result |
|---|---|---|---|---|
| build-equity-rel | 1.411 GiB | 409 | **2026-10-01 21:17** | **SKIPPED**: newest file is on or after 2026-10-01. It has no tracked file and 0 spec/prereg references. |
| build-hygiene | 0.055 GiB | 67 | 2026-09-25 17:30 | deleted |
| deps\hygiene | 0.210 GiB | 5519 | 2026-09-25 17:29 | deleted |
| build-equity\mega-candidate-cache | 19.950 GiB | 1044 | 2026-09-27 16:49 | deleted |
| build-equity\mega-candidate-cache-v6 | 1.843 GiB | 114 | 2026-09-27 19:41 | deleted |
| build-equity\mega-candidate-cache-v6u | 1.843 GiB | 114 | 2026-09-27 20:37 | deleted |
| build-equity\mega-candidate-cache-v7l1 | 1.891 GiB | 117 | 2026-09-28 19:49 | deleted |
| build-equity\mega-candidate-cache-v61-r7 | 1.891 GiB | 117 | 2026-09-28 19:22 | deleted |
| build-equity\mega-candidate-cache-v7rel | 1.891 GiB | 117 | 2026-09-28 18:38 | deleted |
| build-equity\recent-fast-train-2020-2022-v2-fields-v2 | 0.387 GiB | 9 | 2026-09-27 09:42 | deleted |
| build-equity\recent-fast-train-2020-2022-v2-fields-v3 | 0.387 GiB | 9 | 2026-09-27 10:16 | deleted |
| build-equity\recent-fast-train-2020-2022-v2-fields-v4 | 0.387 GiB | 9 | 2026-09-27 10:38 | deleted |
| build-equity\recent-fast-train-2020-2022-v2-fields-v5 | 1.937 GiB | 41 | 2026-09-27 12:38 | deleted |
| build-equity\recent-fast-train-2020-2022-v2-fields-v6 | 1.937 GiB | 41 | 2026-09-27 13:10 | deleted |
| build-equity\recent-fast-train-2020-2022-v1-fields-v1 | 0.533 GiB | 12 | 2026-09-26 21:34 | deleted |
| build-equity\recent-fast-train-2020-2022-v2-lo1-fields-v6 | 1.889 GiB | 40 | 2026-09-27 20:28 | deleted |
| build-equity\recent-fast-train-2020-2022-v2-lo1-fields-v8 | 2.664 GiB | 56 | 2026-09-28 21:40 | deleted |
| build-equity\recent-fast-train-2020-2022-v2-lo1-fields-v7-r7b | 1.986 GiB | 42 | 2026-09-28 19:26 | deleted |
| build-equity\recent-fast-train-2020-2022-v2-lo1-fields-v7-r7c | 1.986 GiB | 42 | 2026-09-28 21:17 | deleted |
| build-equity\v8-i3p4-d-w12-cache | 2.328 GiB | 144 | 2026-09-30 06:13 | deleted |
| build-equity\statarb-cluster-v1 | 0.764 GiB | 134 | 2026-09-27 14:50 | deleted |
| build-equity\v8-i5c-i6-fields | 3.051 GiB | 64 | 2026-09-30 20:54 | deleted |
| build-equity\v8-i3p4-c-fields2 | 3.051 GiB | 64 | 2026-09-30 06:05 | deleted |
| build-equity\v8-cache-b3 | 2.328 GiB | 144 | 2026-09-29 20:48 | deleted |
| build-equity\v8-i3p4-d-w4-cache | 2.328 GiB | 144 | 2026-09-30 06:10 | deleted |

24 deleted, 1 skipped.

## After

- `git -C C:/atx-wt/pool-2 status --short` is clean, and `git ls-files -d` prints nothing.
- These kept items are still present:
  - `build-equity-rel`
  - `build-equity\bin`
  - `build-equity\recent-fast-train-2020-2022-v2-lo1-fields-v6b`
  - `deps\` (the rest of it)
  - 8 `recent-fast-validation-2023-2024-v1*` directories
  - 394 `*-run*` receipt directories
- Nothing was touched in `C:\atx`, `atx-db/`, the Recycle Bin, ccache, the vcpkg directories, or any other pool.
- Combined with step A, free space on C: went from 57.37 GiB to 121.19 GiB.
