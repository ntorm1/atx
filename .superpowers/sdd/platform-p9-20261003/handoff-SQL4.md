# Handoff: lane SQL4 (IC cache indexes on SQLite; ledger index head): RESUME 11 finished, awaiting first review

- Pool `C:/atx-wt/pool-32`, branch `feat/p9-sql4-20261008`. Date 2026-10-15.
- Code head: `c5df575d` (second root merge, root `475f5d8f`).
- Records head: the commit that adds this handoff and `task-SQL4-report.md`.
- Tree clean. Lease untouched (`owner=alive`, LEASE-DEAD rule).
- Review BASE = merge-base with root = `475f5d8f`.
- Full detail is in `task-SQL4-report.md`: evidence, root's identity procedure, deviations, open risks.

## State

| brief task | state | commit |
|---|---|---|
| 1 flags (IC exe, marginal verb) | done | WIP `a1955a71` (IC half), `22e41583` (marginal half) |
| 2 index object | done | WIP; compiled unchanged at `96b2b1e1` |
| 3 signal cache, 4 IC-result cache | done | WIP; compiled unchanged |
| 5 pair cache | done | `22e41583` |
| 6 ledger head, `verify --ledger` | done | `237fc8a6` |
| 7 tests, CMake tails | done | `4aade941`, fix `44fce688` |
| merges | root `e3b6c111` (`82756940`), root `475f5d8f` (`c5df575d`, four CMake tail conflicts: root's block kept verbatim, the lane's block appended) | |

Evidence:

- Debug `p9-sql4-a` .. `-c`, with exes deleted after the run. The IC named filter passed 79 / 79 at `c5df575d`.
  `ResearchCatalog.*` passed 17 / 17, run on the `p9-sql4-b` exe and again on the merged tree.
- The three tripwires pass, and their test files are unchanged.
- Teeth `p9-sql4-d`, one build with 13 mutations: 9 named tests red, restore verified. M8 is unattributed (masked by
  M7's ASSERT in the same test).
- Object trees deleted: 2,962,965,000 bytes.

## Stated for the review / merge

- **SQL4-Q2 (provisional):**
  - `IcRunnerConfig::candidate_cache_index` is in `strategy_ic_runner.hpp`.
  - `CacheIndex*` is threaded through five functions in `strategy_ic_detail.hpp`. All call sites are in
    `strategy_ic_runner.cpp`.
  - A null index is today's path. `IcCacheIndex.FlagAbsentKeepsFileLayoutAndSummary` pins this.
- **SQL4-Q1 (P10 item):** the marginal verb reads candidate signals only from sidecar files
  (`atx-impl/src/strategy_marginal_ic.cpp:338-400`, `accept_sidecar`). A candidate cache filled under
  `--candidate-cache-index sqlite` (no sidecars) cannot feed it.
- **Cross-lane:** SQL2's `store_cli.{cpp,hpp}`, `catalog/ingest.hpp` and `research_catalog_test.cpp:368`
  (`EXPECT_FALSE` becomes `EXPECT_TRUE`), and SQL1's `ops_cache.hpp` / `cache_ops.cpp` (keyed ops; no schema change).
- **Root at merge:** the report's "How root verifies" covers:
  - X-5 u / w with no flag, with and without `index.sqlite`;
  - flag sqlite on a fresh `cache init` dir, cold then warm;
  - the Y-S marginal with shards vs rows;
  - `verify --ledger` on the real catalog after a re-catalogue.
  Delete every index after the runs (SQL-11).

## Next steps

1. PM dispatches the first review (`task-SQL4-review.md`, BASE `475f5d8f`). The reviewer's one build runs HOST-4
   last. An M8-only mutation is optional, to close the unattributed tooth.
2. On APPROVE, and if root has not re-pinned P9-B0 yet: root merges it LAST in M6b with the identity procedure.
   Otherwise P10 whole (SQL-12).
3. If root moves before the review, `git merge feat/platform-p9-20261003` again. Never rebase.

## Notes

- Lane tools: `sql4-logs/tools/subst.py` (count-checked substitutions keeping CRLF) and
  `sql4-logs/tools/sql4_teeth.py` (TEETH-RESTORE, TEETH-CRLF; `--check` verifies the anchors only).
- `sql4-logs/tools/.mypy_cache/` is an untracked editor cache, not committed.
- `build/` in the pool is the lease's failed `dev` configure: known, left alone.
- `build-equity/` keeps `audits/`, the tracked render dir, the `mega-p9-sql4-*` receipts and logs, and the CMake
  cache.
- A configure plus build in this pool rebuilds from ccache: 119 s for the first full build at `p9-sql4-a`.
- Permission refusals: none.
