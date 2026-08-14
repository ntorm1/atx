# atx-vol API restructure — design

Date: 2026-08-14
Status: approved in session (layout: api-is-the-public-tree; compat: hard
cutover; surface: curated minimal)

## Goal

Make the public interface of atx-vol unambiguous and physically enforced:
one tree (`include/atx/vol/api/`) holds everything the library supports for
external consumption, organized into logical modules; everything else lives
next to its implementation under `src/` and is private by construction.
Pure physical restructure — no namespace changes, no semantic changes, no
gate/tolerance/validation-constant edits.

## Decisions (locked)

1. **`api/` IS the public tree.** `include/atx/vol/api/<module>/*.hpp` is
   the only installed/public header set. All internal headers leave
   `include/` and move under `src/<module>/`. Consumers write
   `#include <atx/vol/api/<module>/<name>.hpp>`.
2. **Hard cutover.** No forwarding shims. Every in-repo consumer (tests,
   examples, bench, tools, python bindings, atx-options-engine, install
   rules, test-package) is updated in the same migration.
3. **Curated minimal surface.** A header is public iff transitively needed
   by an out-of-library consumer: atx-options-engine, python bindings,
   tools, examples, bench, or the install smoke package. Headers included
   only by atx-vol src + tests are demoted to private. Borderline calls
   resolve to PRIVATE (promotion later is cheap; demotion of a published
   header is the expensive direction).

## Target layout

```
include/atx/vol/api/
  core/        types, version, log, vol_time, market_env, chain,
               listed_quote_key
  pricing/     black76, american, american_iv, implied_vol, greeks,
               adjusted_greeks, theo, derivatives, swap_leg, batch,
               american_batch, dividend, rates_curve
  simd/        public batch kernels (cpu, math_mode, *_batch)
  fitting/     session, pricer_fitter, vol_curve, vol_surface, surface,
               fit_policy, svi_calib, essvi_calib, c8(+_calib),
               cstar(+_calib), calib, curve_fit, curve_selector, arb,
               parity, surface_parity, deamer, profile, surface_policy,
               dense_slice, spline_curve, fit_metrics, sr_tenor_grid,
               correction, projection
  marketdata/  listed_opra, opra_batch, opra_hive, opra_panel, corpus,
               occ_ess, universe, catalog, data
  storage/     surface_db, surface_archive, backtest_db(+_build),
               research_db, track_key, track_store, dispersion_surface_db,
               s3, snapshot_pool
  analytics/   var, var_report, var_validation, realized_vol,
               scenario_grid, historical_projection, contract_projection,
               pnl_attribution, earnings_repro(+_config),
               earnings_forecast_loader, earnings_term_fit, event_vol,
               breakeven, analytics
  backtest/    backtest, backtest_template, strategy, strategy_pipeline,
               sweep_driver, dispersion, dispersion_strangle,
               listed_dispersion(+_schedule, +_strategy), panel,
               structure_panel, vega_panel, quant_pipeline,
               portfolio_pricer, priced_surface, priced_surface_view,
               deriv_book, query_pricing, margin

src/<module>/  all .cpp for the module, plus every private header:
               the current include/atx/vol/detail/ set (32 files),
               existing src-local headers, and every header the curated
               cut demotes
```

Module placement per header above is provisional. Final placement comes
from the measurement step; the taxonomy (module names and their charters)
is fixed. A header listed in one module may land in another if its include
graph says so; new modules are NOT invented without returning to the user.

Satellite trees are out of scope this pass: `tools/include/atx/vol/tools`
and `research/include/atx/vol/research` already have separate targets and
keep their layout.

## Public-surface determination (measured)

Build the include graph mechanically:

- Roots: every TU in atx-options-engine, atx-vol/python, atx-vol/tools,
  atx-vol/examples, atx-vol/bench, test-package.
- A header reachable (transitively) from any root is PUBLIC → api/.
- A header reachable only from atx-vol/src and atx-vol/tests is PRIVATE →
  src/<module>/.
- `spy_fixture.hpp` and test-support headers are private regardless.
- The measurement produces a placement table (header → api/<module> |
  src/<module>) checked into the plan before any file moves.

Expected outcome: public set well under half of today's ~140 top-level
headers. If the measured public set exceeds ~100, stop and revisit the
cut with the user rather than shipping a fig-leaf api/.

## Private-element splitting

For headers that stay public but contain internal freight — a
`detail::` namespace block, internal structs/constants, or symbols
referenced by exactly one TU — split the internal part into
`src/<module>/<name>_detail.hpp`; the public header keeps only the
supported surface and gains an include of nothing private. Applied only
where the internal part is mechanically identifiable; no cosmetic
rewrites, no API redesign, no signature changes.

## Build / install rework

- `atx-vol/CMakeLists.txt`: source lists become per-module blocks;
  target include dirs: PUBLIC `include/`, PRIVATE `src/`.
- Tests: private include dir on the test target (`src/`) so tests keep
  including internals directly.
- `install(DIRECTORY)` installs `include/atx/vol/api/` only.
- `cmake/atx-volConfig.cmake.in`, `cmake/atx-vol-install.cmake`,
  `cmake/atx-vol-version.hpp.in` updated for the new tree.
- `test-package/smoke.cpp` updated to api/ includes — proves the installed
  interface stands alone.
- Python bindings (`atx-vol/python`) updated to api/ paths.

## Migration mechanics

- `git mv` for every move (history-preserving).
- Include-path rewrite is scripted (deterministic old-path → new-path map
  from the placement table), applied across the whole repo.
- Execution is sub-agent driven in waves:
  1. Measurement agent: include graph, placement table, old→new map.
  2. Parallel per-module move agents on disjoint file sets (moves +
     include rewrites limited to their module's map entries).
  3. Build-system agent: CMake, install, bindings, test-package.
  4. Verification agent: final gate (below).
- Wave boundaries are commit boundaries on a feature branch;
  branch merges to main only when the final gate is green.

## Verification (no full suites)

- Per wave: incremental build via
  `powershell -File C:\atx\scripts\atx-build.ps1 build atx-vol-tests`
  (the verb is required; the no-arg form is a silent no-op) + only the
  moved module's test suites via anchored ctest regex.
- Final gate: clean-configure build from scratch; the 36-suite targeted
  regex set already used for the integration merge; test-package smoke
  compile against the install tree.
- Acceptance claim: build-green + targeted-green + the diff contains only
  file moves, include-path rewrites, header splits, and build-system
  changes. No behavioral claim is made or needed; no full monorepo suite
  is run.

## Out of scope

- Namespace changes, symbol renames, API redesign.
- tools/ and research/ satellite include trees.
- Any change to fit/validation semantics, tolerances, or gates
  (`risk_surface_validation.*` read-only; `audit_fit_inversions` on;
  no oracle tolerances touched).
- Removing or rewriting the python binding surface.

## Risks

- **Hidden includers**: generated files, docs-embedded code, `.in`
  templates. Mitigation: repo-wide grep for `atx/vol/` after each wave;
  the config/version `.in` templates are explicitly in the build-system
  wave.
- **PCH masking**: a missed include may build locally via PCH. Mitigation:
  final gate is a clean configure from scratch.
- **Demoted-header surprises**: a demoted header later needed externally.
  Mitigation: promotion is a `git mv` + one include-path fix; cheap by
  design.
- **Merge conflicts with in-flight work**: the restructure touches
  everything. Mitigation: single feature branch, fast execution, no other
  atx-vol work in parallel.
