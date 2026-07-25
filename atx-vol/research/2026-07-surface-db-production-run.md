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

### Gate 2 — CLI verify: **FAILED, and correctly so**

The plan calls for a query check "via python binding". Python work is paused by operator
instruction, so verification runs through `atx-vol-surface-db`, the management CLI built
for exactly this. It is the stronger check anyway: it maps and ATM-evaluates *every*
cell rather than one.

```
$ ./build/bin/atx-vol-surface-db.exe verify --db C:/atx-data/surface-db/2026 --min-cells 40
partitions 14
partitions_in_db 14
symbols 3
cells_checked 42
cells_ok 33
cells_unmappable 9
cells_non_finite 0
cells_checksum 0
failures_reported 9
failures_elided 0
fail 2026-07-07 AAPL kind=unmappable detail=NotFound: SurfaceArchiveV2::map_symbol: symbol not present
fail 2026-07-07 SPY  kind=unmappable detail=NotFound: SurfaceArchiveV2::map_symbol: symbol not present
fail 2026-07-08 AAPL kind=unmappable ...
fail 2026-07-09 AAPL kind=unmappable ...
fail 2026-07-10 AAPL kind=unmappable ...
fail 2026-07-13 AAPL kind=unmappable ...
fail 2026-07-14 AAPL kind=unmappable ...
fail 2026-07-21 AAPL kind=unmappable ...
fail 2026-07-21 NVDA kind=unmappable ...
min_cells 40
verdict FAILED

$ ...same command >/dev/null; echo $?
1
```

This is verify doing its job: it reaches the failing verdict from the database alone,
naming the same 9 (date, symbol) pairs the build reported, and exits 1. `cells_checksum
0` means the payload-CRC gate added in `a623c89` passed on all 33 stored surfaces, and
`cells_non_finite 0` means every stored surface evaluates finite at the money.

**Exit-code note, recorded because it nearly produced a false finding.** A first
reading showed `verdict FAILED` beside `VERIFY_EXIT=0`, which looks exactly like the
class of silent-green bug this branch spent three commits closing. It was an artifact of
the harness: the command was piped into `tail`, so `$?` was *tail's* status, not the
tool's. Re-run unpiped, verify exits 1 as documented. The same trap corrupted a migrate
run earlier in this log — piping a command whose exit code matters is not safe here.

### Gate 3 — CLI query on a stored cell: **PASSED**

```
$ ./build/bin/atx-vol-surface-db.exe query --db C:/atx-data/surface-db/2026 \
      --key 2026-07-17 --symbol SPY --strike 710 --tenor 0.0833
key 2026-07-17
symbol SPY
strike 710
tenor 0.083299999999999999
iv 0.19869966309823284
total_variance 0.003288813624408758
forward 746.56048464024002
uid 1478221309
n_slices 33
```

Independently sanity-checked rather than accepted: SPY's strike ladder in the hive for
that session runs 50 → 1480 with median 710, so K=710 is at the money. The returned
one-month forward of 746.56 implies a spot of 746.56 / e^(0.043 × 0.0833) ≈ 743.9,
consistent with that ladder, and 19.87% is a reasonable one-month SPY implied vol. The
tenor argument is in **years** — passing 30 returns a forward of 2384, i.e. the surface
honestly extrapolating to a 30-year horizon rather than silently clamping.

### Step 3 verdict

| acceptance item | result |
|---|---|
| configs stored | PASS — 3 symbols configured, 0 disabled |
| ~15 partitions touched | PASS — 14 |
| coverage clean | **FAIL** — 33/42 cells, 9 deterministic failures |
| immediate re-run → `cells_to_fit == 0` | **FAIL** — 9 to_fit, 12 sibling re-fits |
| query one cell | PASS (via CLI, not the python binding) |

Both failures trace to the same 9 cells.

### Root cause

A source investigation found the mechanism, and — because the code was destroying the
evidence — it could not name which predicate actually fired. That gap was closed first
(commit `069669e`), then the build was re-run to measure it.

**The fitter already computed a perfect diagnostic and the code threw it away twice.**
`pricer_fitter.cpp:1425-1444` builds a rejection error carrying the model, the failure
mask, butterfly and calendar slack with the offending slice and log-moneyness, and carry
and inversion status. It was discarded at `corpus_board_fit.cpp:287` (only `.code()`
kept) and again at `surface_db_populate.cpp:416-419` (`++n_failed`). The config stage
already named its failures in `config.failed_symbols`; the fit stage named nothing. So
an operator who lost 9 cells had a number and no next step — which is exactly the
position this run was in.

With the reason preserved, the re-run reports each failed cell. Decoded against
`ValidationFailure` (`surface_policy.hpp:66-89`):

| cells | model | mask | decodes to |
|---|---|---|---|
| AAPL ×5, NVDA ×1 | essvi | 2049 | `CarryGap \| InvalidDomain` |
| AAPL ×2 (07-09, 07-10) | essvi | 2176 | `CarryGap \| InversionResidual` |
| SPY ×2 (07-07, 07-22) | convex-dense | 2064 | `CarryGap \| Butterfly` |

Sample line, verbatim:

```
failed_cell 2026-07-22 SPY code=Unavailable detail=risk surface rejected: model=convex-dense
  mask=2064 butterfly=2 butterfly_slack=0.000107 butterfly_k=-0.104167 butterfly_slice=0
  slopes=-0.999893/-1.000000 calendar=0 ... carry=failed inversion=ok
```

Three readings, in order of what they rule out:

1. **`CarryGap` (bit 11) is set on 100% of failures — and is never the killer.** Its own
   documentation (`surface_policy.hpp:79-88`) says admission *publishes* a candidate whose
   only defect is CarryGap, as Degraded with the reason retained. So the cause of death is
   always the companion bit: `InvalidDomain`, `InversionResidual`, or `Butterfly`.
2. **This is not a missing dividend input.** The obvious guess — that `--r 0.043` with no
   dividend yield distorts the forward — is wrong. Carry is solved *per expiry from the
   board itself* by put-call parity (`curve_fit.cpp:134-143`); `r` is only the fallback
   flat rate when that solve fails (`opra_hive.hpp:113`). CarryGap therefore reports
   genuine carry-solve difficulty on real quotes, not a mis-specified input.
3. **The failures are marginal, not catastrophic.** SPY on 2026-07-22 misses the
   butterfly bound by `0.000107` with slopes of `-0.999893/-1.000000` — a hair over the
   no-arbitrage boundary. That is precisely the regime a different curve family is
   expected to recover.

**Nothing recovers them, because the recovery ladder is switched off for every
production cell.** `generate_symbol_configs` stores `pin_curve = true` unconditionally
(`surface_db_build.cpp:116`, and `:39` for the index leg). A pinned config leaves
`decision_` unset (`pricer_fitter.cpp:1140`), which makes `auto_routed` false at `:1245`,
which disables both fallback ladders — construction failure at `:1249` and admission
rejection at `:1352`. Every cell gets exactly one curve-family attempt with no recovery,
so a board the Svi or ConvexDense rung would have caught becomes a hard cell loss.

One hypothesis was **refuted** and is worth recording, because it is the intuitive one:
that a single config chosen from one date fails to generalise across dates. For these
three symbols it cannot be the cause — SPY is index-pinned without ever looking at a
board, and AAPL and NVDA both hit the compiled-in ticker seed table at confidence 0.95
with a never-populated `fit_context`, so board features never enter the routing decision
and a per-date re-derivation would produce identical configs. It does, however, become
live at production width: **37 of the 51 universe names are unseeded** and route on
date-varying board features.

A second latent trap was found and is **not** active in this run: a stored config with
`decision.curve.kind == LinearVariance` hard-fails every one of that symbol's cells at
`pricer_fitter.cpp:1027`. Cross-referencing the universe against the seed table
(`profile.cpp:250-276`), SPY is the only ETF-classified name present and it is already
the `--index` symbol — so the trap is latent. There is only one `--index` slot, so any
future universe carrying a second ETF would hit it, silently, as a bare count.

### The fix, and what it recovered

`dba34a6` makes pinning a choice instead of a hard-coded constant: `AutoConfigSpec` gains
a knob, defaulting to **not** pinning, with an opt-in CLI flag to restore the old
behaviour. The stored config still carries the selected policy family as the preferred
route — the fitter simply auto-routes, so its ladder stays alive. The change is scoped to
where the pin is *set*, deliberately not to `pricer_fitter.cpp`, which is shared with the
backtest and dispersion pipelines that this work cannot test.

Re-measured on a **fresh** database root over the identical range and symbols, so the
comparison is like-for-like rather than contaminated by surfaces the pinned path had
already stored:

| | pinned (before) | unpinned (after) |
|---|---|---|
| cells stored | 35 / 45 | **42 / 45** |
| cells failed | 10 | **3** |
| AAPL | 7 failed | 1 failed |
| NVDA | 1 failed | 0 failed |
| SPY | 2 failed | 2 failed |

Coverage 77.8% → **93.3%**. The ladder recovers 7 of the 10 lost cells, which is the
empirical justification for the change.

SPY's count is unchanged but its *dates* moved — 2026-07-07 recovered, 2026-07-15 newly
failed — because unpinning also changes SPY's numerics: the index leg now runs the dense
fit at the risk preset's `max_obs_per_slice` (60) rather than the stored `node_cap` (40).
That is the auto route's own value rather than a tuning choice, but it is a real change
to production output and is called out here so nobody reads it as noise.

**A correction, recorded rather than quietly edited.** The implementation reported an
independent justification for the flip — that the manifest does not round-trip a pinned
curve's `parametric` numerics, so pinning overwrote the risk preset's calibration with
defaults — and an earlier revision of this log repeated it. **That mechanism is wrong.**
`encode_symbol_record` / `decode_symbol_record` in `surface_db.cpp` round-trip roughly
thirty `parametric` numeric and enum fields, and `apply_fit_preset` writes exactly two
calib fields, both of which persist. A preset-derived config round-trips losslessly. Do
not go looking for a serializer bug; there isn't one.

The conclusion it was reaching for is nonetheless real, by a different and larger
mechanism. `pricer_fitter.cpp:1151-1159` applies the classified profile's `calib` **only
when `decision_` is populated**, and `decision_` is repopulated only on the unpinned
branch (`:1140-1148`). So a pinned cell fits with a default-constructed `CalibOpts` plus
the two fields `apply_risk_policy` pins, while an unpinned cell fits with the profile's
tuned calibration — `max_outer_iter` 4 → 50, `huber_k` 1.5 → 2.0, `residual_disable`
true → false, `residual_basis_kind` None → HingeQuad (`profile.cpp:58-71`). That is the
auto route working as designed, and it is the most likely reason coverage improved as
much as it did.

It also means **flipping this default changes fitted output, not merely runtime** — by
considerably more than the node_cap note above on its own suggests. Anyone comparing
surfaces built before and after this commit should expect numerical differences
everywhere, not just on the index leg.

### The 3 residual failures

```
failed_cell 2026-07-14 AAPL model=essvi        mask=2049 carry=ok     inversion=ok
failed_cell 2026-07-15 SPY  model=convex-dense mask=2064 carry=failed butterfly_slack=0.000000 slopes=-0.999966/-0.999966
failed_cell 2026-07-22 SPY  model=convex-dense mask=2064 carry=failed butterfly_slack=0.000107 slopes=-0.999893/-1.000000
```

Both SPY residuals are `carry=failed` together with a butterfly violation sitting *on*
the no-arbitrage boundary — slopes of −0.999966 and −1.000000. That is a carry-solve and
model-boundary problem, not a routing one, so the fallback ladder has nothing left to
offer. These are **not** chased here: the honest outcome is 3 named failures with their
reasons on record, and tuning admission thresholds to manufacture a clean number would be
the opposite of what this instrumentation was built for.

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

### Paid run — date extension to the last completed session

Re-ran the free preflight first, because Step 4 had just changed the on-disk state:

```
cells: total=867 to_pull=153 (over 3 sessions)
  2026-07-22 / 07-23 / 07-24: 51 syms est=$0.000000 each
ESTIMATE (remaining spend): $0.0000 = $0.00000000/sym-day x 153 cells (cap $90.00)
```

249 → 153 is exactly the 96 cells Step 4 wrote. The resume filter is now proven twice
against a tree it had just written itself — which is the code path IMP-3 protects.

```
$ python atx-vol/tools/pull_opra_hive.py \
      --universe atx-vol/data/universe/spy_top50_2026-01-01.csv \
      --start 2026-07-01 --end 2026-07-24 --out C:/atx-data/opra-hive \
      --cap 90 --env-file C:/atx/.env

Authorized estimate $0.0000 within cap $90.00. Symbols kept: 51 (dropped 0).
  [1/3] 2026-07-22 pulled: 51 boards (running_spend=$0.0000)
DONE boards_written=153 dates_written=3 unmapped_rows=0 failed_sessions=0
ACTUAL SPEND (realized preflight of pulled cells): $0.0000
STEP5_PULL_EXIT=0
```

### Coverage now complete for 2026-07

```
$ ...same command --dry-run
cells: total=867 to_pull=0 (over 0 sessions)
ALL boards already on disk - nothing to pull, $0.00.
```

**867 of 867 cells on disk** — 51 symbols × 17 sessions, 2026-07-01 .. 2026-07-24. The
hive holds 140 date partitions and 470 MB overall.

**Realized spend: $0.0000. Cumulative: $0.0000 / $100.**

### A caveat on the spend figure

Every dollar number in this log — including "ACTUAL SPEND" — is derived from Databento's
free `metadata.get_cost` estimator, not from an invoice. The tool labels it "realized
preflight of pulled cells", which is honest: it re-prices the cells that were actually
pulled, but it is still the estimator pricing them. A control run against a known-good
date returned the same $0, which distinguishes genuine zero pricing from an empty query
match, so the figure is not vacuous. It is nonetheless **not** an independent
confirmation that this account was billed nothing. Reconcile against the Databento
invoice before treating $0.0000 as settled.

---

## Steps 6-7 — full production build + final report

## Step 6 — Full production build

Full 51-name universe (discovered from the hive, no `--symbols`), 2026-07-01..24,
`--index SPY --r 0.043`, into a **fresh** database root. Fresh on purpose: the smoke
database holds surfaces fitted by the pinned path, and mixing pinned and unpinned
provenance in one production artifact would be dishonest.

### An operational limit, found the hard way

The first attempt ran the whole month as one range and had to be killed: **6.5 GB
resident after 16 minutes with zero partitions written**, on a 15.7 GB machine with 2.2 GB
free. The loader holds every date's decoded tables until the parallel pass — a limitation
already on this branch's minors list, here quantified against real data at production
width for the first time.

Chunking is safe and yields an identical database: partitions are per-date and
independent, the build is cell-aware and resumable, config generation runs once on the
first chunk from the earliest board exactly as the full range would (later chunks report
`n_skipped_existing` and reuse them), and symbol discovery is a union across dates where
every session carries all 51 names.

**Operational guidance: a full-month, full-universe build does not fit in 16 GB. Chunk
it.** Chunks used here: 07-01..09 (6 sessions), 07-10..16 (5), 07-17..24 (6).

Measured later: peak memory is driven far more by symbol width and concurrent fit
workspaces than by date count — the 6-session chunk still reached 5.7 GB. If memory is
tight, lowering `--fit-workers` is the better lever than narrowing the range.

### Results

| chunk | sessions | cells ok / attempted | outcome |
|---|---|---|---|
| A 07-01..09 | 6 | 297 / 300 | exit 0 |
| B 07-10..16 | 5 | 247 / 250 | exit 0 |
| C 07-17..24 | 6 | — | **crashed** (below) |
| C re-run per date | 6 | 297 / 300 | exit 0 each |

Chunk A also reported `config.n_configured 50`, `n_disabled_failed 1`; chunk B reported
`n_configured 0`, `n_skipped_existing 51`, confirming **config selection converges at
production width** — a resume re-selects nothing.

### Chunk C crashed, and the bisect changed the conclusion

```
[10:02:20.833] [C] [column.hpp:235] CHECK failed: raw != nullptr
Illegal instruction     ./build/bin/step6-build.exe ... --from 2026-07-17 --to 2026-07-24
CHUNK_C_EXIT=132
```

Seventeen minutes of work, **zero partitions written** — the process died rather than
failing a cell, so nothing was salvaged. That is a categorically worse failure mode than a
failed fit, which costs exactly one surface.

The obvious reading — a malformed input or a poisonous board — is **wrong**. All six date
files probe structurally normal (51 row groups each, no zero-row groups, 141k-148k rows),
and **every one of the six dates succeeds when run individually**, 297/300 with no crash.
The distinguishing factor was memory: chunk C loaded six dates while a compiler was
running, with roughly 3 GB free. So this is an allocation failure surfacing as a hard
`CHECK` rather than as a clean error.

It is **not root-caused to a line** and is recorded as an open finding with its
reproduction conditions. What is certain is the shape: under memory pressure this pipeline
aborts the process instead of degrading, and a long chunk loses everything.

### Final production database

```
$ atx-vol-surface-db info --db C:/atx-data/surface-db/prod-2026-07
generation 69
symbols 51
symbols_enabled 50
partitions 17
partitions_missing 0
surfaces 841
bytes_on_disk 6631424
```

**841 surfaces of 850 attempted — 98.9% coverage** (867 cells minus BRK.B's 17), across
17 sessions and 6.4 MB. Every one of the 9 failures carries a captured reason.

Coverage at production width is *better* than the 3-symbol smoke's 93.3%, which is
consistent with the unpinned auto route suiting the 37 unseeded names that dominate the
universe.

The 9 failures split into two kinds, and the distinction is only visible because of the
instrumentation:

- **Marginal** — SPY 07-15 and 07-22 miss the butterfly bound by 0.000000 and 0.000107
  with slopes at −1.0, and AAPL 07-14 fails on `InvalidDomain`. These are the same cells
  the 3-symbol smoke failed, so they are symbol-intrinsic rather than universe-dependent.
- **Genuinely arbitrage-violating** — MCD 07-01, COST 07-08 and UNH 07-15 report 18-21
  butterfly violations *together with* 17-28 calendar violations. No curve family should
  fit these boards, and reporting them honestly is the correct outcome.

Before the instrumentation both classes were the same anonymous count.

### Step 6 gates

**Gate — re-run re-fits nothing: FAILS, and the cost is now measured.** Re-running
07-01..09 against the finished database:

```
config.n_skipped_existing 51
coverage.cells_to_fit             3
coverage.cells_refit            147
coverage.cells_already_present  150
coverage.dates_written            3
coverage.dates_skipped_complete   3
```

Three dates hold a failure; each is rewritten whole, dragging all 49 healthy siblings back
through the fitter. **3 useful retries cost 147 wasted re-fits — a 49× amplification, and
the multiplier is the universe width.**

**The plan's gate text, `cells_to_fit == 0`, is unachievable by design.** Failed fits
retry forever so that a transient failure stays recoverable; there is deliberately no
persisted known-failed state, and confirming this at the source found no channel by which
a prior failure could suppress a retry. The only way to satisfy the gate as written would
be to persist known-failed state, which the design explicitly rejects. **The gate is not
being redefined to make it pass.** The 3 retries are correct. The 147 re-fits are the
defect, and the honest invariant is `cells_refit == 0`.

**Gate — CLI verify: FAILED verdict, correctly, and the integrity result is strong.**

```
$ atx-vol-surface-db verify --db C:/atx-data/surface-db/prod-2026-07 --min-cells 800
partitions 17 / partitions_in_db 17 / symbols 50
cells_checked 850
cells_ok        841
cells_unmappable  9
cells_non_finite  0
cells_checksum    0
failures_reported 9 / failures_elided 0
fail 2026-07-01 MCD   fail 2026-07-08 COST  fail 2026-07-09 PG
fail 2026-07-14 AAPL  fail 2026-07-15 SPY   fail 2026-07-15 UNH
fail 2026-07-20 KO    fail 2026-07-22 SPY   fail 2026-07-23 JPM
verdict FAILED          ($? = 1)
```

Verify reaches the same nine cells from the **database alone**, without reading the build
reports — an independent confirmation rather than an echo. And the two integrity counters
are the ones that matter: `cells_checksum 0` means every one of the 841 stored surfaces
passes its payload CRC, and `cells_non_finite 0` means every one evaluates finite at the
money. The verdict is FAILED because nine requested cells are genuinely absent, which is
what a truthful verdict should say.

**Gate — CLI query: PASSES, with economically coherent values.**

```
$ atx-vol-surface-db query --db ... --key 2026-07-24 --tenor 0.0833 --symbol <S> --strike <K>
SPY   K=740  iv 0.1526  total_variance 0.0019  forward 741.15  n_slices 33
NVDA  K=207  iv 0.3855  total_variance 0.0124  forward 206.76  n_slices 19
JPM   K=354  iv 0.2225  total_variance 0.0041  forward 353.55  n_slices 15
```

Each strike is at that name's own one-month forward. The result is checked as economics,
not just as a non-crash: **SPY 15.3% < JPM 22.3% < NVDA 38.6%** is the ordering an options
desk would expect — broad index below a large bank below a high-beta semiconductor — and
each forward matches the name's price level. A first attempt queried all three at K=100,
which returned implied vols of 2.13 / 0.83 / 1.34; that is not a defect but the surface
honestly extrapolating hundreds of points into the wing, and it is recorded here because
a reader who saw only those numbers might conclude otherwise.

### Step 6 verdict

| acceptance item | result |
|---|---|
| partitions, symbols, surfaces, bytes recorded | PASS — 17 / 51 / 841 / 6.4 MB |
| coverage table | PASS — 98.9%, every failure with a reason |
| wall time | PASS — ~24 min per 6-session chunk at 12 fit workers |
| cumulative spend | PASS — $0.0000 |
| re-run proves fits-zero | **FAIL** — unachievable by design; 49× amplification measured |
| database integrity | PASS — 0 checksum, 0 non-finite over 841 surfaces |
| query a cell | PASS — values economically coherent |


## Step 7 — Final report

<!-- STEP7 -->

**Cumulative realized spend for this run: $0.0000 / $100.**
