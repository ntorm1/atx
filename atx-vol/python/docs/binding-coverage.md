# atxvol binding coverage — public API `include/atx/vol/api/` → Python

Lane PY, 2026-10-23, against `main @ a53c48c4` plus this lane's bindings. One row per public
header (109 at this base: 108 module headers + the `vol.hpp` umbrella; the lane brief's
"103" predates the r5/r6 additions). Regenerate the raw evidence with the probe described at
the end; the classification column is a judgement and is the part to review.

## Status vocabulary

| Status | Meaning |
|---|---|
| `bound` | The header's product surface is reachable from Python (a TU includes it, or its types arrive through a bound type). |
| `bound (new)` | As `bound`, added by lane PY. |
| `partial` | Some of the header is bound (named in the note); the rest is listed as missing. |
| `missing-product` | A research user would want it; not bound yet. The remainder list at the end orders these. |
| `missing-internal` | Executors, schedulers, SIMD kernels, fitter internals, policy plumbing. Deliberately not bound. |
| `n/a` | Not a binding target (umbrella / aggregator). |

## Counts

| Status | Headers |
|---|---|
| bound | 36 |
| bound (new) | 9 |
| partial | 9 |
| missing-product | 29 |
| missing-internal | 25 |
| n/a | 1 |
| **total** | **109** |

`bound (new)` + the new parts of the `partial` rows are the r5/r6 surface this lane added:
4 new TUs (`fit_archive.cpp`, `serve.cpp`, `served_analytics.cpp`, `region.cpp`) plus the
`quote_slice.cpp` deltas, 98 bound enum types gated for completeness
(`tests/test_binding_enum_parity.py`).

### Availability update, 2026-10-24

At `3f643bd1`, the current static source gate counts **101** bound qualified enum
types: the historical 98 plus `GreekFieldMask`, `AmericanGreekReason`, and
`PriceStatus`. Scalar American Greeks and the American batch, fitted-chain and
priced-surface grid outputs now expose per-field availability. The grid keeps
its independent fair value separate from the Greek bundle's price. The dedicated
American Unsupported batch sentinel is `-2`; success remains `-1`.

The original matrix and receipt above describe the 2026-10-23 inventory. Its
`backtest/portfolio_pricer.hpp` entry must not be read as a Python `PortfolioPricer`
API: the binding exposes configuration/types (now including `PriceStatus`), while
`PortfolioPricer`, `PriceFrame`, and `PriceTotals` remain unbound. Native
ServedSession partial-result propagation is also a separate pending component.
Validation: 99 focused Python tests passed, one expected source-build wheel
metadata skip, and four owning PCH-off objects passed; evidence is
`repair-iv/greek-python-final.*` and `greek-python-hygiene.log` under the r7 run.

## Matrix

| Header | Status | Where / note |
|---|---|---|
| `analytics/analytics.hpp` | missing-product | vol analytics (realized / forward metrics); no TU |
| `analytics/common_model_analytics.hpp` | missing-product | `european_outputs` needs an `AmericanMarkovModel` binding, which does not exist |
| `analytics/contract_projection.hpp` | missing-product | contract projections |
| `analytics/deriv_pnl.hpp` | missing-product | derivative P&L |
| `analytics/earnings_term_fit.hpp` | missing-product | earnings term-structure fit |
| `analytics/event_vol.hpp` | missing-product | event vol extraction |
| `analytics/pnl_attribution.hpp` | missing-product | non-served P&L attribution (the served path is bound) |
| `analytics/scenario_grid.hpp` | missing-product | non-served scenario grid (the served path is bound) |
| `analytics/served_analytics.hpp` | bound (new) | `served_analytics.cpp`: declaration, clock, hedge Greeks, scenarios, attribution, dated-cash scenarios |
| `analytics/served_session.hpp` | partial | new: `ServedSession` over a `PricedSurface` (`served_analytics.cpp`); the `VolaSession` overload needs a `VolaSession` binding, which does not exist |
| `backtest/backtest.hpp` | bound | `backtest.cpp`, `analytics.cpp`, `dispersion.cpp` |
| `backtest/deriv_book.hpp` | missing-product | derivative book |
| `backtest/dispersion.hpp` | bound | `dispersion.cpp`, `strategy.cpp` |
| `backtest/dispersion_strangle.hpp` | bound | `strategy.cpp` |
| `backtest/listed_dispersion.hpp` | missing-product | listed dispersion pipeline (schedule + strategy are bound) |
| `backtest/listed_dispersion_schedule.hpp` | bound | `dispersion.cpp` |
| `backtest/listed_dispersion_strategy.hpp` | bound | `dispersion.cpp` |
| `backtest/panel.hpp` | bound | `fit.cpp` |
| `backtest/portfolio_pricer.hpp` | bound | `backtest.cpp` |
| `backtest/priced_surface.hpp` | bound | `fit.cpp`, `surface_db.cpp` |
| `backtest/priced_surface_view.hpp` | bound | `surface_db.cpp` |
| `backtest/query_pricing.hpp` | bound | `backtest.cpp` |
| `backtest/research_validation.hpp` | missing-product | research validation harness |
| `backtest/strategy.hpp` | bound | `strategy.cpp`, `backtest.cpp`, `dispersion.cpp` |
| `backtest/vol_edge.hpp` | missing-product | vol-edge signals |
| `core/chain.hpp` | bound | `fit.cpp` |
| `core/market_env.hpp` | bound | `fit.cpp` |
| `core/types.hpp` | bound | `pricing.cpp`, `sides.hpp`, `common.hpp` |
| `core/version.hpp` | bound | `module.cpp` (`__version__`) |
| `core/vol_time.hpp` | missing-product | variance-time calendars |
| `fitting/aggregate_arity.hpp` | missing-internal | fitter plumbing |
| `fitting/american_markov_fit.hpp` | missing-product | Markov model fit (needs the model binding) |
| `fitting/american_markov_policy.hpp` | missing-internal | policy plumbing |
| `fitting/american_markov_seed.hpp` | missing-internal | fitter internals |
| `fitting/american_price_slice.hpp` | partial | node layout via `FitArchive.price_nodes` and the serve record; `AmericanPriceSlice` itself not bound |
| `fitting/arb.hpp` | missing-product | static-arbitrage checks |
| `fitting/c8.hpp` | missing-internal | fitter internals |
| `fitting/calib.hpp` | partial | option enums / `CalibOpts` via `fit.cpp`; calibration entry points not bound |
| `fitting/correction.hpp` | missing-internal | fitter internals |
| `fitting/curve_fit.hpp` | missing-internal | fitter internals |
| `fitting/curve_selector.hpp` | missing-product | `select_curve` (named only in a `surface_db.cpp` docstring) |
| `fitting/deamer.hpp` | missing-internal | fitter internals |
| `fitting/dense_slice.hpp` | partial | `ConvexFitOpts` via `fit.cpp` |
| `fitting/expiry_record.hpp` | missing-product | per-expiry record (pre-H0 serving shape) |
| `fitting/fit_metrics.hpp` | missing-product | fit metrics |
| `fitting/fit_policy.hpp` | missing-internal | policy plumbing |
| `fitting/parity.hpp` | missing-product | put-call parity forward |
| `fitting/prepared_policy.hpp` | missing-internal | policy plumbing |
| `fitting/pricer_fitter.hpp` | bound | `fit.cpp` |
| `fitting/profile.hpp` | missing-internal | fit-profile plumbing |
| `fitting/projection.hpp` | missing-internal | C4 projection machinery (its outcome is read through `QuoteSliceFit` / the archive) |
| `fitting/quote_fit_policy.hpp` | missing-internal | policy plumbing |
| `fitting/quote_slice.hpp` | bound | `quote_slice.cpp` (+ r5/r6 deltas: grade, call floor, grid bound, withheld nodes / gaps / served price, carry curve, serve record, the repair / box-certificate config and records incl. C4's in-box rung), `fit_archive.cpp` |
| `fitting/quote_slice_carry.hpp` | missing-internal | carry identification internals |
| `fitting/s3.hpp` | missing-internal | fitter internals |
| `fitting/serve_book.hpp` | bound (new) | `serve.cpp`: book, transactions, readers, `serve_chain`, `serve_build` (rebuild callback not bound) |
| `fitting/session.hpp` | bound | `fit.cpp`, `surface_db.cpp` |
| `fitting/slice_serve_record.hpp` | bound (new) | `serve.cpp`: record, requests, answers, support, version, policy |
| `fitting/spline_curve.hpp` | partial | `SplineFitOpts` via `fit.cpp` |
| `fitting/sr_tenor_grid.hpp` | missing-internal | tenor-grid internals |
| `fitting/surface.hpp` | bound | `surface.cpp` |
| `fitting/surface_overlay.hpp` | missing-product | surface overlay evaluation |
| `fitting/surface_parity.hpp` | bound | `surface_db.cpp` |
| `fitting/surface_policy.hpp` | partial | `SurfacePurpose` via `fit.cpp` |
| `fitting/vol_curve.hpp` | bound | `fit.cpp`, `surface_db.cpp`, `FittedVolCurve` handle (`common.hpp`) |
| `fitting/vol_surface.hpp` | bound | `surface.cpp`, `surface_db.cpp` |
| `marketdata/american_markov_market.hpp` | missing-internal | Markov market plumbing |
| `marketdata/corpus.hpp` | bound | `backtest.cpp` |
| `marketdata/data.hpp` | bound | `fit.cpp` |
| `marketdata/dividend_schedule.hpp` | missing-product | dividend schedules |
| `marketdata/implied_forward.hpp` | missing-product | implied forwards |
| `marketdata/opra_batch.hpp` | missing-internal | ingest executor |
| `marketdata/opra_hive.hpp` | bound | `surface_db.cpp` |
| `marketdata/opra_panel.hpp` | missing-product | `OpraLoadSpec` (named only in a docstring) |
| `marketdata/universe.hpp` | bound | `strategy.cpp` |
| `pricing/adjusted_greeks.hpp` | missing-product | adjusted Greeks |
| `pricing/american.hpp` | bound | `pricing.cpp`, `region.cpp`, `quote_slice.cpp`, `fit.cpp` |
| `pricing/american_batch.hpp` | bound | `pricing.cpp` |
| `pricing/american_discrete_div_batch.hpp` | bound (new) | `region.cpp`: `DiscreteDivBatch` |
| `pricing/american_discrete_div_kernel.hpp` | partial | new: enums, production opts, IV opts, route, Greeks bundle via `region.cpp`; scalar kernel entry points not bound |
| `pricing/american_iv.hpp` | bound | `pricing.cpp` |
| `pricing/american_markov.hpp` | missing-product | Markov pricer (needs the model binding) |
| `pricing/american_markov_batch.hpp` | missing-internal | batch kernel |
| `pricing/american_markov_grid.hpp` | missing-internal | grid internals |
| `pricing/american_markov_pwl.hpp` | missing-internal | PWL internals |
| `pricing/american_region.hpp` | bound (new) | `region.cpp`: classifier + classified Andersen-Lake (scalar + batch); `AloRegionPricer` not bound |
| `pricing/batch.hpp` | bound | `pricing.cpp`, `surface.cpp` |
| `pricing/batch_chain.hpp` | missing-internal | chain batch executor (the numpy batches cover the product need) |
| `pricing/black76.hpp` | bound | `pricing.cpp` |
| `pricing/cboe_strip.hpp` | missing-product | CBOE variance strip |
| `pricing/chain_tick.hpp` | partial | new: `TickStatus` + outputs through `ServeBookReader.serve_chain`; `chain_tick` entry points not bound |
| `pricing/derivatives.hpp` | missing-product | derivative contracts |
| `pricing/dividend.hpp` | missing-product | dividend models (cash events reach Python as `(tau, amount)` pairs) |
| `pricing/greeks.hpp` | bound | `pricing.cpp` |
| `pricing/implied_vol.hpp` | bound | `pricing.cpp` |
| `pricing/pricing_executor.hpp` | missing-internal | executor |
| `pricing/pwl_strike.hpp` | missing-internal | PWL internals |
| `pricing/rates_curve.hpp` | bound | `fit.cpp` |
| `pricing/swap_leg.hpp` | missing-product | swap leg (README: not the swap lane) |
| `simd/american_boundary_batch.hpp` | missing-internal | SIMD kernel |
| `simd/cpu.hpp` | partial | `SimdIsa` via `pricing.cpp`; CPU detection not bound |
| `simd/greeks_batch.hpp` | missing-internal | SIMD kernel (consumed inside `pricing.cpp`) |
| `storage/fit_archive.hpp` | bound (new) | `fit_archive.cpp`: reader, writer, publish, crc32c |
| `storage/fit_archive_format.hpp` | bound (new) | `fit_archive.cpp`: `ArchiveStatus`, section types, record layouts as copies / numpy dtypes |
| `storage/fit_archive_records.hpp` | bound (new) | `fit_archive.cpp`: decode_fit / decode_curve, pool views |
| `storage/fit_archive_series.hpp` | bound (new) | `fit_archive.cpp`: series open / append / compact |
| `storage/surface_archive.hpp` | bound | `surface_db.cpp` (lane SER may change `SurfaceArchiveV2`; the integrator re-checks) |
| `storage/surface_db.hpp` | bound | `surface_db.cpp`, `backtest.cpp` (same SER caveat) |
| `vol.hpp` | n/a | umbrella |

## Remainder (missing-product), in the order a research user would want them

1. `analytics/common_model_analytics.hpp` and the `VolaSession` overload of
   `ServedSession::bind`: the rest of A1. Both need an upstream object Python cannot build
   today (`AmericanMarkovModel` from `fitting/american_markov_fit.hpp`; `VolaSession`). Bind
   the Markov fit and model first.
2. `pricing/american_markov.hpp` + `fitting/american_markov_fit.hpp`: the Markov pricer
   and fit.
3. `fitting/arb.hpp`, `fitting/parity.hpp`, `marketdata/implied_forward.hpp`,
   `marketdata/dividend_schedule.hpp`: research diagnostics around a fit.
4. The rest of `analytics/` (event vol, earnings term fit, projections, P&L) and
   `backtest/` (research validation, vol edge, deriv book, listed dispersion pipeline).

## How the evidence was gathered

A throwaway probe (not committed) listed each header's `struct` / `class` / `enum class` /
free-function names and searched the binding sources for each name and for a direct
`#include "atx/vol/api/<header>"`. Name hits that came only from docstrings or from generic
words (`size`, `create`, `price`) were discarded by hand.
