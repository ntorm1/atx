# Universe fit / price / risk robustness benchmark

Lane F of the risk-engine sprint (wave 3, 2026-10-24). Design of record:
`.superpowers/sdd/2026-10-24-risk-engine/wave3-fit-robustness-design.md` §4. One command
sequence produces, for a frozen fitter binary and policy:

- a **label-free** robustness report over every chain of one bucket of the 2026-10-14
  SpiderRock board (6,067 underliers, holdout names included, never scored against labels);
- a **labelled** report over a stratified panel of **development-split** underliers at all
  eleven 30-minute buckets (10:00 → 15:00 New York);

both with per-(uid, root, bucket, expiry) chain scorecards, worst-N rankings, an ordered
failure taxonomy, and the C++ probe that prices and Greeks every served row through the risk
engine at the fitted vol.

Sources: `tools/universe_fit_bench.py` (harness), `tools/universe_archetypes.py` (label-free
archetype predicates and rosters), `tools/fit_risk_probe.{hpp,cpp}` (probe core, also compiled
into `atx-vol-tests`), `tools/fit_risk_probe_main.cpp` (CLI). Tests: `tests/risk_fit_probe_test.cpp`
(`FitRiskProbe.*`, risk slice), `python/tests/test_universe_fit_bench.py`.

## Label governance

| tier | dataset | split | labels |
|---|---|---|---|
| labelled panel | `quote_benchmark.py prepare --split development --symbols <panel>` per bucket | `rows_by_split == {development: n}` | srPrc / srVol (`labels.parquet`) and vendor Δ / ν (`probe-labels.csv`) |
| label-free board | `universe-20261014` (built by `build_universe_dataset.py`) | `rows_by_split == {universe: n}` | never opened |

- The ONE label reader of the harness is `universe_fit_bench._read_labels`; it refuses any
  manifest whose `rows_by_split` is not exactly `{"development": n}` (PermissionError). The
  label-free tier never calls it (`test_label_free_tier_never_opens_labels` replaces it with a
  function that fails the test).
- The probe refuses `--labels` unless `--manifest` passes `labels_allowed` (same rule, exit 3;
  `FitRiskProbe.LabelsRefusedOutsideDevelopmentSplit`).
- The panel is drawn from underliers with `split_for(u, 0.2, "atx-vol-v1") == "development"`
  only (`test_panel_contains_no_validation_underlier`). No tool writes or prints validation
  membership; the roster lives under `C:\atx-cache\quote-benchmark\universe-fit-20261014\panel\`
  and only its rule text and sha256 are committed (below).
- Vendor Greeks are taken from the licensed source by global row index for the panel's
  development rows only (`vendor_greeks`, after `require_development`).

## The shadow clock (why the probe owns clock and carry)

The quote-slice fitter prices an expiry on two clocks: volatility on the vendor VARIANCE clock
`T_v` (`years`) and carry on the CALENDAR clock `T_c` (`years_calendar`): the American price is
taken at `(S, K, T_v, σ, r·T_c/T_v, q_v)` where `q_v = converted_yield(market, q_c, model)`
(`src/fitting/quote_slice.cpp`; with an aggregate cash PV it folds `ln(1 − PV/S_fund)/T_v`
into the yield). A `MarketState` has a single `TimeSpec`, so it cannot hold both clocks, and a
risk state built on the real clock would price every row at a different `T` and carry than the
fit served.

The probe therefore maps each chain onto a synthetic Calendar365 expiry
`expiry_ns' = ts + llround(T_v · kCalendarYearNs)` with
`ExpiryCarry{rate = r·T_c/T_v, q = q_v, pv_divs = 0}` and its own synthetic uid (each chain
keeps its own spot: the store carries several per-expiry spots per underlier). Rows of one
chain with different supplied yields get distinct expiry offsets of +j ns (|T' − T_v| is
~1e-16 years, asserted ≤ 1e-12). Under `snapshot.one_spot` the run's per-chain S* (native
`one_spot.chains[]`) is applied through the library's own `rebase_quote_slice_spot` and
`coalesce_quote_slice_carry`. The engine then prices on exactly the fitter's basis:

- `FitRiskProbe.ShadowClockReproducesFitCarry` pins the restated conversion bit-for-bit
  against `QuoteSliceDiagnostics::effective_yield` / `effective_rate` of a real
  `fit_quote_surface` call under all three aggregate dividend models;
- `FitRiskProbe.RepricesServedRowsAtFittedVol` reprices a three-expiry two-clock chain on
  the cold route to ≤ 1e-8 relative;
- on the real panel (auto policy, bucket 1000) the identity holds to 1.5e-9 relative over
  58,309 served rows (0 mismatches) — see the lane report.

Both kernels run with `al_fast_profile(nullopt)`: the accurate Andersen-Lake scheme the fitter
uses (`QuoteSliceConfig::al_opts = nullopt`) on the Fast arithmetic profile, its rounding-level
twin. The probe's theta is a shadow-clock (variance-time) theta and is not reported as a
calendar theta. This is design D20's gap (risk engine single clock); fix lane F-S9 would add a
per-expiry variance/calendar override to the risk header instead.

## CLI

### `universe_fit_bench.py` (run with `"C:/Program Files/Python312/python.exe" -E -P -B -X utf8`)

| subcommand | does |
|---|---|
| `panel --features-dataset <label-free ds> --output <dir>` | label-free features of every underlier (`shape_cohort.classify` + `universe_archetypes.features`), 18 strata, deterministic draw of ≤ 300 development underliers → `panel-roster.csv`, `panel.json` |
| `prepare --roster <csv> --buckets <b,…> --output <dir>` | per bucket `quote_benchmark.py prepare --split development --symbols <roster> --buckets <b> --csv`, then one source pass for vendor Δ/ν → `<b>/probe-labels.csv` |
| `run --dataset <ds> --output <dir> --binary <quote-bench> --probe <probe> --policy auto|<json> --rows-per-shard N [--kernel both] [--labels csv] [--probe-rows]` | shards the dataset by underlier hash (seed `F-shard-v1`) to ≤ N rows, writes shard datasets, runs the fitter per shard through `quote_benchmark.run_native` (hash provenance) with process CPU and a 600 s guard, writes `basis.csv` from `one_spot`, runs the probe (600 s guard); `ATX_VOL_FIT_WORKERS` must be 1..4 |
| `score --pair <ds>=<run> … --tier labelled-panel|label-free-board [--archetypes csv] [--panel json] --output <json>` | chain scorecards, failure taxonomy, worst-50 lists, aggregates → report (schema below) |
| `report --input <name>=<json> … --output summary.md` | totals, by-bucket table, failure modes, failure × archetype matrix, worst-20 lists, cross-bucket stability |

### `atx-vol-fit-risk-probe`

```
atx-vol-fit-risk-probe --quotes quotes.csv --predictions p.csv --out probe.json
    [--rows probe-rows.csv] [--kernel family|cold|both] [--threads N]
    [--basis basis.csv] [--carry-tolerance X] [--dividends funding-pv|escrowed-pv|none]
    [--labels labels.csv --manifest manifest.json]
```

Exit 0 ok, 2 usage / input error, 3 label governance refusal. `--basis` is `chain_id,spot` (S*
per rebased chain); `--carry-tolerance` is the policy's `snapshot.carry_price_tolerance`. The
reprice identity reads the cold kernel's price when the cold kernel runs (`both`, `cold`), else
the family's.

## Strata (panel) and archetypes

The 18 panel strata are the 9 `shape_cohort.py` classes (7 symbol classes; `short_dated` /
`long_dated` as "the uid has an eligible expiry with dte ≤ 3 / > 365"), the 7 new label-free
`universe_archetypes.ARCHETYPE_RULES`, and the two committed rosters (`leveraged_etp`, `adr`;
issuer / depositary listings, metadata not data). Features never read srVol / srPrc / vendor
IVs or Greeks: straddle vols are quote-mid proxies.

| archetype | rule |
|---|---|
| high_distribution | `dist_yield_1y >= 0.05 and two_sided >= 50` (ddiv_pv / S / T_c at the ~1-year expiry) |
| htb | `median_sdiv >= 0.10 and two_sided >= 50` |
| neg_carry | `median_sdiv <= -0.05` |
| event_front | `front_ratio >= 1.4 and two_sided >= 100` (front ATM straddle vol / 30-day straddle vol) |
| event_kink | `kink_ratio >= 3.0 and two_sided >= 100` (max / median forward straddle variance) |
| strike_ladder | `far_strike_share >= 0.10 and spot >= 20` (share of eligible rows with K/S > 3) |
| low_straddle_vol | `straddle_vol < 0.10 and spot >= 5 and two_sided >= 50` |

Draw: forced §2.4 example tickers first (when development and on the board); quotas by
water-filling (equal shares, capped by pool size, leftover one each in strata order); strata
rarest first take unchosen members in `stable_id(["F-panel-v1", u])` order until their quota
is covered; round-robin top-up to 300.

## Failure modes (primary; first matching rule wins; every chain gets exactly one)

| order | mode | rule |
|---|---|---|
| 1 | unsupported_exercise | every row of the chain is `unsupported_exercise` (European / forward framework) |
| 2 | invalid_input | no fit-eligible row, or an input refusal (invalid_input / invalid_market / inconsistent_chain / unsupported_deliverable / snapshot-spot refusals) |
| 3 | strike_ladder | input trait: ≥ 10 % of eligible rows with K/S > 3 at spot ≥ 20 |
| 4 | insufficient_quotes | graded refusal, or no served row and `insufficient_quotes` dominant |
| 5 | calendar_arbitrage | graded refusal, or no served row and `calendar_arbitrage` dominant |
| 6 | arbitrage_failure | graded refusal, or no served row and `arbitrage_failure` / `market_distance_failure` dominant |
| 7 | fit_failure | graded refusal `fit_failure`, or any other chain with no served row |
| 8 | outside_market | served price containment < 0.95 |
| 9 | price_only_dominant | > 50 % of served price rows `ok_price_only` |
| 10 | kIvMin_floor | a served vol at the 0.005 floor |
| 11 | vendor_disagreement | labels: vol RMSE > 300 bp with containment ≥ 0.99 |
| 12 | greek_refusal | > 20 % of probe-evaluated rows not Ok |
| 13 | reprice_mismatch | probe \|ΔP\| > max(1e-6 P, 1e-8) on a served row |
| 14 | slow_fit | chain fit wall > p99 of its stratum and > 1 s (graded runs only; `--policy auto` reports no per-chain wall) |
| 15 | ok | none of the above |

Secondary tags are the uid's archetypes. Shape breaks reuse `quote_benchmark._shape_breaks`
verbatim on a frame keyed by underlier ⊕ root, so each break names its chain (a calendar
break counts on its earlier expiry); totals equal the helper's own.

## Report JSON (`atx-vol-universe-fit-bench`, schema_version 1)

Top level: `schema`, `schema_version`, `provenance` {git_sha, tool_sha256, binary_sha256,
probe_sha256, policy_path, policy_sha256, dividends, source_sha256,
dataset_manifest_sha256[], host {cpu, fit_workers, load_before[], load_after[]}, created_utc,
timing_qualified = false}, `scope` {date, buckets[], tier, split, panel_rule_sha256,
panel_roster_sha256, labels_opened}, `totals` METRICS, `by_bucket[]`, `by_archetype[]`
(+ `n_uids`), `by_dte_band[]` (≤7, 7-30, 30-90, >90 calendar days), `by_moneyness_band[]`
(K/S: <0.8, 0.8-0.95, 0.95-1.05, 1.05-1.2, >1.2), `failure_modes[]` {mode, rule, chains, rows,
uids, by_archetype, examples ≤ 10}, `worst` {containment, refused_rows, vol_rmse_bp,
greek_refusal, reprice_mismatch, fit_wall_us; ≤ 50 CHAINREF each, ties broken by (uid, root,
bucket, expiry, chain_id)}, `run_cost[]` (per shard rows, fit process CPU / wall, probe wall),
`chains[]` (or `{file, sha256, count}` → `<report>.chains.parquet` past 50k chains).

METRICS: rows, eligible_rows, chains, price_coverage, vol_coverage, containment,
mid_err_spreads_{mae,p95}, shape_{monotone,convex,calendar,duplicate}, refusals {status: rows},
price_only_rows, kivmin_rows, fit_cpu_s (Σ graded chain walls), rows_per_cpu_s,
chain_wall_us_{p50,p99}, labels only: vol_bp_{bias,mae,rmse,p95}, price_mae,
price_in_spreads_mae; probe: greeks_{evaluated,refused,sanity_fail}, reprice_mismatch_rows,
reprice_max_abs.

Chain: chain_id, uid, root, bucket_et, expiry, dte_cal, variance_years, archetypes[], rows,
eligible, two_sided, ask_only, admitted, status_counts {status: rows; "absent" = no
prediction}, fit {grade, refusal, error, point_status, method (profile), calendar_gap,
calendar_gap_strike, outside_fraction, wall_us}, served_price_rows, served_vol_rows,
price_only_rows, containment, mid_err_spreads_mae, max_outside_spreads, shape {monotone,
convex, calendar, duplicate}, api_violations (null: the acceptance audit is not run), labels
{n, vol_rmse_bp, vol_mae_bp, vol_bias_bp, price_mae, price_in_spreads_mae, ref_containment} |
null, greeks {evaluated, price_only, invalid_input, routes {family, cold_analytic, cold_fd,
exercised, refused}, flags {near_edge, cold_fallback, outside_box}, availability {lane status:
rows}, nonfinite_greek, sanity_fail {delta_range, gamma_neg, vega_neg, cp_delta},
reprice_max_{abs,rel,vol_bp}, reprice_mismatch, basis, vendor {n, delta_mae, vega_sym_rel} |
null} | null, kivmin_rows, far_strike_rows, unsupported_rows, failure_mode, secondary[].

### probe.json (`atx-vol-fit-risk-probe`, schema_version 1)

`provenance` {kernel, identity_kernel, al_opts, dividend_model, carry_price_tolerance,
family_n_sigma, family_sigma_box_pad, threads, labels}, `totals` COUNTS, `chains[]` {chain_id,
underlier, root, snapshot_ns, expiry_days, variance_years, basis (own | snapshot_spot |
invalid:<why> | none), counts COUNTS}. COUNTS = rows, state {not_served, price_only,
invalid_input, evaluated}, routes {none, family, cold_analytic, cold_fd, exercised, share,
refused}, lane {ok, model_unavailable, numeric_error, invalid_contract, rolled, refused}, flags
{near_edge, cold_fallback, outside_box}, nonfinite_greek, sanity_fail {delta_range, gamma_neg,
vega_neg, cp_delta}, reprice {checked, mismatch, max_abs, max_rel, max_vol_bp}, vendor {n,
delta_abs_sum, delta_abs_comp, vega_sym_rel_sum, vega_sym_rel_comp, delta_mae, vega_sym_rel}.
Non-finite numbers are `null`. Sanity (carry-aware; every bound allows 1e-9): call Δ ∈ [0, B],
put Δ ∈ [−B, 0] with B = max(1, e^{−qT}) (a q < 0 call is never exercised early, so its Δ is the
European e^{−qT}N(d1) and may exceed 1); Γ ≥ 0; ν ≥ 0; and for a call/put pair at one strike
(neither near the edge nor exercised) (Δc − Δp) ≥ e^{−qT} − 0.10. The pair check is one-sided:
early exercise lifts Δc − Δp above the European e^{−qT}, so only a shortfall is a violation. (The
design's symmetric [0, 1] / [−1, 0] / |gap| ≤ 0.10 rule flagged 609 q < 0 calls, 16 Γ/ν at
−1e-13..−1e-11 and 691 early-exercise pairs on panel bucket 1000 — all correct engine output.)
Vendor parity: |Δ − de| and |ν − ve/0.01| / mean(|ν|, |ve/0.01|), both at the vendor's own
vol, so the vega gap mixes model and vol differences.

## Compute budget (stage F0) and regeneration

REGENERATION_PLACEHOLDER
