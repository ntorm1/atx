# Backtest database

`BacktestDb` stores reusable, projection-backed strategy histories in a
checksummed binary database. It is intended for fixed strategy recipes evaluated
over many underliers, such as:

- one long or short 40-delta call and one 40-delta put;
- projected three calendar months from each entry close;
- held to theoretical expiry;
- delta hedged to zero at every stored daily close.

The contracts are theoretical surface coordinates created by
`project_option_contract`. Listed OPRA contracts, expiries, strikes, and quotes
are not used to construct the portfolio. OPRA-derived fitted surfaces may be the
input to `SurfaceDb`, and listed options can be used separately for validation.

## Files and publication model

```text
<root>/
  manifest.atxbtdb
  partitions/
    <template-id-symbol-and-generation-key>.atxrun
```

The manifest and each series partition use `RunArchive` framing. They therefore
inherit its schema stamp, layered CRC-32C checks, mmap reader, and atomic
temporary-file publication.

The manifest contains:

- generation and database schema metadata;
- complete serialized strategy templates and economic fingerprints;
- the sorted `(template_id, symbol)` series catalog and partition identities.

Each series partition contains:

- the full `BacktestResult`;
- source `SurfaceDb` partition dates and content identities;
- the live option lots, delta-hedge share ledger, cash, NAV, financing
  accumulator, next lot ID, global step index, and next cohort counter.

The database is single-writer/many-reader across processes. One `BacktestDb`
instance serializes its own mutations. A writer publishes a series partition
before publishing the next manifest generation, so a crash can leave an
unindexed orphan but cannot make a manifest point to a partially written file.
Long-lived readers call `refresh()` to observe a newer manifest.

Partitions are immutable and generation-versioned. A successful replacement
leaves the retired version unindexed so a reader holding the older manifest
snapshot remains valid; a failed manifest publication likewise leaves the
previous indexed version untouched. The library intentionally does not guess
when those old readers have exited. Retired and crash-orphan partitions may be
removed only by an explicit offline vacuum that retains every filename in the
current manifest.

## Strategy templates

`BacktestStrategyTemplate` is the cookie-cutter definition. Its economic
fingerprint covers:

- every projected maturity, strike, side, quantity, and multiplier;
- entry cadence and holding rule;
- daily hedge rule;
- projection accuracy and query route;
- execution frictions;
- theoretical settlement rule;
- a schema/engine salt.

The catalog ID and display name are metadata and are deliberately excluded from
the economic fingerprint. Re-registering an ID with unchanged economics is a
no-op. Reusing an ID for different economics fails closed.

The standard example is created with:

```cpp
auto strategy =
    atx::vol::make_40_delta_3_calendar_month_strangle_template(-1.0, 1u);
```

`-1.0` is short, `+1.0` is long, and an entry cadence of `1` starts one new
cohort on every source row. The factory supplies the versioned catalog ID
`strangle-40d-3cm-hold-expiry-daily-delta-v1`. The production CLI retains that
version and adds the position and cadence, for example
`short-strangle-40d-3cm-hold-expiry-daily-delta-v1-entry-1`. Economically
different long/short and cadence variants therefore cannot collide, while a
template implementation revision cannot be silently collapsed into an older
catalog entry. Both legs are resolved atomically: if either projection fails,
no partial cohort is inserted.

Calendar-month expiries preserve the source snapshot's UTC time of day. A target
date that is a weekend or standard NYSE holiday advances to the next modeled
NYSE session date. This matters because production surface archives commonly
use a fixed close-ish timestamp such as 19:55 UTC rather than the
DST-dependent official session-close timestamp, and backtest settlement requires
an exact observed timestamp. The rule-based calendar does not know
ad-hoc/emergency closures; those require an explicit calendar override before
such a corpus can guarantee exact settlement.

## Initial build and daily extension

`build_backtest_db` works per `(template, symbol)` cell:

1. **Full** — no stored series exists, so the selected source range is run once.
2. **Unchanged** — all stored source dates and binary content identities match;
   no projection or pricing is performed.
3. **Extended** — the stored source list is an exact prefix of the current list.
   The old final row is loaded only as an anchor, the engine checkpoint resumes
   at its global step index, and only appended dates produce new rows.
4. **Rebuilt** — a historical source identity changed, a date was inserted, or
   the requested range expanded into earlier history. The cell is recomputed
   from inception because its old state is no longer a valid continuation.
5. **Failed** — the requested/source range would remove a stored date, the
   symbol disappears after its first available partition, its UID changes, or its
   projection/backtest fails. Existing stored coverage is left untouched.

Leading partitions before a symbol's first observation are treated as
pre-listing history and excluded from that symbol's cell. Once its history
starts, an interior or trailing missing daily surface fails closed rather than
silently changing the hedge cadence.

Source identity is currently partition-level and deliberately conservative. A
historical rewrite of any bytes in a daily surface archive invalidates every
backtest cell that cites that partition, even if an unrelated symbol caused the
rewrite. This can do extra work but cannot silently reuse a history derived from
unknown old bytes.

The builder also pins the source `SurfaceDb` manifest generation. It reopens and
fully validates the current manifest after the date-major source load,
immediately before each backtest partition publication, and after all cells
have been classified. The final check is load-bearing for an all-unchanged daily
run: such a run cannot report success merely because it performed no
destination write while its source manifest changed underneath it. Generation
drift returns `Unavailable` with retry guidance. No cell is published after
drift is observed; cells committed before a later drift remain valid because
each stores the exact source partition identities it consumed, and the retry
deterministically extends or rebuilds them.

The source and destination databases do not share a cross-process transaction.
A source writer can begin immediately after the final generation check. This
does not make a published series ambiguous—the exact input content identities
are persisted in it—but it means unattended operation should retry an
`Unavailable` result and rerun normally after each source publication. Surface
UID zero is rejected at load time, before a symbol can enter planning or
projection. The supported production schedule publishes the daily `SurfaceDb`
update first and starts the backtest build only after that source writer exits;
the generation checks are a fail-closed defense, not a replacement for that
single-writer schedule.

Incremental equality is an engine invariant: an initial build followed by daily
extensions must produce the same rows and final checkpoint as a one-shot run
over the combined source range. Checkpoints retain open theoretical lots,
ordered hedge shares, cash, NAV, cumulative non-cash financing, monotonically
issued IDs, and the global strategy step number.

## CLI

The production wrapper is built when `ATX_BUILD_EXAMPLES=ON`:

```powershell
powershell scripts\atx-build.ps1 build atx-vol-backtest-db-build
```

Build every symbol registered in the source database:

```powershell
build\bin\atx-vol-backtest-db-build.exe `
  --surface-db D:\atx-data\surfaces `
  --db D:\atx-data\backtests `
  --position short `
  --entry-every 1 `
  --threads 8
```

Build an explicit S&P 500 universe:

```powershell
build\bin\atx-vol-backtest-db-build.exe `
  --surface-db D:\atx-data\surfaces `
  --db D:\atx-data\backtests `
  --symbols-file D:\atx-data\sp500.txt `
  --from 2012-01-03 `
  --to 2026-07-24 `
  --position short
```

The `--symbols` value and symbols file accept comma-separated symbols with
optional surrounding ASCII whitespace, or one symbol per line; `#` starts a
file comment. Empty CSV fields are rejected. A normal unattended daily job
reruns the same command after the new `SurfaceDb` partition is published. Exit
codes are:

- `0`: every requested cell completed or was unchanged;
- `1`: structural source/database/write failure;
- `2`: command-line usage error;
- `3`: the run completed, but one or more cells failed and were preserved.

The command prints aggregate counts and one auditable line per cell, including
rows computed and rows added. `rows_computed=0` on a converged unchanged rerun is
the operational proof that historical pricing was not repeated. Variable report
fields are kept on one line: commas in a detail become semicolons and CR/LF
characters become spaces.

### Offline vacuum

Immutable generation partitions may remain after a successful replacement or a
crash before manifest publication. Vacuum is deliberately a standalone
maintenance mode:

```powershell
build\bin\atx-vol-backtest-db-build.exe `
  --db D:\atx-data\backtests `
  --vacuum
```

Stop the database writer first, and ensure no reader still holds an older
manifest snapshot. Running vacuum concurrently with either is unsafe because an
unindexed old-generation partition may still be live for that reader. The
command refreshes the current manifest and removes only regular, non-symlink
partition files that match the BacktestDb generation filename grammar and are
not referenced by that manifest. Unknown files, directories, symlinks,
temporary writer files, and current partitions are retained. Success prints
`vacuumed=<count>`. `--vacuum` accepts only `--db`; combining it with build
options is a usage error.

## Library use

```cpp
atx::vol::BacktestDbBuildSpec spec;
spec.surface_db_root = "D:/atx-data/surfaces";
spec.backtest_db_root = "D:/atx-data/backtests";
spec.templates = {*strategy};
spec.symbols = {"AAPL", "MSFT", "SPY"};
spec.price_threads = 8;

auto report = atx::vol::build_backtest_db(spec);
```

For retrieval:

```cpp
auto db = atx::vol::BacktestDb::open("D:/atx-data/backtests");
auto owned =
    db->load_series("short-strangle-40d-3cm-hold-expiry-daily-delta-v1-entry-1",
                    "AAPL");
auto mapped =
    db->map_backtest("short-strangle-40d-3cm-hold-expiry-daily-delta-v1-entry-1",
                     "AAPL");
```

`load_series` returns the owned result plus its validated continuation state.
`map_backtest` returns a zero-copy `RaSectionView` that co-owns its mapping for
fast analytics. Both routes validate the catalog/partition identity envelope
and archive CRCs before serving data.

## Scope and limitations

- The standard template models American option marks through the fitted
  `PricedSurface`, but option exercise/assignment remains governed by the
  backtest engine's documented exercise model.
- After a symbol's first available surface, a missing daily surface fails the
  cell; the builder does not silently skip a close because that would no longer
  be a daily-hedged history.
- `record_every_n` is fixed at one for persisted databases. A continuation
  cannot otherwise reproduce a partially accumulated stride block without
  storing additional state.
- Financing is currently the engine default (off) for the standard builder;
  execution frictions are template-owned and fingerprinted.
- An extension re-prices only appended dates, but the v1 store loads and
  atomically rewrites the complete owned series partition when it publishes
  the result. Compute is suffix-only while storage I/O remains O(history);
  retired immutable partition versions are reclaimed by offline vacuum.
- Constituent membership is supplied by the caller or by the symbols registered
  in `SurfaceDb`. `BacktestDb` does not source or reinterpret an S&P membership
  history.
