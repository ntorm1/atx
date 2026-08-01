# `surface_db_dispersion_backtest` — replaying a dispersion book off a SurfaceDb

Point it at a built `SurfaceDb` root and a date window; it replays an
index-vs-names volatility **dispersion** book over that window's partitions and
writes three CSVs. There is no corpus-build input, no quote loader and no fitter
in this route: **the database is the corpus**, and the only artifact you author
is the strategy config.

This is the surface-db consumer of the database that
[`atx-vol-surface-db-build`](surface-db-build.md) produces.

---

## 1. What this runs

A dispersion trade sells index volatility against a basket of single-name
volatilities (or the reverse), sized **vega-neutral**, so a parallel vol move
nets to approximately zero and the position isolates the spread between index
and average single-name vol — i.e. the market's implied correlation
(`atx-vol/include/atx/vol/dispersion.hpp`, "The trade" / "The signal").

Concretely, per step the route:

1. opens the db and builds a `Clock` from **one ref per partition**, ascending
   by ISO date, then windows it to `[--from, --to]` **inclusive**;
2. resolves the basket (§5);
3. loads that session's `.atxvsa` partition as a `MarketSnapshot`, rebinds every
   member symbol to a snapshot-local uid, and builds/marks the book off the
   **fitted surfaces already stored in the db**.

The three inputs are exactly: **the window** (`--from`/`--to`), **the config
file** (`--config`, optional — absent means `DispersionBacktestConfig` defaults),
and **the db root** (`--db`).

The whole run is one library call,
`run_surface_db_dispersion_backtest` (`atx/vol/dispersion_surface_db.hpp`); the
CLI is argv, artifact paths, a console headline and exit codes
(`atx-vol/examples/surface_db_dispersion_backtest.cpp`).

**The db is opened read-only.** The manifest is parsed; partitions are
memory-mapped `ArchiveBacking::Sealed`. The run writes **nothing** under the db
root — every artifact lands under `--out`.

### CLI

```
surface_db_dispersion_backtest --db DIR --from YYYY-MM-DD --to YYYY-MM-DD
    [--config FILE] [--out DIR] [--index SPY] [--universe FILE]
```

| flag | required | default |
|---|---|---|
| `--db DIR` | yes | — (`SurfaceDb::open`'s root) |
| `--from` / `--to` | yes | — ISO `YYYY-MM-DD`, **inclusive** both ends |
| `--config FILE` | no | engine + `DispersionBacktestConfig` defaults (§4) |
| `--out DIR` | no | `%TEMP%\atx-surface-db-dispersion` |
| `--index SYM` | no | `SPY` |
| `--universe FILE` | no | absent ⇒ equal-weight from the db manifest (§5) |

Every flag takes a value, and a flag whose value is missing or empty is a
**usage error** (exit 2), never "leave the default".

**Exit codes:** `0` success · `1` runtime error · `2` usage error.

---

## 2. Build

The target is gated behind the cmake cache flag **`ATX_BUILD_EXAMPLES`, which is
OFF by default** (`atx-vol/CMakeLists.txt:224`, target at `:431`). If your build
tree was configured without it, re-configure with `-DATX_BUILD_EXAMPLES=ON` —
the same flag [surface-db-build.md](surface-db-build.md#build) documents for its
own CLI.

Then, from the worktree root, in **PowerShell** (not a nested shell):

```powershell
# Release binary — this is the one to run against a real db.
.\scripts\atx-build.ps1 build surface_db_dispersion_backtest -Preset rel --parallel 8
# -> .\build-rel\bin\surface_db_dispersion_backtest.exe

# Debug library + tests:
.\scripts\atx-build.ps1 build atx-vol-tests --parallel 8

# The route's own gate suite (Debug):
.\scripts\atx-build.ps1 -Ctest -R "SurfaceDbDispersionBacktest"
```

> `rel` is a **preset**, not a verb: `atx-build.ps1 rel --parallel 8` falls
> through to raw `cmake rel --parallel 8` and dies with
> `CMake Error: Unknown argument --parallel`. Use `build <target> -Preset rel`.
> Do not pass `-j`.

Observed `-Ctest -R "SurfaceDbDispersionBacktest"` result: **22 tests, 21 passed,
1 skipped** — the skip is `RealSp100Baseline`, which is gated on
`ATX_SP100_SURFACE_DB` and does nothing unless you point it at a db.

---

## 3. Quickstart — a real, reproducible run

Run this **exactly**. It uses the 17-session window `2026-06-03..2026-06-26`,
which is the longest window the production `sp100-2026` db can actually replay —
see §6 for why, and read that section before you widen it.

```powershell
# One-time: the shipped production config plus the two keys this corpus needs.
Copy-Item atx-vol\examples\sp100_dispersion_config.tsv "$env:TEMP\sp100_dispersion_replayable.tsv" -Force
Add-Content -Path "$env:TEMP\sp100_dispersion_replayable.tsv" -Value "unpriced`texclude_and_report"
Add-Content -Path "$env:TEMP\sp100_dispersion_replayable.tsv" -Value "hedge_kind`tnone"

.\build-rel\bin\surface_db_dispersion_backtest.exe `
    --db C:\atx-data\surface-db\sp100-2026 `
    --from 2026-06-03 --to 2026-06-26 `
    --config "$env:TEMP\sp100_dispersion_replayable.tsv" `
    --out "$env:TEMP\sp100-dispersion"
```

Real output (Release, `build-rel`, db generation 225):

```
=== surface-db dispersion backtest ===
db: C:\atx-data\surface-db\sp100-2026 (generation 225) | window: 2026-06-03 .. 2026-06-26 (17 steps)
index: SPY | basket: 101 names | universe: surface_db_manifest | config: C:\Users\<you>\AppData\Local\Temp\sp100_dispersion_replayable.tsv
[pnl] total=22391.52762 sharpe=0.9273034383 max_drawdown=60922.036 total_return=22391.52762
[timing] engine: 2385.1 ms over 17 steps (7.1 steps/s)
[snapshot-cache] loads=0 hits=0 prefetches=0 (all-zero == the engine's PRIVATE Sealed cache, by design)
[wrote] C:\Users\<you>\AppData\Local\Temp\sp100-dispersion\series.csv
[wrote] C:\Users\<you>\AppData\Local\Temp\sp100-dispersion\strategy_metrics.csv
[wrote] C:\Users\<you>\AppData\Local\Temp\sp100-dispersion\engine_metrics.csv
```

Three things in that headline are worth reading deliberately:

- **`loads=0 hits=0 prefetches=0` is correct, not a bug.** `stats.cache` reports
  a **caller-supplied** `SnapshotCache`, and this route deliberately supplies
  none: a null `run.snapshot_cache` is exactly what makes the engine build its
  **private** cache, and only the private cache may map archives
  `ArchiveBacking::Sealed`. Zeros are the private path's signature.
- **`basket: 101 names`** is the manifest's 102 enabled symbols minus the index
  leg, not a count of what was tradeable on any given day.
- **`[timing]` brackets the engine call alone**, not process start-up or the
  CSV writes. It is one observation on one machine — for the recorded baseline
  and the knobs that move it, see §7.

**Where things are written.** `--out` and nothing else. Nothing under
`C:\atx-data` is created, modified or touched: a directory listing of
`C:\atx-data\surface-db\sp100-2026` (141 files: name, size,
`LastWriteTimeUtc`) hashes identically before and after these runs
(`9C97CF9519DCD7FB2EDF50210F61E9ACC8C875C0B191B1AF7A9BA2FA103CB4FD`).
Keep `--out` in `%TEMP%` or another scratch root; never point it inside the
repo or inside a data lake.

### Outputs

Three CSVs in `--out`, each prefixed with the **same 8-key `#` meta block** so a
file read six months later still says which db, which generation, which window
and which basket produced it:

```
# data_source=surface_db
# db_root=C:\atx-data\surface-db\sp100-2026
# db_generation=225
# window=2026-06-03..2026-06-26
# window_resolved=2026-06-03..2026-06-26
# index=SPY
# n_names=101
# universe=surface_db_manifest
```

`window` is what you **asked for** — `--from`/`--to` verbatim, unresolved.
`window_resolved` is what the run **actually covered**: the first and last
recorded session, the same range the headline prints. The two agree above
because the quickstart's ends are both real partitions, but a window end outside
the db's available range **clamps** (§8) rather than erroring, so a request can
name dates the run never touched. **Read `window_resolved` when you need to know
what the numbers in the file cover**; read `window` only to recover the command
that produced it.

`universe` is the literal `surface_db_manifest` on the equal-weight route, or
the `--universe` path when one was given.

| file | contents |
|---|---|
| `series.csv` | one row per recorded step: `date, ts_ns`, the P&L attribution columns (`pnl_total, nav, pnl_delta … pnl_unexplained`), `pnl_settlement, pnl_shares, financing, cost, cash`, book Greeks (`gross_delta, gross_gamma, gross_vega, gross_theta`), `turnover_notional, turnover_vega`, `n_open_lots`, **`n_unpriced_lots`, `n_unpriced_greeks`**, and the dispersion diagnostics `implied_corr, n_names_dropped, corr_vega, corr_gamma` |
| `strategy_metrics.csv` | `metric,value` tearsheet + result summary |
| `engine_metrics.csv` | `wall_clock_ms, steps_per_s, n_steps, cache_loads, cache_hits, cache_prefetches` |

From the quickstart's `strategy_metrics.csv` (verbatim excerpt):

```
total_return,22391.52762
sharpe,0.9273034383
max_drawdown,60922.036
hit_rate,0.625
avg_open_lots,200
peak_open_lots,200
total_unpriced_lots,76
total_unpriced_greeks,40
n_steps,17
```

`total_unpriced_lots,76` is not noise — it is the price this corpus charges for
being replayable at all. §6 explains it.

---

## 4. Config reference

`--config` is a flat **`key<TAB>value`** TSV, the same family as `read_run_spec`.
Blank lines and `#` comments are ignored. **Every key is optional**; an absent
key keeps its default. A repeated key is last-one-wins. A row that is not
exactly `key<TAB>value` — no tab, or a present-but-empty value — is
`InvalidArgument`, because an empty value is an authoring error, not "use the
default".

The reader accepts **exactly these 20 keys**
(`atx-vol/src/dispersion_surface_db.cpp`, `read_dispersion_backtest_config`).
Defaults are `DispersionBacktestConfig` / `StrikePolicy` / `FrictionModel` /
`RunConfig` as shipped.

| key | type | default | notes |
|---|---|---|---|
| `target_dte_days` | double | `30` | option tenor targeted at entry |
| `roll_dte_days` | double | `7` | roll horizon; `LifecycleSpec::roll_at_T = roll_dte_days / 365.25` |
| `gross_index_vega` | double | `10000` | **dollars of index gross vega per ONE vol point** (a 0.01 move in sigma) |
| `delta_band` | double | `0` | hedge band; `0` = hedge to zero every time the hedge fires |
| `min_names` | size_t | `2` | minimum **surviving** basket size; below it the step is a diagnosed no-trade (§6). Must be ≥ 2 |
| `entry_every_n` | unsigned | `21` | `LifecycleSpec::Entry::EveryNDays` cadence |
| `record_diagnostics` | `0`/`1` | `0` | gates the whole diagnostic block: `implied_corr`, `n_names_dropped`, `corr_vega`, `corr_gamma`. **Off ⇒ those columns are not emitted at all** |
| `multiplier` | double | `100` | contract multiplier |
| `side` | enum | `short_index_long_names` | \| `long_index_short_names` |
| `weighting` | enum | `vega_neutral` | \| `equal_vega` \| `gamma_neutral` \| `theta_neutral` |
| `strike_rule` | enum | `atm_forward_straddle` | \| `fixed_moneyness` \| `delta_strangle` |
| `log_moneyness` | double | `0` | `fixed_moneyness` only; `0` == ATM forward |
| `target_abs_delta` | double | `0.25` | `delta_strangle` only; in (0, 1) |
| `hedge_kind` | enum | `delta_to_zero` | \| `none` |
| `hedge_cadence` | enum | `daily` | \| `at_entry` |
| `half_spread_bps` | double | `0` | → `run.frictions.half_spread_bps`; see the spread-lane note below |
| `per_contract_cost` | double | `0` | → `run.frictions.per_contract_cost`; `$` per contract traded, charged in every lane |
| `n_threads` | unsigned | `0` | → `run.price.n_threads`; `0` = all hardware cores. Output is bit-identical at any thread count |
| `prefetch_depth` | size_t | `1` | → `run.prefetch_depth`; snapshot look-ahead depth. Output is bit-identical at any depth (§7) |
| `unpriced` | enum | `error` | → `run.unpriced`; \| `exclude_and_report`. **Read §6 before changing this** |

Numbers must parse **whole**: `45x` is rejected, never read as `45`, and a
double must additionally be finite.

**Spread lane.** The engine reads `half_spread_bps` only under
`FrictionModel::SpreadKind::PriceBps`, and the default kind is `None` — so a
half-spread authored alone would be a silently ignored knob. A **nonzero**
`half_spread_bps` therefore also arms `run.frictions.spread_kind = PriceBps`.
Zero leaves the lane untouched, so a frictionless config is bit-identical to the
default.

**Not exposed on purpose** — they are caller/route decisions, not file knobs:
`project_to_calendar_expiry`, the `entry`/`holding` lifecycle shape, `limits`,
and the rest of `RunConfig`.

### Errors are self-diagnosing

Every rejection names the key, the value and the line:

```
read_dispersion_backtest_config: InvalidArgument: dispersion config: unknown key 'min_nmaes' (line 2)
read_dispersion_backtest_config: InvalidArgument: dispersion config: unrecognized token for key 'unpriced' value 'exclude' (line 1)
```

> **Authoring gotcha.** The reader has no BOM handling: a config written by
> Windows PowerShell's `Set-Content -Encoding utf8` (which emits a BOM) fails
> with `line 1 is not a key<TAB>value row: '<BOM>#...'`. Use `Copy-Item` +
> `Add-Content` as the quickstart does, or write UTF-8 without a BOM.

### The shipped config

`atx-vol/examples/sp100_dispersion_config.tsv` is the worked SP100 shape:

```
target_dte_days   30
roll_dte_days     7
gross_index_vega  10000
min_names         60
entry_every_n     21
side              short_index_long_names
weighting         vega_neutral
strike_rule       atm_forward_straddle
record_diagnostics 1
n_threads         0
prefetch_depth    2
```

(separators shown as spaces for readability — the file's are tabs).

It is the **production** shape and it is deliberately left fail-closed: on the
current `sp100-2026` db it aborts (§6). The quickstart overlays two keys onto a
copy rather than editing it.

---

## 5. Universe — which names are traded

### Default: equal weight from the db manifest

With no `--universe`, the basket is derived by `universe_from_surface_db` and
**frozen for the whole run**: `--index` becomes the index leg, every **other
enabled** symbol in the manifest becomes a basket name at weight `1/n`, and the
index member carries weight `1.0` (which the engine ignores — only constituent
weights enter the signal and sizing).

The universe is **the manifest's symbol table, not the partitions.** Those are
orthogonal namespaces: a partition stores whatever symbols it was handed and
registers none of them, while the symbol table is where the operator states
which underlyings the db is *for* and which are switched off
(`SymbolFitConfig::enabled`). Reading the table answers "the universe this db
was built for"; reading a partition would answer "what happened to be fitted on
one date", which would silently resize the basket whenever a single day's fit
was thin. So you author a universe **by building a db**, not by maintaining a
second symbol list beside it.

- `--index` is matched **case-insensitively** and every member carries the
  manifest's canonical upper-case spelling.
- Order is the manifest's, which `DbManifest::open` already enforces to be
  strictly ascending — so the same db always yields the same basket in the same
  order.
- The enabled filter runs **before** the index match, so an index that is
  present but `enabled=0` is rejected exactly like an absent one:

  ```
  error: InvalidArgument: run_surface_db_dispersion_backtest: universe_from_surface_db: universe_from_surface_db: index symbol 'QQQ' is not an enabled symbol in the SurfaceDb manifest (102 symbols)
  ```

- `DispersionMember::uid` stays `0` in the derived universe. A uid identifies a
  surface inside **one** `MarketSnapshot`, so binding one here would be wrong on
  every date but the one it came from; the engine rebinds per step via
  `MarketSnapshot::uid_of`.

Inspect the table the run will use with the db's own CLI — no Python:

```powershell
.\build-rel\bin\atx-vol-surface-db.exe symbols --db C:\atx-data\surface-db\sp100-2026
```

### Escape hatch: `--universe` (point-in-time)

`--universe FILE` switches to the **point-in-time** route: the basket is
re-resolved on **every step**, so a mid-window reconstitution — or a **removal**
— is honoured instead of freezing day-1 membership.

**The two routes build DIFFERENT books, and which one runs is decided by that
flag alone — there is never a fallback.** A `--universe` file that is missing or
malformed is an error, not a silent downgrade to the equal-weight route: running
a basket the operator did not author and reporting success is the one failure a
backtest cannot survive.

The file is a `UniverseRow` TSV with this **exact** header line:

```
effective_date	symbol	raw_weight	source	as_of
```

Five tab-separated columns per row, and:

- `raw_weight` must parse and be **> 0**;
- `symbol` and `source` must be non-empty;
- `as_of` must be **≤** `effective_date`;
- `(effective_date, symbol)` must be unique — a duplicate is `AlreadyExists`,
  not last-writer-wins;
- the file must have at least one row.

**PIT semantics:** each `effective_date` block is a **full vendor-style
snapshot**. Membership on a date is exactly the rows carrying the latest
`effective_date` on or before it — so a name present in an earlier block but
absent from that latest block **has left the basket**. The basket is not
append-only. `--index` is never a constituent.

Worked example (a 5-name basket that reconstitutes to 6 mid-window):

```
effective_date	symbol	raw_weight	source	as_of
2026-06-03	AAPL	1.0	demo	2026-06-01
2026-06-03	AMZN	1.0	demo	2026-06-01
2026-06-03	META	1.0	demo	2026-06-01
2026-06-03	MSFT	1.0	demo	2026-06-01
2026-06-03	NVDA	1.0	demo	2026-06-01
2026-06-15	AAPL	1.0	demo	2026-06-12
2026-06-15	AMZN	1.0	demo	2026-06-12
2026-06-15	GOOGL	1.0	demo	2026-06-12
2026-06-15	META	1.0	demo	2026-06-12
2026-06-15	MSFT	1.0	demo	2026-06-12
2026-06-15	NVDA	1.0	demo	2026-06-12
```

Run against it. The config's `min_names` must be ≤ the **smallest** block's size,
or every step before the reconstitution is a no-trade (§6) — here `5`:

```
=== surface-db dispersion backtest ===
db: C:\atx-data\surface-db\sp100-2026 (generation 225) | window: 2026-06-03 .. 2026-06-26 (17 steps)
index: SPY | basket: 6 names | universe: C:\Users\<you>\AppData\Local\Temp\atx-doc-universe.tsv | config: ...
[pnl] total=130657.556 sharpe=2.48301994 max_drawdown=93977.83852 total_return=130657.556
```

`n_names` in the meta block (`6` here) is the count of **distinct constituents
the schedule ever names**, because on the PIT route membership varies by step
and no single-day count would be honest.

There is **no cap-weighted SP100 route**: the db manifest carries no weight
source. Author a `--universe` TSV if you have one.

---

## 6. Absence semantics — what happens when a surface isn't there

A real surface db has holes. This route has **two separate absence lanes**, they
are governed by different machinery, and only one of them has a knob.

### Lane A — absence at **entry**: drop, renormalize, and the `min_names` gate

This route hardwires `MissingNamePolicy::DropRenormalize`
(`dispersion_config_from`, `atx-vol/src/dispersion_backtest.cpp`). A basket name
with no surface on the session the book is being built is **dropped**, the
surviving weights **renormalize** so the book stays vega-neutral over what is
actually there, and the drop is reported per row in `n_names_dropped` — which
requires `record_diagnostics 1`, so **turn it on if you intend to audit
absence**. Every drop carries a reason (`NotInSnapshot` / `SurfaceNotFound` /
`Unavailable`) and the underlying message verbatim — a drop is never silent.

If fewer than **`min_names`** names survive, the step is a **diagnosed no-trade
step, not an error**: no lots are opened, the existing book is left untouched,
the run continues, `implied_corr` is `NaN`, and `n_names_dropped` is still
recorded so the series stay full-length and the reason is visible.

Verified on the real db, same window, with `min_names` raised to `200` (above
the 101-name basket, so the gate fires on every step):

```
[pnl] total=0 sharpe=0 max_drawdown=0 total_return=0
[timing] engine: 1551.0 ms over 17 steps (11.0 steps/s)
```
```
2026-06-03  nav=-0  n_open_lots=0  implied_corr=nan  n_names_dropped=2
2026-06-04  nav=0   n_open_lots=0  implied_corr=nan  n_names_dropped=3
2026-06-05  nav=0   n_open_lots=0  implied_corr=nan  n_names_dropped=2
```

Exit code `0`, 17 rows, no book. **A flat series is the symptom to look for.**
Check `n_names_dropped` and `min_names` before concluding the strategy did
nothing.

### Lane B — absence under a **held book**: the `unpriced` key

Once lots are open, a name going absent costs a **mark**, and that is a
different question: `RunConfig::unpriced` decides it.

| token | behaviour |
|---|---|
| `error` (**engine default**) | any step on which a held lot has no surface **aborts the run** with `NotFound`. NAV can never silently truncate. |
| `exclude_and_report` | the lot is excluded from that step's P&L / Greek lane and counted in `n_unpriced_lots` (Greeks separately in `n_unpriced_greeks`). The run continues. |

`exclude_and_report` **is not free, and the cost is permanent**: the excluded
step's P&L is *never recovered* when the surface reappears, so **NAV
permanently diverges from liquidation value**, and the only record of it is that
per-row count. Author it deliberately and read that column.

Under the default, the real db's shipped config aborts:

```
error: NotFound: run_backtest: 4 held lot(s) have no surface this step (first uid=256831654)
```

### Two guards `unpriced` does **not** cover

Both are correct fail-closed behaviour and neither has a policy knob:

- **Delta-hedge share fill** — a hedge needs the name's spot, and an absent
  surface has none:
  ```
  error: NotFound: run_backtest: no surface for delta hedge on uid=256831654 (share fill would price at spot 0.0)
  ```
  This is why the quickstart also sets `hedge_kind none`: on this corpus the
  daily delta hedge cannot be evaluated on sessions whose names are missing.
- **Roll-close execution mark** — closing a lot without an economically valid
  mark would destroy cash value, so the executor always fails closed. This is
  what caps the quickstart window at 17 sessions; extending it to `2026-06-29`
  (18 sessions) with the identical config gives:
  ```
  error: NotFound: run_backtest: no surface for roll-close lot id=13 uid=2154630856
  ```

### The index is not droppable

There is nothing to disperse *against*, so the index is never dropped under any
missing-name policy. Which error you get depends on where the gap lands:

- **On a session where the book is built** (inception always builds one) —
  `resolve_universe_uids` refuses to bind the universe at all. Measured on
  `--from 2026-07-15`, an index-absent session:
  ```
  error: NotFound: dispersion: symbol 'SPY' not present in snapshot directory
  ```
- **On a pure mark-to-market session under a held book** — it is Lane B. Under
  `error` the run aborts; under `exclude_and_report` it continues with the index
  lots counted in `n_unpriced_lots` and the diagnostics `NaN`. Measured over
  `2026-07-14..2026-07-16` (SPY absent on the 15th):
  ```
  2026-07-14  lots=198  unpriced=0   ugreeks=0  ic=0.10671120042282639  dropped=3
  2026-07-15  lots=198  unpriced=4   ugreeks=4  ic=nan                  dropped=nan
  2026-07-16  lots=198  unpriced=10  ugreeks=6  ic=0.11037907964786212  dropped=4
  ```

### Data coverage of `sp100-2026` (generation 225)

**These are properties of that database build, not of this code.** Measured
directly from the db at generation 225 by diffing every partition's surface list
against the manifest symbol table (`atx-vol-surface-db info` / `partitions
--key`):

| fact | value |
|---|---|
| manifest symbols (all enabled) | **102** (SPY + 101 names) |
| partitions | **140**, none missing |
| stored surfaces | **13 922** of a possible 14 280 ⇒ **358 absent cells** (2.5%) |
| sessions with the **index** absent | **18 of 140** |
| sessions with ≥ 1 **basket name** absent | **131 of 140**, 1–6 names each |
| worst recurring names | `AMT` (47 sessions), `BK` (46), `CMCSA` (28), `SYK` (25), `KHC`/`CL` (14 each) |
| longest **index-complete** stretch | **28 sessions**, `2026-06-03..2026-07-14` |
| longest window this route can actually replay | **17 sessions**, `2026-06-03..2026-06-26` |

> On that "≥ 1 basket name absent" row, the sprint's Task 6 report gives
> *118 of 140*. The two are counted differently, not measured differently:
> **131** is a pure **partition census** — a session counts if any of the 101
> basket names has no stored surface in that partition (the index has its own
> row above) — and the report's narrower figure uses a different criterion.
> Every other figure in this table reproduces that report exactly, date for
> date. Read either as "most sessions in this db are incomplete"; neither
> contradicts the other.

**`BK` is not scattered — it stops.** It is absent on **every one of the 44
sessions from 2026-05-21 through the end of the corpus (2026-07-24)**, plus two
isolated earlier dates (2026-02-23 and 2026-05-05). That is the signature of a
**systematic fitter failure for that symbol**, not of a random data outage: no
window overlapping 2026-05-21 or later can carry `BK` at all, which is why the
quickstart window drops it on all 17 of its sessions. `AMT`'s 47 absences, by
contrast, are diffuse — scattered from 2026-01-02 to 2026-07-17 with no cutover.
When choosing a window, check the *shape* of a name's absence, not just its
count.

Index-absent sessions, in full:

```
2026-01-22  2026-01-26  2026-01-30  2026-02-24  2026-03-02  2026-03-05
2026-03-06  2026-03-10  2026-04-07  2026-04-16  2026-05-04  2026-05-05
2026-05-20  2026-05-22  2026-05-27  2026-06-02  2026-07-15  2026-07-22
```

Consequently **the full window `2026-01-02..2026-07-24` cannot be replayed.**
Under the shipped config it aborts on `2026-01-05`, the window's second session
(`CMCSA`/`MO`/`QCOM`/`WMT` absent × 2 straddle legs = 8 held lots). Adding
`unpriced exclude_and_report` moves the failure to the delta hedge; suppressing
that moves it to the roll-close; suppressing that reaches the missing index,
which has no remedy. The 17-session quickstart window is the honest maximum, and
even there **76 lots go unpriced across 16 of its 17 sessions**.

Widening the window is a **data** problem — rebuild the db with complete fits —
not a config problem.

---

## 7. Performance

### Recorded baseline

Release (`rel`, clang-cl), real `sp100-2026` db, window `2026-06-03..2026-06-26`
(17 sessions), `n_threads=0`, ~200 open lots, warm page cache. From the sprint's
Task 6 measurement (`SurfaceDbDispersionBacktest.RealSp100Baseline`; its config
shape holds `delta_band = 1e18` so the daily hedge pass still *runs* on every
step but never trades, which keeps the dominant full-book pricing cost intact):

| `prefetch_depth` | wall (ms) | ms / session | vs depth 1 |
|---|---|---|---|
| 1 | 2632 | 155 | 1.00x |
| 2 (shipped) | **1692** | **100** | 1.56x |
| 8 | 1029 | 61 | 2.56x |

**Both derived columns are computed from this table's own `wall` column** —
`ms / session` is `wall / 17`, the ratio is `2632 / wall` — so the three rows
are internally consistent. Read them as *the three walls Task 6 headlined*, not
as one continuous sweep: depths 1 and 2 are its recorded baseline run, depth 8
is the best rep of its separate 2-rep depth sweep. That sweep's own reps are
noisier and land at different ratios against **its** depth-1 rep (1.51x / 2.09x
/ 2.78x for depths 2 / 4 / 8). **Do not mix a ms value from one with a ratio
from the other**; if you need a like-for-like comparison, re-measure.

Archive opens == steps (17) at every depth — no reloads — and the **output is
bit-identical across depths** (`bit_cast` on `nav` and `pnl_total`).

The quickstart's own `[timing]` line is not this number: it runs a different
shape (`hedge_kind none`) and is a single observation.

### The two knobs

- **`prefetch_depth`** — how many future snapshots may be in flight. Depth `D`
  turns `economics + total_load` into roughly `max(economics, total_load / D)`.
  **It changes only *when* a snapshot is deserialized, never which bytes**, so
  output is unaffected at any depth. The private cache is sized **`depth + 2`**
  (`private_snapshot_cache_capacity`, `atx-vol/src/backtest.cpp:67-69`), so
  raising the depth raises resident whole-board snapshots too — **10** at depth
  8 against **4** at the shipped depth 2. **The memory cost of depth 8 was not
  measured**, which is why the shipped config stays at 2.
- **`n_threads`** — pricer fan-out; `0` = all hardware cores. Output is
  bit-identical at any thread count (`PortfolioPricer`'s serial-scatter
  reduction), so parallel-by-default is free.

### Where the time goes

This run is **snapshot-load-bound** — roughly 90% of the depth-1 wall. Serial
whole-board loads of the same 17 partitions cost ~2.5–2.8 s on their own, and
each load deserialises ~100 surfaces and prepares their query tier: that is the
cost, not file I/O. The depth-8 wall implies an economics floor of ~60
ms/session. Sealed-vs-Mutable mmap measured **inside run-to-run noise** on this
corpus (~500 KB partitions, warm cache); Sealed remains the right default
because it is free and cannot be slower, but it is not where this route's time
goes.

`DispersionStrategy` has no `referenced_uids()`, so the private cache loads the
**whole board**. Even if it subsetted there would be nothing to recover: the
basket is 101 of the db's 102 symbols, so the subset *is* the board.

### Phase profiling

`ATX_VOL_PROFILE` is a **compile-time CMake option, not an environment
variable** (`CMakeLists.txt:256`; `atx-vol/CMakeLists.txt:183-184`). With it OFF
— the default — `ATX_VOL_PROFILE_SCOPE` expands to `((void)0)` and no timer
storage is compiled. To get phase timings you must **re-configure and rebuild**
with `-DATX_VOL_PROFILE=ON`; setting it in the environment does nothing.

---

## 8. Diagnosing a failed run

Errors name the stage, and the stage is the knob to fix.

| message | meaning |
|---|---|
| `SurfaceDb::open('…'): SurfaceDb: manifest not found` | `--db` is not a SurfaceDb root |
| `Clock::between('…','…'): no snapshots in [a, b] (available X..Y)` | the window selects no partition. **The available range is in the message** — an empty window is an error, never an empty run |
| `universe_from_surface_db: index symbol 'X' is not an enabled symbol in the SurfaceDb manifest (N symbols)` | wrong `--index`, or the symbol is `enabled=0`. The symbol count tells you whether you also opened the wrong db |
| `dispersion: symbol 'SPY' not present in snapshot directory` | the index is absent on a session the book is built on (§6) |
| `N held lot(s) have no surface this step (first uid=…)` | Lane B under `unpriced error` (§6) |
| `no surface for delta hedge on uid=… (share fill would price at spot 0.0)` | no knob; drop the hedge or shorten the window |
| `no surface for roll-close lot id=…` | no knob **by design**; a close always requires a valid mark |
| `dispersion config: unknown key '…' (line N)` | typo in `--config` |
| `read_universe: …` | malformed `--universe` TSV (§5) |

Out-of-range window **ends clamp** — asking for `2020-01-01..2030-01-01` gives
you everything there is, which is not an error. Only a window selecting *no*
partition fails. When a clamp happens the emitted CSVs still record both ends of
it: `window` is the request, `window_resolved` is what ran (§3).

---

## 9. Python

The same composition is available through `atxvol`. The snippet below is the
shape of `atx-vol/python/tests/test_surface_db_dispersion.py`, which is the
working reference — copy from there, not from here, when you need something that
must run. Note that `DispersionUniverse` / `DispersionMember` are bound as
`py::init<>()` plus `def_readwrite`, so members are built by **field assignment,
not kwargs**:

```python
import atxvol as av

db = av.SurfaceDb.open(str(db_root))
clock = av.Clock.from_surface_db(db).between("2026-06-03", "2026-06-26")

index = av.DispersionMember()
index.symbol = "SPY"
index.uid = 0            # uids are snapshot-local; the engine rebinds per step
index.weight = 1.0

names = []
for sym in basket:                       # a list of symbol strings
    m = av.DispersionMember()
    m.symbol = sym
    m.uid = 0
    m.weight = 1.0 / len(basket)
    names.append(m)

universe = av.DispersionUniverse()
universe.index = index
universe.names = names

cfg = av.DispersionBacktestConfig()
cfg.min_names = 2
cfg.entry_every_n = 21
cfg.run.price.n_threads = 0              # nested `run` is a reference property

result = av.run_dispersion_backtest(clock, universe, cfg)
```

`Clock.between` is inclusive on both ends, non-mutating, clamps out-of-range
ends, and raises `av.AtxError` with `ErrorCode.INVALID_ARGUMENT` on an empty or
inverted window — with the db's available range in the message.

The `UniverseRow` front end **is** bound — `av.read_universe(path)`,
`av.universe_at(rows, date)`, `av.all_symbols(rows)` — so the point-in-time
schedule of §5 is reachable from Python, though none of those three take an
`index_symbol` argument: the bindings pin the C++ default, `"SPY"`.
What is **not** bound is
`read_dispersion_backtest_config`, `universe_from_surface_db` and the one-call
`run_surface_db_dispersion_backtest`; the `key<TAB>value` config file and the
equal-weight-from-manifest derivation are C++/CLI only.

---

## 10. Limits — what this route is **not**

- **No listed-contract execution realism.** Every mark here is a **model mark
  off a fitted surface**, and the fitted surface is a MID surface with no stored
  bid/ask. Frictions (`half_spread_bps`, `per_contract_cost`) are a *documented
  model*, not observed quotes. If you need real listed contracts, real
  definitions and OCC-ESS evidence, that is the **corpus/listed route**
  (`spy_dispersion_backtest`, `listed_dispersion_pipeline.hpp`) — a different
  tool with different inputs.
- **No corpus build, no fitting, no admission decisions.** Whatever the db
  contains is what gets replayed. If a fit was bad, this route replays the bad
  fit; use `atx-vol-surface-db verify` to interrogate the db itself.
- **Absence is a property of the db, not a strategy signal.** A dropped name
  reflects a missing fit, not a delisting. Do not read `n_names_dropped` as
  index reconstitution.
- **NAV under `unpriced exclude_and_report` is not liquidation value.** See §6.
- **No cap-weighted universe.** The manifest carries no weights; use
  `--universe`.
- **No shared snapshot cache**, deliberately, and callers should not install one
  in `spec.config.run.snapshot_cache`: it would cost a whole-archive copy per
  date, forfeit the private cache's Sealed mmap, and gain nothing on a
  single-pass replay.

---

## See also

- [surface-db-build.md](surface-db-build.md) — building and verifying the
  database this route consumes.
- `atx-vol/include/atx/vol/dispersion_surface_db.hpp` — the entry point's
  contract, in full.
- `atx-vol/tests/surface_db_dispersion_backtest_test.cpp` — the 22-test gate
  suite. The route's own behaviour above (config reader, universe derivation,
  window handling, stage errors, absence lanes, cache/prefetch) is pinned there;
  the delta-hedge and roll-close guards are engine-level and live in
  `atx-vol/src/backtest.cpp`.
