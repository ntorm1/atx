# Quote-only American options benchmark

`tools/quote_benchmark.py` builds a reusable benchmark from an intraday options
Parquet source. Native pricing and fitting consume raw quotes and contract
inputs. SpiderRock `srPrc` and `srVol` are evaluation labels in a separate file.
The historical oracle Mode A uses reference volatility as a pricing input and
must not be used as evidence for this benchmark.

## Prepare, run, score

```powershell
python atx-vol/tools/quote_benchmark.py prepare `
  --source C:/Users/natha/Downloads/OptionIntradayHist_30min_2026-10-14.parquet `
  --output C:/atx-cache/quote-benchmark/20261014-dev `
  --split development --max-underliers 20 --buckets 1030 --csv

python atx-vol/tools/quote_benchmark.py run `
  --dataset C:/atx-cache/quote-benchmark/20261014-dev `
  --binary build-vol-rel-avx2/bin/atx-vol-quote-bench.exe `
  --curve svi --output C:/atx-cache/quote-benchmark/predictions-svi.csv

python atx-vol/tools/quote_benchmark.py score `
  --dataset C:/atx-cache/quote-benchmark/20261014-dev `
  --run-receipt C:/atx-cache/quote-benchmark/predictions-svi.csv.run.json `
  --output C:/atx-cache/quote-benchmark/score-svi.json

python atx-vol/tools/quote_benchmark.py compare `
  --dataset C:/atx-cache/quote-benchmark/20261014-dev `
  --run-a C:/atx-cache/quote-benchmark/predictions-svi.csv.run.json `
  --run-b C:/atx-cache/quote-benchmark/predictions-essvi.csv.run.json `
  --output C:/atx-cache/quote-benchmark/compare-svi-essvi.json
```

The binary path is the native target's location in your build. Additional native
options follow `--` on the `run` command, for example `-- --report timing.json`.
Use a release build for performance measurements. End-to-end wrapper time
includes process startup and file IO; native stage timings describe kernel work.

`run` defaults to `--policy auto`. Raw near-money strike coverage, relative
spreads and bidless fractions select a named dense, ordinary or sparse profile
once per underlying/root/exact snapshot. An explicit `--curve` is a fixed-model
experiment and is mutually exclusive with `--policy`. A JSON policy file can
define alternative profiles and exact symbol overrides; no ticker mapping is
compiled into the library. Model selection never reads evaluation labels.

The run receipt contains the native report, including expanded profile settings,
resolved decisions and actual model families. For `--policy path.json`, it also
stores the complete JSON document and its SHA-256, checked again after execution.
Global quote-filter overrides belong to explicit fixed-curve experiments;
per-symbol experiments put those settings in the corresponding profile.
The built-in profiles are starting configurations, not calibrated universal
settings. Every profile retains the same final strike and calendar admission
requirements, and rejected contracts remain in coverage reporting.

Graded admission is opt-in (`"admission": "graded"`, default `"static"`). Each
snapshot is fitted by `fit_quote_surface`: expiries the profile's point fitter
serves are grade A and keep their exact prices; the others are refitted by the
channel knot spline (every contract's bid/ask volatility channel, a
spread-normalised mid anchor, and a leave-one-out term prior from the
snapshot's other expiries), graded B (at least `grade_b_min_mid_strikes` mid
strikes) or C. Calendar crossings refuse only the lower-confidence slice after
an optional in-quote price repair and one stronger-prior retry. Top-level
`"graded"` keys: `term_prior_weight`, `prior_decay_log_time`,
`prior_quote_halflife`, `channel_node_weight`, `calendar_retry_factor`,
`grade_b_min_mid_strikes`, `calendar_repair_half_spreads`. Profile `"channel"`
keys tune the channel spline (`channel_weight`, `mid_weight`, `itm_mid_weight`,
`default_shape_weight`, `prior_scale`, `roughness`, `min_half_width_vol`,
`mult_floor`, `mult_ceil`, `max_iterations`, `min_channel_quotes`,
`repair_half_spreads`); `"curve": "knot-spline"` makes it the profile's point
method as well. `channel.family` (`knot-spline` default, or `essvi`, `svi`,
`c8-event`, `convex-dense`, `spline-vol`) solves the same channel objective with
that point family by bounded active-set reweighting (`family_iterations`,
default 12); predictions report `configured_curve` `channel-<family>`. Graded
output appends a `grade` column and the native report adds `graded_stats`
(per-grade chains/rows and per-slice diagnostics, including the channel solve
time `solve_us` and refit count). Every served slice still passes the unchanged
American price and calendar certificates; refusals keep their reason. Graded
admission requires supplied carry (no `carry_mode`).

Preparation projects only the fixed input allowlist and two label columns. It
processes bounded Arrow batches; memory scales with batch size and the number of
distinct underliers, not source rows. `--max-underliers` performs a preliminary
scan of just the underlier column. Cohort selection is independent of labels:
whole underliers are assigned by seeded BLAKE2b hash to development (80%) or
validation (20%). This keeps every root, expiry and snapshot of an underlier
together. The optional symbol cap uses deterministic hash order. `--symbols`
selects explicit underliers; `--buckets` selects New York HHMM intervals, without
excluding the open by default. The default 30-minute selection buckets can be changed with
`--bucket-minutes`. Snapshot identity always retains the exact source `date`;
different instants in a selection bucket never share a chain. Use `--split all` only when an explicitly mixed dataset is
needed. Validation results must not select hyperparameters.

Preparation creates a new directory and refuses to overwrite it. `manifest.json`
is written last; a directory without it is incomplete. It contains the source
SHA-256, adapter SHA-256, selection, complete admission counts, and hashes of all
generated files. Source data and row-level output belong outside the repository.

## Native input contract, schema version 1

Both `quotes.parquet` and optional `quotes.csv` contain these columns in order:

| Column | Type | Meaning |
| --- | --- | --- |
| `row_id` | uint64 | Original zero-based source row position |
| `chain_id` | uint64 | BLAKE2b64 of underlier, root, exact snapshot instant, expiry |
| `snapshot_ns` | int64 | Exact source UTC snapshot instant, nanoseconds since epoch |
| `observation_ns` | int64 | Source update timestamp, UTC nanoseconds |
| `underlier`, `root` | string | Raw underlying key and option root |
| `expiry_days` | int32 | Contract expiry, days since 1970-01-01 |
| `strike` | float64 | Contract strike |
| `is_call` | uint8 | 1 call, 0 put, 255 invalid |
| `exercise` | string | Source exercise style; only American is eligible |
| `spot_bid`, `spot_ask`, `spot` | float64 | Raw underlying quotes and their midpoint |
| `bid`, `ask` | float64 | Raw option prices per share |
| `bid_size`, `ask_size` | int32 | Raw quoted size |
| `years`, `years_calendar` | float64 | Variance and calendar year fractions |
| `rate`, `sdiv` | float64 | Annualized rate and continuous carry input |
| `ddiv`, `ddiv_pv` | float64 | Aggregate cash dividend and discounted aggregate |
| `split` | string | `development` or `validation` |
| `input_status` | string | `ok` or categorical admission rejection |

The spot is always `(uBid + uAsk)/2`; neither `uPrc` nor `synSpot` is read.
Vendor bid/ask IVs, ATM vol, greeks, model flags and `error` are excluded from
fitting, admission and cohort selection. The converter refuses any input frame
outside the allowlist. Per-row environmental inputs are retained: they are not
silently replaced with a single board rate or calendar clock.

Input admission requires an American contract, positive strike and underlying
quotes, finite positive clocks, finite carry inputs, nonnegative dividend
amounts, and finite noncrossed option prices. Zero bids and locked quotes are
retained for the model's more specific admission policy. Every selected row,
including rejected rows, remains in both quote and label files. First rejection
reasons are `unsupported_exercise`, `invalid_contract`, `invalid_underlier_quote`,
`invalid_time`, `invalid_carry`, and `invalid_option_quote`.

The library must distinguish variance time from carry time. Aggregate `ddivPv`
does not identify ex-dividend dates; an effective continuous-carry approximation
must be reported as an approximation. It is not an exact discrete-dividend
American model. Contract exercise/root separation, input dispersion and the
chosen approximation should be included in native run diagnostics.

`labels.parquet` contains only `row_id`, `reference_price`, and `reference_vol`.
Missing/sentinel labels do not remove rows from the model input cohort.

An option-root suffix from 1 through 9 identifies a nonstandard adjusted
contract under the [OCC OSI convention](https://infomemo.theocc.com/infomemos?number=26853).
The number does not describe its deliverable. [OCC equity option specifications](https://www.theocc.com/clearance-and-settlement/clearing/equity-options-product-specifications)
distinguish standard 100-share contracts from adjusted deliverables. The reporting
classification `standard_symbol` means only that this suffix is absent; it does
not certify the deliverable. A root/underlier spelling difference alone is not
an adjustment rule. Production ingestion needs explicit deliverable components,
exercise amount and premium multiplier. A native runner may refuse unsupported
deliverables, but that refusal must remain visible against the original input
coverage denominator.

The supplied `sdiv` need not be constant across strikes. SpiderRock documents
strike-dependent carry adjustments and a separate sdiv spline in its
[live surface calibration description](https://docs.spiderrockconnect.com/docs/Documentation/PlatformFeatures/Analytics/LiveVolSurfaces/).
The [pricing specification](https://docs.spiderrockconnect.com/docs/Documentation/PlatformFeatures/Analytics/OptionPricing/)
defines continuous carry as `rate - sdiv` and treats dated cash dividends
separately. Median aggregation of the supplied row inputs loses that strike
variation. A quote-derived carry estimate must use raw put/call quotes and
account for American exercise premiums; ordinary European parity alone is a
diagnostic approximation. Neither reference prices nor reference volatilities
may calibrate carry or determine contract admission.

## Predictions, provenance, and metrics

Native predictions are CSV or Parquet with required columns `row_id`,
`predicted_price`, `predicted_vol`, `status`, `chain_id`. Additional native
diagnostics are allowed. Success is exactly `status=ok`; failures carry a
categorical reason. Native execution receives only the quote file path.

The wrapper verifies quote hashes, records the executable hash, command,
wall time, quote hashes and output hash in `<predictions>.run.json`, and verifies
the executable/inputs remain unchanged across the run. Scoring requires that
receipt, verifies quote/label/output hashes, rejects duplicate or foreign row
identities and chain mismatches, and left joins from the selected quote cohort.
Missing predictions reduce coverage instead of silently improving an error
metric. Raw-input admission is independent of reference label availability.

The scorecard reports counts, price/volatility prediction and scoring coverage, signed bias, MAE,
RMSE, median absolute error, p95 and p99 error, price containment in raw bid/ask,
and reference price containment. Missing reference labels reduce scoring coverage
but do not reduce prediction coverage. A raw-midpoint price baseline uses exactly
the same scored population as the fitted price. Prices are dollars per share. Vol errors are
absolute volatility basis points. Spread-normalized price errors divide by
`max(ask-bid, tick_size)`; default tick size is $0.01 and is configurable. DTE/side
breakdowns use calendar time and contract side, independent of label values.
Exact quantiles use memory proportional to the selected cohort; preparation,
unlike scoring, has bounded row memory. Solver failures and label-missing rows
remain visible through counts rather than disappearing from the population.

Each score preserves the full selected cohort headline and adds mutually
exclusive partitions from raw input columns:

| Partition | Definition |
| --- | --- |
| `contract_kind` | Numeric suffix 1–9: `adjusted_suffix`; no suffix: `standard_symbol`; missing root: `unknown` |
| `moneyness_band` | `abs(log(strike/spot)) <= 0.2`, `(0.2, 0.7]`, or `> 0.7`; invalid values: `unknown` |
| `cash_dividends` | `ddiv_pv` zero, positive, or invalid/unknown |
| `liquid_core` | Absolute log-moneyness at most 0.2, positive bid, spread/mid at most 0.5 |

These partitions never change admission. Existing calendar-DTE/side breakdowns
remain available. `reference_nbbo_diagnostic` separately reports reference prices
inside or outside raw bid/ask. It is explicitly a label diagnostic, not a market
input, fitting feature, filter, or rule for excluding difficult labels. A fair
value outside the raw quote interval still contributes to headline error.

## Paired comparisons

`compare` verifies both run receipts against the same quote digest and validates
all row/chain identities before joining. Each arm's original prediction and
scoring coverage appears beside its own-population metrics. Those own-population
errors are descriptive; they cannot establish improvement when the populations
differ.

Paired price statistics use exactly the rows where both arms produced finite
nonnegative prices with valid reference prices. Volatility uses its own exact
intersection of finite positive predictions and labels. Reported bias, MAE,
RMSE and quantiles for the two arms use identical rows within each comparison.
The paired reduction is `abs(error_A) - abs(error_B)`; a positive result favors B
on that fixed population. Per-row improvement/worsening/tie fractions and counts
scored only by one arm expose coverage changes. An empty intersection produces
null errors, never a perfect-fit zero. The same paired metrics and per-arm
coverage are broken down by the raw partitions above. Reports make no automatic
winner claim. Both `score` and `compare` require new output paths and refuse to
overwrite input evidence or existing reports.

Reference parity alone cannot establish model truth or production readiness.
Numerical reference convergence, no-arbitrage behavior, discrete dividends,
deterministic replay, tail latency and out-of-sample behavior require separate
measurements. A comparison is valid only for its declared cohort, coverage,
environment conventions, executable and hardware.

## Mandatory final-price admission

`fit_quote_slice` accepts a slice only after constructing an owned
`AmericanPriceSlice`. Its canonical serving values are piecewise linear in
American price on the finite input-strike domain. The constructor enforces
intrinsic and upper bounds, decreasing calls, increasing puts, vertical-spread
bounds, convexity, and conservative American call/put bounds. These constraints
hold between knots because the representation preserves their affine
inequalities. Checks use a fixed numerical tolerance, with no disable switch.
This closes the gap between a butterfly-free European volatility curve and
American prices evaluated with strike-dependent carry.

Price-only lookup is allocation-free. A query requesting both price and IV
between knots inverts the canonical price through the same American model;
this costs more than price-only lookup. Outside-domain queries fail instead
of extrapolating an unchecked wing. Piecewise-linear strike prices do not
provide smooth strike second derivatives; smooth model Greeks and spot/time
scenario dynamics require a separate consistent-model representation.

The native runner also checks every expiry pair for a given underlying/root
and exact snapshot. It checks both calls and puts on the union of their knots
inside the overlapping strike domain, so crossings between the original
quotes cannot escape detection. Both offending chains are refused before any
prices are published. Calendar failures stay in the original coverage
denominator. Underlying/root, expiry and exact snapshot identity are required.

These are explicit static price constraints, not a claim that an arbitrary
dividend/carry surface comes from one joint optimal-stopping model. General
American model existence needs stronger conditions. For the non-dividend
case, see [Cox and Hoeggerl's American/European consistency conditions](https://arxiv.org/pdf/1301.5467).
The next model layer must use one positive transition system, common funding
and carry dynamics, and dated dividend events across all strikes and expiries.
The aggregate-PV route remains an approximation to exercise economics.

Quote-quality thresholds (`--max-spread-to-mid`, `--max-spread-vol`,
`--max-weight`, `--min-quotes`) may change which raw quotes influence the fit.
They cannot disable final-price or calendar admission. A bidless quote is not
a midpoint observation; its contract can still receive a price from an
accepted fit. Coverage-changing settings must be compared on paired rows.
