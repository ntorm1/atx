# P9 RESUME 12: per-agent dispatch (PM, 2026-10-15)

ROOT-SDD = `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/`. Every agent: Opus; reads the contract
(`resume4-dispatch.md`, `resume6-addendum.md`, `resume7-addendum.md`, `resume8-addendum.md`, `resume9-addendum.md`,
`resume10-addendum.md`, `resume11-addendum.md`, `resume12-addendum.md`), then its section here, then the files its
section names, then greps its ruling ids in `progress.md`. Hand-back under 200 words per `resume12-addendum.md`.
Commit trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Work ONLY in pool-2 (`cd` there first:
the shell starts in `C:/atx`, where nothing is ever run). Never any git write in pools 8, 24 or any lane pool.

## ROOT M6B-FINISH (pool-2; one root agent; the GATE-0 / M6b root group continued; R12-RESUME)

Read `r7-root-merge.md` (the per-merge contract: step 6 "the lane's root items" is this group), `handoff-root-m6b.md`
WHOLE (your state; "Done", "Exes (EXE-CUR)", "NOT done" items 1-9, "Exact next step", the tools under
`m6b-logs/tools/`), `handoff-root-m6.md` "Exes (EXE-CUR)" and "What the next root group must know",
pool-32 `task-SQL4-review.md:274-350` ("What root must check at merge", items 3-12: the source of M6b's items),
ledger rulings by grep: SQL4-S, SQL4-ADD, SQL4-CUTOFF, SQL4-CUTOFF-2, M6B-SCOPE, SQL-11, SQL2-CLS, SQL3-2, MERGE-SHA,
EXE-CUR, EXE-FREEZE, SPDLOG-RELINK, RUN-1, RUN-2, ENV-1, HOST-4, HOST-5, PERM-1, DISPATCH-COMMIT, TEETH-RESTORE.

0. Housekeeping docs commit, as the files stand (never edit a ledger line): `progress.md`, `next-goal-prompt.md`;
   `git add -f status-12.md resume12-addendum.md r12-dispatch.md`. Message
   `docs(p9): RESUME 12 dispatch records and the STOP 11 / RESUME 12 ledger lines (DISPATCH-COMMIT)`. NEVER the
   PNGs under `docs/plans/`, never a cache dir, never `.mypy_cache`. Then verify: trial ledger 133 lines, sha256
   prefix `27e40f9f`; `build-equity/research-store/` absent (SQL3-2); `git status --porcelain` shows only the five
   PNGs; code head `06dc9e5a`.
1. Item 1: re-hash the four `p9-3e` exes on disk against `build-equity/mega-p9-3e-receipt.json` (SPDLOG-RELINK:
   a mismatch = re-receipt before use; state it). Record in `m6b-logs/exe-rehash.txt`.
2. Item 2: the two gtest filters on the `p9-3e` exes (`m6b-logs/tools/gt.sh`): the IC filter 79 / 79 (the three
   tripwires and the three flag-absent fakes among them: item 7); the catalog filter 0 failures, `ResearchCatalog.*`
   17 / 17; plus SQL2's full `StoreCli.*` filter (SQL4-S S-3). Logs in `m6b-logs/`.
3. Item 3: the Python set with `m6b-logs/tools/pyt.sh` (env and basetemp as the handoff states; the store suite at
   seeds 0 and 1 with `FixtureChain` RUNNING, "total checked 12 mismatches 0"; the one `test_research_cycle.py`
   case; `test_research_store_hook.py`; the classes guard before / after: no new literal).
4. Item 4: the post-merge fixture catalog digest with the `p9-3e` store exe (`m6b-logs/tools/fixture_dump.sh`),
   `post-dump/` vs `pre-dump/`: only `ledger_state.head_sha256` / `head_rule` (NULL -> B2's chain head and
   `kChainRule`) and the volatile run ids may differ; record the new `catalog_digest` with that cause; edit no
   expected value. Then delete the scratchpad `root-m6b/pre-cat.sqlite` by name.
5. Item 5: the real X-5 u / w identity on the `p9-3e` IC exe (`rerun.py`, `cmp.py`, `counters.py`): 1a (no flag,
   real cache; vs the `-pre` dirs; allowed `seconds` keys only), 1b (`cache init` on the real cache dir; index SHA
   unchanged, no `-wal` / `-shm`; delete `index.sqlite` by name), 2 (fresh `D` under `build-equity/`, flag `sqlite`,
   u cold then warm, and w; the allowed paths of the review; cold hits 0, warm hits 58 and ic_results hits 58; no
   `*.json` under D; warm u wall time logged; delete D by name). Every run through the bounded runner
   (`--seconds 600 --max-rss-mib 2048`); M6B-SCOPE for a case that cannot complete in bounds or that the host cannot
   admit (check free RAM and disk before the cold pass: it writes about 7.3 GB). Compare by SHA and JSON path only;
   READ NO STATISTIC.
6. Item 6: the marginal shard identity (the Y-S pool-only argv of record; P1 empty cold / warm; P2 `cache init` +
   `--pair-cache-index sqlite` cold / warm; the byte rule outside `/inputs/pair_cache` and `/stage_seconds`; the
   counters; P2 holds only `index.sqlite*`; delete P1 / P2 by name). M6B-SCOPE applies.
7. Item 7: `m6b-logs/tools/reparse_points.py C:/atx-wt/pool-2` (names only); a fresh real-tree catalog OUTSIDE
   `build-equity/research-store/` through the bounded runner, then `verify --catalog <it> --ledger
   build-equity/trials.jsonl`: exit 0, `index ok`, one `prefix <k>` per recorded head, `other-ledger 0`,
   `not-a-prefix-head 0`. The trial ledger is only read (133 / `27e40f9f` before and after).
8. Item 8: clean-up by name (no index, copy, D, P1, P2 left; the pre-merge reference dirs `p9-m6b-x5-{w,u}-pre` and
   their `-pre-run` dirs deleted by name once 1a is compared and recorded); `cache-lo3-listing` at the end =
   `155872de50833fca`; `build-equity/research-store/` still absent.
9. Item 9: records. `root-wave3-merge-report.md` section "SQL4" (ROOT-1 attribution row: every file of SQL4's range
   by merge commit `06dc9e5a`); the M6b row of `integration-log.md` completed; `handoff-root-m6b.md` updated in
   place with every result (holds / fails per item, the new catalog digest and its cause, the exe table re-hashed,
   kept / deleted outputs, next free tag, "What the gate root must know": the SQL4 verdict of this group =
   "checks green: SQL4 in the gate head" or "BLOCKED at <item>: SQL4 P10 at the gate, merge not reverted"
   (SQL4-CUTOFF-2); any "not run in P9 (RUN-1 bound / host)" case (M6B-SCOPE) with its name). Code (a slip, if any)
   and docs in separate commits.

A red: a slip (a path, an include, a test expectation of a volatile id) is fixed in place, one commit, logged with
the before / after; anything else stops that item: record it, go on with the independent items, hand back
DONE_WITH_CONCERNS with the SQL4 verdict "BLOCKED at <item>" (the merge is not reverted; SQL4-CUTOFF-2). Nothing of
the gate: no Release build, no whole suite, no P9-B0, no canary (M6B-SCOPE). HOST-4: your build (if any slip needs
one; tag `p9-3f`, Debug, the four SQL4 targets) has priority; HOST-5. Final message per `resume12-addendum.md`:
STATUS, code head, docs head, tag used (if any), per item holds / fails, the SQL4 verdict, files written, disk
freed, any permission refusal (exact command), open questions one line each.

## ROOT GATE (GATE-1; a chain of fresh root agents; GATE-DISPATCH-12)

Dispatch text = `r11-dispatch.md` "ROOT GATE" + "ROOT GATE: rulings from the whole-wave review" with
`root-gate-brief.md` (steps 1-7 and 9; step 0 DONE at GATE-0, step 8 DONE: WAVE-REV-EARLY + SQL4-ADD), plus the
facts below, filled by the PM at dispatch on M6B-FINISH's hand-back:

- Gate head: `06dc9e5a` (root's code head after M6B-FINISH: no slip). Docs head at dispatch: `0a481348` (docs
  agents commit beside root by pathspec, PAR-1).
- SQL4 in the head: checks green at M6B-FINISH (SQL4-CUTOFF-2); a red in SQL4's 24 files attributes to `06dc9e5a`.
- GATE-IDX: `build-equity/mega-candidate-cache-v8-lo3/index.sqlite` (left by M6b case 1b; deletion refused to
  root, PERM-1) is the owner's to delete; no run of record names that cache while it exists; steps 1-2 run now;
  the check is immediately before step 3's first run naming the cache; else BLOCKED-OWNER after the cache-free steps.
- Next free build tag: `p9-3f`; tags continue single-use from there (Debug `equity-dev`, Release `equity-rel`
  `-Canary`). GATE-PAR: low-RAM suites may run concurrently; builds and bounded runs alone.
- Step 0 re-verification only (no new slip expected): head, clean tree (only the five PNGs), ledger 133 / `27e40f9f`,
  `build-equity/research-store/` absent, `ucrtbase.dll` version + SHA-256, disk and RAM.
- Step 1 exe set = `root-gate-brief.md` step 1 + GATE-EXES (`atx-research-composition`, `atx-research-admission`);
  receipts per exe; the on-disk SHA-256 of every exe re-hashed after the last link (SPDLOG-RELINK).
- Step 2: the suites as the brief and `r11-dispatch.md` state them; GATE-COVDIFF; known-red set EMPTY; the one named
  Release skip; ENV-1.
- Step 3: the parents' flag-absent identity with every new flag absent (SQL4's index flags included); D3-Q1a;
  COMPOSE-1; the golden at 1 and 4 workers; the canaries.
- Step 4: P9-B0 (substitution list committed first; READ NO STATISTIC; OD5-1 flag line); step 5 rho; step 6 the
  re-pin (EXE-FREEZE from here; COVMV-S2 S-b + NAV-S S-2); step 7 the store step, scoreboard (PY-S S-6 by hand),
  G-P ticks, CRT record.
- Step 9: `root-gate-report.md` (every item with its evidence path; the BLOCKED attributions by merge commit if any;
  the round-2b-ii facts by Appendix IV item), `integration-log.md`, `handoff-root-gate.md` (code head, docs head, the
  exe table, the reference dirs, known-red set, next free tag, what the cells agent must know). The ledger and this
  file committed as they stand (DISPATCH-COMMIT).
- Chain: when low on context, finish the step to a committed state, write `handoff-root-gate-<n>.md` with "Exact
  next step", report DONE_WITH_CONCERNS; the PM dispatches the next root agent from that file.

## GATE-REV (one fresh reviewer; after the gate root's hand-back and the PRE agent's commit; GATE-REV)

Filled at dispatch.

## PRE-2BII (one fresh PRE agent; after the gate root's hand-back; PRE-2BII-AGENT)

Filled at dispatch.

## CELLS (the cells root agent; after the PM rules GATE-REV and the prereg)

Filled at dispatch.
