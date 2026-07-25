# `atx-vol-surface-db-build` — the production surface-database build CLI

Point it at an OPRA hive-v2 tree and a `SurfaceDb` root; it create-or-opens the
db, loads the date window, auto-generates the per-symbol fit configs, and
cell-aware-streaming-populates every `(symbol, date)` vol surface. One call over
`build_surface_db` (`atx/vol/surface_db_build.hpp`) wrapped in a hand-rolled arg
loop (`tools/surface_db_build_main.cpp`).

It is **fully resumable at every stage**: re-running over an unchanged hive fits
**zero** surfaces and spends nothing. See "Resume semantics" below.

Once a database is built, its companion tool **`atx-vol-surface-db`** inspects
and verifies it entirely from the command line — see
[Managing and verifying a built database](#atx-vol-surface-db--managing-and-verifying-a-built-database).
**No Python is involved in verification.**

## OPRA hive v2 — the on-disk layout it reads

```
<hive-root>/date=YYYY-MM-DD/data.parquet     # ALL symbols for that session
```

- True hive partitioning (`date=` key), so DuckDB / `pyarrow.dataset` also read
  the tree natively with partition-column inference.
- **Exactly one parquet file per session**, holding every underlying's rows. The
  date file is written atomically (tmp + rename) and is the **unit of resume**.
- Schema: 8 columns, unchanged from the corpus format —
  `ts, underlying, symbol, instrument_id, bid_px, ask_px, bid_sz, ask_sz`.
  Prices are `int64` 1e-9 fixed-point; an unset side is `INT64_MIN`. The `date`
  partition value lives in the directory name, not as an in-file column.
- Rows are sorted by `underlying`, then `symbol`, so `underlying` column
  statistics support predicate pushdown for future selective readers. The C++
  loader does **one materialized read per date file** and splits by `underlying`
  in memory (one IO pass per date regardless of universe size); per-symbol
  row-group pruning is a documented future optimization, not built now.
- Snapshot minute is the fixed **19:55:00Z** hive convention (uniform with the
  existing corpus; the DST rationale is documented in the pull tool).

This replaces the old per-symbol tree `<root>/<symbol>/<date>.parquet` (one file
per `(symbol, date)`). To convert an existing per-symbol tree, see
[Sibling tools](#sibling-tools).

## Build

The tool target is **gated behind the cmake cache flag `ATX_BUILD_EXAMPLES`,
which is OFF by default** — the plain `configure` verb
(`pwsh scripts/atx-build.ps1 configure`, i.e. `cmake --preset dev`) builds the
library and tests but **omits** this CLI. Enable the flag explicitly at configure
time, then build the target:

```bash
# Configure with the tool enabled. The `configure` verb does not forward extra
# -D flags, so pass the flag through the wrapper's raw cmake path:
pwsh scripts/atx-build.ps1 --preset dev -DATX_BUILD_EXAMPLES=ON
# (equivalently, straight cmake in the MSVC dev env: cmake --preset dev -DATX_BUILD_EXAMPLES=ON)

# Build just the CLI:
pwsh scripts/atx-build.ps1 build atx-vol-surface-db-build
# -> build/bin/atx-vol-surface-db-build(.exe)
```

The exact cache flag is **`-DATX_BUILD_EXAMPLES=ON`**. It only ADDS the
example/tool targets (it does not change the library or tests). If you forget it,
the build fails with `ninja: error: unknown target 'atx-vol-surface-db-build'` —
that means the current build dir was configured without the flag; re-run the
configure line above.

## Usage

```
atx-vol-surface-db-build --db <root> --hive <root>
    --from YYYY-MM-DD --to YYYY-MM-DD
    [--symbols A,B,C] [--index SPY] [--preset populate] [--r 0.045]
    [--deep-selection] [--fit-workers N] [--report out.csv]
```

| Flag | Required | Meaning |
| --- | --- | --- |
| `--db <root>` | yes | `SurfaceDb` root. Created if absent, else **opened/resumed**. |
| `--hive <root>` | yes | OPRA hive-v2 root holding `date=<YYYY-MM-DD>/data.parquet`. |
| `--from` / `--to` | yes | Inclusive date window (every calendar date in range is enumerated). |
| `--symbols A,B,C` | no | CSV universe. **Omit (or empty) to discover** every underlying present in the window. Surrounding whitespace per field is trimmed. |
| `--index SPY` | no | Designated index leg — pinned to the dense index recipe (bypasses per-board selection), for both config generation and the populate. |
| `--preset NAME` | no | `fast` \| `accurate` \| `robust` \| `hft` \| `populate`. Default `populate`. Drives both the manifest seeding and the populate fit tier. |
| `--r RATE` | no | Flat continuously-compounded carry rate (`OpraHiveSpec.r`). Default **`0.0`**. **Must match the rate the hive's quotes were priced under** — read [Interest rate / carry](#interest-rate--carry--the-single-most-likely-way-a-build-produces-nothing) before every run. Must be a finite number consuming the whole token; `abc`, `0.03x`, `nan`, `inf` and a missing value are all **exit 2**, never a silent `0.0`. Negative rates are accepted. |
| `--deep-selection` | no | Additionally run the full held-out `select_curve` OOS search per symbol and pin its winner (falls back to the fit-policy decision when the selector has no scorable holdout). |
| `--fit-workers N` | no | Outer fit fan-out. `0` = auto (honors `ATX_VOL_FIT_WORKERS`); `1` = serial. |
| `--report out.csv` | no | Also write the two-section CSV report to this path. |

**Discover-all vs explicit `--symbols`.** With an explicit list, exactly those
underliers are loaded for every date (a date whose file lacks a requested symbol
is a visible coverage hole — counted in `n_coverage_holes`, never a silent gap).
With no `--symbols`, the effective universe is the sorted distinct **union** of
`underlying` across every readable date in range; every date then spans the full
union, so the ingest grid is rectangular (date × union) with visible holes where
a symbol is absent from a given date.

Real hives have **non-uniform** per-date coverage (names list, delist, or simply
were not pulled that day), so a discover-all build over a wide universe reports a
**large `n_coverage_holes` and that is healthy** — it is the sparseness of the
grid, not a data defect. `n_load_errors` is the counter that means something is
wrong. The two are classified structurally by the loader (a hole is a present,
readable date file that does not carry that symbol), never guessed from an error
code, so real corruption can never hide in hole noise.

### Interest rate / carry — the single most likely way a build produces nothing

**`--r` sets the flat continuously-compounded carry rate, and it defaults to
`0.0`.** The default is only correct for a hive whose quotes were priced at
**zero** carry; with it, the implied forward is the spot.

**Pass `--r` on every real run.** If the hive's quotes embed a non-zero
funding/borrow rate (any real OPRA data does), leaving `--r` at `0.0` makes every
put-call-parity forward wrong and **every full fit fails, identically**. The
per-symbol config classification is tolerant enough to pass anyway, so the
failure shows up in the coverage counters, not in stage 1:

- `coverage.cells_ok` is **0** (or far below `cells_to_fit`),
- `coverage.cells_failed` carries the whole universe, with each
  `symbol.<S> ... ok=0 failed=N` row confirming it, and
- **no partition is written at all** — the database ends up empty.

**This is no longer a silent green exit.** A build that scheduled work and fitted
**nothing** (`cells_to_fit > 0` and `cells_ok == 0`) now exits **3** and prints a
diagnostic on stderr naming the carry rate it used:

```
$ atx-vol-surface-db-build --db /db --hive /hive --from 2026-07-01 --to 2026-07-06
... (the full report still prints to stdout, and --report is still written) ...
atx-vol-surface-db-build: TOTAL FIT FAILURE: 9 cells scheduled, 0 fitted (9 failed).
  Most likely cause: the carry rate does not match the hive. This build used --r 0.
  If the hive's quotes embed a non-zero funding/borrow rate, every put-call-parity
  forward is wrong and every fit fails identically. Re-run with the matching --r <rate>.
$ echo $?
3
```

The decision lives in the library as
`is_total_fit_failure(const SurfaceDbBuildReport&)`
(`atx/vol/surface_db_build.hpp`), unit-tested in the `SurfaceDbTotalFitFailure`
suite — the CLI only maps it to an exit code.

**It is deliberately narrow, and the two neighbouring shapes stay green:**

- **Partial** failure (`cells_ok > 0` alongside some `cells_failed`) is **normal
  production output** — real hives carry unfittable boards. Exit `0`.
- **Nothing to do** (`cells_to_fit == 0`) is the **resume** path over an already
  complete database, and the un-pulled empty window. `cells_ok` is legitimately
  `0` because nothing was scheduled. Exit `0` — the build's convergence guarantee
  ("a re-run fits zero") depends on it.

So exit 3 answers exactly one question — "did this run get anything at all?" — and
**partial coverage is still your job to inspect.**

**Operator checklist:** a green exit no longer hides a *totally* dead build, but
it still does not mean full coverage. After any build, **inspect `cells_ok` vs
`cells_failed` and the per-symbol rows** (or the `--report` CSV's section 2), then
run `atx-vol-surface-db verify --db <root> --min-cells <expected>` (below), which
turns "the database is the size and shape I expected, and every cell evaluates"
into a single exit code.

The finer fix for hives with a real term structure (rather than one flat rate) is
the per-cell **market-inputs** path (`OpraHiveSpec.market_inputs` /
`yc_pillar_t`/`yc_pillar_r`), which the CLI does **not** expose — `--r` is the
single flat rate only.

### Examples

```bash
# Explicit 3-symbol build over a July window, SPY as the index leg, CSV report:
atx-vol-surface-db-build \
  --db   C:/atx-data/surfdb-2026-07 \
  --hive C:/atx-data/opra-hive \
  --from 2026-07-01 --to 2026-07-31 \
  --symbols SPY,AAPL,MSFT --index SPY --r 0.0425 \
  --report C:/atx-data/surfdb-2026-07/build_report.csv

# Discover the whole universe present in the window (no --symbols):
atx-vol-surface-db-build \
  --db C:/atx-data/surfdb-2026-07 --hive C:/atx-data/opra-hive \
  --from 2026-07-01 --to 2026-07-31 --r 0.0425

# Deep per-symbol curve selection, serial fit (reproducible), robust tier:
atx-vol-surface-db-build \
  --db /db --hive /hive --from 2026-07-01 --to 2026-07-31 --r 0.0425 \
  --preset robust --deep-selection --fit-workers 1
```

**Every example passes `--r`.** Omitting it is a build at zero carry — see
[Interest rate / carry](#interest-rate--carry--the-single-most-likely-way-a-build-produces-nothing).

### Output and exit codes

Every scalar report field prints one-per-line to **stdout** as `key value`
(mirroring the CSV `key,value` section), followed by `config.failed_symbols` and
one `symbol.<S> attempted=.. ok=.. failed=.. disabled=..` line per symbol. With
`--report`, the same data is written as CSV: a `key,value` scalar section then a
`symbol,n_attempted,n_ok,n_failed,n_disabled` row per symbol.

| Exit | When |
| --- | --- |
| `0` | Build succeeded — including **partial** coverage, a no-op **resume**, and a graceful empty-window no-op. |
| `1` | A build error — malformed hive spec, or a db config/write failure. Message on stderr, **no report printed**. |
| `2` | A usage error — unknown flag, a missing required flag, an unknown `--preset`, or a malformed `--r`. Usage on stderr. |
| `3` | **Total fit failure** — the build ran to completion but fitted **nothing** (`cells_to_fit > 0` and `cells_ok == 0`). The full report still prints and `--report` is still written; a diagnostic naming the carry rate goes to stderr. See [Interest rate / carry](#interest-rate--carry--the-single-most-likely-way-a-build-produces-nothing). |

`3` is deliberately distinct from `1`: `1` means *atx or the database broke* (no
report to read), `3` means *the tool worked and your inputs produced nothing* —
almost always a `--r` mismatch. A script can branch on that.

Note: a single unloadable or unselectable board never aborts the build — it is
tallied (and, for config, stored **disabled** = fail-closed) and the call still
succeeds. Partial failure is **not** exit 3.

## Report fields

**Config generation** (`generate_symbol_configs`, stage 1). The three
disposition counters partition the distinct symbols seen:
`n_configured + n_skipped_existing + n_disabled_failed == n_symbols`.

| Field | Meaning |
| --- | --- |
| `config.n_symbols` | Distinct symbols across the loaded boards. |
| `config.n_configured` | Freshly configured (or overwritten), enabled. |
| `config.n_skipped_existing` | Already in the manifest, left untouched (idempotent resume). |
| `config.n_disabled_failed` | Selection failed → stored **disabled** (never silently served). |
| `config.failed_symbols` | The disabled names (sorted). |

**Populate coverage** (`populate_universe_streaming`, stage 2). Cells are
`(symbol, date)` pairs; the counters describe what the cell-aware resume did.

| Field | Meaning |
| --- | --- |
| `coverage.cells_loaded` | Boards handed to the populate (available parquet cells). |
| `coverage.cells_to_fit` | NEW `(symbol, date)` cells scheduled this run. A **config-disabled** cell is never counted — it can never be added, so counting it would keep its date pending forever. |
| `coverage.cells_refit` | Already-present cells re-fit by a same-date rewrite. |
| `coverage.cells_already_present` | Skipped: symbol already in its date partition. |
| `coverage.cells_ok` / `cells_failed` | Fit outcomes over the (re)written dates. |
| `coverage.dates_total` | Distinct dates among the loaded boards. |
| `coverage.dates_written` | Dates that needed a (re)write this run. |
| `coverage.dates_skipped_complete` | Dates with **nothing left to add**: every loaded cell is either already present or config-disabled. |
| `coverage.dates_skipped_would_drop` | Dates skipped to avoid dropping an existing symbol (safety guard). |
| `symbol.<S> ...` | Per-symbol populate stats over the written dates. |

**The cell counters do not reconcile against `cells_loaded`** — do not read them as
a partition. A config-disabled cell that is absent from its partition on a
skipped-complete date appears in none of `cells_to_fit`, `cells_refit` or
`cells_already_present`, and the per-symbol `disabled=` column only covers the
dates this run actually wrote. `cells_loaded` is the input count.

**Hive ingest** — the first two counters describe distinct **dates**, the last two
describe **cells**:

| Field | Meaning |
| --- | --- |
| `n_dates_loaded` | Distinct dates that produced at least one board. |
| `n_dates_missing` | Distinct in-range dates that produced **none** (a fully absent OR fully unreadable date). The window is enumerated as **calendar** days, so every weekend and market holiday in range is counted missing — a July window always shows ~9. |
| `n_load_errors` | **Cell** count of real ingest **defects**: a present file that is unreadable/unparseable, has the wrong schema, or whose market inputs quarantined the cell. Never reaches the fit. **This is the counter to alarm on.** |
| `n_coverage_holes` | **Cell** count of **coverage holes**: the date file is present and readable, the symbol is simply not in it. Expected and healthy on a sparse universe; never reaches the fit. |

The two cell counters exhaust the loader's erroring cells
(`n_load_errors + n_coverage_holes == OpraBatchResult::n_error`) and are split by
the loader **structurally** — a hole is decided from the date file's own
distinct-`underlying` set, not from an error code (a hole and a wrong-schema file
both surface `InvalidArgument`, so a code test would report a corrupt date as
holes and let real corruption hide).

**Double-count, by design.** A date whose file is present but fully corrupt is
counted in **both** `n_dates_missing` (it produced no boards, so it is not a
loaded date) **and** `n_load_errors` (each of its cells is a present-but-
unparseable file). This is deliberate: `n_dates_missing` answers "how many
in-range sessions have no usable surfaces?" and `n_load_errors` answers "how many
present files failed to parse?" — a corrupt date legitimately answers yes to
both. A merely absent date (no file at all) bumps only `n_dates_missing`.

## Resume semantics

The build is idempotent and resumable because each stage independently no-ops on
work already done:

1. **Create-or-open db.** A manifest at `--db` root → open (resume); absent →
   create. A re-run reuses the same root.
2. **Config idempotence.** A symbol already in the manifest is left **untouched**
   (`n_skipped_existing`), so a re-run never clobbers an operator override. (The
   library exposes an `overwrite_existing` escape hatch; the CLI does not — it is
   deliberately non-destructive.)
3. **Cell-aware populate resume.** A partition (= date) is (re)written only when a
   loaded board adds a symbol the partition does not already carry. So as the
   OPRA pull dribbles in new `(symbol, date)` cells, only the new work is fit; a
   re-run over unchanged data fits **zero** (`cells_to_fit == 0`,
   `dates_written == 0`, `dates_skipped_complete == dates_total`) **once every
   loaded cell has either fitted successfully or been config-disabled** — a
   disabled cell is excluded from the pending tally, since it can never be added.
   A **grown** hive (new dates, or new symbols on existing dates) fits only the
   delta.
   - A cell that **fails to fit** is *not* suppressed: there is no persisted
     known-failed state, so it is retried on every run, which keeps its date in
     the rewrite set and re-fits that date's siblings. That is the deliberate
     cost of giving a transient failure another chance — a name that fails
     permanently should be disabled in the manifest to converge.
   - Safety guard: a date is **skipped, never rewritten**, if its partition
     already holds a symbol NOT present in this run's loaded set — a
     whole-partition rewrite would drop it (`dates_skipped_would_drop`). This
     cannot happen on the intended grow-only workflow but guards a
     narrower-symbol re-run from data loss.

An **empty window** (un-pulled days) is a graceful success: all-zero coverage,
the db still created. Absent dates in range are non-fatal (`n_dates_missing`).

The pull unit upstream is `(date, symbol-set)`: a present date file is read for
its on-disk symbol set (footer statistics, no data scan) and only the missing
symbols are pulled, then the date file is rewritten as the union — so the hive
never re-bills for data already on disk. See the pull tool below.

## Scale posture

Target shape: thousands of symbols × ~250 sessions/yr → ~1M surfaces
(≈ 4k symbols × 250 dates).

- **Per-date partition file**: 4k surfaces × ~2–6 KB ≈ 10–25 MB — comfortably
  inside `SurfaceArchiveV2` mmap + the LRU partition-view cache (16 resident
  partitions by default; O(1) `map_surface` probe per query).
- **Manifest** (`manifest.atxdb`): 4k × 256 B symbol records + 250 × 128 B
  partition records/yr ≈ ~1 MB, rewritten atomically — fine at this scale.
- **Asserted limits**: partition key ≤ 32 chars, symbol ≤ 32 chars.
- **Out of scope**: multi-year growth (manifest > ~100 MB or partitions
  > ~100k). The scaling seam is **one db root per year** (documented, not built —
  no sharding, YAGNI).

**Build-time peak memory scales with the date-range length in discovery mode.**
When `--symbols` is omitted, the hive loader runs a serial pre-pass that
materializes each date's table to compute the discovered union, and it **holds
each date's table in memory from that pre-pass until the date's panel pass
completes**. So the loader's peak RSS grows with the number of dates in the
requested window, not just with a single date. The downstream **populate** stays
`O(dates in flight)` (per-date fit → serialize → release), but the loader's
discovery retention is the memory ceiling for a wide window. For very long
ranges, build in date chunks (the cell-aware resume makes chunked runs stitch
losslessly) or pass an explicit `--symbols` list (which skips the discovery
pre-pass).

## `atx-vol-surface-db` — managing and verifying a built database

**Verifying a built database requires no Python.** Everything below runs from the
command line against a `SurfaceDb` root: what it contains, how each symbol is
configured, what one cell evaluates to, and whether every cell is healthy. The
pybind11 wrapper is not needed, not installed, and not on the verification path
— the old production run-plan step "query check via python binding
(`map_surface` one cell)" is replaced by `atx-vol-surface-db query` and
`atx-vol-surface-db verify`.

The tool is a **thin shell**: every subcommand parses flags, calls exactly one
function in `atx/vol/surface_db_admin.hpp`, and prints. All logic — and the test
gate (`SurfaceDbAdmin`, `atx-vol/tests/surface_db_admin_test.cpp`) — lives in
that library, so a service or notebook consumes the same structs without parsing
this text.

### Build

Same `ATX_BUILD_EXAMPLES` gate as `atx-vol-surface-db-build` (see
[Build](#build) above — if that CLI is in your build dir, this one is too):

```bash
powershell scripts/atx-build.ps1 build atx-vol-surface-db
# -> build/bin/atx-vol-surface-db(.exe)
```

### Subcommands

```
atx-vol-surface-db <subcommand> --db <root> [flags]
```

`--db <root>` is required by every subcommand.

| Subcommand | Flags | Answers |
| --- | --- | --- |
| `info` | — | What is in this database? Generation, symbol/partition counts, surface count, bytes, then one `partition` line each. |
| `partitions` | — | One `partition` line per partition (the `info` tail on its own, for piping). |
| `partitions` | `--key KEY` | What does THIS partition actually hold? Reads the `.atxvsa` directory, not the manifest. |
| `symbols` | — | One `symbol` line per configured symbol. |
| `config` | `--symbol SYM` | One symbol's full stored fit config + provenance. |
| `query` | `--key KEY --symbol SYM --strike K --tenor T` | What does this cell evaluate to? iv / total variance / forward / uid / slices. |
| `verify` | `[--from KEY] [--to KEY] [--symbols A,B,C] [--include-disabled] [--probe-tenor T] [--max-failures N] [--min-cells N]` | Is the whole thing healthy? |

`query` and `verify` both go through **`SurfaceDb::map_surface`** — the zero-copy
path production readers use (and the one the retired Python check made) — so a
green result is evidence about the path that actually serves.

### Output format

Line-oriented and stable for scripting; **no JSON**. Two shapes only:

- **Scalars** print as `key value` (one per line), mirroring
  `atx-vol-surface-db-build`'s stdout.
- **Repeated records** print as `<record> <id> field=value field=value ...`.
  Record types: `partition`, `surface`, `symbol`, `fail`.

Fields never move and record lines never wrap, so
`grep '^fail '`, `awk '$1=="partition"'`, and
`... | grep '^cells_ok ' | cut -d' ' -f2` are all stable.

**`info`**

```
root <path>
generation <u64>            # manifest generation (++ on every rewrite)
symbols <n>                 # configured symbols
symbols_enabled <n>         # of those, not fail-closed-disabled
partitions <n>
partitions_missing <n>      # manifest entries whose file is NOT on disk
surfaces <u64>              # sum of per-partition surface counts
manifest_bytes <u64>        # sum of the sizes the manifest recorded at write time
bytes_on_disk <u64>         # sum of the sizes the files have NOW
partition <KEY> surfaces=<n> manifest_bytes=<n> bytes_on_disk=<n> present=<0|1> created_ts_ns=<n>
```

The two byte figures are **not** redundant. `manifest_bytes` is what the manifest
recorded when the partition was written; `bytes_on_disk` is what is there now. A
mismatch, or `present=0` / a non-zero `partitions_missing`, means the directory
was edited behind the manifest's back — exactly the corruption an inspector
exists to find.

**`partitions --key KEY`** — read from the archive directory:

```
partition <KEY> manifest_surfaces=<n> archive_surfaces=<n> manifest_bytes=<n> bytes_on_disk=<n>
surface <SYM> uid=<n> slices=<n> bytes=<n>
```

`manifest_surfaces` disagreeing with `archive_surfaces` means the manifest and
the file have drifted apart.

**`symbols`**

```
symbol <SYM> enabled=<0|1> preset=<name> pin_curve=<0|1> curve=<kind> provenance=<0|1>
```

`preset` uses the same vocabulary as the build CLI's `--preset`
(`fast|accurate|robust|hft|populate`), so a listing feeds straight back into a
rebuild. `curve` is only meaningful when `pin_curve=1`.

**`config --symbol SYM`** — `key value` scalars: `symbol`, `enabled`, `preset`,
`pin_curve`, `curve`, `band_k`, `policy.*`, `provenance` (0/1) and, when
provenance is present, `provenance.purpose`, `.quality_mode`, `.state`,
`.admitted`, `.validation_failures`, `.source_generation`, `.served_generation`,
`.legacy_format`.

**`query`** — `key value` scalars: `key`, `symbol`, `strike`, `tenor`, `iv`,
`total_variance`, `forward`, `uid`, `n_slices`. Doubles print at `%.17g`
(round-trip exact).

**`verify`**

```
partitions <n>              # partitions in range (the walk's rows)
symbols <n>                 # symbols in the walk (the walk's columns)
cells_checked <n>           # == cells_ok + cells_unmappable + cells_non_finite
cells_ok <n>
cells_unmappable <n>        # map_surface failed (file gone/corrupt, or symbol absent)
cells_non_finite <n>        # mapped, but the ATM probe produced no usable number
failures_reported <n>       # fail lines below (capped by --max-failures)
failures_elided <n>         # faults NOT listed -- truncation is never silent
fail <KEY> <SYM> kind=<unmappable|non_finite> detail=<message>
min_cells <n>
verdict <ok|FAILED>
```

Each cell is **mapped and then evaluated**: `K` = that surface's own
`forward_at(probe_tenor)`, `T` = `probe_tenor` (default 30/365). Mapping alone
only proves the bytes parse; the ATM evaluation proves the surface produces a
finite, positive vol.

### `verify` flags

| Flag | Default | Meaning |
| --- | --- | --- |
| `--from KEY` / `--to KEY` | unbounded | Inclusive partition-key range, compared lexicographically on the canonical (upper-cased) key. ISO dates sort correctly, so `--from 2026-07-01 --to 2026-07-31` is a July restriction. |
| `--symbols A,B,C` | manifest symbol table | Restrict the columns. Whitespace per field is trimmed, same rule as the build CLI. |
| `--include-disabled` | off | Include fail-closed **disabled** symbols. By default they are skipped: a disabled symbol is never populated into any partition, so checking it would report a missing cell on every date of every healthy database. Turning this on is how you *prove* a disabled name is genuinely absent. |
| `--probe-tenor T` | `30/365` | Tenor for the per-cell ATM evaluation. Must be finite and > 0. |
| `--max-failures N` | `32` | Cap on `fail` lines. Overflow is counted in `failures_elided`, never dropped silently, and the `cells_*` totals stay exact. `0` prints no detail at all and elides everything. |
| `--min-cells N` | `0` | Fail when fewer than `N` cells were checked. **Read the trap below.** |

### The empty-database trap — why `--min-cells` exists

An empty database has no broken cell, so `verify` correctly reports
`verdict ok`. That is honest and it is also a **silent pass**: a build whose
every fit failed (the carry-mismatch trap documented above — `cells_ok 0`) writes
*no partition at all*, and the resulting database then passes an unguarded
`verify`:

```
$ atx-vol-surface-db verify --db /db
partitions 0
cells_checked 0
...
verdict ok            # nothing was checked, so nothing failed
```

In a script, always assert the expected size:

```bash
atx-vol-surface-db verify --db /db --min-cells 9   # -> verdict FAILED, exit 1
```

`--min-cells` is a CLI-side floor on `cells_checked`; the library
(`verify_db`) deliberately keeps vacuous-ok so a legitimately fresh root is not
an error.

The build CLI now catches that *specific* case at its own exit (a totally failed
build exits `3`, above), so the two guards are complementary rather than
redundant: exit 3 catches "this **run** produced nothing", `--min-cells` catches
"this **database** is smaller than I expected" — including a database that never
got built, was built over the wrong window, or lost partitions to a later
accident. Keep `--min-cells` in every script.

### Exit codes

| Exit | When |
| --- | --- |
| `0` | Succeeded. For `verify`: every checked cell passed **and** `cells_checked >= --min-cells`. |
| `1` | A runtime failure (message on **stderr**, no `verdict` line) — db won't open, unknown partition/symbol, unreadable partition file. **Or** `verify` found failing cells / too few cells (`verdict FAILED` on **stdout**). |
| `2` | A usage error — unknown subcommand, unknown flag, or a missing required flag. Usage on stderr. |

The `verdict` line disambiguates the two meanings of exit 1: a health failure
always prints one, a runtime failure never does.

### Worked session

```bash
$ atx-vol-surface-db info --db /db
root /db
generation 7
symbols 3
symbols_enabled 3
partitions 3
partitions_missing 0
surfaces 9
manifest_bytes 12288
bytes_on_disk 12288
partition 2026-07-01 surfaces=3 manifest_bytes=4096 bytes_on_disk=4096 present=1 created_ts_ns=...

$ atx-vol-surface-db partitions --db /db --key 2026-07-01
partition 2026-07-01 manifest_surfaces=3 archive_surfaces=3 manifest_bytes=4096 bytes_on_disk=4096
surface AAA uid=3061902210 slices=2 bytes=544

$ atx-vol-surface-db query --db /db --key 2026-07-01 --symbol AAA --strike 100 --tenor 0.0821917808
iv 0.24600158507884576
total_variance 0.0049739819064085963
forward 100.26404035644119

# Health gate, sized: nine cells, all of them healthy.
$ atx-vol-surface-db verify --db /db --min-cells 9 && echo HEALTHY
cells_checked 9
cells_ok 9
verdict ok
HEALTHY

# After `rm /db/partitions/2026-07-02.atxvsa` -- every cell of that date is named:
$ atx-vol-surface-db verify --db /db
cells_checked 9
cells_ok 6
cells_unmappable 3
fail 2026-07-02 AAA kind=unmappable detail=NotFound: SurfaceArchiveV2::open_file: file not found
fail 2026-07-02 BBB kind=unmappable detail=NotFound: SurfaceArchiveV2::open_file: file not found
fail 2026-07-02 CCC kind=unmappable detail=NotFound: SurfaceArchiveV2::open_file: file not found
verdict FAILED
$ echo $?
1
```

**Operator checklist after a build:** `atx-vol-surface-db verify --db <root>
--min-cells <expected>`, then read `info` for `partitions_missing` and the
`manifest_bytes` vs `bytes_on_disk` agreement. That, plus the build's
`cells_ok` / `cells_failed` check above, is the whole acceptance path — no
Python.

## Sibling tools

The build CLI only **reads** a hive; two Python tools produce and maintain it:

- **`atx-vol/tools/migrate_opra_hive.py`** — converts the old
  `<root>/<symbol>/<date>.parquet` tree into the new `date=*/data.parquet` hive.
  Pure local IO ($0), atomic per date, idempotent (complete date files skipped),
  verifies row counts and schema equality per date, writes a migration manifest
  CSV.
- **`atx-vol/tools/pull_opra_hive.py`** — the v2 Databento pull targeting the new
  layout. Free `get_cost` preflight for missing cells only, hard `--cap` with
  degrade-to-top-N-by-weight, `--dry-run`, DBN cache, atomic writes, spend log.
  One `get_range` per date over the union of missing parents; output is the
  merged date file (per the resume/merge rule above).
