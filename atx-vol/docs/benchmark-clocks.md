# Benchmark clocks, stages, counters, affinity and the IV reference contract

Frozen definitions for gate G1 of the vol-sota master plan (lane B0, 2026-10-22; plan
sections 3.1, 3.2 and 8; deep review F05 and 5.6). Every later speed or IV-accuracy claim
toward the sub-second SPY fit is measured with these definitions. This page defines
measurements; it makes no performance claim.

Implementation: `tools/bench_clocks.{hpp,cpp}` (clock arithmetic, receipts, cold reset),
the `bench` object of every `atx-vol-quote-bench --model quote-slice` report
(`tools/quote_bench_main.cpp`), the de-Am stage clock hook (`src/fitting/deam_iv_cache.*`
`DeAmStageClock`), `bench/run_cold_trials.ps1` (fresh-process trials) and
`tools/iv_comparator_main.cpp` (`atx-vol-iv-comparator`). Tests: `atx-vol-bench-clocks`
(gtest) and the `test_native_bench_*` / `test_native_cold_reset_*` /
`test_native_affinity_*` cases of `python/tests/test_quote_bench_cli.py`.

## 1. Clock source and units

All in-process durations use `std::chrono::steady_clock` (monotonic; on Windows
`QueryPerformanceCounter`, whose frequency the affinity receipt records). Every duration
is in **seconds** unless the field name says `_us` / `_ns`. The process startup sub-timer
is the one exception: kernel process creation time (`GetProcessTimes`) to `main`
entry, read against `GetSystemTimePreciseAsFileTime`, best effort.

Statistics over reps or trials (`min`, `median`, `p95`, `p99`, `max`) are linear
interpolations between order statistics: quantile `p` sits at position `p * (n - 1)` of
the sorted values. A p95 / p99 of fewer than 100 trials is a descriptive order statistic,
not a tail estimate; F1/F2 qualification needs at least 100 fresh trials (plan 3.3).

## 2. Clocks (`bench.clocks`)

| Key | Start | End | Contents |
|---|---|---|---|
| `legacy_fit_reprice_s` (= report `fit_and_reprice_seconds`) | `run()` after row classification and chain grouping | after per-profile counts | the historical timer, retained unchanged for comparisons |
| `cold_calibration_s` | the resident rows are handed to `run()`, right after the policy load | same end as legacy | classification, grouping, chain consistency, chain markets (median), ambiguity, policy routing, dividend-schedule inference, carry preparation, every fit (de-Am inversions and inverse memories, curve solve, certification, repairs), calendar validation, row publication. Excludes file I/O and the policy load (`policy_load_s`). No previous fitted surface: a fresh process, or `--cold-reset` |
| `ready_to_quote_s` | — | — | `status: not_built`: quote-bench builds no serving tier (records, ladders, primary risk). Never reported as zero |
| `process_wall_s` | kernel process creation | predictions CSV written | sub-timers below; the report writes are excluded (they carry the value) |

`process_wall_s.sub_timers`: `startup_s` (creation -> `main`), `setup_s` (arguments and
pinning), `read_s` (file open, header, every `getline`), `decode_s` (per-record parse and
admission checks), `compute_s` (every rep of `run()`, policy load included),
`policy_load_s` (rep 1, inside `compute_s`), `write_s` (predictions CSV).
`unattributed_s` = value - the sum of the other sub-timers.

Warm update and hot query (plan 3.1) are not measured by quote-bench: it has no published
version to update or query.

`--reps N` (1..1000) repeats `run()` in-process; `bench.per_rep[]` holds each rep's
legacy and cold clocks, policy load, de-Am thread-memo entries at start and end, the solve
ledger delta, the cold-reset receipt and `predictions_identical_to_rep1` (every published
field, prices and vols bit for bit). Rep 1's statistics, stages, counters and predictions
are the run's.

## 3. Exclusive stages (`bench.stages`, requires `--timing-detail`)

`PhaseClock` laps: each lap charges the time since the previous lap to one stage, so the
laps partition `[cold start, cold end]` with no gap and no overlap. A fit lap is split by
what the fit measured (fit_timing buckets of its chains, the de-Am stage clock); the part
of the lap no chain claimed goes to the lap's residual stage. Over-claims are clamped and
reported, never charged.

| Stage | Source |
|---|---|
| `normalization` | harness laps: `run()` from cold start to the fit dispatch; each snapshot's worklist build; each static chain's preparation |
| `family_build_validation` | `seconds: null, status: not_built` at this root: no family build or family validation on the fit route |
| `inverses` | fit_timing `deam` bucket - `endpoints` - `reference_exceptions` |
| `endpoints` | `DeAmStageClock.endpoint_ns`: band-edge prices at kIvMin / kIvMax and their ambiguous-edge accurate re-prices (`DeAmIvView::band`) |
| `curve_repair` | fit_timing `solve` + `project` (the repair-rung cross-cut is inclusive of buckets and not added) |
| `final_grid_validation` | fit_timing `price` + `certify` + `calendar`; the static path's calendar checks |
| `reference_exceptions` | `DeAmStageClock`: the whole wall of every Premium inversion that fell back to Search (`premium_fallback_ns_by_reason`, index = `IvPremiumFallback`) and of every Fast inversion redone on the accurate kernel (`fast_redo_ns`) |
| `publication` | harness laps: row publication after each fit, the post-run calendar-refusal reset and per-profile counts |
| `fit_other` | fit_timing `other` bucket (chain preparation, containment, carry) |
| `surface_other` | a surface-fit lap minus its chains' buckets (term prior, pass scheduling) |

`sum_s` = the stage sum; `unattributed_s` = `cold_calibration_s - sum_s` (floating
rounding only, by construction); `reconciles_within_1pct` = |unattributed| <= 1% of the
cold clock; `attribution_conflict_s` = clamped over-claims (fit_timing rounds each bucket
to 1 us); `attribution_valid` = one fit worker and zero conflict. With more than one fit
worker chains overlap in wall time and the split is not exclusive: attribution runs use
`ATX_VOL_FIT_WORKERS=1`. The instrumentation reads the clock at every bucket switch:
`--timing-detail` timings are diagnostic unless the host is quiet.

## 4. Counters (`bench.counters`)

- `rows`: `input_rows`, `supported_input_rows` (input status ok), `admitted_quotes`,
  `served_rows` (status ok), `price_only_rows`, `served_price_rows` (ok + ok_price_only),
  `served_iv_rows` (ok with a finite vol), `rows_by_status`, `fitted_chains`, `chains`,
  `publication` (`rows_published`, `graded_slice_records`).
- `solve_ledger`: the always-on solve ledger delta of rep 1 (`sl_al_boundary_solves` =
  source boundary solves, `sl_iv_newton_iters`, Greek bundles, ...), merged over threads.
- `fit_route` (`--timing-detail`): fit_timing summed over timed chains: bucket walls (us),
  AL boundary solves by bucket, `grid_prices_al_solves` (price bucket),
  `repaired_node_al_solves` (certify bucket), certification passes, channel solves,
  Jacobian evaluations, rung attempts, calendar pairs, projections, repair-rung visits,
  `inversions`, `premium_inversions`, `premium_fallbacks` and `_by_reason`,
  `inversion_al_solves_by_reason`, `confirms`, `confirms_skipped`, `ambiguous_edges`,
  `seed_routed`.
- `deam_stage_clock` (`--timing-detail`): `endpoint_bands`, `endpoint_s`,
  `premium_fallback_s` and its ns by reason, `fast_redos`, `fast_redo_s`.
- `families` (attempted / built / refused / lane prices / lane vegas): `unavailable: no
  family route on this fit`.
- `simd` (`vector_pack_dispatches` = complete 4-lane AVX2 packs, `scalar_equivalent_lanes`
  = known scalar wrapper lanes + scalar fallback lanes): from the exact counter plane;
  compiled out of the shipping build (`ATX_VOL_COUNTERS` off), so `unavailable`, never 0.

## 5. In-process cold reset (`--cold-reset`, `bench::cold_reset()`)

Before every rep: clears the calling thread's de-Am IV memo (`deam_iv_memo_clear`; the
fit worker at `ATX_VOL_FIT_WORKERS=1`; wider fan-outs run each pass on fresh jthreads whose
memo dies at the join) and the process-wide carry memo (`carry_memo::reset`). The receipt
lists both with the entry counts before, and what is deliberately not reset: the per-expiry
rung memo (a member of each `fit_quote_surface` call; never crosses fits), the American IV
thread ALO slot (allocation scratch re-initialised per inversion), market-independent
static tables (first-use only; the fresh-process runner pays them), telemetry (ledger,
sampler; reps read deltas), and OS / CPU caches.

Proof: `BenchColdReset.ResetEmptiesTheThreadMemoSoTheSecondRepRepeatsTheFirst` (memo size
0 at rep 2, no reuse, identical inversion / model-price / AL-solve counts, identical rows;
a warm control without reset reuses the memo and does less work) and
`test_native_cold_reset_repeats_rep1_work_where_a_memo_crosses_reps` (graded,
`rung_memo false`). Timing bound, secondary to the exact work equality: a cold rep 2 takes
at least `kColdRepMinRatio = 0.5` of rep 1's wall (a warm memo hit runs several times
faster).

## 6. Affinity receipt (`bench.affinity`; `--affinity CPU`, `--require-pcore`)

`--affinity CPU` (group 0, CPU < 64) sets the process affinity mask and the calling
thread's mask to that CPU before the input is read; it requires `ATX_VOL_FIT_WORKERS=1`
(one fit worker, no nested fan-out). `--require-pcore` refuses, before any output, a CPU
that is not on a performance core. Receipt fields: `requested_cpu`, `pinned`,
`cpu_model` (CPUID brand), `logical_processors`, `physical_cores`, `heterogeneous`,
`max_efficiency_class`, the subject CPU's `efficiency_class`, `is_pcore`
(`EfficiencyClass == max` of `GetLogicalProcessorInformationEx` RelationProcessorCore),
`core_mask`, `smt`, `smt_siblings`, `process_affinity_mask`, `system_affinity_mask`,
`thread_affinity_mask`, `current_cpu_at_receipt`, `busy` (per-CPU kernel + user - idle over
the compute window from `NtQuerySystemInformation`: `subject_cpu_pct`, `smt_sibling_pct`,
`smt_sibling_idle_below_5pct`, `system_pct`; best effort, null when a counter did not
advance), `fit_workers` (`env_ATX_VOL_FIT_WORKERS`, `resolved_auto`), `nested_pools`
(surface expiry fan-out width; the pricing executor is not on the quote-slice route),
`power` (`active_scheme_guid`; `cpu_mhz` from `CallNtPowerInformation`, which reports the
rated / limit frequency, not the instantaneous turbo clock), `timer` (QPC frequency,
system timer resolution in 100 ns: coarsest / finest / current), process priority class and
thread priority. An unpinned run carries the same receipt for the CPU it was on.

## 7. Fresh-process runner (`bench/run_cold_trials.ps1`)

`-Trials N` fresh processes, one trial each, `ATX_VOL_FIT_WORKERS=1`, optionally pinned
(`-Affinity`, `-RequirePCore`), `-TimingDetail` for stages. Never overwrites (`-OutDir` must
not exist). `manifest.json`: command template; sha256 of binary, policy and input; source
SHA and dirty flag of `-SourceRoot`; `build_info` from the binary's embedded build info
(`bench.build_info`: compiler, `cxx_flags`, `isa_compiled`, `fma_compiled`, `fast_math`,
`iec559_double`, runtime SIMD dispatch, exact-counter plane) or, when absent, from
`-BuildDir`'s `CMakeCache.txt`, with `build_info_source` saying which; `powercfg` active
scheme; per-trial exit code, clocks, process sub-timers, stages and affinity summary;
summaries of every clock; `pcore_proof` (every trial on the requested CPU and on a
performance core); `predictions_identical` with the predictions sha256. Exit 0 only when
every trial exited 0, the binary is not fast-math, and every trial's predictions are
byte-identical.

## 8. Independent IV comparator (`atx-vol-iv-comparator`)

For each served row (status ok) of a prediction CSV, the American IV of the served price is
bracketed with the cold reference pricer and bisected: bracket from `served_iv * (1 -+
1e-3)`, widened x4 inside `[kIvMin, kIvMax]`, then halved until `hi - lo <= 2 * tolerance`
(default 1e-12). No Newton step, no family, no memo. **Units: absolute volatility (0.20 =
20%); the requirement `1e-7` is 0.001 vol bp.** Reference: Andersen-Lake accurate preset
(`std::nullopt`, `AlProfile::Reference`). The contract is rebuilt from the input CSV:
chain market = median of the chain's supported rows; `r_eff = rate * T_cal / T_var`;
`q_eff` = the row's own yield on the variance clock with the `--dividends` aggregate PV
folded in; `T = T_var`.

Row statuses: `bracketed` (counted in `abs_iv_error` and its `over_threshold`);
`not_identified` (the reference price does not move with sigma at the served IV: a
1e-5 relative step on both sides stays within `1e-12 * max(P, 1)`, e.g. a deep
in-the-money put priced at intrinsic; reported with the sigma interval that reprices it
and whether the served IV lies in it, never counted as a root error); `unbracketed`
(outside the identifiable range); `reference_error`; `price_only`; `dated_cash_route`
(excluded with `--report`: not a continuous-carry contract). Reference uncertainty: the
same price on a finer scheme (32 nodes, quadrature 64 / 64, 16 sweeps, tol 1e-14) minus
the accurate preset at the bracketed root, in USD and through the reference vega in vol;
`reference_unresolved_rows` counts rows whose uncertainty exceeds the threshold (those are
not passes). Continuous-carry price table: `|P_ref(served_iv) - served_price|` in USD and
relative (P >= 0.01) and its Newton distance in vol. Per-expiry and per side x moneyness
(`|ln K/S| <= 0.1`, below, above) tables accompany every aggregate. The tool refuses to
compile under fast-math (`#error` on `__FAST_MATH__`, `_M_FP_FAST`, finite-math) and its
summary carries the embedded build info.

## 9. Receipt fields (plan section 8) and where they live

| Field | Location |
|---|---|
| source_sha, source_dirty | runner `manifest.source` |
| binary_sha256, policy_sha256, input_sha256 | runner `manifest.binary` / `policy` / `input` |
| compiler, flags, isa | `bench.build_info` (`compiler`, `cxx_flags`, `isa_compiled`), runner `build_info` |
| reference_sha, eligibility_version | not measured by quote-bench (scorer / reference lanes) |
| cpu_model, core_type, affinity, smt_state | `bench.affinity` (`cpu_model`, `is_pcore` / `efficiency_class`, masks, `smt_siblings` + `busy`) |
| nested_workers, load, clock_state | `bench.affinity` (`fit_workers`, `nested_pools`, `busy.system_pct`, `power`, `timer`) |
| clock_scope, cold_state_reset, reps | `bench.clocks.*.scope`, `bench.cold_reset` + `per_rep[].cold_reset`, `bench.reps` |
| min, median, p95, p99, max | `bench.clocks.*.summary` (in-process reps), runner `summary` (fresh trials) |
| stage_seconds | `bench.stages` |
| boundary_solves, inverse_iterations, confirmed_roots | `bench.counters.solve_ledger`, `fit_route` |
| family_builds, family_prices | `bench.counters.families` (`unavailable` at this root) |
| skipped_roots_by_error_class, reference_fallbacks_by_reason | `fit_route.confirms_skipped`, `premium_fallbacks_by_reason`, `deam_stage_clock` |
| input_rows, served_price_rows, served_iv_rows, price_only_rows, refused_rows_by_reason, complete_expiries | `bench.counters.rows` |
| eligible_price_rows, identifiable_iv_rows | scorer (`tools/quote_benchmark.py`), comparator `status_counts` |
| price_residual_usd, independent_iv_error, reference_uncertainty | comparator summary (`price_table`, `abs_iv_error`, `reference_uncertainty_*`) |
| greek_errors, containment, mae_usd, mae_spread, iv_bp_quantiles, shape_violations, paired/gained/lost/per-class metrics | scorer and later lanes (not B0) |
| published_bytes, peak_memory, allocations_hot, version_race_failures | not measured by B0 |
| commands, exit_codes, artifact_manifest | runner `command_template`, `exit_codes`, `trials[]` with report sha256 |
| reviewer_sha, decision | review / gate records |
