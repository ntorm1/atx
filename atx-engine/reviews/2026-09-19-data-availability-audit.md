# Research availability audit — 2026-09-19

## Implemented slice

`DatasetSchema::pit_delay` previously appeared only in schema/reporting code.
`align_onto`, `DatasetCatalog::value_at`, and `reference_spans` read observations
as soon as their raw DateKey was reached, ignoring the declared reporting delay.
This could admit a period-end fundamental before publication.

The dataset now computes an immutable availability axis at construction and the
three readers resolve that axis. `DateKeyEncoding` explicitly distinguishes
opaque keys, epoch days, YYYYMMDD, and Unix nanoseconds. Calendar-day delay uses
valid calendar arithmetic for YYYYMMDD, checked addition for numeric encodings,
and 24-hour days for nanoseconds. Positive delay with opaque keys is rejected,
including empty datasets. Typed joins require identical encodings. Delayed rows
outside the alignment window count as unavailable date cells.

The default remains opaque with zero delay; existing keys and values retain
their meaning. The report records explicit encoding only when specified, keeping
legacy report bytes intact. The enum was appended to the aggregate schema, so
existing positional source initializers retain their order. A rebuild is required
because C++ object layouts changed. Search found no binary DatasetSchema writer
or schema-byte digest; disk persistence uses other data representations.

`pit_delay` remains unsigned u16: it cannot represent a negative lag. Callers
must validate signed external input before converting it to this existing type.

## Scope and evidence

The SEC distinguishes reporting periods from filing dissemination and states that
its frames endpoint selects the last-filed fact for a reporting period. This
supports preserving availability separately from an observation's economic date:
[SEC EDGAR API documentation](https://www.sec.gov/search-filings/edgar-application-programming-interfaces).
The explicit encoding and fixed-delay implementation are engineering decisions,
not a claim that a fixed lag reconstructs publication history.

This slice does **not** add per-cell publication timestamps, revisions, restatement
versions, exchange calendars, or stale-data expiry. `as_of.effective_dated` remains
descriptive metadata. Raw `column()` access is intentionally not an as-of view.
`price_to_panel` rejects positive lag because its date-free output cannot enforce
availability. `AlignedView` carries already-delayed values and no schema; any
Dataset wrapper must set zero delay. The existing `corp_on_price_axis` wrapper
already creates a fresh zero-delay schema. A focused round-trip test pins this
contract. Dataset creation still accepts unordered axes for compatibility, while
the shared lookup rejects them in O(1) using a construction-time ordering flag.
Existing internal loaders retain zero-delay opaque keys; adopting explicit units
for a pipeline requires setting its connected dataset schemas consistently.

## Validation

Focused target: `atx-engine-data-tests`.
New suite: `DataAvailability.*` covers delayed catalog/alignment/reference reads,
leap/month/year transitions, invalid dates, numeric overflow, nanosecond time of
day, negative epoch keys, maximum lag, opaque rejection, mixed-unit rejection,
prefix invariance, delayed-panel rejection and aligned-value rewrapping.
Compatibility suites: `DataDataset`, `DataAlign`, `DataCatalog`, `DataAdaptFactor`,
`DataAdaptPanel`, `CatalogReport`.

The root agent built the final Debug `equity-dev` targets and ran the combined
gate: 241 passed, zero failed, five real-data fixture skips in 14.31 seconds.
All 13 `DataAvailability` regressions passed, as did the selected compatibility
and synthetic integration suites. Evidence: `build-equity/iteration1-final.xml`
and the [validation receipt](2026-09-19-validation.json). The skipped real-data
tests and interrupted dense-oracle battery are recorded in the program log;
they are not accepted as passing.

## Next bounded opportunities

1. **Fold-local feature selection.** `learn/latent.cpp::select_interactions`
   filters finite labels and feature dates but does not require label maturity
   at the fit cutoff. `fit_linear` and `fit_gbt` copy the externally fitted PCA
   and interaction selection into every fold, while fitting only normalization
   per fold. Store label horizons/availability in FeatureMatrix, require matured
   labels, and fit PCA/interaction recipes on explicit training rows. Verify
   held-out feature/label perturbations cannot alter a fold's fitted transform.
   Target: `atx-engine-learn-tests`; suites `Latent`, `LinearAlpha`, `Gbt`.
   [scikit-learn's primary documentation](https://scikit-learn.org/stable/common_pitfalls.html#data-leakage)
   explicitly requires feature selection and PCA to fit only training subsets.

2. **Elapsed-time financing.** `BacktestLoop::on_time_slice` calls
   `cost::accrue_borrow` once per bar, and `daily_borrow` charges one full day
   regardless of elapsed time. Intraday bars repeat daily charges; daily-only
   feeds omit weekend/holiday gaps. Add an explicit financing accrual clock and
   charge elapsed holding intervals with documented book/mark timing. Measure
   identical financing on economically identical daily/intraday replays and
   Friday-to-Monday holdings. Target: `atx-engine-core-tests`; suite `BacktestBorrow`.
   [IBKR short-sale pricing](https://www.interactivebrokers.com/en/pricing/short-sale-cost.php)
   distinguishes daily borrow fees and proceeds interest; instrument-specific
   rates, actual collateral marks, settlement timing, locates and recalls remain
   follow-on requirements.
