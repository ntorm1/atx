# Handoff SQL3 (P9 wave 3, pool-13): stopped at the owner's STOP during the read phase

- Branch `feat/p9-sql3-20261004`, worktree `C:/atx-wt/pool-13`, lease run id `p9-sql3-20261004` (not touched).
- Head before this handoff commit: `5c57c1ef` (= root's head at dispatch; the "merge root first" step was a no-op,
  root had not moved). Root SHA merged: `5c57c1ef`. No code was changed, nothing is WIP.
- Builds: none (no tag used; next free lane tags `p9-sql3-a`, ...). Build trees: none created; the lease's failed
  `dev` configure left `build/` (ignore it, per `r6-wave3-common.md`). Tests run: none. Suites: not run (tree = root's).
- B2 (post-identity redo, pool-14) was NOT in root at stop time (`git -C C:/atx-wt/pool-2 log --format=%s -20`
  shows no B2 merge).
- No permission refusal.

## Required items of the dispatch: status

| # | item | status |
|---|---|---|
| 1 | store hook per class (run-receipt, run-start, stage-receipt, cycle-binding, cycle-verdict, wave-result, wave-candidate, wave-manifest), `store` block (SQL-6) | not started (design below) |
| 2 | `scripts/research_wave.py` (only `Chain(...)` construction, `chain_of` :34-38) | not started |
| 3 | W3-4 `seal_guard.cpp` narrowing + gtest; N3 refusal documented | not started (design below) |
| 4 | W3-5 `book_monitor.py --fit-work` refuses an indexed work dir | not started (design below) |
| 5 | hooks in `cycle_verdict.py` / `cycle_resume.py` / `wave_stage_record.py` | pending on B2 (do last) |

## What the read established (anchors at `5c57c1ef`, located by symbol)

Writers (one hook call after each write):
- `run-start`: `scripts/run_bounded_research.py` `main`, `(output / "start.json").write_text(...)` :484.
- `run-receipt`: same file, `finally:` block, `with (output / "receipt.json").open("x") ... json.dump` :547-549;
  the receipt is also printed to stdout :550. Root = `(args.root or repo).resolve()` :445; argv = `command[1:]`.
- `stage-receipt`: `atx-engine/tools/stage_chain.py` `Chain._run`: failed receipt `self.write(self.failed_path(i),
  ...)` :264 (then `raise ChainError`), ok receipt `self.write(self.receipt_path(i), ...)` :268. Add
  `Chain.__init__(..., on_write=None)` :112; the only construction is `scripts/research_wave.py` `chain_of` :37.
- `cycle-binding`: `scripts/cycle_resume.py` `write_binding` :183-192 (root = `cycle.res.root`, a
  `cycle/resolver.Resolver`). PENDING B2.
- `cycle-verdict`: `scripts/cycle_verdict.py` `write_verdict` :141-147 (main file) and `keep_copy` :126-138 (copy
  `verdicts/<mode>-<k>.json`). PENDING B2.
- `wave-result`: `scripts/wave_stage_record.py` `record` :263-264 (`w.write_json(path, doc)`). PENDING B2. Plan: add
  `Wave.store_record(rel, cls)` to `scripts/wave_context.py` (SQL3's file) so the B2-file edit is one call line.
- `wave-candidate`: `scripts/wave_queue.py` `write` :136-142 (covers `new`, `pin` and `record_wave`).
- `wave-manifest`: `scripts/wave_queue.py` `main` emit branch :344. Class `wave-manifest` has `render: null`, so
  `check_one` cannot render it (reason "the class has no render rule"): the hook should check only presence (artifact
  row with that class and the file's SHA-256) for a class without a render rule.

Store CLI and checker:
- `atx-research-store ingest --catalog DB --class C --path P [--root R] [--classes FILE]` (`store_cli.cpp`
  `cmd_ingest` :310-344): exit 0 / 2 usage / 3 refusal (sealed root, duplicate candidate id N3, path outside root)
  / 4 error. `--path` may be absolute inside R or R-relative. Pass `--classes` = the repo's
  `atx-engine/schemas/research_store/classes.json` (same file `research_store_identity.DEFAULT_REGISTRY` uses), so
  ingest and check use one registry even for a test root.
- `research_store_identity.check_one(db, path, cls=None, *, root=None, registry=None)` :310; path = root-relative.
  The typed `run` table already maps receipt key `store` -> column `store` (`TYPED["run"]`, :75-84).

Store block (SQL-6, brief task 2 + carry item 6): `research_store_hook.selection(root, argv) -> dict | None`:
`{"schema": "atx.run-store/v1", "catalog": {"path": "build-equity/research-store/catalog.sqlite", "exe_sha256": ...}
| null, "indexes": [sorted POSIX paths relative to root]}`; catalog on iff `<root>/build-equity/research-store/
catalog.sqlite` and `<root>/build-equity/bin/atx-research-store.exe` both exist (once per process per root). Indexes:
for each argv token (and `--flag=value` value) naming an existing dir `d` (relative tokens resolve against root, the
child's cwd): `d/index.sqlite`, else `d/../index.sqlite` (mirror `record_store.RecordStore.__init__` :95-104), plus
`d/*/index.sqlite`. Write `store` LAST in both files: `start.json` gets `dict(receipt, store=store)`; in `finally`
set `receipt["store"] = store` after `logs`, before the dump; None = key absent = today's bytes. The hook uses the
same recorded selection (no second check).

New literal `atx.run-store/v1`: the guard `atx-engine/tools/test_research_store_classes.py::
test_every_schema_literal_is_registered` turns red unless registered. Plan: add `"registers": ["atx.run-store/v1"]`
to the `run-receipt` and `run-start` rows of `classes.json` (both globs literal and disjoint, so
`shared_literal_problems` passes); list it as a cross-lane edit / SQL2-CLS item for root.

Hook behaviour (`scripts/research_store_hook.py`, new): `record(root, path, cls)`; no-op when the catalog is off (no
subprocess, no import of sqlite3 / identity module); else `ingest` (60 s timeout, output captured), then
`check_one` (or the presence check); any failure prints ONE line to stderr `store: WARNING <cls> <relpath>: <why>
(the JSON stands; repair: atx-research-store catalog --rebuild)`; catch `Exception` only; never raises, writes
nothing beside the JSON, adds no path depth. The chain's `on_write` call: wrap in try/except in `stage_chain`.

W3-4 design (`seal_guard.cpp` `sealed_root` :120-122, header comment `seal_guard.hpp` :9-11, :55-57): bounded
upward walk from `normal_root(root)`; the OUTERMOST ancestor holding a `.git` entry (file or dir) is the repo top
(outermost, not nearest: a `.git` planted in a sealed dir must not exempt it); scan `has_sealed_year` on the
remainder below the top only (the top's own name and its ancestors are environment); no `.git` ancestor = whole
path as today (fail closed); an fs error = no `.git`. Callers unchanged (`catalog.cpp:739,752`,
`cache_index.cpp:226`, `store_cli.cpp:266,317,464`). New gtest in `atx-engine/tests/research/
research_catalog_seal_test.cpp`, e.g. `SealGuard.RootTokenScannedOnlyBelowTheRepoTop`: `<tmp>/env-2025/repo/.git`
-> `repo` and `repo/scripts` not sealed, `repo/v8-2024-oos` sealed, a planted `repo/x-2024/.git` still sealed,
`<tmp>/env-2025/norepo` sealed. Existing `SealGuard.*` (%TEMP% has no `.git` ancestor: checked, `C:/Users/natha`,
`C:/atx-wt`, `C:/` hold none) stay green. Build: `powershell scripts\research-build.ps1 -Tag p9-sql3-a -Targets
"atx-engine-research-catalog,atx-engine-research-catalog-tests,atx-research-store"` (equity-dev; configure first
with `powershell scripts\atx-build.ps1 configure -Preset equity-dev`); run `--gtest_filter=ResearchCatalog*:
SealGuard*:StoreCli*`. N3: candidates stay keyed by id (W3-4): document in the report; the hook's warning names
`catalog --rebuild`.

W3-5 design (`atx-impl/tools/book_monitor.py`): in `Inputs` before `fit_records` (:239-240) refuse (`require` ->
`MonitorError`, exit 1, nothing written) when an `index.sqlite` exists at `W`, `W/..` or any `W/*/` (the v8 store
roots are `<work>/<role sha16>-<window>/`, `fit_composition_weights.WorkStore.__init__` :1330-1333,
`stored_factor_series` :1374-1395); use `record_store.INDEX_NAME`. Fitter untouched. Test in
`atx-impl/tools/test_book_monitor.py` (World.fit_work :121-139 builds the dir; plant an empty `index.sqlite`).

Tests planned (no TDD): `scripts/tests/test_research_store_hook.py` (selection cases; no catalog -> subprocess never
called and files byte-identical; fake runner -> one ingest per write with class / path / `--root` / `--classes`; a
failing exe -> one warning, writer return and exit unchanged; a check mismatch -> one warning; real exe (hardlink or
copy of the built `atx-research-store.exe` into `<tmp root>/build-equity/bin/`, `init` the catalog) -> every class
ingested and found by `check_one(db, rel, cls, root=...)`; tiny-world fake wave (`wave_fixture.build` + queued
candidates as `test_wave_driver.queued_root`, `driver` `receipt_digest: content`, `record_date`, fixed
`GIT_AUTHOR_DATE` / `GIT_COMMITTER_DATE`) run without catalog and with a failing fake exe at the same root path:
files identical after `stage_chain.time_free`, exit codes equal). Runner golden: capture the normalized start /
receipt of the PRE-change runner first (child `python -c pass`, `--no-git`), freeze it as a literal in the test.
`test_stage_chain.py`: `on_write` once per ok and per failed receipt, never for a done stage.
Suites per `r6-wave3-common.md`: `scripts/tests` under PYTHONHASHSEED 0 and 1, `atx-engine/tools/test_stage_chain.py`
and the store tests (`test_research_store_{classes,blind,identity,fixtures}.py`, `test_research_store.py`) in their
own invocations, `atx-impl/tools/test_book_monitor.py`.

## Next step for a fresh agent

1. `git merge feat/platform-p9-20261003` (root's head), check whether B2 is merged in root.
2. Implement items 1 (except the B2 files), 2, 3, 4 as designed above; configure equity-dev and build `p9-sql3-a`;
   run the gtests and pytest suites; commit; then do item 5 if B2 is in root, else report it pending (three call
   lines: `cycle_resume.write_binding`, `cycle_verdict.write_verdict` + `keep_copy`, `wave_stage_record.record`).
3. Report `task-SQL3-report.md` with the "For root" section (real-tree catalog run is root's, M2A-4; G-P10 (a)-(e)).
