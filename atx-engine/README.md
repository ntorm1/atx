# Equity systematic research engine

ATX is a C++20 equity research and portfolio simulation engine. The existing stack
includes a signal expression language, alpha search, statistical validation,
learned signal models, signal combination, factor risk, constrained optimization,
execution simulation, and a command-line pipeline in `atx-impl`.

The platform improvement program is tracked in
[PLATFORM_PROGRESS.md](docs/PLATFORM_PROGRESS.md). Historical roadmap claims are
not current acceptance evidence. Current profitability and institutional readiness
remain unverified.

The working `atx-impl equity-book` command now connects weekly signal preferences
to allocation against actual marked holdings, with explicit turnover and post-fee
exposure checks. Explicit hold/close instructions preserve existing units and
remove machine-scale allocation dust within a declared correction budget. The
startup book has 381 positions; its continuous solution agrees with an independent
optimum to `4.34e-19` in every canonical weight. An explicit research policy now
fixes unheld names without current prices to zero before optimization. The latest
run reaches four allocations, then fails on a missing held PCS price after its
2013 corporate transition. No investment result is accepted.
Development is isolated in
`C:/atx/.worktrees/equity-platform`; see the
[implementation guide](../atx-impl/docs/EQUITY_BOOK_CONSTRAINED.md).

The `atx-impl equity-ic` command measures cross-sectional forecast skill
(Pearson and rank IC, decay, block-bootstrap ICIR, decile spread, signal
autocorrelation) for the three deployed signals against a pre-registered,
hash-chained trial ledger, reading signals from the published `equity-baseline`
`combo.bin` without recomputing them. See
[docs/EQUITY_IC.md](../atx-impl/docs/EQUITY_IC.md); results are sign-and-shape
evidence only.

The `atx-impl equity-universe` command (checkpoint 15) builds a point-in-time
dollar-volume universe from sealed archive segments with the engine's
`data::PitUniverseBuilder` (`data/point_in_time_universe.hpp`): 84 monthly
rebalances over 2013-2019, six size x band cuts side by side, churn, coverage,
union sizes, an inferred delisting table and a survivorship lower bound. See
[docs/EQUITY_UNIVERSE.md](../atx-impl/docs/EQUITY_UNIVERSE.md) and the
[design](reviews/2026-09-20-iteration15-point-in-time-universe-design.md);
membership lists and counts only, not a forecast.

Checkpoint 16 turned those memberships into the first multi-year, net-of-cost
alpha measurement: 13 (year x cut) cells over 2013-2019 on top-1000 and
top-3000, driven by three new `panel` flags `--universe-membership`,
`--universe-cut` and `--universe-eval-start` (all three or none) that restrict
the panel to a year-union allow-list at compaction. All three signals FAIL the
pre-registered bars; verdict NO CANDIDATE. See the
[alpha scorecard](reviews/2026-09-20-equity-alpha-scorecard-cp16.md), its
[design](reviews/2026-09-20-iteration16-alpha-scorecard-design.md) and
[addendum 1](reviews/2026-09-20-iteration16-alpha-scorecard-design-addendum-1.md).

The signed stock-and-cash security transition and cash-claim payment planners
(checkpoint 12) are now built and natively validated on synthetic fixtures;
see the [transition guide](docs/SECURITY_TRANSITIONS.md) and the
[validation receipt](reviews/2026-09-20-security-transition-validation.json).
Real corporate-action admission, including the MetroPCS/T-Mobile 2013-05-01
transition behind the PCS failure above, remains rejected; checkpoint 13
minimal-closed with the engine-side replay seam only (not wired into
`atx-impl`), and the eight preconditions listed in its design section 8 are
still unmet.

## Build the equity stack

The `equity-dev` preset builds core, time-series storage, engine, and pipeline
without requiring the optional volatility, knowledge-base, agent, or options
modules. It uses a separate `build-equity` directory. Prerequisites are Visual
Studio 2022 with clang-cl, CMake, Ninja, vcpkg, and the dependencies in the root
manifest. Initialize the Databento submodule when using a fresh checkout.

Run from the repository root in PowerShell:

```powershell
& .\scripts\atx-build.ps1 configure -Preset equity-dev -Groups 'risk;data;core'
& .\scripts\atx-build.ps1 build atx-engine-risk-tests atx-engine-data-tests atx-engine-core-tests atx-shm-worker atx-impl -Preset equity-dev
& .\scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '^(RiskConstraintDispatch|RiskOptimizer|RiskMultiHorizon|DataAvailability|DataDataset|DataAlign|DataCatalog|DataAdaptFactor|DataAdaptPanel|BacktestLoop|ExecSim)\.'
```

The default preset expects the shared dependency installation configured in
`CMakePresets.json`. To use an existing classic vcpkg installation, explicitly pass
`'-DVCPKG_MANIFEST_MODE=OFF'` and
`'-DVCPKG_INSTALLED_DIR=C:/path/to/vcpkg/installed'` to configure. Ensure its GoogleTest
package matches the compiler runtime; do not accidentally use an unrelated Python
environment's installation. Machine-specific overrides are not committed into the
preset. `check <file.cpp> -Preset equity-dev` compiles an individual translation unit.

## Research discipline

Use the cycle **Research -> Review -> Implement -> Measure -> Repeat**. Separate
synthetic correctness evidence from real-data investment evidence. Account for
information availability, universe changes, delistings, transaction and borrow
costs, portfolio limits, and repeated hypothesis testing before accepting a signal.
Do not interpret a passing software test as evidence of alpha or trading capacity.

Dataset callers that specify a positive `pit_delay` must now also set
`date_encoding` to `EpochDays`, `YYYYMMDD`, or `UnixNanoseconds`. The delay means
calendar days. For example, `YYYYMMDD` observation `20240228` with delay `2`
becomes visible on `20240301`. Opaque dates remain valid with zero delay. This
fixed-lag contract does not reconstruct actual filing revisions or publication
timestamps; see the [availability audit](reviews/2026-09-19-data-availability-audit.md).

Constrained optimization preserves turnover penalties and gross capacity limits.
Multi-horizon optimization rejects partial execution with augmented constraints
until the realized book can be constrained directly, and rejects participation or
ownership caps without reference data. The single-period optimizer accepts those
references through `PortfolioOptimizer::ref`.

The backtest simulator replaces the pending book at each successful rebalance,
retaining latency for identical outstanding quantities. It must be dedicated to
one strategy. This models synchronous simulation cancellation; it does not model
broker acknowledgments or replace a production order-management system.

`BacktestLoop` accrues borrow on the preceding slice's short holdings and marks
over actual elapsed UTC time, before processing new prices or fills. The first
slice establishes the time anchor; opening a position at the final slice incurs
no future fee. ACT/360 and ACT/365 include weekends. A 252-session basis requires
a calendar and is rejected by this elapsed-time API. Invalid rates, backward
time, and unrepresentable charges fail explicitly. The standalone daily helper
retains its legacy one-day contract. Portfolio marks refresh after execution
impact before sizing and sampling. These are research accounting conventions;
broker settlement, collateral, rebates, and per-security borrow availability
remain separate work. See the
[financing audit](reviews/2026-09-19-financing-accounting-audit.md).

The supplied `tbltickerhistory3_10y.zip` is integrated through the existing
`load_orats_history` adapter. Dates and positive security IDs are validated, and
duplicate date/ID keys fail explicitly. Research OHLC uses
`raw_price * cumulReturnFactor` consistently; raw close, volume, and reported
shares determine liquidity and market-cap screens. The
[preparation tool](tools/prepare_tickerhistory.py) quarantines source-quality
failures into a counted policy before native ingestion while preserving accepted
row bytes. The [input audit](reviews/2026-09-19-tbltickerhistory-input-audit.md)
documents the measured source, factor direction, and publication limitations.
Existing adjusted panel caches need rebuilding. The archive is a historical
snapshot; a passing data check does not establish historical information timing.

For identified baseline failures, `tools/audit_tickerhistory_reconciliation.py`
audits a bound required-mark inventory against original and accepted rows. Run
it with `--help` for the manifest/input arguments and choose a fresh output
directory. Its completed manifest and evidence ZIP preserve row bytes, QA
reasons and original-neighbor factor diagnostics. The tool makes no source
corrections. The [measured source audit](reviews/2026-09-20-equity-required-marks-audit.md)
records why terminal corporate events and intermittent missing bars need
different treatment.

Historical panel builds now preserve ordered session dates, security IDs, and the
original column mapping in a SHA-256-bound `.manifest.json` beside APNL v1.
Pipeline consumers require that manifest by default and verify axes and parent
artifacts before joining signals or books. Book schedules and combination fit
boundaries are hash-bound companions. Use fresh output paths; identified artifacts
are immutable. The explicit `--allow-unidentified-panels true` option is for legacy
diagnostics and cannot mix known and unknown identities in a join.

Pass `--preparation-manifest <path>` to `load` to bind the completed preparation
receipt to the accepted ZIP and resulting segments. Without that receipt, upstream
source provenance is recorded as unknown. Publication/revision timing and
instrument-type eligibility remain unknown even when all content hashes match.
The old incremental CLI mode now rejects identity inference from numeric shape;
rebuild into a fresh path until an artifact-aware append contract is available.

Identified portfolio reports now replay daily marked holdings and cash. Targets
execute after `--replay-execution-delay` stored observations (default 1), hold
total-return-index units between rebalances, and use `--report-aum` as initial
NAV. The final observation only values existing holdings. Held prices must be
finite and positive even after a name leaves the desired universe. This is a
hypothetical close-execution convention; it does not establish historical input
availability, physical-share fills, or verified corporate-action adjustments.

Use `--replay-trade-bps` for each absolute dollar actually traded and
`--replay-annual-borrow-bps` for an annual simple short fee with
`--replay-day-basis 360|365`. Both rates default to zero and remain explicitly
recorded. A nonzero optimizer planning `--cost-bps` requires a deliberate replay
fee choice in `run`; standalone identified reports reject legacy `--cost-bps`
and flat-per-period `--borrow-bps`. Replay flags on unidentified inputs reject.
Cash interest, market impact, locates, and settlement are not modeled here.

Fresh report directories contain `ledger.csv`, `trades.csv`, `final_tri_units.csv`,
summaries, and a hash-bound `manifest.json` published last. The ledger reports
dollar NAV and every observed interval; it replaces the old additive return
attribution output for identified inputs. Post-fit attribution starts only when
a qualifying decision becomes effective and does not certify a valid holdout.
Trade participation uses actual dollar deltas against 21 prior raw-dollar-volume
observations, with explicit unknown coverage. Strategy capacity remains unavailable
until a calibrated chronological impact replay exists. Unidentified legacy
diagnostics retain their original attribution format.

The [KLAC source exception](reviews/2026-09-19-klac-source-adjustment-audit.md)
records a confirmed June 2026 split missing from the supplied cumulative factors
and an invalid OHLC row. Source-derived numeric equality alone is insufficient to
certify economic returns; the full diagnostic containing the held missing mark
must fail instead of assigning it zero P&L.

Data ingestion details are in [docs/README.md](docs/README.md). Current audit notes
live in [reviews](reviews), and the ongoing implementation record links each change
to its measurement and remaining limitations.
