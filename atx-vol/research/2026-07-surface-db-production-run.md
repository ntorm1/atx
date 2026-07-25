# 2026-07 Surface DB Production Run

Executed 2026-07-24 from the `feat/surface-db-prod` worktree. Plan: Task 10 of
`docs/superpowers/plans/2026-07-22-surface-db-production.md`.

**Budget: $100. Every paid step is preceded by its own free `get_cost` preflight,
whose output is pasted below before the paid invocation.**

Data roots:
- v1 source hive: `C:/atx-data/spy-dispersion/opra` (`<symbol>/<date>.parquet`, 53 symbol dirs)
- v2 hive: `C:/atx-data/opra-hive` (`date=YYYY-MM-DD/data.parquet`)
- surface db: `C:/atx-data/surface-db/2026`

Carry rate: **`--r 0.043`**, matching the repo's existing real-OPRA pipelines
(`examples/american_iv_bench.cpp:134`, and the documented usage lines of
`examples/universe_surfdb_populate.cpp` / `examples/mag7_surfdb_populate.cpp`).
`r = 0.0` is NOT usable against real data — see Step 3 notes.

---

## Step 1 — Migrate v1 → v2 (free, local IO)

Dry run first:

```
$ python atx-vol/tools/migrate_opra_hive.py --src C:/atx-data/spy-dispersion/opra \
      --dst C:/atx-data/opra-hive --dry-run
  date=2026-07-17  files=  51  rows=   145231  planned
dates=135 written=0 skipped=0 rows=0
```

**BLOCKER FOUND AND FIXED (first real-data use of the T8 tool).** The first real
invocation hard-failed on *every* source file:

```
ValueError: schema drift in C:\atx-data\spy-dispersion\opra\AAPL\2026-01-02.parquet:
file schema does not match the canonical OPRA v2 schema.
  expected: ts: timestamp[ns] / underlying: string / ...
  got:      ts: timestamp[ns] not null / underlying: string not null / ...
```

`_validate_schema` used `pa.Schema.equals`, which compares field **nullability**.
The real v1 corpus is written with every field `not null`; `CANONICAL_SCHEMA`
declares them nullable. The tool's own tests never caught this because its
fixtures are written by pyarrow, which defaults to nullable — so the check passed
9/9 against synthetic data and rejected 100% of production data. Fixed to compare
column names and types in order (the actual contract the C++ loaders validate);
genuine drift — renamed, reordered, missing, retyped — still fails closed.
Regression suite still 9/9 after the change.

Real run:

```
$ python atx-vol/tools/migrate_opra_hive.py --src C:/atx-data/spy-dispersion/opra \
      --dst C:/atx-data/opra-hive
dates=135 written=135 skipped=0 rows=17743990
manifest: C:\atx-data\opra-hive\migration_manifest_20260724T212311_379267.csv
MIGRATE_EXIT=0
```

- 135 date partitions, 2026-01-02 .. 2026-07-17, **17,743,990 rows**, 427 MB
- one `data.parquet` per date (3.4 MB at 51 symbols/date)

Idempotence re-run: `dates=135 written=0 skipped=135 rows=0`, exit 0.

**Cost: $0 (pure local IO).**

---

## Step 2 — Smoke pull (3 names x 2 sessions)

### Free preflight (dry run)

```
$ python atx-vol/tools/pull_opra_hive.py --symbols-file <SPY,AAPL,NVDA> \
      --start 2026-07-20 --end 2026-07-21 --out C:/atx-data/opra-hive \
      --cap 5 --dry-run --env-file C:/atx/.env

universe=3 sessions=2 [2026-07-20..2026-07-21] dataset=OPRA.PILLAR schema=cbbo-1m snap=19:55Z
cells: total=6 to_pull=6 (over 2 sessions)

FREE preflight (metadata.get_cost - no egress):
  2026-07-20: 3 syms est=$0.000000  (unit $0.00000000/sym-day)
  2026-07-21: 3 syms est=$0.000000  (unit $0.00000000/sym-day)

ESTIMATE (remaining spend): $0.0000 = $0.00000000/sym-day x 6 cells (cap $5.00)
Authorized estimate $0.0000 within cap $5.00. Symbols kept: 3 (dropped 0).
DRY RUN - no data pulled.
```

**Zero-estimate control.** A $0 estimate is ambiguous: "free" and "this query
matches no data" print identically, and the hard `--cap` is enforced against the
*estimate*, so a broken estimator would make the cap toothless. Ran the same free
preflight against **2026-07-17**, a date known to hold real data (it is in the
migrated hive), with `--force` to defeat the on-disk skip:

```
  2026-07-17: 3 syms est=$0.000000  (unit $0.00000000/sym-day)
```

Same zero on known-good data => the zero is this account's pricing for OPRA
cbbo-1m at this volume, not an empty match. Proceeded with the 6-cell probe,
which is exactly what the plan sizes this step for.

### Paid run

```
$ python atx-vol/tools/pull_opra_hive.py --symbols-file <SPY,AAPL,NVDA> \
      --start 2026-07-20 --end 2026-07-21 --out C:/atx-data/opra-hive \
      --cap 5 --env-file C:/atx/.env

  [1/2] 2026-07-20 pulled: 3 boards (running_spend=$0.0000)
  2026-07-21 pull retry 1: 504 The remote gateway timed out.
DONE boards_written=6 dates_written=2 unmapped_rows=0 failed_sessions=0
ACTUAL SPEND (realized preflight of pulled cells): $0.0000
PULL_EXIT=0
```

- 2 new partitions: `date=2026-07-20` (0.48 MB), `date=2026-07-21` (0.47 MB)
- the retry path fired on its first real use (one 504) and recovered

**Realized spend: $0.0000. Cumulative: $0.0000 / $100.**

---

## Step 3 — Smoke build

### First attempt — stopped by operator

Invocation was:

```
$ ./build/bin/atx-vol-surface-db-build.exe \
      --db C:/atx-data/surface-db/2026 --hive C:/atx-data/opra-hive \
      --from 2026-07-01 --to 2026-07-22 --symbols SPY,AAPL,NVDA --index SPY \
      --r 0.043 --report C:/atx-data/surface-db/smoke_report.csv
```

Killed before it produced a report. On-disk state at the stop: `manifest.atxdb`
(192 B) and an empty `partitions/` directory — the db root was created and the
manifest initialized, but no partition had been written yet. No fit results, no
coverage numbers, and **no conclusion may be drawn about whether real OPRA data
fits at `r = 0.043`** — that question is still open.

The partial state is safe to resume over: the build is cell-aware and a partition
is only published after its cells are written, so re-running the same command
starts clean rather than resuming into a half-written partition.

**Cost: $0 (local compute only).**

### Second attempt — rerun to completion

Same command, re-run verbatim over the partial state (manifest present,
`partitions/` empty).

```
config.n_symbols 3
config.n_configured 3
config.n_skipped_existing 0
config.n_disabled_failed 0
coverage.cells_loaded 42
coverage.cells_to_fit 42
coverage.cells_refit 0
coverage.cells_already_present 0
coverage.cells_ok 33
coverage.cells_failed 9
coverage.dates_total 14
coverage.dates_written 14
coverage.dates_skipped_complete 0
coverage.dates_skipped_would_drop 0
n_dates_loaded 14
n_dates_missing 8
n_load_errors 0
n_coverage_holes 0
config.failed_symbols
symbol.AAPL attempted=14 ok=7 failed=7 disabled=0
symbol.NVDA attempted=14 ok=13 failed=1 disabled=0
symbol.SPY attempted=14 ok=13 failed=1 disabled=0
report C:/atx-data/surface-db/smoke_report.csv
SMOKE_BUILD_EXIT=0
```

14 partitions written, 2026-07-01 .. 2026-07-21.

**The open question from the first attempt is answered: real OPRA quote surfaces DO
fit at `r = 0.043`.** 33 of 42 cells produced a stored surface. Every fixture used
during development was put-call-parity-consistent by construction; real quotes are
not, and they still fit.

`n_dates_missing 8` is not a gap — it is exact. The range 2026-07-01..22 holds 22
calendar days; 6 are weekend, 2026-07-03 is the observed Independence Day holiday, and
2026-07-22 was not yet in the hive. 6 + 1 + 1 = 8, leaving the 14 loaded. `n_load_errors
0` and `n_coverage_holes 0` confirm the IMP-2 classification split behaves on real data:
absent sessions are counted as missing dates, not as corruption.

### Gate 1 — re-run must re-fit nothing: **FAILED**

The plan's Step 3 acceptance is "immediate re-run → `cells_to_fit==0`". Re-running the
identical command gave:

```
config.n_configured 0
config.n_skipped_existing 3
coverage.cells_loaded 42
coverage.cells_to_fit 9
coverage.cells_refit 12
coverage.cells_already_present 21
coverage.cells_ok 12
coverage.cells_failed 9
coverage.dates_written 7
coverage.dates_skipped_complete 7
RERUN_EXIT=0
```

Config selection did converge (`n_skipped_existing 3`, nothing re-selected). Coverage did
not, and the decomposition is exact:

- The **9 permanently-failing cells retry every run.** There is no persisted
  known-failed state — `surface_db_build.hpp:214` says so deliberately, so that a
  transient failure is retryable.
- The **7 dates holding a failure get rewritten whole**, which drags their **12 already-OK
  sibling cells back through the fitter** (`cells_refit 12`). The 7 fully-clean dates
  contribute the 21 `cells_already_present` and are skipped outright.

9 + 12 + 21 = 42. So the plan's gate does not hold on data containing permanent fit
failures, and the cost is not proportional to the failures: at production width (51
symbols per date) **one failing symbol on a date re-fits the other 50, on every
subsequent run, forever** — converting a cheap resume into a near-full rebuild. That is
the same class of non-convergence the IMP-1 fix closed for *disabled* cells, reaching
the same outcome through the failed-fit path.

### The 9 failures are not a data deficit

Failures are deterministic — the re-run failed the identical 9 cells. Their locus, read
back from the partitions with `atx-vol-surface-db partitions --key <date>` (a symbol
absent from a date = its fit failed there):

| symbol | failed | dates |
|---|---|---|
| AAPL | 7/14 | 07-07, 07-08, 07-09, 07-10, 07-13, 07-14, 07-21 |
| SPY | 1/14 | 07-07 |
| NVDA | 1/14 | 07-21 |

Counting hive rows per (date, underlying) rules out thin data as the cause:

| date | SPY rows | AAPL rows | NVDA rows | AAPL two-sided |
|---|---|---|---|---|
| 2026-07-06 (all pass) | 13,690 | 3,398 | 3,917 | 3,131 |
| 2026-07-07 (SPY+AAPL fail) | 13,690 | 3,455 | 3,913 | 3,184 |
| 2026-07-08 (AAPL fails) | 13,690 | 3,460 | 3,946 | 3,169 |

AAPL carries ~3,400 rows and ~3,100 two-sided quotes on **every** date in the range,
failing and passing alike, and 07-07 is row-for-row indistinguishable from its
neighbours. Whatever rejects these cells, it is not quote volume.

<!-- STEP3-DIAGNOSIS -->

---

## Step 4 — Top-up pull (full existing universe, 2026-07-20..21)

### Free preflight (dry run) — logged BEFORE the paid call

```
$ python atx-vol/tools/pull_opra_hive.py \
      --universe atx-vol/data/universe/spy_top50_2026-01-01.csv \
      --start 2026-07-20 --end 2026-07-21 --out C:/atx-data/opra-hive \
      --cap 20 --dry-run --env-file C:/atx/.env

universe=51 sessions=2 [2026-07-20..2026-07-21] dataset=OPRA.PILLAR schema=cbbo-1m snap=19:55Z out=C:\atx-data\opra-hive
cells: total=102 to_pull=96 (over 2 sessions)

FREE preflight (metadata.get_cost - no egress):
  2026-07-20: 48 syms est=$0.000000  (unit $0.00000000/sym-day)
  2026-07-21: 48 syms est=$0.000000  (unit $0.00000000/sym-day)

ESTIMATE (remaining spend): $0.0000 = $0.00000000/sym-day x 96 cells (cap $20.00)

Authorized estimate $0.0000 within cap $20.00. Symbols kept: 51 (dropped 0).
DRY RUN - no data pulled.
DRYRUN_EXIT=0
```

`total=102` (51 symbols x 2 sessions) against `to_pull=96` is the resume path
proving itself on real data: exactly the 6 cells Step 2 already paid for are
excluded, and `48 syms` per session is `51 - 3`.

### Paid run

```
$ python atx-vol/tools/pull_opra_hive.py \
      --universe atx-vol/data/universe/spy_top50_2026-01-01.csv \
      --start 2026-07-20 --end 2026-07-21 --out C:/atx-data/opra-hive \
      --cap 20 --env-file C:/atx/.env

Authorized estimate $0.0000 within cap $20.00. Symbols kept: 51 (dropped 0).
  [1/2] 2026-07-20 pulled: 48 boards (running_spend=$0.0000)
DONE boards_written=96 dates_written=2 unmapped_rows=0 failed_sessions=0
ACTUAL SPEND (realized preflight of pulled cells): $0.0000
kept N=51 dropped=0 manifest=C:\atx-data\opra-hive\manifest_hive_2026-07-20_2026-07-21_1955.csv
STEP4_PULL_EXIT=0
```

96 boards over 2 sessions, `unmapped_rows=0`, `failed_sessions=0`. 2026-07-20 and
2026-07-21 now hold the full 51-name universe.

**Realized spend: $0.0000. Cumulative: $0.0000 / $100.**

---

## Step 5 — Scale decision

### Free preflight over ALL of 2026-07 — logged BEFORE any paid call

```
$ python atx-vol/tools/pull_opra_hive.py \
      --universe atx-vol/data/universe/spy_top50_2026-01-01.csv \
      --start 2026-07-01 --end 2026-07-24 --out C:/atx-data/opra-hive \
      --cap 90 --dry-run --env-file C:/atx/.env

universe=51 sessions=17 [2026-07-01..2026-07-24] dataset=OPRA.PILLAR schema=cbbo-1m snap=19:55Z out=C:\atx-data\opra-hive
cells: total=867 to_pull=249 (over 5 sessions)

FREE preflight (metadata.get_cost - no egress):
  2026-07-20: 48 syms est=$0.000000  (unit $0.00000000/sym-day)
  2026-07-21: 48 syms est=$0.000000  (unit $0.00000000/sym-day)
  2026-07-22: 51 syms est=$0.000000  (unit $0.00000000/sym-day)

ESTIMATE (remaining spend): $0.0000 = $0.00000000/sym-day x 249 cells (cap $90.00)

Authorized estimate $0.0000 within cap $90.00. Symbols kept: 51 (dropped 0).
SCALE_DRYRUN_EXIT=0
```

The cap is the plan's rule applied literally: remaining budget ($100) minus the
$10 reserve. The 249 cells decompose exactly as `48 + 48 + 51 + 51 + 51` over
2026-07-20..24 — the twelve sessions 2026-07-01..17 are already complete at 51
symbols each from the Step 1 migration, so the resume filter drops them entirely.

### DECISION: stay at 50 names. Widen dates, not the universe.

The plan's Step 5 asks for a **top-100** universe preflight. **A top-100 universe
is not derivable from this repo's inputs**, and inventing one would be worse than
not having it:

- `atx-vol/data/universe/spy_top50_2026-01-01.csv` is generated by
  `atx-vol/tools/build_spy_top50_universe.py` from the SEC N-PORT filing at
  `C:/atx-data/spy-dispersion/universe-source/spy-nport-2025-12-31.xml`, ranked by
  `pctVal`, cut at 50.
- That generator resolves CUSIP → ticker through a **fixed 50-row map**
  (`examples/spy_top50_symbol_map.tsv`, 51 lines incl. header) and **raises** on any
  unmapped CUSIP. Constituents 51-100 have no ticker mapping in the repo.
- So a top-100 list would require sourcing 50 CUSIP → ticker mappings from outside
  the repository. That is a data-provenance change, not a scale decision, and it
  would silently break the run's determinism guarantee.

The budget gate itself is **not** what binds here: at $0.0000 estimated against a
$90 cap, the money test passes with room. The blocker is purely that the wider
constituent list does not exist as a reproducible artifact.

What the run does instead, which is the actionable half of the same question: hold
the universe at 51 names (SPY + 50) and extend **date** coverage from 2026-07-21 to
the last completed session, 2026-07-24. Those numbers are the preflight above.

<!-- STEP5-RESULT -->

---

## Steps 6-7 — full production build + final report

<!-- STEP67-RESULT -->

**Cumulative realized spend for this run: $0.0000 / $100.**
