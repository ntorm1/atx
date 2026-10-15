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
  --binary build-vol-release/bin/atx-vol-quote-bench.exe `
  --curve svi --output C:/atx-cache/quote-benchmark/predictions-svi.csv

python atx-vol/tools/quote_benchmark.py score `
  --dataset C:/atx-cache/quote-benchmark/20261014-dev `
  --run-receipt C:/atx-cache/quote-benchmark/predictions-svi.csv.run.json `
  --output C:/atx-cache/quote-benchmark/score-svi.json
```

The binary path is the native target's location in your build. Additional native
options follow `--` on the `run` command, for example `-- --report timing.json`.
Use a release build for performance measurements. End-to-end wrapper time
includes process startup and file IO; native stage timings describe kernel work.

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

Reference parity alone cannot establish model truth or production readiness.
Numerical reference convergence, no-arbitrage behavior, discrete dividends,
deterministic replay, tail latency and out-of-sample behavior require separate
measurements. A comparison is valid only for its declared cohort, coverage,
environment conventions, executable and hardware.
