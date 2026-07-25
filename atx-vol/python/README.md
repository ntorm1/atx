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

## Fitting a surface from quotes

The library's blessed lifecycle is chain → fit → priced surface → archive → book.
The front half is reachable from Python: bring your own quote columns, install
them into an `OptionChain`, fit, and price the whole board as numpy SoA.

```python
import atxvol as av

frame = av.QuoteFrame.from_arrays(
    uid="SPY", snapshot_iso="2026-06-19", spot=600.0, rate=0.043,
    expiry_iso=expiry_iso,                 # one entry per row
    strike=strike, side=side,              # side: int(av.Side.CALL) / int(av.Side.PUT)
    bid=bid, ask=ask,                      # numpy float64 columns
)
chain = av.OptionChain.from_frame(frame, av.MarketEnv.flat(600.0, 0.043, now_ns))

cfg = av.PricerConfig()
cfg.preset = av.FitPreset.ROBUST
cfg.curve_kind = av.VolCurveKind.CONVEX_DENSE   # None => the policy routes it
fitter = av.PricerFitter(cfg)
fitter.fit(chain)

cols = fitter.value_chain(chain, av.OutputField.MODEL_IV | av.OutputField.GREEKS)
cols["model_iv"], cols["vega"]            # numpy arrays aligned with cols["ids"]

priced = fitter.surface().to_priced_surface()   # into the archive / backtest half
```

`av.make_spy_synthetic_panel()` returns a deterministic known-truth board (frame
plus a ready `MarketEnv`) if you want to try the pipeline with no data on hand.

Notes that matter:

- **`value_chain` is bit-identical for any `n_threads`** (disjoint output slots,
  pure const reads). `0` uses the config's thread count, `1` is serial.
  `tests/test_fit.py` pins that from Python with a `tobytes()` comparison.
- **Unrequested columns come back empty**, not zero-filled, so a field you did
  not ask for can never be mistaken for a field that came back zero.
- `value_chain_ids(chain, ids, fields)` prices only a selection, preserving order
  and duplicates — the quote-update path, with work proportional to the selection.
- `fit`, `value_chain` and `to_priced_surface` all release the GIL.
- `OutputField` is a bit set: `MODEL_IV | GREEKS` works exactly as in C++.

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

`RunConfig()` is a passthrough of the engine's `RunConfig{}` — the bindings never
re-declare a default. Python therefore inherits engine-side policy changes rather
than being silently more permissive than the C++ library; in particular
`RunConfig.unpriced` currently defaults to `UnpricedLotPolicy.EXCLUDE_AND_REPORT`,
and a run that must not tolerate unpriced lots sets
`run_cfg.unpriced = av.UnpricedLotPolicy.ERROR` explicitly.
`tests/test_backtest.py::test_run_config_defaults_mirror_the_engine_header` pins
the current values so an engine-side flip is reviewed, not discovered at a call
site.

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

The renderer looks for `surface_pnl_track.tsv`, `pnl_track.tsv`,
`surface_backtest.tsv` and `backtest.tsv` in that order, and merges metadata from
`run_spec.tsv`, `surface_tearsheet.tsv` and the track's own `# key=value` header
(closest to the numbers wins).

### The friction regime is mandatory

`build_report_from_run` and `build_report` both **refuse** a run that carries no
`friction_regime`, raising `ValueError`. This is not a formality: on the pinned
82-session run the same strategy over the same surfaces returns **+247.41**
frictionless, **+12.81** under retail frictions and **-64.60** once square-root
impact is added — roughly 95% friction-dominated, and the sign flips. An
unlabelled headline is misleading rather than merely incomplete, which is why
`write_dispersion_tearsheet` leads both artifacts with `friction_regime` /
`friction_detail` ("THE REGIME IS NOT OPTIONAL METADATA", `dispersion_run.hpp`).

When the key is present the report carries it in four places: a full-width
colour-coded `Banner` under the masthead before any number, a caption on every
headline tile, the masthead byline, and the P&L chart title. Colour is never the
only channel — the banner always prints its own text badge. An unrecognised
regime string still renders, on the neutral `unknown` tone with its raw text as
the badge, so a new engine-side regime name cannot black out a report or borrow
another state's colour.

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

## Errors, batch status, and the GIL

Functions returning `atx::core::Result<T>` raise `atxvol.AtxError` on failure.
The exception carries the structured code as well as the message, so failures can
be dispatched on programmatically rather than by matching prose:

```python
try:
    av.implied_vol(1.0, -1.0, 100.0, 0.5, 0.99, av.Side.CALL)
except av.AtxError as err:
    assert err.code is av.ErrorCode.INVALID_ARGUMENT
```

Vectorized entry points follow the C++ layer's **NaN + per-lane status**
convention instead of aborting the batch on the first bad lane — real chains
carry uninvertible quotes, and discarding the good lanes would make the
vectorized path useless on exactly the data it exists for:

```python
vols, status = av.implied_vol_batch(price, F, K, T, df, av.Side.CALL)
ok = status == av.STATUS_OK        # failed lanes are NaN in `vols`
codes = status[~ok]                # int(ErrorCode) per failed lane
```

A raised exception from a batch function always means the *call* was malformed
(shape mismatch, wrong rank), never that one lane misbehaved.

Long-running American and batch kernels, `run_backtest`, and the TSV writers all
release the Python GIL. `AloPricer.price` deliberately does **not**: it mutates
the pricer's cached exercise boundary, so the GIL is what keeps two Python
threads sharing one pricer from racing in C++. Use one `AloPricer` per thread, or
the batch entry points, for concurrency.

