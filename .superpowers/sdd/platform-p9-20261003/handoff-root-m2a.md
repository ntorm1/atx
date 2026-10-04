# Handoff: root M2a (SQL1, SQL2, A3) at the owner stop, 2026-10-03

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Head at stop: `1c90011a` (A3 flip merge) + the docs
commit that adds this file. Detail per lane: `root-wave2-merge-report.md` section M2a; one row per lane in
`integration-log.md`. Nothing is half-merged; nothing of mine is running. Next free build tag: **p9-1w**.

## Per lane

### SQL1: MERGED and verified

- Merged `1a5b051d` -> `d31c5367`; ruled N1 fix `8d4cc52c` (`connection.cpp` `absolute()` before `weakly_canonical`;
  `DbConnection.RefusesUncPath` extended with a bare relative name from a local temp cwd).
- Built: p9-1s FAILED (unknown target `atx-engine-store-tests`: root tree lacked test group `store`; reconfigured
  through the wrapper with `-Groups "alpha;factory;learn;data;eval;combine;risk;book;library;store"`); **p9-1t** exit 0,
  0 warnings (6 targets incl. `atx-impl-tests`).
- Tested: `Db*` 44p; `ResearchStore*` 27p; `atx-engine-store-tests` 40p; `atx-engine-library-tests` 66p (2 disabled);
  atx-impl `AtxImplStoreDiscover/AtxImplProvenance/AtxImplProvenanceDigest` 23p; pytest SQL1 4 files 31p x2 seeds;
  record-store consumers 166p. PCH-off satisfied in equity-dev (no PCH flags on the SQL1 TUs).
- Not done: full `atx-engine/tools` / `atx-impl/tools` suites (wave-2 gate); SQL1's opt-in X-5 identity (moved to
  SQL2's real-tree items). pool-21 build object tree deleted (DISK-2b), receipts kept.

### SQL2: MERGED and verified

- Merged `cfe2de87` -> `1300fd6a`; SQL2-CLS `1892a183` (`atx.nav-rules/v1` -> class `nav-output`; the only missing
  literal on the merged tree).
- Built: **p9-1u** exit 0, 0 warnings (`atx-engine-research-catalog, atx-engine-research-catalog-tests,
  atx-research-store` + SQL1's three). `records_ops.cpp` 23.9 s (`.ninja_log`, -j4).
- Tested: catalog gtests 27p; pytest 5 files 43p / 129 subtests x2 seeds (FixtureChain ran); fixture chain digest
  `d32f7655e6cdd860e1af36ceadb00a1c1603136a69d17ad954e629cae4e84e69` (28/25/0/4), identity 12 checked 0 mismatches,
  relative = absolute root.
- Not done: real-tree bounded catalog run (review R4) and the X-5 opt-in cache identity (`cache init`, cold / warm,
  then delete the index); full `atx-engine/tools` suite. pool-23 object tree was already deleted by the lane.

### A3: MERGED (pre-flip verified; TRAIN identity held; flip merged; post-flip re-checks NOT run)

- Merged `a217fd7e` -> `ff1637ac`; S-R1 comment slip `08b0695d`; flip `50cb1571` -> `1c90011a` (after identity).
- Built: **p9-1v** exit 0, 0 warnings (first launch refused by the memory gate, no receipt). `atx-research-fields`
  `b2c30d47...5543`.
- Tested (pre-flip): fields-tests 66/66 (M1a-RED gtest pair green), A3 filter 19p; pytest real exe 8/11/3/2/35/4 p.
- TRAIN identity: six fields, flip registry blob as a scratch file, no reuse: py = engine = v15 for 6/6; manifests
  differ only in the six producer blocks (54 paths). `rows_sealed_value_decoded` = `rows_sealed_dropped` = 7,592,840
  per field, 0 of 262 row groups pruned. Peaks 542 MiB (py) / 950 MiB (engine).
- **Not done (resume here):**
  1. Registry check on the flipped file (from `atx-engine/tools`, fresh interpreter, repository window), expect
     `ok 92` and round-trip:
     `cd C:/atx-wt/pool-2/atx-engine/tools; & "C:/Program Files/Python312/python.exe" -B -c "import prepare_research_fields as b, field_registry as fr; d = fr.load(); fr.bind_modules(vars(b), fr.python_modules(d)); fr.check(vars(b), d); assert fr.dump(fr.regenerate(vars(b), d)) == open('field_registry.json','rb').read(); print('ok', len(d['fields']))"`
     (check the exact bind call against wave-1 M1a's `flip_registry.py` recipe in `root-wave1-merge-report.md:133-140`).
  2. Post-flip pytest, real exe, each file its own session (`PYTHONDONTWRITEBYTECODE=1`, `-q -rs -p no:cacheprovider`,
     `--basetemp` in scratch, `ATX_RESEARCH_FIELDS_EXE=C:\atx-wt\pool-2\build-equity\bin\atx-research-fields.exe`):
     `atx-engine/tools/test_field_registry.py` (expect 35p),
     `atx-engine/tests/fixtures/research_fields/test_vendor_engine_path.py` (expect 9p),
     `atx-engine/tests/fixtures/research_fields/test_research_fields_engine_path.py` (expect 11p).
  3. If green: append the result to the A3 section of `root-wave2-merge-report.md` (replace "post-flip re-checks NOT
     run"), update the A3 row in `integration-log.md`, then DISK-2: delete pool-12's `build-equity` object tree
     (keep `mega-*` receipts / logs, `a3-fix1-configure.log`, git-tracked `audits/`, `v8-interim3-pitch-render-run2/`).
  4. The TRAIN identity outputs stay in `build-equity/p9-a3-train-{py,engine}{,-run}` and
     `build-equity/p9-a3-train-identity/field_registry.flip.json` (git-ignored; delete after the PM has read this).

## Uncommitted state

- `progress.md` modified (PM's; not touched). Untracked `docs/plans/2026-10-02-x5-equity-curve.png` (owner's) and
  `docs/plans/2026-10-03-p9-b0-yf0-equity-curve.png` (appeared during M2a; not mine; left alone).
- Git-ignored run outputs listed above. Session scratch: `a3-train-run.ps1`, `a3_compare.py`, `gt-*.log`, `pyt-*.log`.

## Open questions for the PM

1. **A3 TRAIN field list:** identity ran on the six fields (no reuse) instead of v15's 84-name list, because v15's
   argv reuses and any reuse prior carries the six Python entries (the engine would never run), and a no-reuse
   84-field build exceeds the host budget. The history is the same (price / ohlc `source_checks` equal v15's on every
   shared path). Accept?
2. **Seal push-down on the real file:** 0/262 row groups pruned; all 7,592,840 sealed rows per field had their values
   decoded with the chunk and released unread (A3-FIX1 behaviour). Accept as is, or carry page-level / sorted-file
   pruning to A4?
3. **Root tree config:** `build-equity` now has test group `store` (needed for SQL1's named `atx-engine-store-tests`).
   Keep?
4. **Still open for SQL1 / SQL2:** the real-tree bounded catalog run and the X-5 opt-in cache identity were not in
   M2a's dispatch. Who runs them, and when?

Known-red now: `scripts/tests/test_research_mine.py::test_fields_are_the_rule_applied_to_the_registry` (E2) and
M1c-RED x2 Release `BookNormalScore.*` (C2). Both M1a-RED gtests are green. 0 trials: `build-equity/trials.jsonl`
133 lines, `27e40f9f`.
