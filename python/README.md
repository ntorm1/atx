# atxpy

A high-quality, NumPy/pandas-native Python wrapper for **atx-engine**, the C++
quant research + backtesting engine. Built with pybind11 (thin `atxpy._core`
extension) plus a hand-written Pythonic facade (`atxpy.*`). Preserves the engine's
determinism and no-look-ahead guarantees across the language boundary.

## What's covered

| Area | Python surface | Engine module |
|------|----------------|---------------|
| Exact money / symbols | `Decimal`, `Symbol`, `SymbolTable` | atx-core |
| Backtest | `run_backtest(bars_df, signals=… | alpha=…)` → equity/fills DataFrames | loop / exec / portfolio |
| Alpha DSL | `compile_alpha`, `evaluate_alpha`, `parse_expr`/`analyze`/`compile` | alpha |
| Strategy from DSL | `run_backtest(..., alpha="rank(delta(close,1))")` | alpha + loop (VmSignalSource) |
| Performance eval | `performance(result)`, `return_metrics`, `eval.deflated_sharpe`, `eval.pbo`, `eval.cpcv_folds` | eval |
| Portfolio risk | `factor_model`, `optimize`, `FactorModel`, `PortfolioOptimizer` | risk |
| Alpha mining | `mine_alphas(panel, seed_exprs=…)` → `MineResult` (report + pool) | factory |
| Combination | `combine_pool(pool, method=…)` | combine |
| ML alphas | `learn.elastic_net`, `learn.fit_linear`, `learn.predict_at` | learn |
| Multi-strategy book | `fund.sleeve_cov`, `fund.meta_allocate` | fund |

## Build (development)

Run from a **VS 2022 Developer shell** (so `clang-cl` + the MSVC environment are
present), with `VCPKG_ROOT` set:

```powershell
python -m pip install scikit-build-core pybind11 ninja pytest numpy pandas
python -m pip install -e ./python --no-build-isolation
python -m pytest python/tests -v
```

`pyproject.toml` pins the vcpkg toolchain, `x64-windows` triplet, `clang-cl`, and
reuses the monorepo's `build/vcpkg_installed` tree (arrow/parquet/zstd/openssl).
The first build compiles the whole monorepo (atx-core / atx-tsdb / atx-engine) plus
the extension, so expect several minutes cold; later builds are incremental.

## End-to-end example

```python
import numpy as np, pandas as pd, atxpy

# 1. A long-format bars DataFrame (timestamp = int64 unix-nanos).
rows = []
for s, sym in enumerate(["AAA", "BBB"]):
    for i in range(60):
        rows.append(dict(symbol=sym, timestamp=i * 86_400_000_000_000,
                         open=100., high=101., low=99.,
                         close=100. + (i if s == 0 else -i), volume=1e6))
bars = pd.DataFrame(rows)

# 2. Backtest a DSL alpha as the strategy.
res = atxpy.run_backtest(bars, symbols=["AAA", "BBB"],
                         alpha="rank(delta(close, 1))")
print(res.equity_curve.tail())   # DataFrame[timestamp, equity, gross, net]
print(res.fills.head())          # DataFrame[symbol, qty, price, fee, impact, timestamp]

# 3. Evaluate performance straight off the result.
m = atxpy.performance(res)
print(m.sharpe, m.max_dd, m.hit_rate)

# 4. Evaluate an alpha over a research panel (dates x instruments).
closes = np.random.default_rng(0).normal(100, 1, size=(120, 8)).cumsum(0)
signal = atxpy.evaluate_alpha("rank(ts_mean(close, 5))", {"close": closes})

# 5. Mine alphas, then combine the pool.
mined = atxpy.mine_alphas({"close": closes, "rev": -np.diff(closes, axis=0, prepend=closes[:1])},
                          seed_exprs=["rank(close)", "rank(rev)", "delta(close, 1)"],
                          master_seed=1, generations=3, min_sharpe=0.0, min_fitness=0.0)
print(mined.report.admitted, mined.report.digest)
if mined.pool.n_alphas >= 2:
    weights = atxpy.combine_pool(mined.pool)

# 6. Portfolio optimization with a factor model.
fm = atxpy.factor_model(X=np.array([[1.0], [0.5], [-2.0]]),
                        F=np.array([[0.04]]), D=np.array([0.1, 0.2, 0.05]))
w = atxpy.optimize(alpha=[0.02, -0.01, 0.03], model=fm, gross_leverage=1.0)

# 7. ML alpha (deterministic elastic-net).
beta = atxpy.learn.elastic_net(X=np.random.normal(size=(200, 4)),
                               y=np.random.normal(size=200))

# 8. Allocate capital across strategy sleeves.
omega = atxpy.fund.sleeve_cov([np.random.normal(size=200) for _ in range(3)])
capital = atxpy.fund.meta_allocate(omega, max_gross=2.0)
```

## Layout

- `src/atxpy/` — the Python package: facade modules (`backtest`, `alpha`, `eval`,
  `risk`, `mining`, `learn`, `fund`), re-exports, and type stubs (`_core.pyi`).
- `src/_bindings/` — the C++ pybind11 translation units (one per engine area) plus
  `shim/` (the template-erasing, lifetime-owning C++ shims for backtest + mining).
- `tests/` — pytest suite (parity, determinism, no-look-ahead, lifetime).

## Design notes

- **Determinism preserved:** the engine's fixed-index reductions, FIFO settle, and
  RNG-free search are untouched; the wrapper only marshals data in and values out.
  Repeated runs are byte/scalar-identical (asserted in tests).
- **Lifetime safety:** templated `RollingPanel<Cap>`/`BacktestLoop<Cap>` are hidden
  behind C++ shims that own the entire collaborator graph (runtime Cap dispatch), so
  no non-owning pointer crosses into Python; borrowed spans are copied to owned NumPy
  arrays at the boundary.
- **Exact money:** `Decimal` is bound as a real type (exact nano fixed-point); money
  is never silently floated.
