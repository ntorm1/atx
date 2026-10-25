# Benchmark clocks, stages, counters, affinity and the IV reference contract

Historical definitions for gate G1 of the vol-sota master plan (lane B0, 2026-10-22;
plan sections 3.1, 3.2 and 8; deep review F05 and 5.6), plus the explicitly versioned
retained-publication boundary in section 11. Comparisons must identify their clock
contract and use the same boundary. This page defines measurements; it makes no
performance claim. The economic admission policy treats strict IV error and equality
between different valid fitted solutions as diagnostics.

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

Warm update and hot-query latency (plan 3.1) are not measured by these quote-bench
clocks. Section 11 retains a published surface for correctness queries only; that
diagnostic is not a hot-query latency benchmark.

`--reps N` (1..1000) repeats `run()` in-process; `bench.per_rep[]` holds each rep's
legacy and cold clocks, policy load, de-Am thread-memo entries at start and end, the solve
ledger delta, the cold-reset receipt and `predictions_identical_to_rep1` (every published
field, prices and vols bit for bit). This equality field is diagnostic, not economic
qualification. Rep 1's statistics, stages, counters and predictions are the run's.
The post-fit audit mode in section 11 requires exactly one repetition.

## 3. Exclusive stages (`bench.stages`, requires `--timing-detail`)

`PhaseClock` laps. A harness lap charges the time since the previous mark to one stage
(the stage's boundaries are the marks). A fit lap charges only what the fit measured:
the fit_timing buckets of its chains, with the family build wall and the de-Am stage
clock carved out of the deam bucket. The part of a fit lap that no chain timer measured
(surface scheduling and the term prior on the graded path, the static path's work around
its chain recorder) is charged to NO stage: it is accumulated as `unclaimed_lap_s`.
Over-claims are clamped and reported as `attribution_conflict_s`, never charged.

| Stage | Source |
|---|---|
| `normalization` | harness laps: `run()` from cold start to the fit dispatch; each snapshot's worklist build; each static chain's preparation |
| `family_build_validation` | fit_timing `deam_family_build_us` (sigma / (sigma, q) family node solves and probe validation, `graded.family`), carved out of the `deam` bucket. `seconds: null, status: not_built` when the run built no family (the default policy) |
| `inverses` | fit_timing `deam` bucket - `family_build_validation` - `endpoints` - `reference_exceptions` |
| `endpoints` | `DeAmStageClock.endpoint_ns`: band-edge prices at kIvMin / kIvMax and their ambiguous-edge accurate re-prices (`DeAmIvView::band`) |
| `curve_repair` | fit_timing `solve` + `project` (the repair-rung cross-cut is inclusive of buckets and not added) |
| `final_grid_validation` | fit_timing `price` + `certify` + `calendar`; the static path's calendar checks |
| `reference_exceptions` | `DeAmStageClock`: the whole wall of every Premium inversion that fell back to Search (`premium_fallback_ns_by_reason`, index = `IvPremiumFallback`) and of every Fast inversion redone on the accurate kernel (`fast_redo_ns`) |
| `publication` | harness laps: row publication after each fit, the post-run calendar-refusal reset and per-profile counts |
| `fit_other` | fit_timing `other` bucket only (chain preparation, containment, carry inside the chain recorder) |

`sum_s` = the stage sum (measured time only); `unattributed_s` = `cold_calibration_s -
sum_s`, where the cold clock is its own pair of stamps: the time no stage measured. It is
NOT zero by construction: an uninstrumented region inside a fit lap (an untimed family
build, surface scheduling) lands here, never in a stage. `unclaimed_lap_s` = the lap
clock's own record of that time, and `span_check_s` = `unattributed_s - unclaimed_lap_s`
(zero to rounding when the laps span the cold clock; nonzero means a mark was missed).
`reconciles_within_1pct` = |unattributed_s| <= 1% of the cold clock, and it fails when the
stages left more than 1% unmeasured (gtest
`BenchStages.UntimedRegionIsUnattributedAndFailsTheOnePercentCheck`).
`attribution_conflict_s` = clamped over-claims (fit_timing rounds each bucket to 1 us);
`attribution_valid` = one fit worker and zero conflict. With more than one fit worker
chains overlap in wall time and the split is not exclusive: attribution runs use
`ATX_VOL_FIT_WORKERS=1`. The instrumentation reads the clock at every bucket switch:
`--timing-detail` timings are diagnostic unless the host is quiet.

Known inclusions: the harness stages `normalization` and `publication` also hold
quote-bench's own diagnostic JSON bookkeeping (`policy_decisions`,
`graded_stats.slices`, per-profile counts; about 10 ms on SPY) inside the cold clock.
Under `--timing-detail` the static (non-graded) path's fits run inside a fit_timing
`ChainRecorder`; with `--deam-audit` that also runs the audit's extra Search inversions
on static chains (predictions unchanged; an opt-in path's cost changes).

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
- `families` (`--timing-detail`, fit_timing): `built`, `build_validation_s`,
  `iterate_prices`, `refused_premium_attempts`, `grid_prices`,
  `grid_prices_on_fast_instead` (real counts; zero when the policy builds no family);
  `lane_vegas` (lane K4b) counts the native route's lane-vega hook (an acceptance that placed
  its confirm with the lane's fused vega: no covering envelope, or a refused chord);
  `attempted` (family sides the adaptive rule asked for), `refused` (= attempted - built) and
  `refused_by_reason` (lane K4b, review-K4 m8: `not_offered`, `no_continuous_model`,
  `short_expiry`, `invalid_carry`, `call_no_carry`, `prepare_refused`, `uncertified`; they sum
  to `refused`; per chain `graded_stats.slices[].timing.deam_family_refused_by_reason`).
- `fit_work.native_route` (`--timing-detail`, the native fit route; lane K4, sub-clocks K4b):
  `subclock` (ms, summed over the chains): `request` (every Premium request's wall),
  `lane` (the family-native lane: `invert_many`), `family_accept` (K3's enclosure / family
  sides / chord family prices), `confirm` (confirm-kernel solves), `leg` (the seed leg net of
  its confirms), `reference` (the bounded reference route), `other` = request minus the five,
  `family_side` = lane + family_accept, `endpoint` (band edge decisions), `prefill` (the
  batched route's per-chain prefill, `graded.native_batch`: its lane / family / confirm work
  is ALSO charged to those clocks), `grid_family` / `grid_kernel` (the certification grid's
  family per-point-bound prices / kernel prices). `batch`: `calls`, `lanes`, `avx2_calls`
  (lanes prepared, AVX2 kernel calls), `lane_hits` / `lane_misses` (requests that consumed a
  prepared lane / ran their own), `confirms` / `confirm_hits` (decisive batched confirm prices
  prepared / consumed; accurate confirm kernel only). `reference_wall_refused`: requests the
  reference wall budget stopped (`graded.reference_budget.wall_us`). Per chain:
  `graded_stats.slices[].timing.deam_sub_*_ns`. No sub-clock runs (or costs) without
  `--timing-detail`.
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
performance core); `predictions_identical` with every trial's predictions sha256.
Under the economic admission update, equality is diagnostic and differing hashes
no longer cause launcher exit 3. Successful collection still requires successful
processes, no fast-math and every explicitly requested host gate. Qualification
requires the independent verifier: different solutions need their own full proof.

SMT sibling: `pcore_proof` is the core-type proof only. Each trial also records
`smt_sibling_busy_max_pct` and `smt_contended` (sibling busier than
`-SiblingBusyThresholdPct`, default **10%**, over the compute window; null when the
sampler did not advance). The manifest carries `smt_sibling_busy_threshold_pct`,
`smt_contended_trials`, `smt_unmeasured_trials` and `smt_sibling_idle_all_trials`, and the
`COLD_TRIALS` line prints `smt_contended=k/n` next to `pcore_proof`. `-RequireIdleSibling`
fails the run (exit 4) on any contended or unmeasured trial.

Named workloads (the G1 receipt names one; their numbers are not comparable):
- **default graded-v1 SPY**: `quotes-SPY.csv` (cohort5 fit-timing input) with
  `slice-coverage-20261016-policies/graded-v1.json` (sha256 `d7f89d13...`, the R3 graded
  defaults, no family route).
- **section 9 (deep review) SPY**: the same input with
  `SPY1s-20261021-phase3/policies/graded-v1-famq-grid-native.json` (sha256 `9091b102...`:
  `graded.family adaptive`, `family_q`, `family_native`, `grid_kernel family`,
  `seed_gate_kappa 4.3e-05`). Needs `graded.family_native`, present from root
  `9f05a40e` (W2c-d); older binaries refuse the policy.

## 8. IV root-finding comparator (`atx-vol-iv-comparator`)

**What it checks and what it does not.** The pricer is the fitter's own Andersen-Lake
accurate preset (`std::nullopt`, `AlProfile::Reference`): the comparator's independence is
in the ROOT FINDING only (bracket + bisection; no Newton, no family, no memo). On a
curve-priced row the served price IS the reference price at the served IV (within 1e-10
USD: summary `served_pair_consistent`, e.g. 1279 of 1279 bracketed SPY rows), so the IV
check there is circular for pricer error: |served - bracketed| is bounded by the bisection
tolerance by construction, and only the fine-scheme reference uncertainty speaks to the
pricer. The genuine root checks are the rows whose served pair is not consistent
(re-implied, repaired or moved nodes, excluded-side rows). The scope is served (price,
iv) pairs; the fitter's quote inversions (de-Am bid / ask / mid IVs) are not compared.
The bracket is seeded at the served IV; on a monotone price map the root does not depend
on the seed. Follow-up: **PDE-backed pricer option (D0's
`american_discrete_div_reference` with zero dividends) for true pricer independence.**

For each served row (status ok) of a prediction CSV, the American IV of the served price is
bracketed and bisected: bracket from `served_iv * (1 -+ 1e-3)`, widened x4 inside
`[kIvMin, kIvMax]`, then halved until `hi - lo <= 2 * tolerance` (default 1e-12).
**Units: absolute volatility (0.20 = 20%); the requirement `1e-7` is 0.001 vol bp.** The
contract is rebuilt from the input CSV:
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
`reference_unresolved_rows` counts rows whose uncertainty exceeds the threshold or is
undefined (no reference vega); those are not passes.

Populations (summary `populations`, per-row CSV column `population`; every served row in
exactly one; plan 3.2; rule in `bench_clocks.hpp` `classify_iv_row`, with u = the
reference uncertainty in vol and err = |served - bracketed|):
- `resolved_pass` / `resolved_fail`: u finite and <= the threshold; err <= / > the
  threshold.
- `decidable_fail`: u above the threshold, but **err > threshold + 3 u**
  (`decidable_fail_margin` = 3): even a reference off by three times its own estimated
  error cannot make the row pass. Applied to every bracketed row with u above the
  threshold.
- `reference_unresolved`: every other bracketed row (u above the threshold and err within
  threshold + 3 u, or u undefined because the reference vega is zero), with
  `reference_unresolved_over_threshold` and `uncertainty_undefined`. Not a pass.
- `iv_not_identified`, and `other_statuses`.

`populations.non_circular` counts, per population, the bracketed rows whose served pair is
NOT consistent (|P_ref(served_iv) - served price| > 1e-10 USD): only those are genuine root
checks; every other bracketed row is circular for pricer error. A resolved-pass count is
therefore NOT a coverage claim: on the r5 defaults SPY has 1026 resolved passes of which 0
are non-circular, and cohort5 5258 of which 5 are non-circular (one non-circular row,
AAPL call K=10, is the one `decidable_fail`). The headline reads `resolved pass (n
non-circular) / resolved fail / decidable fail / reference unresolved / IV not
identified`; the `abs_iv_error.over_threshold` of all bracketed rows is not a failure
count. Schema `atx-vol-iv-comparator-v3` (v2 plus `decidable_fail`, `non_circular`,
`decidable_fail_margin`, `decidable_fail_rows`; a v2 `reference_unresolved` row with a
decidable failure is a v3 `decidable_fail`). Continuous-carry price table: `|P_ref(served_iv) - served_price|` in USD and
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
| family_builds, family_prices | `bench.counters.families` (`built`, `iterate_prices`, `grid_prices`; `--timing-detail`) |
| skipped_roots_by_error_class, reference_fallbacks_by_reason | `fit_route.confirms_skipped`, `premium_fallbacks_by_reason`, `deam_stage_clock` |
| input_rows, served_price_rows, served_iv_rows, price_only_rows, refused_rows_by_reason, complete_expiries | `bench.counters.rows` |
| eligible_price_rows, identifiable_iv_rows | scorer (`tools/quote_benchmark.py`), comparator `status_counts` |
| price_residual_usd, independent_iv_error, reference_uncertainty | comparator summary (`price_table`, `populations`, `abs_iv_error`, `reference_uncertainty_*`, `served_pair_consistent`) |
| greek_errors, containment, mae_usd, mae_spread, iv_bp_quantiles, shape_violations, paired/gained/lost/per-class metrics | scorer and later lanes (not B0) |
| published_bytes, peak_memory, allocations_hot, version_race_failures | not measured by B0 |
| commands, exit_codes, artifact_manifest | runner `command_template`, `exit_codes`, `trials[]` with report sha256 |
| reviewer_sha, decision | review / gate records |

## 10. CPU interval validity (2026-10-24)

New receipts identify `busy.interval_policy = strict-per-cpu-v1` and retain the
before/after per-CPU busy and total counters as decimal strings in 100-ns units.
This preserves all integer bits for independent replay. The Windows sampler
rejects incomplete native records, negative signed counters and idle time above
kernel time; kernel includes idle. An unavailable sample stays empty.

A CPU percentage requires equal nonempty vector shapes, busy no greater than
total in each snapshot, nondecreasing busy, strictly increasing total, and a busy
delta no greater than the total delta. Invalid intervals produce JSON `null`,
never a clamped zero. A system percentage requires every per-CPU interval to pass
and checked integer delta sums not to overflow. A valid subject percentage can
coexist with an unavailable system percentage when another CPU is invalid.
Unknown topology cannot establish an idle SMT sibling, and an out-of-range
subject cannot alias a valid core through a mask shift.

The two retained runtime regressions show the old defect: a busy counter of
50 then 49 with increasing total was published as 0%, and a peer whose busy
delta exceeded its total delta still produced a system percentage. Seven new
controls cover these cases, shape mismatches, exact large-counter arithmetic,
aggregate overflow and raw receipt retention; the full owning executable passes
20 tests. This does not retrospectively repair archived measurements without raw
counters. Earlier contaminated runs retain their original unqualified status;
new quiet-host admission still requires both actual sibling and whole-host gates.

## 11. Retained publication and post-fit actual-API proof (2026-10-24)

`--post-fit-acceptance-audit` is an opt-in tool mode for one graded quote-slice run
with an explicit policy, predictions and report path. It is incompatible with the
historical inline `--acceptance-audit`, de-Am audit, inverse replay and candidate
capture; combined census builds also reject a simultaneous row census.
The historical inline audit remains a nonqualifying diagnostic inside the fit.

The new mode moves the actual fitted `QuoteSurfaceSlice` vectors into a tool-owned
container, together with their original member IDs. Capture allocation, identity
copying and moves occur inside the cold interval. It does not copy or reconstruct
fits, reload an archive, or run a second fit. Refused slots remain in the retained
vector. Whole-fit errors, missing owners and unsupported static fallback are
explicitly incomplete evidence, never silently omitted proof.

The clock contract is `retained_surface_publication_v1`, emitted both in
`bench.clocks.cold_calibration_s.contract` and the proof marker. Cold latency ends
after predictions and a still-queryable owned surface have been published. The
historical graded path destroys its local fitted surface inside the cold interval;
this new mode retains it. A campaign or control/candidate comparison must not mix
these lifetime boundaries. No estimated diagnostic or destruction duration is
subtracted from a larger measured interval.

The execution order is publication and cold stop, return through the fit stage
clock and counter snapshots, process compute stop and CPU sample, prediction CSV
write, then actual-API proof on those same retained objects. The existing
`process_wall_s` still ends when predictions have been written. The proof and final
report write occur afterward. API queries and cold Reference prices can touch
diagnostic caches/counters, but cannot contaminate already frozen fit counters or
CPU intervals.

The same native report contains `graded_stats.acceptance_audit`, with its existing
`atx-vol-final-api-audit-v1` schema and additional `phase: post_fit`. The new
`bench.post_fit_acceptance_audit` marker has schema `atx-vol-post-fit-api-proof-v1`
and phase `after_cold_publication_and_fit_snapshots`. It records:

- attempted, retained and serialized snapshot/member counts and chain IDs;
- same-instance ownership, retention inside cold, and release after proof;
- surface-fit invocation counts before/after proof and zero refits;
- input, policy, executable and this trial's prediction hashes;
- missing/unsupported IDs, capture failures and completion status;
- retained vector capacities, `audit_s` and `retained_owner_destruction_s`.

The retained owner is released after proof; its destruction has its own interval.
This is not all process cleanup: serving records may have another shared owner,
and rows/statistics remain alive. A completion marker says evidence was produced,
not that it passes the financial checks. Hash changes or missing evidence produce
an incomplete marker and nonzero exit where a diagnostic report can be written;
allocation or serialization failures cannot produce a qualifying receipt.

The root economic verifier validates the marker, process identity, artifact hashes,
clock definition and every trial's own full thirteen actual-API checks. It does so
even when a marked trial happens to match another CSV. Historical ordinary trials
may reuse an already qualified identical-output proof under the separately bound
binary/input/policy contract. This optional reuse is not a numerical identity
requirement for new algorithms. The original raw boxes, complete valid-row/slice
coverage, arbitrage checks, truthful IV semantics and economic price-error budget
remain unchanged.

Producer controls cover real moved-fit lifetime, lifecycle ordering, failed and
refused outcomes, duplicate/member shape errors, injected allocation failure, CLI
admission, actual API/prediction joins and unchanged fit counter snapshots. These
are correctness tests; no new fit latency is claimed by this change.
