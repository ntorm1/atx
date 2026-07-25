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

**STOPPED BY OPERATOR mid-run.** Invocation was:

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

---

## Steps 4-7 — not started

Top-up pull, scale decision, full production build, and final report are all
untouched. **Cumulative realized spend for this run: $0.0000 / $100.**
