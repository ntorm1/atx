# atxvol

`atxvol` is the pybind11 wrapper for the C++20 `atx-vol` library. It exposes:

- Black-76 price, Greeks, and implied-volatility inversion
- Andersen-Lake and BAW American pricing, Greeks, and implied vol
- NumPy batch pricing, Greeks, fused value+vega, and inversion for both Black-76
  and American, plus cross-strike American pricing
- lightweight SVI/eSSVI slices and interpolated surfaces
- calibration-grade `VolSurface` parameters and evaluators
- priced surfaces (`CurveSurface` -> `PricedSurface`) and the on-disk `SurfaceDb`
- the declarative strategy DSL (`StrategySpec`, `DeclarativeStrategy`) and the
  dispersion-strangle spec builder
- the backtest engine (`Clock`, `RunConfig`, `run_backtest`) with the recorded
  series and the run's signals exposed as NumPy arrays
- backtest analytics: `tearsheet` plus the deterministic TSV exports

Not the swap lane. `BacktestResult`'s `swap_pv` and `swap_pnl`, and the eight
`swap_explain_*` P&L-attribution columns, have no binding and are not reachable
from Python — `to_dict` and the per-column properties both hand-list what they
expose, and the swap columns are on neither list. The C++ example
`examples/varswap_compare_example.cpp` is the entry point that produces them: it
sets `RunConfig::swap_pnl_explain` and attaches them as signal columns before
writing its TSV, which is what feeds the renderer's attribution panel. A reader
who wants the explain panel wants that track, not this API. See
"The three `RunConfig` fields Python does not get" for why the flag itself is
unbound.

## Build and install

This is a **standalone** scikit-build-core project (its own `project()`, its own
build tree) that `add_subdirectory`s the entire monorepo. Building the wheel
builds atx-core and atx-vol too — around 190 targets in Release. Budget minutes,
not seconds, and read the next section before you start one.

### It looks like a hang. It is not.

`pip` captures build output. Without `-v` you get

```
Building wheel for atxvol (pyproject.toml) ...
```

and then **nothing at all** — no compiler lines, no progress, no error — until
the whole build finishes. Every expensive step below is silent, so with the
documented invocation the first honest signal is the exit code, tens of minutes
later. This has cost more than one person an hour of deciding whether to kill it.

- **Always pass `-v`.**
- If a run is already going without it, look at whether
  `atx-vol/python/build/<wheel-tag>/` is still growing before concluding it is
  stuck. Silence is the normal state, not evidence of a stall.

Durations quoted here are log facts from a shared developer box, not benchmarks:
treat them as orders of magnitude. With dependencies already populated, ccache
warm and `-j 2`: configure 7.1 s, complete wheel 6m53s.

### What it needs before it can be fast

**An MSVC environment.** `pyproject.toml` pins `clang-cl`, and CMake needs the
Windows SDK's `mt.exe` on `PATH` to link even its compiler probe. Run everything
from a `vcvars64.bat` shell. (`scripts/atx-build.ps1` does this for the monorepo
build; `pip` does not do it for you.)

**A vcpkg tree.** The manifest is arrow[parquet], openssl, zstd, zlib-ng and
gtest. `CMakeLists.txt` reuses `<repo>/build-rel/vcpkg_installed` or
`<repo>/build/vcpkg_installed` when either exists — **a fresh worktree has
neither**, and vcpkg then builds all of it from source into the wheel's build
tree. That is ~1.1 GB installed and by far the longest thing that can happen
here. Point it at a tree you already have.

**A populated FetchContent tree.** The root project clones spdlog, tl-expected,
unordered_dense, Eigen and xsimd, plus databento's json/httplib/date: nine git
clones on a cold tree, also silent (49 s when measured here). `$ATX_DEPS_DIR` —
or `FETCHCONTENT_BASE_DIR` — points them at a shared cache.

**A parallelism cap, on a shared or memory-limited box.** `pip` passes no `-j`,
so ninja uses every core, and a heavy clang-cl TU in this tree holds 1-3 GB.
`CMAKE_BUILD_PARALLEL_LEVEL` is the only lever you have from here.

### The recipe

From a `vcvars64` shell, in `<repo>\atx-vol\python`:

```powershell
$env:CMAKE_BUILD_PARALLEL_LEVEL = "2"
$env:SKBUILD_CMAKE_DEFINE = "VCPKG_INSTALLED_DIR=<abs>/vcpkg_installed;FETCHCONTENT_BASE_DIR=<repo>/deps/py"

python -m pip wheel . --no-deps -w dist -v
# Resolves whatever `pip wheel` just built -- no version, ABI or platform tag to
# go stale. --force-reinstall: without it pip calls the installed copy already
# satisfied and does nothing. --no-deps: --force-reinstall re-resolves
# dependencies too, and `dist` holds only this wheel.
python -m pip install --no-index --find-links dist --force-reinstall --no-deps atxvol
```

Two steps rather than `pip install .` on purpose: `pip install .` **replaces
whatever `atxvol` is already installed** — including an editable install
pointing at a different checkout — and it does that before you know whether the
build even works. Building the wheel first keeps a failed build from taking your
working install with it.

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

### Iterating on the extension only

When you only need `_core` (which is all `atx-vol/tests/CMakeLists.txt`'s
`atx-vol-python` ctest lane consumes), skip packaging and drive CMake directly.
Configure measured at 14.3 s against a populated dependency tree:

```powershell
cmake -S . -B build/py -GNinja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_INSTALLED_DIR=<abs>/vcpkg_installed `
  -DFETCHCONTENT_BASE_DIR=<repo>/deps/py `
  -DATX_BUILD_TESTS=OFF -DATX_BUILD_BENCH=OFF -DATX_BUILD_EXAMPLES=OFF
cmake --build build/py --target _core -j 2
```

Copy the resulting `_core.*.pyd` beside the package at `src/atxvol/`; the ctest
lane imports it from there.

### One thing that is *not* the cause

A silent 23-minute configure was once attributed to `FetchContent` re-running its
nested subbuilds, with `-DFETCHCONTENT_FULLY_DISCONNECTED=ON` recommended as the
fix. That does not survive measurement: the same configure with no such flag,
against the same populated dependency tree, completes in 14.3 s, and the update
steps account for about 3 s of it. Do not adopt the flag as a habit — it makes
population impossible, so it fails outright on a genuinely fresh tree. The silent
minutes are pip's output capture, vcpkg, and the first clone, in that order.

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

## Numpy-native batch American and surface grids

Chain-scale American valuation goes through the C++ SoA batch in one call rather
than a Python loop over the scalar entry points. All of these use the NaN +
per-lane status convention above.

```python
prices, status = av.american_price_batch(S, K, T, sigma, r, q, side)
g = av.american_greeks_batch(S, K, T, sigma, r, q, side)     # dict of SoA columns
ivs, status = av.american_implied_vol_batch(price, spot, K, T, r, q, av.Side.PUT)

cols = priced_surface.grid(K, T, side)   # iv / total_variance / fair_value / greeks
```

These are **parity-exact**, not approximate: `american_price_batch` on the
`SimdIsa.FORCE_SCALAR` route is bit-identical to a loop over `american_price`
**for every `method` / `opts` the signature admits**,
`american_greeks_batch(analytic=False)` is bit-identical to a loop over
`american_greeks_fd`, `american_implied_vol_batch` is bit-identical to a loop
over `american_implied_vol`, and `PricedSurface.grid` is bit-identical to the
per-point `iv` / `total_variance` / `fair_value` / `greeks` calls.
`tests/test_batch.py` pins all four with `tobytes()` / `==` comparisons.

`american_price_batch` routes on engagement to keep that claim true. The default
engagement (`method=ANDERSEN_LAKE`, `opts=None`) takes the laned C++ batch; any
other `method` or an engaged `AlOpts` takes the exact scalar `american_price` per
lane inside the same single GIL release, because the laned entry point has no
channel for either knob. So `method=BAW` returns the BAW price rather than
silently returning the Andersen-Lake one, at the cost of the pack dispatch;
`isa` selects the laned kernel and therefore does nothing on the scalar route.

What the batch buys is structural: one pybind dispatch and one GIL release for
the whole book instead of one per contract, with the kernel free to group the
genuine early-exercise lanes into a single pack. The magnitude of that win is
hardware- and load-dependent — measure it on your own quiet host rather than
trusting a number quoted here.

### Black-76 Greeks, fused value+vega, and pricing from precomputed log-moneyness

The three remaining public batch kernels, on the same one-call shape. With these
every entry in `batch.hpp` is reachable from Python:

```python
g = av.black76_greeks_batch(F, K, T, sigma, r, df, side)   # dict of SoA columns
value, vega = av.black76_value_and_vega_batch(F, K, T_scalar, sigma, df, side)
px = av.black76_price_from_lnfk_batch(F, K, T_scalar, sqrt_t, sigma, df_scalar, ln_fk, side)
```

`black76_greeks_batch` returns `delta`/`gamma`/`vega`/`theta`/`rho`/`vanna`/
`volga`/`charm` plus `price`, all computed in one pass off shared `d1`/`d2`.
`black76_value_and_vega_batch` keys on **one expiry slice**: `T` and `sqrt_t` are
shared scalars while `F`, `K`, `sigma`, `df` are per-lane columns. `sqrt_t >= 0`
is used as given; the default `-1.0` is the kernel's own "compute `sqrt(T)`"
sentinel. `side` on any of them is a per-lane integer column *or* a single `Side`
broadcast across the batch.

`black76_price_from_lnfk_batch` is the bind-step shortcut: a caller that already
holds `ln(F/K)` and `sqrt(T)` for a slice — as the portfolio engine does — passes
both instead of paying for them again. `T`, `sqrt_t` and `df` are shared scalars;
`F`, `K`, `sigma`, `ln_fk` are per-lane. **`sqrt_t` is required and has no
sentinel** — unlike the fused batch, this kernel consumes it verbatim, so there
is no negative value that means "recompute". The scalar companion
`black76_price_from_lnfk` is bound too.

**None of the three returns a `status` column, and that is the convention rather
than an exception to it.** A parallel status exists where a kernel has a per-lane
failure channel that the binding must not erase. These kernels have none:
`black76_greeks`, `black76_value_and_vega` and `black76_price_from_lnfk` are
total functions, so a degenerate lane (`T <= 0` or `sigma <= 0`) collapses to the
documented degenerate result — intrinsic-step delta, other Greeks zero — instead
of failing. A column of `STATUS_OK` would advertise a diagnostic that carries no
information. Only a malformed *call* raises: a shape or rank mismatch
(`ValueError`), or a float / unrecognised `side` code (`AtxError` with
`ErrorCode.INVALID_ARGUMENT`).

**Greeks and value+vega agree with their scalar kernels to a tolerance, not
bit-for-bit; from-lnFK is bit-identical.** The first two dispatch to the 4-lane
AVX2 route at `n >= 4` on an AVX2 host, whose interior lanes use deterministic
vector transcendentals. The C++ gate is **per output column**: ~1e-6 absolute +
1e-7 relative on prices and Greeks, and ~1e-5 absolute on the fused batch's
`vega` (vega is the larger quantity, so the same relative error is a looser
absolute one). `tests/test_batch.py` tracks those exact numbers rather than
asserting anything tighter than the library conforms to.
`black76_price_from_lnfk_batch` has no vector kernel at all, so it is pinned with
`tobytes()` against a scalar loop. The American batch entry points above keep
their own bit-identity claims as stated there — `american_price_batch` on the
`SimdIsa.FORCE_SCALAR` route, `american_greeks_batch(analytic=False)` and
`american_implied_vol_batch` outright.

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
env = av.MarketEnv.flat(600.0, 0.043, now_ns)
# Optional validated non-flat zero-rate curve. Empty means env.flat_rate.
env.yield_curve = av.YieldCurve.create(
    [0.05, 0.25, 1.0, 2.0],
    [0.041, 0.042, 0.044, 0.045],
)
chain = av.OptionChain.from_frame(frame, env)

cfg = av.PricerConfig()
cfg.preset = av.FitPreset.ROBUST
curve = av.CurveConfig()
curve.kind = av.VolCurveKind.CONVEX_DENSE
curve.convex.lambda_ = 2.5e-3
curve.convex.node_cap = 32
curve.parametric.huber_k = 1.25
cfg.curve = curve                    # None => the profile policy routes it
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
- `CurveConfig` exposes its value-owning convex, parametric and spline knobs.
  `SplineFitOpts.grid` is read-only because the C++ field is a borrowed span
  over the library's standard grid; the remaining spline knobs are writable.

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
run_cfg.reconcile_nav = True
run_cfg.book_entry_fill_slippage = True
run_cfg.reconcile_nav_tol = 1.0e-6
run_cfg.financing.share_dividends = [
    av.ShareDividend(uid=123, ex_ts_ns=ex_date_ns, amount=0.42)
]

result = av.run_backtest(
    clock, av.DeclarativeStrategy(av.make_dispersion_strangle_spec(cfg)), run_cfg
)
sheet = av.tearsheet(result)
print(sheet.total_return, sheet.sharpe, sheet.max_drawdown)

series = result.to_dict()          # every column as a NumPy array
series["nav_liquidation"]          # populated when reconcile_nav is enabled
av.write_backtest_pnl_tsv(result, {"strategy": "spy_dispersion_vega_flat"}, "pnl_track.tsv")
```

`Clock.between(date_lo, date_hi)` carves a run window out of a db-backed clock.
Both ends are **inclusive**, bounds outside the corpus **clamp** (asking for
`"2020-01-01".."2030-01-01"` is the whole corpus, not an error), and the source
clock is unchanged. A window that selects no partition — an inverted pair, or a
real gap the corpus does not cover — raises `AtxError` with
`ErrorCode.INVALID_ARGUMENT` whose message names the **available** range, so the
window can be corrected without dumping the manifest:

```python
clock = av.Clock.from_surface_db(db).between("2026-01-06", "2026-01-09")
result = av.run_dispersion_backtest(clock, universe, cfg)
```

`tests/test_surface_db_dispersion.py` runs that whole composition —
`SurfaceDb` -> `Clock.from_surface_db` -> `between` -> `run_dispersion_backtest` —
over a synthetic db it builds through these bindings.

`RunConfig()` is a passthrough of the engine's `RunConfig{}` — the bindings never
re-declare a default. Python therefore inherits engine-side policy changes rather
than being silently more permissive than the C++ library; in particular
`RunConfig.unpriced` defaults to `UnpricedLotPolicy.ERROR`, so a lot the pricer
could not value aborts the run rather than leaving the book quietly. A run that
would rather carry on and account for the gap sets
`run_cfg.unpriced = av.UnpricedLotPolicy.EXCLUDE_AND_REPORT` explicitly and reads
the reported count.
`tests/test_backtest.py::test_run_config_defaults_mirror_the_engine_header` pins
the current values so an engine-side flip is reviewed, not discovered at a call
site.

### The three `RunConfig` fields Python does not get

`RunConfig` has **eighteen** fields in the engine (`backtest.hpp` pins the count
with a `static_assert`). The binding hand-lists **fifteen**. The three it leaves
out are the per-step run-control pair plus one output opt-in. The pair are
omitted by a decision recorded at the C++ declaration rather than by oversight;
the third is not, and says so:

- **`step_observer` — there is no progress or observation hook in the Python API
  at all.** Not a slower one, not a coarser one: none. `StepObserver` is a
  `std::function<Status(const StepEvent &)>`, and every member of the `StepEvent`
  it receives (`snapshot`, `strategy`, `ref`) is borrowed and valid only for the
  duration of that one call. There is no honest pybind11 translation of that
  lifetime, so nothing is exposed instead of exposing something that could hand
  Python a dangling reference mid-run.
- **`cancel` — a Python `run_backtest` cannot be asked to stop.** `CancelToken`
  holds a raw pointer to a caller-owned `std::atomic<bool>`; there is no owner on
  the Python side for that flag to belong to. The call runs to completion or
  raises.
- **`swap_pnl_explain` — the swap lane's P&L explain cannot be switched on from
  Python.** Unlike the pair above, nothing resists binding it: it is a plain
  `bool` with no lifetime or ownership obstacle, and no C++ declaration records a
  decision to leave it out. It is unbound, not ruled out. The consequence is that
  a Python run always leaves `BacktestResult::swap_explain_*` empty, so the swap
  explain columns cannot be produced from Python at all — see the note under the
  feature list at the top of this file for the entry point that does produce
  them.

The practical shape of the first two: a Python backtest is one blocking call that
either returns a whole `BacktestResult` or raises `atxvol.AtxError`. Plan for the
run length up front — there is no mid-run readout to watch and no way to
interrupt it from inside the API. Long runs belong in a subprocess you can
signal.

Binding either run-control knob means designing a new API (a lifetime-safe event
object; an owned cancellation handle), so both are **post-v1 candidates**,
deliberately not smuggled in as part of a documentation pass. The C++ path keeps
both.

The projection-specific `DispersionBacktestConfig` exposes the same material
controls as C++: side, multiplier, risk limits, weighting and strike policies,
hedge kind/cadence/band, and the nested `RunConfig`. Both
`make_dispersion_backtest_strategy` and `run_dispersion_backtest` accept either
a frozen `DispersionUniverse` or an effective-dated `list[UniverseRow]`; the
latter re-resolves membership point-in-time on every step. Listed replay accepts
explicit `ScheduleMarkPolicy` and `ScheduleFillPolicy` arguments, including
quote-mid and cross-spread fills.

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

To render a shipped `spy_dispersion_backtest` run directory directly:

```python
from atxvol.report.dispersion import build_report_from_run
build_report_from_run("runs/golden-run", "pnl_track.html")
```

The renderer first opens `run.atxrun` and reads its `backtest` section. This is
the output published by the shipped `run-backtest` CLI; no loose result TSV is
required. For older runs it falls back to `surface_pnl_track.tsv`,
`pnl_track.tsv`, `surface_backtest.tsv` and `backtest.tsv` in that order. It
merges metadata from `run_spec.tsv`, the effective `run_config.tsv`,
`surface_tearsheet.tsv`, and the archive or track metadata (closest to the
numbers wins).

`run_config.tsv` is not a diagnostic and is not gated by any verbosity flag.
Neither is `quote_rejects.tsv`. Both are **provenance rather than commentary** —
evidence about what the run *was*, not a report about how it went — and that
classification is a ruling, not an accident: it is why they are written
unconditionally and why they stay that way.

Each has exactly one writer, and no route writes both. `run-backtest`
(`run_backtest_command`, `tools/spy_dispersion_backtest.cpp:535`) writes
`run_config.tsv`; `build-schedule` (`build_schedule_command`, same file, `:374`)
writes `quote_rejects.tsv`.

The render-safety claim attaches to `run_config.tsv` **alone**: it is the sole
carrier of `friction_regime` for a run, which is exactly why the next section
can refuse a run without it — suppress it and a pipeline that expects to render
a report afterwards will fail. `quote_rejects.tsv` is the schedule-admission
audit trail and has **no programmatic reader today**; it is kept because an
admission decision nobody recorded is one nobody can audit, not because anything
downstream parses it.

Legacy loose TSV input is schema-aware: required economics must be present or
the renderer refuses the run instead of folding binding-created zero
placeholders. Optional risk/counter omissions are called out as unavailable and
their panels are omitted.

### The friction regime is mandatory

`build_report_from_run` and `build_report` both **refuse** a run that carries no
`friction_regime`, raising `atxvol.AtxError` with
`code == ErrorCode.INVALID_ARGUMENT` — the same coded channel as the rest of this
API, so `except atxvol.AtxError` catches it. This is not a formality: on the pinned
82-session run the same strategy over the same surfaces returns **+24740.62**
frictionless, **+1280.83** under retail frictions and **-6460.23** once square-root
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
(shape mismatch, wrong rank, or an unrecognised `side` code), never that one lane
misbehaved.

Three batch-specific details:

- **A batch function with no per-lane failure mode returns no `status` column.**
  `black76_price_batch`, `black76_greeks_batch`, `black76_value_and_vega_batch`
  and `black76_price_from_lnfk_batch` wrap total kernels — a degenerate lane
  collapses to the documented degenerate result rather than failing — so there is
  nothing for a status column to say. The convention above is about not *erasing*
  a channel the kernel has; it is not a requirement to invent one.

- **`PricedSurface.grid` has lossless per-family channels.** Read
  `iv_status`/`iv_valid`, `total_variance_status`/`total_variance_valid`,
  `fair_value_status`/`fair_value_valid`, and
  `greeks_status`/`greeks_valid`; each Greek additionally has its own
  `<name>_valid` mask. Families fail independently. The compatibility
  `status` column is retained as the first failure in that order, but new code
  should not use it to decide which columns are usable. Because the C++ IV and
  variance queries return bare doubles, a non-finite value maps to
  `ErrorCode.OUT_OF_RANGE`; the Result-returning families retain their exact
  codes.
- **`american_price_batch` / `american_greeks_batch` carry a distinct two-state
  regime channel.** Their kernel reports `LaneStatus::Ok | Unsupported`, not an
  `atx::core::Status`; `Unsupported` maps to the dedicated negative
  `AMERICAN_BATCH_UNSUPPORTED_REGIME`, so it cannot be confused with a genuine
  `ErrorCode.NOT_IMPLEMENTED`. `implied_vol_batch` and
  `american_implied_vol_batch` carry the true per-lane code, as does
  `american_price_batch` on its non-default `method` / `opts` route (which runs
  the scalar pricer and therefore has real codes to report).

Long-running American and batch kernels, `run_backtest`, and the TSV writers all
release the Python GIL. `AloPricer.price` deliberately does **not**: it mutates
the pricer's cached exercise boundary, so the GIL is what keeps two Python
threads sharing one pricer from racing in C++. Use one `AloPricer` per thread, or
the batch entry points, for concurrency.

`PricerFitter` is the other mutating object, and it is handled differently
because it cannot use the same answer. `PricerFitter.fit` mutates (it stores the
surface, replacing any prior fit) while `value_chain` is const and internally
parallel — and `value_chain` is long enough that it *must* release the GIL, at
which point the GIL no longer serializes anything against it. So the binding
carries its own reader/writer lock: `fit` and `set_threads` take the writer lock,
`value_chain` / `value_chain_ids` / `surface` / `fitted` take the reader lock,
and every one of them releases the GIL first. Concurrent `value_chain` calls on
one fitter therefore run together, as `pricer_fitter.hpp` promises, and a
concurrent `fit` waits for them instead of freeing the surface underneath them.
Distinct fitters never contend.

`PricerFitter.surface()` returns a handle that **co-owns** its generation, so a
later `fit()` publishes a new generation without invalidating a handle you still
hold, and the handle outlives the fitter itself.

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

