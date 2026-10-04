# P9 wave-1 pools (root, R0-0 / DEC-19; 2026-10-03)

Leased by root from `C:/atx-wt/pool-2` with `scripts/lease-worktree.ps1` (v3 heartbeat leases, `-MaxPool 20`), base
`d7c1c520` = `d7c1c520c3caa162ef453349b256669ce8de3c80`. Verified after leasing: branch, `git rev-parse HEAD`, submodule
`atx-core/third-party/databento-cpp` initialised at `2a960737` (v0.59.0), tree clean, `-Status` owner alive.

| lane | pool path | branch | run id | heartbeat id | HEAD | owner state |
|---|---|---|---|---|---|---|
| A1 | `C:/atx-wt/pool-12` | `feat/p9-a1-20261003` | `p9-a1-20261003` | `p9-a1-hb` | `d7c1c520c3caa162ef453349b256669ce8de3c80` | alive (keeper 26400) |
| A2 | `C:/atx-wt/pool-13` | `feat/p9-a2-20261003` | `p9-a2-20261003` | `p9-a2-hb` | `d7c1c520c3caa162ef453349b256669ce8de3c80` | alive (keeper 19424) |
| B1 | `C:/atx-wt/pool-14` | `feat/p9-b1-20261003` | `p9-b1-20261003` | `p9-b1-hb` | `d7c1c520c3caa162ef453349b256669ce8de3c80` | alive (keeper 25384) |
| C1 | `C:/atx-wt/pool-15` | `feat/p9-c1-20261003` | `p9-c1-20261003` | `p9-c1-hb` | `d7c1c520c3caa162ef453349b256669ce8de3c80` | alive (keeper 20868) |
| D1 | `C:/atx-wt/pool-20` | `feat/p9-d1-20261003` | `p9-d1-20261003` | `p9-d1-hb` | `d7c1c520c3caa162ef453349b256669ce8de3c80` | alive (keeper 11120) |
| E1 | `C:/atx-wt/pool-17` | `feat/p9-e1-20261003` | `p9-e1-20261003` | `p9-e1-hb` | `d7c1c520c3caa162ef453349b256669ce8de3c80` | alive (keeper 17736) |
| S1 | `C:/atx-wt/pool-18` | `feat/p9-s1-20261003` | `p9-s1-20261003` | `p9-s1-hb` | `d7c1c520c3caa162ef453349b256669ce8de3c80` | alive (keeper 26256) |
| T1 | `C:/atx-wt/pool-19` | `feat/p9-t1-20261003` | `p9-t1-20261003` | `p9-t1-hb` | `d7c1c520c3caa162ef453349b256669ce8de3c80` | alive (keeper 13684) |

Release each with `powershell -NoProfile -File scripts\lease-worktree.ps1 -Release pool-N -RunId p9-<id>-20261003`.

Deviation from plan section 3.3: **D1 is on pool-20, not pool-16** (pool-16 could not be released; see below). The script
cannot target a pool number: it takes the lowest-numbered existing free tree, else the lowest number that is neither a
directory nor a registered worktree (pools 1, 3-6, 9, 11 are still registered as prunable, so none of them is picked;
do not run `git worktree prune` before the next lease, or pool-1 becomes the next fresh slot). Lease order A1, A2, B1,
C1 (-> 12-15), E1, S1, T1 (-> 17-19), D1 (-> 20) kept every other lane on its planned pool. Plan wave 2 puts D2 on 16
and AL-SIG on 20; the PM reassigns at wave 2.

Lease script note: every lease above exited 1 with `configure failed; lease remains held: C:\atx-wt\pool-N`. The step
that failed is the script's optional cold-tree `scripts\atx-build.ps1 configure -Preset dev`, which cannot succeed on
this branch (`CMake Error at cmake/atx-vol-install.cmake:117 (install): install TARGETS given target "atx-vol" which
does not exist.` -- `atx-vol/` is absent at `d7c1c520`; root builds with `research-build.ps1 -Preset equity-dev` into
`build-equity`). Branch switch, frozen-base check and `submodule update --init --recursive` all ran before it, and the
lease record and keeper were published before it. Each pool holds an ignored, incomplete `build/` from that attempt;
lanes never build, so nothing depends on it.

## Pools 12-16 (v8y lanes) release outcomes

| pool | run id | branch head | ancestor of `feat/platform-v8-20260929` | `git status --porcelain` | outcome |
|---|---|---|---|---|---|
| 12 | `v8y-yops-20261002` | `b25c2a2f` | yes | clean | RELEASED, re-leased to A1 |
| 13 | `v8y-ypre-20261002` | `87a9e0f4` | yes | clean | RELEASED, re-leased to A2 |
| 14 | `v8y-ycomb-20261002` | `cc08ac35` | yes | clean | RELEASED, re-leased to B1 |
| 15 | `v8y-yinfra-20261002` | `85a98a5b` | yes | clean | RELEASED, re-leased to C1 |
| 16 | `v8y-yarch-20261002` | `88cd7d49` | **no** | **22 entries** | **left leased** (owner alive, keeper 18756) |

Pool-16 detail (why it is not released):

- `88cd7d49` "fix(engine): research-field numpy tests read numpy's own results from the fixture (YARCH, PM8-18)" is
  one commit beyond the merged `ffbbcb56` (merge-base) and is not on `feat/platform-v8-20260929`.
- The working tree holds uncommitted YARCH slice 3b work in the middle of a conflicted stash apply (`AUTO_MERGE` in
  `C:/atx/.git/worktrees/pool-16`; `stash@{0}: On feat/platform-v8-yarch-20261002: yarch slice 3b wip`):
  - staged / modified: `atx-engine/CMakeLists.txt`, `research/fields/field_stats.hpp`, `field_stats.cpp`,
    `atx-engine/tests/CMakeLists.txt`, fixtures `expected/manifest.normalized.json`, `expected/numpy_cases.json`,
    `role/manifest.json`, `test_research_fields_fixture.py`;
  - conflicted (UU): `atx-engine/tests/fixtures/research_fields/make_research_fields_fixture.py`;
  - untracked: `derived_fields.{hpp,cpp}`, `market_return_field.{hpp,cpp}`, `ohlc_bar_fields.{hpp,cpp}`,
    `tests/research/research_fields_ported_test.cpp`, fixture dirs `derived/`, `expected/derived/`,
    `expected/role_bars/`, `vendor/`, `role/close.f64`, `role/raw_close.f64`.
- Releasing would be refused by the script (dirty tree) and would strand that work. It needs the YARCH owner or the PM:
  commit or park the slice 3b work, merge or abandon `88cd7d49`, then release with `-RunId v8y-yarch-20261002`.

Not touched: pools 1, 2, 6, 10 (rule), 7 and 8 (dead owners; recovery is a later step).

## Wave-3 re-lease: pool-15 -> C3 (root M2e, 2026-10-04; Ruling W3-11)

After C2's merge (`a25453be`) and verification, root released pool-15 and leased it again for lane C3, from pool-2
with `scripts/lease-worktree.ps1` (v3 heartbeat lease). Log: `m2e-logs/pool15-c3-lease.log`.

- **Release:** the record still held wave 1's C1 lease (`run_id p9-c1-20261003`, agent p9-c1, heartbeat `p9-c1-hb`,
  keeper 20868 dead): lane C2 had reused the pool without a lease call. `-Release pool-15 -RunId p9-c1-20261003` ->
  `RELEASED pool=pool-15 run_id=p9-c1-20261003`, exit 0. The run id matched the record, so no stale recovery was
  involved (the script consults the owner state only on a run-id mismatch). Tree clean before (0 porcelain lines on
  `feat/p9-c2-20261003` at `1ea664f2`); the script detached it at the record's base `d7c1c520`.
- **Lease:** `-Branch feat/p9-c3-20261004 -Base e1c160519a753e932827bfd9043773ef989a2e94 -Agent p9-c3 -RunId
  p9-c3-20261004 -HeartbeatId p9-c3-hb -MaxPool 20`. pool-15 was the only free tree, so the script took it (no new
  worktree). Exit 1 with `configure failed; lease remains held: C:\atx-wt\pool-15` -- the known failure of the script's
  optional cold-tree `dev` configure (`cmake/atx-vol-install.cmake`: no `atx-vol` target on this branch), after the
  lease record, keeper, branch switch and submodule update. It left an ignored, incomplete `build/` in pool-15; lanes
  build with `research-build.ps1` into `build-equity`, so nothing depends on it.
- **Verified after:** `-Status`: `pool-15 LEASED run_id=p9-c3-20261004 | agent=p9-c3 | owner_kind=heartbeat |
  owner=alive | keeper_pid=24052`; branch `feat/p9-c3-20261004`, HEAD `e1c160519a75` (root's M2e code head: C2 and
  AL-COMB merged, COV in), tree clean, submodule `atx-core/third-party/databento-cpp` at `2a960737` (v0.59.0).
- **Release later with:** `powershell -NoProfile -File scripts\lease-worktree.ps1 -Release pool-15 -RunId
  p9-c3-20261004`.

| lane | pool path | branch | run id | heartbeat id | HEAD | owner state |
|---|---|---|---|---|---|---|
| C3 | `C:/atx-wt/pool-15` | `feat/p9-c3-20261004` | `p9-c3-20261004` | `p9-c3-hb` | `e1c160519a753e932827bfd9043773ef989a2e94` | alive (keeper 24052) |

Not changed here: pool-19 (AL-COMB merged; W3-11 assigns it to AL-CLOCK) still holds wave 1's T1 record
(`run_id p9-t1-20261003`, owner dead), tree clean on `feat/p9-alcomb-20261003`; it can be released and re-leased the
same way when AL-CLOCK is dispatched.
