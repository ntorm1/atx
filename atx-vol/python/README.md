# atxvol

`atxvol` is the pybind11 wrapper for the C++20 `atx-vol` library. It exposes:

- Black-76 price, Greeks, and implied-volatility inversion
- Andersen-Lake and BAW American pricing, Greeks, and implied vol
- NumPy batch pricing/inversion and cross-strike American pricing
- lightweight SVI/eSSVI slices and interpolated surfaces
- calibration-grade `VolSurface` parameters and evaluators

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

Functions returning `atx::core::Result<T>` raise `atxvol.AtxError` on failure.
Long-running American and batch kernels release the Python GIL.

