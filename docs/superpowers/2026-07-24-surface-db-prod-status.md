# Surface DB Production — Sprint Status

**Date:** 2026-07-24 (stopped by operator)
**Branch:** `feat/surface-db-prod` — worktree `C:\atx\.claude\worktrees\surface-db-prod`
**Tip:** `bc00cb5` · 39 commits ahead of merge-base `717e08d` (includes the 15 merged from `main`)
**Plan:** `docs/superpowers/plans/2026-07-22-surface-db-production.md`
**Spec:** `docs/superpowers/specs/2026-07-22-surface-db-production-design.md`
**Ledger:** `.superpowers/sdd/surface-db-prod/progress.md` (gitignored, worktree-local)

**Money: $0.0000 spent of the $100 budget.** One paid Databento call was made (the
6-cell smoke pull); it realized $0.00 on this account's plan.

---

## Bottom line

The library, both CLIs, and the tooling are **complete, reviewed, and green**.
The production data run is **partially executed and stopped at Step 3 of 7**.

Nothing is half-edited: the working tree is clean apart from the two markdown
files this status refers to, and every code change is committed.

---

## What is done

### Tasks 1-9 (the feature itself)

| Task | Commits | State |
|---|---|---|
| T1 `load_opra_cbbo_from_table` seam | `79e35ff` | reviewed clean |
| T2 synthetic hive fixture | `0b5e1c9` | reviewed clean |
| T3 `load_opra_hive` v2 loader | `a862878`, `bd31d7b` | re-reviewed clean |
| T4 `generate_symbol_configs` | `0abcd66` | approved |
| T5 `build_surface_db` driver | `6948727` | approved |
| T6 build CLI + doc | `78b0454`, `d9be2cd` | re-reviewed approved |
| T7 python binding | `bfdd307` | **committed but UNVERIFIED — see Open items** |
| T8 `migrate_opra_hive.py` | `87bff2e`, `bc00cb5` | reviewed clean; real-data fix added |
| T9 `pull_opra_hive.py` | `594f6d0`, `e1bbf68`, `0efdeea` | re-reviewed clean |

### Whole-branch review and its fixes

Final review returned **0 Critical, 3 Important**. Two were fixed:

- `e7a4325` — **a build never reached a fixed point.** Disabled and failed-fit cells
  never entered the written partition, so the resume filter marked them `to_add`
  every run, rewriting the whole date and re-fitting every sibling, forever. This
  falsified the branch's headline "re-running re-fits ZERO" guarantee and would
  have failed Task 10's own acceptance gates on real data. Fix excludes disabled
  configs from the tally (monotone toward skipping, so it cannot cause data loss)
  while keeping failed fits retryable; the promise was narrowed to the truth.
- `cd0f72e` — **`n_load_errors` conflated coverage holes with corrupt files.** An
  absent symbol on a present, readable date landed in the corruption counter, so a
  discover-all production build over a sparse universe would have reported a large
  number meaning "sparse", with real corruption hidden in it. The two classes are
  *not* distinguishable by `ErrorCode` (a hole and a wrong-schema file are both
  `InvalidArgument`), so classification is now structural in the loader, with
  `n_coverage_holes` as a sub-count of `n_error` — the partition invariant and the
  subtraction are safe by construction.
- `868d25e` — prose the fixes falsified, plus the missing test for the documented
  "schema-broken AND hole" case (the test previously cited for it took a different
  code path entirely).

The third Important is **still open** — see below.

### CLI-native management (added after the operator asked to drop the Python dependency)

- `ebb78cc` — `surface_db_admin` library: `describe_db`, `describe_partition`,
  `describe_symbol`, `query_surface` (through `map_surface`, the zero-copy path
  production readers use), `verify_db`.
- `2f64c96` — `atx-vol-surface-db` CLI: `info`, `partitions`, `symbols`, `config`,
  `query`, `verify`. Logic in the library, CLI thin on top.
- `cd9b491` — **`--r` flag + exit 3 on a totally failed build.** The build CLI
  hardcoded `r = 0.0`; against a hive priced under a nonzero rate every fit failed,
  the tool **exited 0**, and wrote zero partitions. Green exit, empty database.
- `b4befe4` — **exit 3 when every symbol fails config selection.** That path left
  `cells_to_fit == 0`, which is indistinguishable from the healthy nothing-to-do
  resume path — the same silent-green trap one stage earlier.
- `a623c89` — **`verify` hardening.** Three ways it could call a broken database
  sound: `--min-cells` parsed with unchecked `strtoull` and failed *open* when the
  shell dropped its value; zero-cell selection (no partitions / all symbols
  disabled / a date range matching nothing) printed `verdict ok`; and payload-CRC
  corruption mapped cleanly because `verify` never called `validate_symbol`, while
  the 30-day-bracketed probe missed a corrupt far-dated slice. All closed. A
  `--no-checksum` opt-out was deliberately **not** added.

### Test gate

Controller-run (not agent-reported) at each stage:

```
122/119 -> 138/135 -> 143/140 -> 153/150
final: 153 tests / 19 suites - 150 PASSED, 3 SKIPPED, 0 FAILED
```
The 3 skips are pre-existing absent-fixture skips (`spy_real_test`,
`opra_breadth_corpus` x2).

### Merge of `main`

`250a7b2` merged local `main` (15 commits: Wave B `listed_dispersion_pipeline`,
RunArchive hardening) with **no conflicts**. Post-merge full suite:
**2041 tests, 1995 passed, 43 skipped, 2 FAILED.**

**Those 2 failures are pre-existing red on `main`, not from this branch.** Proof:
`AllQualityModes/SurfaceV2Qualification.RiskBuildRunsTheModeCarryAndInversionBudgets/{Latency,Balanced}`
at `surface_v2_qualification_test.cpp:273` expects `max_borrow_pairs` 6 and 12 and
gets 5; neither `main`'s 15 commits nor this branch's commits touch that test,
`pricer_fitter.cpp`, `session.cpp`, or `deamer.hpp`; and the test's last change
`f99d796` (2026-07-19) is an ancestor of the fork point. `pricer_fitter.cpp:1112-1131`
sets 5/5/12 — nothing anywhere sets 6. **Someone else's expectation table has
drifted from their constants; `main` is currently red.**

---

## T10 production run — stopped at Step 3 of 7

Full log: `atx-vol/research/2026-07-surface-db-production-run.md`

| Step | State | Cost |
|---|---|---|
| 1 — Migrate v1→v2 | **DONE** — 135 partitions, 17,743,990 rows, 427 MB, idempotent on re-run | $0 (local) |
| 2 — Smoke pull (3 names × 2 sessions) | **DONE, PAID** — 6 boards, 2 partitions; one 504 absorbed by the retry path | **$0.0000 realized** |
| 3 — Smoke build | **STOPPED MID-RUN** — db root + manifest created, `partitions/` empty, no report | $0 |
| 4-7 | not started | — |

Two things worth carrying forward:

**Step 1 hit a blocker on first real-data use.** `migrate_opra_hive.py` validated
with `pa.Schema.equals`, which compares field **nullability**. The real v1 corpus
is `not null`; `CANONICAL_SCHEMA` is nullable — so the tool rejected **100% of
production files** while passing 9/9 against its own pyarrow-default fixtures.
This is the second tool on this branch that passed review but had never been
pointed at real data. Fixed in `bc00cb5` (compare names + types in order; genuine
drift still fails closed) with explicit operator approval, since it was the sole
blocker and Python work was otherwise paused.

**The $0 estimate was verified, not assumed.** A zero `get_cost` is ambiguous —
"free" and "matches no data" print identically — and the hard `--cap` gates on the
*estimate*, so a broken estimator makes the cap decorative. Re-ran the free
preflight against known-good 2026-07-17 with `--force`: same $0. That distinguishes
pricing from an empty match, and cost nothing.

**Carry rate:** all builds use `--r 0.043`, the repo's established real-OPRA value
(`examples/american_iv_bench.cpp:134`, and the usage lines of both
`*_surfdb_populate.cpp`). Not invented.

**Unanswered:** whether real OPRA quote surfaces actually fit at `r = 0.043`. The
smoke build was killed before producing coverage numbers. Every synthetic fixture
to date is put-call-parity-consistent by construction; real quotes are not. This
is the first thing Step 3 will tell you on resume.

---

## Open items

1. **IMPORTANT — `migrate_opra_hive.py` can destroy paid-for data.** `:117-119`
   under-reports destination underlyings when a row group spans several names
   (`min != max`) or stats are absent; the superset check then fails, control falls
   into `_merge_date`, and the destination is rewritten from the v1 sources alone —
   **dropping every symbol added by a paid pull**. Same root cause T9 fixed in
   `pull_opra_hive.py` (`0efdeea`), where it only cost a redundant re-pull. The fix
   is the four lines already in `pull_opra_hive.py:243-245`.
   *Latent:* needs a foreign writer (DuckDB / `pyarrow.dataset` repack) or a
   stats-less file — both atx writers emit one row group per underlying. Step 1
   migrated into an empty destination, so this run never risked it.
   **Must close before any future migrate over a populated tree.** Left open
   because it is Python.
2. **T7 python binding (`bfdd307`) is committed but unverified.** It compiles and
   the extension exports the symbol; its pytest has never run and it has had no
   task review. Do not treat it as tested. Note the machine's editable `atxvol`
   install points at `C:\atx` (main tree) — do not `pip install -e` from this
   worktree while another session uses main.
3. **T10 Steps 3-7** — resume with the Step 3 command in the run log.
4. **`main` is red** (2 pre-existing `SurfaceV2Qualification` failures, above).
   Not this branch's to fix, but it will muddy any post-merge gate.
5. **Merge conflict expected.** `bfdd307` adds
   `atx-vol/python/src/bindings/surface_db.cpp`; the main tree carries uncommitted
   WIP doing the same bindings split. Expect conflicts on that file, `module.cpp`,
   and `python/CMakeLists.txt`.
6. **Branch not merged.** Left for the operator.

---

## Deferred by operator instruction (not defects)

- All Python development: the T7 pytest, the IMP-3 fix above, and a killed
  in-progress migrate fix whose 71 lines of uncommitted test edits were reverted.
- Verification was moved off the Python wrapper and onto `atx-vol-surface-db`,
  which is what the management CLI above exists for.
