# Task H-3 report: mining verb (STOPPED at owner instruction, part 1 of the engine glue committed)

Lane H3, worktree `C:/atx-wt/pool-4`, branch `feat/platform-v8-h3-20260929`, base `32630662`.
Nothing was built or run (lane rules). One code commit: `11ff84c3`. The PM relayed the owner's
instruction to stop at the next logical break point; the component in progress (fidelity helpers)
was finished, the role-adapter lift that was already written was wired into the runner so nothing is
left half-connected, and the two source pins were re-set. The verb, the search-driver changes and the
fixture acceptance are **not** written; section "STOPPED HERE" lists them with the design worked out.

## What was built (commit `11ff84c3`)

| file | change |
|---|---|
| `atx-engine/include/atx/engine/data/role_panel.hpp`, `src/data/role_panel.cpp` (new) | G1 panel adapter, lifted from the IC runner |
| `atx-engine/include/atx/engine/factory/ic_research.hpp`, `src/factory/ic_screen.cpp` | label accessor + the runner's IC recipe as a function |
| `atx-engine/include/atx/engine/factory/fidelity.hpp`, `src/factory/fidelity.cpp` | G6 instrument-stride racing helpers |
| `atx-engine/include/atx/engine/data/research_window.hpp` (new here) | imported byte-identical from W0-1 (lane w0e, `880faac7`), sha256 `cad9268d...51d4` |
| `atx-impl/src/strategy_ic_library.cpp`, `strategy_ic_runner.cpp` | call the lifted functions; the local copies are deleted |
| `atx-impl/src/strategy_ic_signal_cache.cpp`, `strategy_ic_result_cache.cpp` | pins re-set |
| `atx-engine/CMakeLists.txt` | `src/data/role_panel.cpp` source; Debug `/O2` entry for it |

Interfaces as coded:

```cpp
namespace atx::engine::data {                       // data/role_panel.hpp
// The runner's dsl_panel, verbatim: base columns + extra columns, all borrowed; only the
// 1 B/cell presence mask is owned (copied from base).
core::Result<alpha::Panel> overlay_panel(const alpha::Panel& base, std::vector<std::string> extra_names,
                                         std::vector<std::span<const f64>> extra_columns);
// The runner's guard_for, verbatim: cumulative excluded one-day returns (|log r| > 1.5, or
// |log r| > |raw log r| + .10), the prepare_research_ic bad_return_prefix.
core::Result<std::vector<u32>> research_return_guard(const StrategyRoleData& role);
}
namespace atx::engine::factory {                    // factory/ic_research.hpp
std::span<const f64> ResearchIcCache::labels(usize horizon_index) const noexcept;  // rows x names, NaN = no label
usize ResearchIcCache::label_rows(usize horizon_index) const noexcept;             // == rank_series(k).size()
IcScreenConfig research_window_ic_config(usize begin, usize end, usize min_names, usize min_dates,
                                         u64 max_cache_bytes) noexcept;  // equivalence, {5,21,63,0}, maturity = end
// factory/fidelity.hpp
core::Result<std::array<Rung,3>> instrument_rungs(std::span<const u32> strides);   // {1,s,0}... then full
bool instrument_only(const FidelityCfg& cfg) noexcept;  // every low rung: date_stride 1, n_folds 0
template <class T> core::Result<std::vector<T>> strided_cells(std::span<const T> cells, usize dates,
    usize instruments, u32 date_stride, u32 inst_stride);  // strided_panel's geometry (masks, guards)
}
```

What was lifted from the runner, and how: `dsl_panel` (strategy_ic_library.cpp) and `guard_for`
(strategy_ic_runner.cpp) moved to `engine::data` with their bodies unchanged (only `co::`/`al::`
spelled `core::`/`alpha::` and the lines re-wrapped to 100 columns); the five lines of `score_role`
that configured the IC screen became `research_window_ic_config` with the same field values. Call
order in `score_role` and `FieldResidency::panel_for` is unchanged.

## How root verifies

Build (target-scoped): `atx-impl-strategy-ic-tests atx-equity-strategy-ic atx-engine-factory-tests`.
`atx-engine-factory-tests` compiles the fidelity/ic_research header changes in every factory TU.

gtest:
- `atx-impl-strategy-ic-tests --gtest_filter=StrategyIcRunner.*:NoComposition.*:FieldCaps.*:Workers.*:StrategyIcComposition.*:IcScreen.*:ResearchIc.*:MarginalIc.*:CombineMarginalRankIc.*`
  (includes the tripwires `StrategyIcRunner.VmSourcesPinnedToSemanticsVersion` and
  `StrategyIcRunner.IcSourcesPinnedToSemanticsVersion`, which must pass with the new digests).
- `atx-engine-factory-tests --gtest_filter=FactoryFidelity*:FactorySearch*:ResearchIc*:IcScreen*`
  (no expected value changed; `strided_panel`, `race`, `first_full_rung` untouched).

Pins (re-set with the Python mirror of `expect_sources_pinned`, which reproduced both committed
digests on the base before any edit):
- `dsl_vm_sources`: 30 -> 32 paths (`data/role_panel.hpp`, `src/data/role_panel.cpp`: they are now
  in the IC runner TUs' include closure and build the DSL panel), digest `fa1e9d0f...0aab` ->
  `ec50691894f3f1b3ac5a40deef2afe5bbcaaa043451403b9bcbe12f010b18afa`. Closure check clean.
- `ic_result_sources`: same 10 paths, digest `5bc47755...1954` ->
  `e7a40331a3f2f1a4268feece00d354961ae7ab8215a379733d5855f40f61579a` (ic_research.hpp, ic_screen.cpp).
- No semantics bump: `dsl_vm_semantics_version` and `ic_result_semantics_version` stay 1. No VM or IC
  bit can change: the moved code is the same arithmetic (the guard is comparisons of log differences,
  no multiply-add a compiler could contract), the IC config carries the same values, the new
  accessors are read-only. Cache keys are unchanged (the signal key never hashes sources; the IC key
  hashes the guard and config by value), so warm caches stay warm.
- If another lane edits a pinned engine file before merge, re-pin at merge (as B-3 notes).

Identity (the move must be value-preserving): the v7.1 u pass, warm, with the argv of the B-3 report
(`atx-equity-strategy-ic --library atx-impl/strategies/fund_industry_ic_v71.json --library-sha256
787c802e...2259 --train .../recent-fast-train-2020-2022-v2-lo1/manifest.json --train-sha256 3e79978a...b809
--train-fields ...-fields-v9 --train-fields-sha256 8fd00e9f...769b --output <new> --max-memory-mib 1536
--min-names 1000 --workers 4 --save-combined --candidate-cache build-equity/mega-candidate-cache-v71`):
`candidate_cache.hits` 48 and `ic_results.hits` 48 (the IC-result scope key includes the guard content
and the config, so a hit proves both unchanged), and `recipe.json`, `orientations.json`,
`train_daily_ic.csv`, `train_planned_targets.csv`, `train_combined.*` SHA-equal to `mega-v71-train-u-1`.
A cold pass additionally exercises `overlay_panel` on the field candidates.

## Deviations

- The research window header was not in this tree (W0-1 is on lane w0e, not merged). It was imported
  byte-identical from `feat/platform-v8-w0e-20260929` per the lane rules. Lane D carries a different,
  shorter variant of the same file (the brief's snippet); the two will conflict at merge. W0-1's is
  the authoritative one; code in this lane uses only names both variants define
  (`kTrainBeginNs`, `kTrainEndExclusiveNs`, `kSealBeginNs`, `kResearchWindowId`, `is_sealed`).
- `role_panel.cpp` is added to the engine's Debug `/O2` list: the guard used to run in an `/O2`
  runner TU and costs four logs per cell on a full role; without it a Debug runner pass slows down.

## Cross-lane edits

- `atx-engine/CMakeLists.txt` (no lane owner): one source line, one Debug `/O2` block.
- `atx-impl/src/strategy_ic_library.cpp`, `strategy_ic_runner.cpp`, `strategy_ic_signal_cache.cpp`,
  `strategy_ic_result_cache.cpp` (lane B files, named in the dispatch as the lift source).
- `atx-engine/include/atx/engine/data/research_window.hpp` (W0-1's file, imported unchanged).

## STOPPED HERE

Done: G1 engine side (panel adapter + guard, runner wired), G6 helpers (ladder, check, strided
masks), the label accessor the marginal term needs, the lifted IC recipe, the pins. Not started:
everything below. The design is worked out against the code as read; it is recorded so the next
lane does not re-derive it.

### Remaining engine components (additive, default off)

1. `factory/signal_fitness.hpp` (new): the fitness functor.
   ```cpp
   struct SignalLevel { usize rung{}; u32 inst_stride{1}; bool full{true}; };
   struct SignalScore { f64 raw{}; std::array<f64,kMaxObjectives> objectives{}; u8 n_objectives{}; bool rejected{}; };
   struct SignalFitnessBinding { usize workers{}, dates{}, instruments{}; std::span<const Rung> low_rungs{}; };
   class SignalFitness { public: virtual ~SignalFitness()=default;
     virtual core::Status bind(const SignalFitnessBinding&)=0;   // serial, before any score
     virtual core::Result<SignalScore> score(const Genome&, std::span<const f64> signal,
                                             const SignalLevel&, usize worker)=0; };  // worker-local state only
   ```
2. `search_driver.{hpp,cpp}` (additive `SearchConfig` members at the end, defaults = today):
   `std::span<const u8> cross_section_mask{}` (copied into every full engine at `search_driver.cpp:321`
   and, strided with `strided_cells`, into every rung engine in `fidelity_reject`);
   `SignalFitness* signal_fitness{nullptr}`; `OpCatalogCfg op_catalog{}` (catalog rebuilt at the top of
   `run()`; the default rebuild equals the constructor's). `SearchResult` gains
   `fidelity_rejected_hashes`, `unscored_hashes`, `signal_path_invalid`, `signal_path_error`.
   With `signal_fitness` set: refuse residual/execution rules, a non-empty pool, a weak panel, an active
   IC screen or injected cache, output dedup, deflation, capacity/turnover objectives, resume/sink,
   CPCV DateV2, and any low rung that is not `instrument_only`. In `evaluate_generation` the
   representative's score comes from `signal_fitness->score(...)` on the same SignalSet (no second
   evaluate); `score_slot` is pre-filled with `residual_unavailable_score()` so a compile/VM failure is
   an unscored trial, a `rejected` score becomes `ic_rejected_score()` (lands in `ic_rejected_hashes`),
   an `Err` aborts the run with `signal_path_error`. In `fidelity_reject` the rung evaluator compiles,
   evaluates on the rung engine and returns `score(...).raw` (NaN on reject/error).
3. `op_catalog.{hpp,cpp}` (`op_catalog.cpp:83`): `struct OpCatalogCfg { bool literature_ops{false};
   std::vector<std::string> deny{}; }`; `OpCatalog(const alpha::Library&, const OpCatalogCfg& = {})`;
   `build(cfg)` skips denied names and, with `literature_ops`, also files `literature_ops()` (add_op
   already drops record and hparam rows, so this adds ts_resid_on, ts_beta_on, cs_resid_on,
   ts_count_increases, group_cross). Miner deny list: trade_when, hump, kalman_level, ou_filter, kalman,
   split2.
4. `factory/research_ic_fitness.{hpp,cpp}` (new): `ResearchIcScorer` (a `ResearchIcCache` copy,
   <= 11 borrowed regressor rows, the borrowed member mask, per-worker workspace: `ResearchIcScratch`,
   `MarginalRankIcScratch`, rank row, daily series) with `score(signal, marginal, worker)` ->
   `{reason, practical_null, ic_defined, ic_mean, ic_se, ic_t, sign, marginal_mean, marginal_t,
   marginal_dates, spanned_dates, series}`: f1 from `evaluate_research_ic` (horizon index 1, h = 21,
   `pool = nullptr`, cache prepared with workers 1), f2 per date = `marginal_rank_ic_day(centred_tied_ranks
   (signal row over member row), regressor rows, labels(1) row)` then `summarize_rank_ic(daily, 21)`.
   `ResearchIcFitness : SignalFitness` = full-level scorer + one rung scorer per low rung (rung caches
   prepared on `strided_panel(role.panel, 1, s)` with `strided_cells` member and guard, min_names scaled
   by 1/s): full level -> screened iff IC undefined, practical null, marginal undefined or sign 0;
   f1 = |ic_t|, f2 = sign x marginal_t, raw = min(f1, f2), objectives {f1, f2}; rung -> raw = |ic_t|
   (the marginal needs full-width regressors). Full-level records are kept per worker and merged sorted
   by canon hash (`take_trials()`), so the log is worker-invariant.
5. Engine test `atx-engine/tests/factory/factory_signal_fitness_test.cpp`: default-off identity
   (same digest with the new members at defaults), mask applied to full and rung engines, the functor
   replaces pool fitness, a date-strided rung is refused, catalogue deny/literature, `labels()` equals
   the values `evaluate_research_ic` correlates, `research_window_ic_config` field values.

### Remaining atx-impl components

6. `strategy_research_role.{hpp,cpp}` (the reusable role + fields loader both verbs can call):
   `ResearchRole::load({manifest, manifest_sha256, fields_directory, fields_sha256, fields, max_bytes})`
   -> pinned role JSON (`ic_detail::pinned_json`), `ic_detail::bind_fields` with a Library whose
   declared/extra fields are the requested ones (schema, role binding, point-in-time flags, extents),
   `read_strategy_role`, `ic_detail::load_pinned_f64` per field (SHA while loading, inf refused),
   `overlay_panel`, `research_return_guard`. Non-movable (the panel borrows its columns).
7. `strategy_mine.{hpp,cpp}` + `tools/equity_strategy_mine.cpp` (`atx-equity-strategy-mine`):
   windows are `--discover-begin/--discover-end/--confirm-begin/--confirm-end` dates, validated inside
   TRAIN from `research_window.hpp`, chronological and non-overlapping; a role with any session
   `is_sealed` is refused. Pool: `atx.mine-pool/v1` (regressors used as given, members ranked; every
   payload SHA-pinned and bound to the role manifest SHA and geometry). Stage 1 = one driver run over
   the enumerated templates `rank(T(f))`, T in {identity, ts_mean, delta} x windows from the house set
   {5, 21, 63, 126, 252}, population = template count, 1 generation, no grammar fill; stage 2 = a driver
   run seeded with the stage-1 front (NSGA-II, parsimony on, novelty off, literature ops on, deny list).
   Both: `cross_section_mask = decision_member`, racing `instrument_rungs`, the ResearchIcFitness.
   Registry: one V3 `TrialRegistry` per campaign (`--registry`, reopened with `--registry-head` anchor if
   it exists), pnl calendar = discover h21 label rows; every distinct canon hash recorded once in first-
   seen order: full trials with the oriented daily rank IC (NaN -> 0) and its per-period Sharpe,
   screen-rejected / racing-rejected (fidelity 1) / failed via `record_screened`; family tag
   `trial_tag("mined")` (K5 origin), theme tag `trial_tag(campaign_id)`; chain head written with
   `write_chain_head` and copied into `campaign.json` and a ready-to-append `ledger_line.json`
   (`atx.trial-ledger/v1`, kind `mining-campaign`, count = new records). Promotion (`mined-v1`, numbers
   verbatim from the brief): marginal IC HAC t >= Bonferroni value for the campaign's effective trial
   count, `-norm_ppf(.05/(2N))` = 3.5 at 100, 4.1 at 1,000, 4.6 at 10,000, with N = registry n_raw;
   sign frozen from discover; greedy in f2 order with `abs(rho)` <= .70 (mean daily correlation of
   centred ranks on discover, `PairwiseRowCorrelation`) to every pool row and every earlier promotion;
   one confirm read at HAC t 2.0 with Benjamini-Yekutieli p <= .10 (`eval::p_adjust_by`, one-sided
   p = 1 - Phi(t)) over the K promoted; all admitted members share theme `mined` (`mined_members.json`).
8. Tests `atx-impl/tests/strategy_mine_test.cpp` on a synthetic role built in the test: 16 names,
   daily sessions 2018-12-14..2023-12-31 (score_begin 383 = 2020-01-01; discover [2020-01-01,
   2023-03-01), confirm [2023-03-01, 2024-01-01)), returns r(t) = 0.008 (p1+p2+p3+m1)(t-2) + noise with
   every driver i.i.d. over time (so no close-derived expression predicts), fields p1..p3 (planted),
   copy = m1 + 0.02 noise, n1 (noise); pool regressor = centred rank of m1, member m1. Acceptance as
   tests: every p_i read by a promoted expression and every promoted expression reads a p_i (so no noise
   and no copy-only promotion) in 5 seeds; `rank(copy)` evaluated with |f1| above the hurdle and f2
   below it; same seed twice and at 1 vs 4 workers -> equal chain head and promoted list; registry
   n_raw == evaluated + racing-rejected + screen-rejected (+ failed, expected 0); a role with a
   2024-01-02 session is refused. CMake: sources in `atx-impl-core`, exe target, focused
   `atx-impl-strategy-mine-tests`.

### What a real campaign needs from the owner (OD-7)
- Budget: the 180 s bounded-run cap holds roughly 180 full evaluations; a campaign needs a longer cap
  or resume (the driver's resume/sink is refused on the functor path in this design; the registry
  already accumulates across runs by content address).
- Windows: option A (discover 2012-03..2019-12 on the O7 roles, confirm TRAIN) or B (inside TRAIN);
  the verb only accepts windows inside TRAIN as briefed.
- Operator set and field theme batches (<= 5 resident extras per run, G9), and the pool rows
  (composite + theme composites as regressors; members for the rho check) for the book.

## Open risks

- Not compiled. First things to check if the build fails: the template `strided_cells` in
  fidelity.hpp (included by every factory TU) and the ternary types in the new `ResearchIcCache`
  accessors.
- Fixture statistics (for whoever writes item 8): Bonferroni controls the family-wise rate at 5% per
  campaign, and a Bartlett-21 HAC t on a few hundred dates has fatter tails than normal, so "no noise
  promoted in 5 seeds" can fail by chance. Keep noise and signal i.i.d. over time, the discover window
  long (> 1,000 dates) and the lookback cap short; expect a few percent failure probability per seed.
- The research window header conflicts with lane D's variant at merge (see Deviations).
