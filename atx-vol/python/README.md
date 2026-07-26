# atxvol

`atxvol` is the pybind11 wrapper for the C++20 `atx-vol` library. It exposes:

- Black-76 price, Greeks, and implied-volatility inversion
- Andersen-Lake and BAW American pricing, Greeks, and implied vol
- NumPy batch pricing/inversion and cross-strike American pricing
- lightweight SVI/eSSVI slices and interpolated surfaces
- calibration-grade `VolSurface` parameters and evaluators
- priced surfaces (`CurveSurface` -> `PricedSurface`) and the on-disk `SurfaceDb`
- the declarative strategy DSL (`StrategySpec`, `DeclarativeStrategy`) and the
  dispersion-strangle spec builder
- the backtest engine (`Clock`, `RunConfig`, `run_backtest`) with every result
  column exposed as a NumPy array
- backtest analytics: `tearsheet` plus the deterministic TSV exports

## Build and install

Build from an environment where `VCPKG_ROOT` points at the vcpkg installation:

```powershell
cd C:\atx\atx-vol\python
python -m pip install .
```

For an editable development install with tests:

```powershell
python -m pip install -e ".[test]"
python -m pytest tests
```

The default suite is fast and self-contained: it runs against committed fixtures
(`tests/data/`) and never shells out to the C++ pipeline. A second `slow` tier
holds the contracts only the built CLI can demonstrate — that `run-backtest`
publishes `run.atxrun` and no longer writes the loose result TSVs. It is
deselected by default (`addopts = -m 'not slow'` in `pyproject.toml`) and needs
`build-rel\bin` plus the local paired run fixture:

```powershell
python -m pytest tests -m slow
```

Run pytest from this directory (`atx-vol/python`); from the repository root the
project's `pyproject.toml` is not the rootdir and nothing is collected.

The build uses scikit-build-core, CMake, Ninja, clang-cl, and pybind11. It reuses
the monorepo's `build-rel/vcpkg_installed` or `build/vcpkg_installed` tree when
present and otherwise lets vcpkg install the manifest dependencies.

## Quick start

```python
import math
import numpy as np
import atxvol

F, K, T, sigma, r = 102.0, 100.0, 0.5, 0.25, 0.04
df = math.exp(-r * T)

price = atxvol.black76_price(F, K, T, sigma, df, atxvol.Side.CALL)
iv = atxvol.implied_vol(price, F, K, T, df, atxvol.Side.CALL)
greeks = atxvol.black76_greeks(F, K, T, sigma, r, df, atxvol.Side.CALL)

prices = atxvol.black76_price_batch(
    np.array([F, F]),
    np.array([95.0, 105.0]),
    np.array([T, T]),
    np.array([sigma, sigma]),
    np.array([df, df]),
    atxvol.Side.CALL,
)
```

## Backtesting

The full `examples/spy_dispersion_pnl.cpp` pipeline is available from Python —
`SurfaceDb` -> `Clock` -> `make_dispersion_strangle_spec` ->
`DeclarativeStrategy` -> `run_backtest` -> `tearsheet` ->
`write_backtest_pnl_tsv` — and the resulting TSV feeds
`tools/spy_dispersion_pnl_report.py` unchanged:

```python
import atxvol as av

db = av.SurfaceDb.open(r"C:\path\to\surface-db")
clock = av.Clock.from_surface_db(db)

cfg = av.DispersionStrangleConfig()
cfg.names = ["AAPL", "MSFT", "NVDA", "AMZN"]
cfg.index_symbol = "SPY"
cfg.target_abs_delta = 0.40
cfg.tenor_days = 90.0
cfg.hold_to_expiry = True
cfg.hedge = av.HedgeSpec(av.HedgeSpec.Kind.DELTA_TO_ZERO, av.HedgeSpec.Cadence.DAILY, 0.0)

run_cfg = av.RunConfig()
run_cfg.snapshot_cache = av.SnapshotCache()

result = av.run_backtest(
    clock, av.DeclarativeStrategy(av.make_dispersion_strangle_spec(cfg)), run_cfg
)
sheet = av.tearsheet(result)
print(sheet.total_return, sheet.sharpe, sheet.max_drawdown)

series = result.to_dict()          # every column as a NumPy array
av.write_backtest_pnl_tsv(result, {"strategy": "spy_dispersion_vega_flat"}, "pnl_track.tsv")
```

A corpus can also be authored from Python: build a `CurveSurface` with
`push_essvi`, seal it via `PricedSurface.create`, and archive a partition with
`SurfaceDb.write_partition([(symbol, surface), ...])`. Note that
`PricedSurface.create` **moves from** its `CurveSurface` argument, which is left
empty — build a fresh one per surface.

## Reporting

`atxvol.report` is a small component library — masthead, numbered sections, stat
rows, tables, figures, small-multiple grids — plus hand-rolled SVG charts, over
one light theme. Pure stdlib: a rendered report is a single HTML file with no
external assets and no network access.

```python
from atxvol.report import Report, Section, Figure, Table, Column, charts
from atxvol.report.charts import Series

svg = charts.line_chart([Series("NAV", nav, area=True, label_end=True)], dates)
report = Report(title="Backtest", eyebrow="atx-vol")
report.add(Section("Result", body=[Figure(svg, title="Cumulative P&L")]))
report.write("report.html")
```

To render a `spy_dispersion_backtest` run directory directly:

```python
from atxvol.report.dispersion import build_report_from_run
build_report_from_run("runs/golden-run", "pnl_track.html")
```

The series palette in `atxvol.report.theme` is validated, not chosen by eye — it
passes the lightness-band, chroma-floor, colorblind-separation (worst adjacent
ΔE 8.6), normal-vision (ΔE 17.2) and 3:1 contrast checks against the theme's own
surface. **The slot order is the colorblind-safety mechanism**: reordering
`theme.SERIES` weakens the adjacent-pair separation, so re-run the validator if
you change it. `series_color` raises past the last slot rather than cycling.

### Building a `BacktestResult` by hand

The result columns are writable so a TSV can be read back and re-folded by the
library's own `tearsheet`. Because every consumer indexes all columns by the row
count, **call `resize(n)` before assigning columns**:

```python
result = atxvol.BacktestResult()
result.resize(len(rows))
result.date = [...]
result.nav = [...]
result.validate()           # raises if any column length disagrees
```

`tearsheet` and both TSV writers validate their input and raise `ValueError` on
a ragged result rather than reading out of bounds.

Functions returning `atx::core::Result<T>` raise `atxvol.AtxError` on failure.
Long-running American and batch kernels, `run_backtest`, and the TSV writers all
release the Python GIL.

## Known limitation: `atxvol` and `pyarrow` cannot share a process on Windows

**Symptom** (search-friendly): importing `atxvol` and `pyarrow` in the same
process — in either order — fails with

```
ImportError: DLL load failed while importing _core: The specified procedure could not be found.
```

or, if `pyarrow` happens to import first, the identical failure moves to
`pyarrow` instead:

```
ImportError: DLL load failed while importing lib: The specified procedure could not be found.
```

**Why this happens, and why reordering the imports will not fix it.** `atxvol._core`
now dynamically links vcpkg's `arrow.dll`/`parquet.dll` (pulled in by the OPRA hive
loader that backs `atxvol.build_surface_db` — this is new; earlier bindings that
never touched hive/parquet data did not need these DLLs). Separately, the `pyarrow`
wheel bundles its **own**, differently-versioned copies of DLLs with the exact same
base filenames. On Windows, once a DLL of a given name is loaded anywhere in a
process, the loader resolves any later same-name import to that already-resident
copy — regardless of search path, `PATH`, or the importing module's own directory.
So whichever of `atxvol._core` / `pyarrow.lib` imports first claims the process-wide
`arrow.dll`/`parquet.dll` slot, and the other one fails looking for an exported
symbol that isn't in that (wrong-version) copy. This has been verified in **both**
import orders — it is a genuine ABI incompatibility between the two vendored Arrow
builds, not an import-ordering bug, so swapping which import comes first only moves
the failure, it does not remove it.

**Why this matters:** design spec §5's stated goal for these bindings is "so a
notebook can build and query the same db the C++ tools produce." A real analysis
notebook is very likely to also import `pyarrow` (directly, or transitively via
`pandas`), so this is a product-level limitation on the intended workflow, not just
a test-environment quirk.

**Known real fixes (neither implemented yet):**
- A `delvewheel`-style repair step on the built wheel/extension that renames the
  vendored DLLs (content-hash suffix) and patches `_core`'s import table to match,
  so its copies never collide with another package's same-named DLLs. This is the
  standard fix for this exact class of Windows wheel problem.
- Statically link Arrow/Parquet into `atx-vol` (a vcpkg triplet change) so `_core`
  has no runtime dependency on a shared `arrow.dll`/`parquet.dll` at all.

**Workaround available today:** don't import `atxvol` and `pyarrow` in the same
process. If you need to produce OPRA-hive parquet files and then build/query a
surface db from them, do the parquet-writing step in a separate Python process
(e.g. `subprocess.run([sys.executable, "-m", ...])`) that never imports `atxvol`,
then run `atxvol.build_surface_db(...)` in a process that never imports `pyarrow`.
`atx-vol/python/tests/test_surface_db_build.py` follows exactly this pattern: its
synthetic-hive fixture writer lives in the standalone script
`atx-vol/python/tests/_gen_opra_hive.py`, invoked as a subprocess, specifically so
the test process itself only ever imports `atxvol`.

