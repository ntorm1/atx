# Dispersion two-route comparison — status and next steps

**Date:** 2026-07-19
**Goal:** compute SPY dispersion two ways — (A) real listed OPRA contracts, (B)
projected option definitions repriced daily off the interpolated surface — and
establish whether the two agree.

**Status: comparison not established.** The work to run both routes is done and
verified, but the comparison window is capped at 3 sessions by reference-data
coverage, and on those 3 sessions the two routes disagree materially. One
concrete cause was isolated (a 100x vega-unit mismatch); a residual sign
disagreement survives correcting for it and is not yet explained.

---

## 1. The two routes

Both are subcommands of `atx-vol/examples/spy_dispersion_backtest.cpp`, both run
over the same corpus of fitted surfaces, and both fold P&L through the same
engine (`run_backtest`). They differ only in where a contract comes from.

| | **Listed (A)** | **Projection (B)** |
|---|---|---|
| Command | `run-backtest` | `run-surface-backtest` |
| Output | `backtest.tsv` | `surface_backtest.tsv` |
| Strategy | `ListedDispersionStrategy` | `DispersionStrategy` |
| Contract source | real OPRA contracts, frozen into `trade_schedule.tsv` by `build-schedule` | synthetic: ATM-forward strike at a projected calendar expiry |
| Strike | nearest listed strike | exact ATM-forward, continuous |
| Expiry | real exchange expiry, 21–60 DTE window | `valuation_ts + round(30d)`, a calendar anchor |
| Daily reprice | same fixed contract, residual `T` re-derived each step | same synthetic `(K, expiry)`, residual `T` re-derived each step |
| Execution tier | `QueryExecution::ColdReference` (forced) | `QueryExecution::Configured` |
| Selection | cold-authored, validated against each archive mark | resolved live each entry |

Prerequisite files per route:

- **Both:** `run_spec.tsv`, `universe_schedule.tsv`, `surface_manifest.tsv`, `archives/*.atxvsa`
- **Listed only:** `definitions.tsv` (contract definitions), `occ_ess/` +
  `occ_ess_inventory.tsv` (deliverable-adjustment evidence), OPRA parquet panels

---

## 2. What was built and verified

### 2.1 Python bindings — new `src/bindings/dispersion.cpp`

Both routes are now drivable from Python.

Projection route: `RunSpec`, `UniverseRow`, `read_run_spec`, `read_universe`,
`universe_at`, `all_symbols`, `DispersionUniverse`, `DispersionMember`,
`DispersionSide`, `DispersionBacktestConfig`, `DispersionStrategy`,
`make_dispersion_backtest_strategy`, `run_dispersion_backtest`.

Listed route: `ListedScheduleLeg`, `ListedScheduleRoll`,
`ListedDispersionSchedule`, `read_listed_dispersion_schedule`,
`write_listed_dispersion_schedule`, `ListedDispersionStrategy.create`.

Also added to `src/bindings/backtest.cpp`: `read_corpus_manifest` /
`write_corpus_manifest`. These were the missing link — a corpus archive
directory is indexed by `manifest.tsv`, which is **not** the same on-disk format
as a `SurfaceDb`'s `manifest.atxdb`. Opening an archive tree with
`SurfaceDb.open` fails with `NotFound: SurfaceDb: manifest not found`. The
archive path is `read_corpus_manifest` -> `Clock.from_manifest`.

`bind_dispersion` is registered **last** in `module.cpp`: it defaults a
`py::arg` to `DispersionBacktestConfig{}` (whose nested `RunConfig` must already
be registered by `bind_backtest`) and subclasses the `IStrategy` that
`bind_strategy` registers.

### 2.2 Verification — projection route is bit-identical to C++

Driven from Python over the 84-session `bt-sota-baseline` corpus:

```
spec:   SPY listed-options dispersion bt-sota baseline  2026-01-02..2026-04-30
clock:  82 steps, 2026-01-02 .. 2026-04-30
names:  AAPL AMZN AVGO GOOGL JPM LLY META MSFT NVDA XOM   (index SPY)
python run: rows 82, final nav 247.4065016444

nav        bit-identical to C++ surface_backtest.tsv: True
pnl_total  bit-identical: True
gross_vega bit-identical: True
```

The freshly built C++ binary independently reproduces the golden run byte-for-byte
(`dates=82 final_nav=247.4065016`, `diff` clean against the archived output).

### 2.3 Report library additions

`atxvol/report/charts.py` gained two forms the comparison needs:

- `scatter_chart` — agreement scatter on a **shared** scale for both axes with
  the `y = x` diagonal drawn, plus an optional OLS overlay. Independent axes are
  wrong for an agreement question: a 30% bias renders as a tight fit.
- `paired_bar_chart` — grouped columns, one cluster per category, one bar per
  method. Series hold their palette slot across clusters, so a category where the
  ranking flips does not repaint.

Neither has test coverage yet (see §6).

---

## 3. Finding 1 — the comparison window is capped at 3 sessions

The listed route's reference data stops at 2026-01-06.

| Input | Coverage | Span |
|---|---|---|
| OPRA quote panels | 135 sessions | 2026-01-02 .. 2026-07-17 |
| Fitted surfaces (`bt-sota-full`) | 137 dates | 2026-01-02 .. 2026-07-17 |
| Fitted surfaces (`bt-sota-baseline`) | 84 dates | 2026-01-02 .. 2026-04-30 |
| **Contract definitions** | **3 as-of dates** | 2026-01-02, 01-05, 01-06 |
| **OCC ESS evidence** | **3 dates** | 2026-01-02, 01-05, 01-06 |

Every `definitions.tsv` on disk carries exactly 3 as-of dates — including
`runs/bt-sota-full/definitions.tsv`, which despite the 137-date corpus is the
same 13.2 MB 3-date snapshot. `definitions-parts/` has 1 date;
`definitions-sourced/` has 3.

Consequence: `build-schedule` over an 84-date clock fails with
`NotFound: listed OPRA join: contract definition missing` as soon as it reaches a
date whose OPRA panel contains a contract absent from the definitions table (new
expiries and strikes get listed continuously).

Extending requires a Databento definitions pull via
`examples/databento_spy_dispersion_definitions.cpp`, which needs
`DATABENTO_API_KEY` (currently unset) and is likely billable.

### 3.1 A related wart, found and left alone

`persist_occ_ess_evidence` writes `occ_ess_inventory.tsv` **only** when the spec
carries an `occ_ess_root` (`spy_dispersion_backtest.cpp:119`), but
`verify_occ_ess_evidence` demands that inventory **unconditionally**
(`:167`, called at `:346` and `:440`). So a corpus built without an
`occ_ess_root` can never reach `build-schedule` or `verify` — it fails with
`NotFound: cannot open <run>/occ_ess_inventory.tsv`.

This was relaxed during investigation and then **reverted**: the definitions cap
blocks the window regardless, so relaxing a deliverable-adjustment correctness
check bought nothing. The working tree is clean of that change. Worth fixing on
its own merits, separately from this work.

### 3.2 Archive format split

`runs/listed-dev-*` archives are `ATXVSA03`; `runs/bt-sota-*` are `ATXVSA20`.
The current example reads V2 and fails on the V3 archives with
`ParseError: SurfaceArchiveV2::open: bad magic`. Any pairing that reuses an
existing listed dev run must first refit that corpus with current code — which
is what §4 does.

---

## 4. Finding 2 — the routes disagree on the paired 3-session run

A fresh corpus was built with current code over 2026-01-02..2026-01-06
(`build-corpus`: `admitted=33 quarantined=0 source_failed=22`), then **both**
routes run over it. Same corpus, same universe, same spec, same engine.

```
build-schedule:       rolls=1
run-backtest:         dates=3 rolls=1 final_nav=-456.611313
run-surface-backtest: dates=3        final_nav=  46.153335
```

Column comparison at the final step:

| column | listed | projection |
|---|---:|---:|
| `nav` | -456.6113 | 46.1533 |
| `pnl_total` | -600.8384 | 9.0866 |
| `pnl_vega` | -2195.0020 | 26.2165 |
| `pnl_gamma` | 3472.3868 | 53.1366 |
| `pnl_theta` | -1936.3543 | -31.9394 |
| `pnl_delta` | -1602.1750 | -42.3891 |
| `pnl_unexplained` | 57.8676 | -33.7503 |
| `gross_vega` | -1764.0382 | -12.0730 |
| `gross_gamma` | 348.0224 | 5.7198 |
| `gross_delta` | 0.0000 | 0.0000 |
| `n_open_lots` | 22 | 22 |
| `cost` | 0.0000 | 0.0000 |

NAV paths: listed `[-0.00, 144.23, -456.61]`, projection `[-0.00, 37.07, 46.15]`.

Both books have the same 22 legs (10 names + SPY, straddle = 2 legs each) and
both hedge delta to zero. The greeks differ by 30–60x.

### 4.1 Root cause found: a 100x vega-unit mismatch

Reading the generated schedule back through the new bindings:

```
roll_date=2026-01-02 cohort=1 n_names=10 n_legs=22
target   gross index vega/vol-pt = 10000.0
achieved gross vega/vol-pt       = 20000.0
achieved net   vega/vol-pt       = 2.33e-12      <- construction is sound

sum |vega per vol point|   =    20,000.00
sum |vega per unit vol|    = 2,000,000.00
ratio unit-vol / vol-point =       100.00
```

The single `gross_index_vega` key in `run_spec.tsv` is wired into two
differently-scaled library fields, without conversion:

- `spy_dispersion_backtest.cpp:415` — `build.gross_index_vega_target_per_vol_point = spec.gross_index_vega;`
- `spy_dispersion_backtest.cpp:536` — `config.gross_index_vega = spec.gross_index_vega;` (-> `DispersionConfig::target_vega`, per **unit vol**)
- `spy_dispersion_backtest.cpp:611` — `dispersion.target_vega = spec.gross_index_vega;` (projected-VaR command, same unit as :536)

The library types name their units correctly (`..._per_vol_point` vs
`target_vega`, documented in `dispersion.hpp:157` as "index-leg gross vega the
book scales to", a per-unit-vol quantity). The defect is in the CLI wiring, not
the library. For the same spec, **the listed book is 100x the projection book.**

### 4.2 Correcting the scale does not reconcile the routes

Re-running the projection route at `gross_index_vega * 100`:

| run | nav path | `attr_gamma` | `attr_vega` |
|---|---|---:|---:|
| listed | `[-0.00, 144.23, -456.61]` | 7778.6 | **-1481.6** |
| projection x1 | `[-0.00, 37.07, 46.15]` | 122.0 | 187.5 |
| projection x100 | `[-0.00, 3706.67, 4615.33]` | 12203.4 | **+18751.4** |

Scaling brings gamma attribution to the same order (12,203 vs 7,779) but vega
attribution **has the opposite sign** (+18,751 vs -1,482). The routes are not
the same book beyond a scale factor.

Unseparated candidate causes for the residual:

1. **Strike discretization** — listed snaps to a real strike (e.g. SPY 687.00);
   projection uses the exact ATM forward. At 30 DTE a strike offset moves the
   straddle off ATM, changing the sign of its vega response.
2. **Expiry** — listed uses a real exchange expiry inside a 21–60 DTE window;
   projection anchors at exactly `valuation + 30 calendar days`. Different
   residual `T` on every mark, and gamma/theta scale as `1/sqrt(T)` relative to
   vega, which matches the observed pattern (vega ratio ~31 vs gamma/theta ratio
   ~62 before scaling).
3. **Execution tier** — `ColdReference` vs `Configured`.
4. **Sizing basis** — listed sizes per-contract with `multiplier=100` and
   continuous quantities; projection sizes per-share straddle vega. Interacts
   with (1).

None of these has been isolated. **Three sessions with a single roll cannot
settle it.**

---

## 5. Reproduction

Binary (Release, examples on):

```
cmake --preset rel -DATX_BUILD_EXAMPLES=ON -DCMAKE_MAKE_PROGRAM=<ninja>
cmake --build build-rel --target atxvol_spy_dispersion_backtest
```

Requires a VS 2022 developer environment; `vcvars64.bat` then `cmake`. Ninja is
not on PATH — it lives at
`C:\Users\natha\vcpkg\downloads\tools\ninja-1.13.2-windows\ninja.exe`.

Paired 3-session run:

```
build-corpus         --spec <spec.tsv> --out <run>     # date_hi = 2026-01-06
build-schedule       --run  <run>
run-backtest         --run  <run>                      # -> backtest.tsv         (listed)
run-surface-backtest --run  <run>                      # -> surface_backtest.tsv (projection)
```

Both routes from Python:

```python
import atxvol as av
from atxvol.report.io import read_backtest_tsv

spec  = av.read_run_spec(run + r"\run_spec.tsv")
rows  = av.read_universe(run + r"\universe_schedule.tsv")
clock = av.Clock.from_manifest(av.read_corpus_manifest(run + r"\surface_manifest.tsv"))
uni   = av.universe_at(rows, clock.refs[0].date)

cfg = av.DispersionBacktestConfig()
cfg.target_dte_days  = spec.target_dte_days
cfg.roll_dte_days    = spec.roll_dte_days
cfg.gross_index_vega = spec.gross_index_vega
cfg.delta_band       = spec.delta_band
cfg.min_names        = spec.min_names
cfg.run.unpriced     = av.UnpricedLotPolicy.ERROR

projection = av.run_dispersion_backtest(clock, uni, cfg)          # route B
listed, _, _ = read_backtest_tsv(run + r"\backtest.tsv")          # route A

schedule = av.read_listed_dispersion_schedule(run + r"\trade_schedule.tsv")
```

Artifacts live in the session scratchpad (`scratchpad/paired/run`,
`scratchpad/twoway`) — not durable.

---

## 6. Next steps

**To make the comparison answerable at all:**

1. Pull Databento contract definitions covering 2026-01-02..2026-04-30 (or
   ..07-17 to match OPRA). Needs `DATABENTO_API_KEY`. Without this there is no
   listed route beyond 3 sessions and no agreement claim is possible.
2. Pull OCC ESS reports for the same window, or decide deliberately that the
   deliverable-adjustment check is out of scope for research runs and fix §3.1
   so a no-`occ_ess_root` corpus is a supported configuration rather than a dead
   end.
3. Rebuild the corpus with current code (V2 archives) over the full window and
   re-run both routes.

**Independent of the above:**

4. **Decide the canonical unit for `gross_index_vega`** and convert at the CLI so
   one spec key means one thing. Picking a convention silently rescales every
   run already in `atx-data`, so existing outputs would stop reproducing — this
   is a deliberate call, not a drive-by fix. Sites: `:415`, `:536`, `:611`.
5. Isolate the residual divergence with controlled experiments on the 3-session
   corpus: force the projection route onto the listed strikes, then onto the
   listed expiry, then onto `ColdReference`, one at a time. Each step should
   close a known fraction of the gap; whatever remains after all three is the
   genuine model-vs-listed difference.
6. Add tests for `scatter_chart` and `paired_bar_chart` in
   `python/tests/test_report.py` — viewBox containment, shared-scale invariance,
   and palette-slot stability across clusters.
7. Add a Python gate that runs the projection route and asserts bit-identity
   against a checked-in `surface_backtest.tsv`, locking in §2.2.

**Not recommended:** rendering a comparative report on the current evidence. The
3-session paired run shows disagreement, not agreement, and the sample cannot
distinguish a units defect from a genuine methodological difference.

---

## 7. Files touched

Modified:

- `atx-vol/python/CMakeLists.txt` — added `dispersion.cpp` to `pybind11_add_module`
- `atx-vol/python/src/bindings/module.cpp` — declared and registered `bind_dispersion` last
- `atx-vol/python/src/bindings/backtest.cpp` — added `read_corpus_manifest` / `write_corpus_manifest`
- `atx-vol/python/src/atxvol/report/charts.py` — added `scatter_chart`, `paired_bar_chart`

Added:

- `atx-vol/python/src/bindings/dispersion.cpp`

Reverted (no net change):

- `atx-vol/examples/spy_dispersion_backtest.cpp` — OCC ESS gate relaxation, backed out

No changes were made to library sources under `atx-vol/src/` or
`atx-vol/include/`.
