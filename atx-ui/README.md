# atx-ui

`atx-ui` is the desktop volatility-trading workspace for ATX. It is an
immediate-mode, dockable application built on GLFW, OpenGL, Dear ImGui, ImPlot,
and the production `atx-vol` pricing and surface engines.

The code is split along reusable boundaries:

- `atx-ui-framework` owns application lifecycle, versioned dock layouts, theme,
  panel registration, status/menu shell, and shared display primitives.
- `VolSurfaceSource` is the feed-independent contract consumed by volatility
  panels. OPRA replay is one adapter; live feeds and scenario/replay surfaces can
  implement the same interface.
- `VolWorkspaceState` and `vol_workspace_model.hpp` contain pure, tested
  interaction and analytics logic with no ImGui dependency.
- `atx-ui-vol` coordinates symbol summary, expiration strip, curve controls,
  volatility slice, model diagnostics, quote/Greeks ladder, and all-expiry ATM
  term structure.
- `atx-ui` is a thin composition binary, not a second home for business logic.

The current OPRA workspace loads a real CBBO-1m snapshot, builds an
`atx::vol::OptionChain`, fits the HFT `LinearVariance` surface, and publishes
stable view models to the panels. The UI includes:

- symbol/snapshot/model summary and a scrollable expiry strip;
- configurable normalized-strike or strike axes and IV or total-variance views;
- call/put market mids, American-IV bid/ask whiskers, and theoretical surface;
- a cross-expiry ATM-vol/forward term structure with double-click selection;
- selected-slice and whole-surface calibration diagnostics;
- a horizontally scrollable option ladder with price and IV markets,
  theoretical price, price/vol edge, delta, gamma, daily theta, and vega;
- source-independent selection/state models and compatibility for the original
  `SpyOpraSurface` API.

## Build and run

From a configured repository root:

```powershell
cmake --build --preset dev --target atx-ui atx-ui-model-tests
.\build\bin\atx-ui.exe
```

Build, deploy to `Desktop\ATX Vol Release`, and launch the Release executable:

```powershell
cd C:\atx\atx-ui
.\release.ps1
```

Load another compatible snapshot or symbol:

```powershell
.\build\bin\atx-ui.exe `
  --symbol SPY `
  --data data\spy_opra_cbbo1m_2026-06-05T1955Z.parquet `
  --snapshot 2026-06-05T19:55:00Z `
  --expiry 2026-12-18 `
  --rate 0.043
```

When `--expiry` is omitted, the adapter selects the listed expiry nearest 180
days. Validate the complete load, fit, American-IV, and Greeks path without
creating a window:

```powershell
.\build\bin\atx-ui.exe --headless
```

Dock positions persist in the user's local ATX application-data directory.
Each workspace declares a layout schema ID, so newly added panels receive a
deterministic default dock without destroying layouts belonging to other
workspaces. Use **Workspace -> Reset layout** at any time.

## Test

```powershell
.\build\bin\atx-ui-model-tests.exe
ctest --test-dir build -L atx_ui --output-on-failure
```

See [docs/platform-architecture.md](docs/platform-architecture.md) for the
module boundaries and the remaining institutional-workspace roadmap.

