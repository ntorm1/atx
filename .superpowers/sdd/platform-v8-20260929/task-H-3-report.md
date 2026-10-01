# Task H-3 report: mining verb, built (engine glue parts 1-2, verb part 3), not compiled

Lane H3, worktree `C:/atx-wt/pool-4`, branch `feat/platform-v8-h3-20260929`.
Nothing was built or run in C++ (lane rules). Python was run only on synthetic data (a numpy replica
of the test fixture, see "Fixture statistics"). No real data, no pushes, no subagents.

| commit | what |
|---|---|
| `11ff84c3` | part 1 (previous session): role adapters lifted from the IC runner, IC recipe function, racing helpers |
| `95859cc9` | merge of root `feat/platform-v8-20260929` (integration 3) into this lane |
| `2beb83c2` | part 2 (engine, additive, default off): signal-fitness search path, OpCatalogCfg, research IC fitness, engine test |
| `12fc27a9` | part 3 (atx-impl): research-role loader, `atx-equity-strategy-mine`, rule `mined-v1`, fixture tests, CMake |

## Merge (step 0)

`git merge --no-ff feat/platform-v8-20260929 -m "Merge root (integration 3) into lane H3"` = `95859cc9`.
- `research_window.hpp`: root's version, byte for byte.
- One conflict, `atx-impl/src/strategy_ic_signal_cache.cpp` (the `dsl_vm_sources` pin): root's 31 paths
  and root's digest kept, plus this lane's two paths (array size 33). `ic_result_sources` merged cleanly.

## What was built

### Part 1 (commit `11ff84c3`, previous session; unchanged here)

- `data/role_panel.hpp`: `overlay_panel`, `research_return_guard`, lifted from the IC runner.
- `factory/ic_research.hpp`: `ResearchIcCache::labels(k)`, `label_rows(k)`, `research_window_ic_config`.
- `factory/fidelity.hpp`: `instrument_rungs`, `instrument_only`, `strided_cells<T>`.

### Part 2, engine (commit `2beb83c2`): additive, default off

| file | change |
|---|---|
| `factory/signal_fitness.hpp` (new) | the functor seam |
| `factory/search_driver.{hpp,cpp}` | `SearchConfig` gains `cross_section_mask`, `signal_fitness`, `op_catalog` (last members); `SearchResult` gains `fidelity_rejected_hashes`, `unscored_hashes`, `signal_path_invalid`, `signal_path_error` |
| `factory/search_state.hpp` | `ScoreOrigin::Unscored = 5`, `unscored_score()`; `is_rejected_score` includes it |
| `factory/op_catalog.{hpp,cpp}` | `OpCatalogCfg{literature_ops, deny}`; `OpCatalog(lib, cfg = {})` |
| `factory/research_ic_fitness.{hpp,cpp}` (new) | `ResearchIcScorer`, `ResearchIcFitness` |
| `atx-engine/CMakeLists.txt` | `src/factory/research_ic_fitness.cpp` |
| `tests/factory/factory_signal_fitness_test.cpp` (new) | engine tests (the factory group globs it) |

The interfaces, as coded:

```cpp
namespace atx::engine::factory {
struct SignalLevel { usize rung{}; u32 inst_stride{1}; bool full{true}; };
struct SignalScore { f64 raw{}; std::array<f64, kMaxObjectives> objectives{}; u8 n_objectives{}; bool rejected{}; };
struct SignalFitnessBinding { usize workers{}, dates{}, instruments{}; std::span<const Rung> low_rungs{}; };
class SignalFitness {  // bind() serial, once per run; score() concurrent, worker-local state only
  virtual core::Status bind(const SignalFitnessBinding&) = 0;
  virtual core::Result<SignalScore> score(const Genome&, std::span<const f64> signal,
                                          const SignalLevel&, usize worker) = 0; };
struct OpCatalogCfg { bool literature_ops{false}; std::vector<std::string> deny{}; };
// SearchConfig (appended): std::span<const u8> cross_section_mask{}; SignalFitness* signal_fitness{nullptr};
//                          OpCatalogCfg op_catalog{};
struct ResearchIcWindow { usize begin{}, end{}, min_names{}, min_dates{}; u64 max_cache_bytes{}; };
struct ResearchIcRead { IcScreenReason reason; bool ic_defined; f64 ic_mean, ic_se, ic_t; int sign;
                        f64 marginal_mean, marginal_t; usize marginal_dates, spanned_dates; };
f64 research_ic_f1(const ResearchIcRead&);  // |IC t|
f64 research_ic_f2(const ResearchIcRead&);  // sign x marginal IC HAC t
enum class ResearchIcScreen { None, IcUndefined, PracticalNull, MarginalUndefined, NoSign };
class ResearchIcScorer {  // the IC runner's recipe + the K6 marginal kernel on one window
  static Result<ResearchIcScorer> prepare(const Panel&, const ResearchIcWindow&, span<const u8> member,
      span<const u32> guard, std::vector<span<const f64>> regressors /*borrowed*/, bool marginal);
  Status bind(usize workers);
  Result<ResearchIcRead> read(span<const f64> signal, bool marginal, usize worker);
  span<const f64> daily_rank_ic(usize worker) const; span<const f64> daily_marginal_ic(usize worker) const; };
struct ResearchIcTrial { u64 canon_hash; ResearchIcRead read; ResearchIcScreen screen; std::vector<f64> daily_rank_ic; };
struct ResearchIcFitnessInputs { const Panel* panel; ResearchIcWindow window; span<const u8> member;
                                 span<const u32> guard; std::vector<span<const f64>> regressors; };
class ResearchIcFitness final : public SignalFitness {
  static Result<ResearchIcFitness> prepare(ResearchIcFitnessInputs);
  std::vector<ResearchIcTrial> take_trials();  // full-pass reads since the last call, by canon hash
};
}
```

Behaviour, in brief:
- **Signal path.** With `signal_fitness` set, the driver scores each fresh representative with
  `score(g, signal, SignalLevel{}, wid)` on the same SignalSet. There is no second evaluation and no
  `pool_aware_fitness`; parsimony is still added when enabled.
  - A `rejected` score becomes `ic_rejected_score()` and goes into `ic_rejected_hashes`.
  - A missing signal or a non-finite raw keeps the `Unscored` sentinel and goes into `unscored_hashes`.
  - An `Err` sets `signal_path_invalid` and stops the run.
- **Racing rungs** call `score(..., SignalLevel{r, s, false}, wid)`; NaN means rejected. Every racing
  rejection, on any path, goes into `fidelity_rejected_hashes`.
- **Refusals** (`signal_path_invalid`, before any evaluation):
  - a mask that is not panel cells in {0, 1}, or a mask with a weak panel (these apply without a functor too);
  - with a functor: a non-legacy objective or execution rule or context, a non-empty pool, a weak panel,
    an IC screen or injected cache, output dedup, deflation, capacity or turnover objectives, a sink or
    resume, CPCV DateV2, and any low rung that is not `instrument_only`.
- **Mask:** copied into every full engine, and strided alike (`strided_cells`) into every rung engine.
- **Catalogue:** rebuilt at the top of `run()` from `cfg.op_catalog`. The default rebuild equals the
  constructor's.
- **ResearchIcFitness:**
  - Full level: screened when the IC is undefined, the IC is a practical null, the marginal t is
    undefined, or the sign is 0. Otherwise `f1 = |IC t|`, `f2 = sign x marginal t`, `raw = min`, and
    the objectives are {f1, f2}. Every full read is logged per worker.
  - Rung level: the IC only, on `strided_panel(panel, 1, s)` with strided member and guard, and
    `min_names` scaled by 1/s. `raw = |IC t|`.
  - Rung scorers are re-prepared only when the strides change.

### Part 3, atx-impl (commit `12fc27a9`)

| file | role |
|---|---|
| `src/strategy_research_role.{hpp,cpp}` | reusable loader: pinned role + pinned fields -> one DSL panel |
| `src/strategy_mine_pool.{hpp,cpp}` | `atx.mine-pool/v1` pool manifest + payloads, bound to the role |
| `src/strategy_mine_rule.{hpp,cpp}` | `mined-v1` arithmetic (pure) |
| `src/strategy_mine.{hpp,cpp}` | config, windows, the two stages, outputs, CLI (`dispatch_mine`) |
| `src/strategy_mine_detail.hpp`, `src/strategy_mine_trials.cpp`, `src/strategy_mine_promote.cpp` | internal: trial log + registry; promotion |
| `tools/equity_strategy_mine.cpp` | `atx-equity-strategy-mine` main |
| `tests/strategy_mine_test.cpp` | rule tests + fixture acceptance |
| `atx-impl/CMakeLists.txt`, `atx-impl/tests/CMakeLists.txt` | sources in `atx-impl-core`; exe (EXCLUDE_FROM_ALL); focused `atx-impl-strategy-mine-tests` |

```cpp
namespace atx::impl::strategy {
struct ResearchRoleSpec { std::string manifest, manifest_sha256, fields_directory, fields_sha256;
                          std::vector<std::string> fields; u64 max_bytes{1 GiB}; };
Result<u64> research_role_bytes(usize dates, usize names, usize extras);
class ResearchRole {  // non-copyable, non-movable (the panel borrows its columns)
  static Result<Geometry> geometry(const ResearchRoleSpec&);           // metadata only
  static Result<std::unique_ptr<ResearchRole>> load(const ResearchRoleSpec&);
  const StrategyRoleData& data() const; const Panel& panel() const;   // base + extras overlay
  span<const u8> member() const; span<const u32> guard() const;
  const std::vector<ResearchFieldReceipt>& extras() const; const std::string& fields_sha256() const; };
Result<MinePoolManifest> read_mine_pool_manifest(const std::string& path, const std::string& pin);
Result<MinePool> load_mine_pool(const MinePoolManifest&, const ResearchRole&);
f64 mined_hurdle(u64 trials);                                          // -norm_ppf(.05 / (2N))
std::vector<usize> mined_shortlist(span<const MinedRead>, f64 hurdle, usize cap);
std::vector<MinedRho> mined_rho_select(const PairwiseRowCorrelation&, usize pool_rows, usize candidates);
std::vector<MinedConfirm> mined_confirm(span<const f64> oriented_t);
std::vector<std::string> mine_templates(span<const std::string> fields);
Result<u64> mine_working_bytes(const MineFootprint&);
Status run_mine(const MineConfig&, std::ostream& progress);
int dispatch_mine(int argc, char** argv, std::ostream& out, std::ostream& err);
}
```

**Loader (`ResearchRole::load`).** The steps, in order:
1. The pinned manifest (`ic_detail::pinned_json`). A `score_end_ns` after `kSealBeginNs` is refused
   here, from metadata alone, before any payload.
2. The admission check against `max_bytes`.
3. `ic_detail::bind_fields`, with a Library whose declared and extra fields are the requested non-base
   names. This checks the schema, the role binding, the point-in-time flags and the extents.
4. `read_strategy_role`. The loaded manifest SHA is re-checked, and every session is checked with `is_sealed`.
5. `ic_detail::load_pinned_f64` per field (SHA-256 while reading; inf refused).
6. `overlay_panel` and `research_return_guard`.

No logic is copied from the IC runner: every step calls the shared function.

**Verb (`run_mine`):**
- **Before any payload:**
  - config bounds, and the output directory must be new;
  - windows: `--discover-begin/--discover-end/--confirm-begin/--confirm-end` dates must be
    chronological, non-overlapping and inside TRAIN [2020-01-01, 2024-01-01);
  - registry anchor: an existing registry needs `--registry-head`;
  - role axes (the seal refusal applies), pool manifest, and the working-bytes admission.
- **Role and pool:** load the role; bind the windows to decision rows (inside the score window, at least
  25 rows); load the pool.
- **Fitness:** `ResearchIcFitness` on the discover window, against the pool's regressors.
- **Stage 1:** the templates `rank(f)`, `rank(ts_mean(f, w))` and `rank(delta(f, w))`, with
  w in {5, 21, 63, 126, 252}. Population = template count, 1 generation, MultiObjective {f1, f2}, no
  grammar fill, no immigrants, novelty off.
- **Stage 2:** NSGA-II seeded with the first `--stage2-seeds` of the stage-1 admitted front. Seed
  `seed_for(seed, 2, 0)`, parsimony on, `mutate_seed_copies`, literature ops on, and deny
  {trade_when, hump, kalman_level, ou_filter, kalman, split2}.
- **Both stages:** `cross_section_mask = decision_member`, racing on `instrument_rungs(--race-strides)`
  with eta `--race-keep`, `max_lookback` 252.
- **Trials:**
  - Every distinct canonical hash, in first-seen `all_scored` order (stage 1, then stage 2). Status
    priority: evaluated > screen-rejected > racing-rejected > failed.
  - V3 registry: `pnl_len` = discover h21 label rows, `keep_sketches` off, reopened against the anchor
    when the file exists.
  - `config_hash` = FNV-1a64 of `atx.mine-trial/v1|recipe_sha|canon_hash_hex|dsl`. `recipe_sha` is the
    SHA-256 of the scoring recipe JSON: role, fields and pool SHAs, discover window, IC and marginal
    recipe, min_names, min_dates.
  - Meta: `family_tag` = `trial_tag("mined")`, `theme_tag` = `trial_tag(campaign_id)`, InSample.
  - Evaluated trials are recorded with the oriented daily h21 rank IC (NaN as 0) and its per-period
    Sharpe; a degenerate series becomes screen-rejected ("degenerate-series").
  - Every other trial goes through `record_screened`, with fidelity 1 for racing rejections.
  - The chain head is written to `registry_head.txt`, then copied into `campaign.json` and
    `ledger_line.json`.
- **mined-v1:**
  - N = registry `n_raw` after recording; hurdle = `-norm_ppf(.05/(2N))`.
  - Shortlist: the evaluated trials with f2 >= hurdle, by f2 then canonical hash, capped at
    `--max-promotions`.
  - Their signals are re-evaluated with the masked VM.
  - Greedy `|rho| <= .70` against the pool members and the earlier kept candidates: mean daily
    correlation of centred ranks over the discover decision rows (`PairwiseRowCorrelation`).
  - One confirm read (`ResearchIcScorer` on the confirm window, same regressors). t = discover sign x
    confirm marginal HAC t; p = Phi(-t); BY across the rho-passing candidates.
  - Admitted iff t >= 2 and p_BY <= .10. Members get theme `mined`.
- **Outputs:**
  - `campaign.json` (`atx.mine-campaign/v1`: inputs, windows, recipe_sha256, search, trial counts,
    registry {records, head, n_raw, new_records, anchor}, hurdle, promotions);
  - `trials.csv`;
  - `mined_members.json` (`atx.mined-members/v1`);
  - `ledger_line.json`: one compact sorted-key `atx.trial-ledger/v1` line: kind `mining-campaign`,
    count = new records, origin `mined`, window_id, campaign_id, rule, registry, and
    `trial_id` = sha256(`["mining-campaign",campaign_id,head_hex]`)[:16] (research_ledger.py's rule).
    `prev_sha256` is added by the chained append;
  - `registry_head.txt`.

Interpretations taken (flagged for review):
- "confirm read at HAC t 2.0" reads the **marginal** IC HAC t (the promotion statistic). The raw IC t
  is reported beside it as `confirm_ic_t`.
- The rho check runs before the confirm read. BY therefore counts only the candidates that get a
  confirm read.
- An undefined rho pair does not block a candidate.

## How root verifies

**Build (target-scoped):**
- `atx-engine-factory-tests`: compiles the part-2 test through the factory glob; unity build, named
  namespace.
- `atx-impl-strategy-mine-tests`: `strategy_mine_test.cpp` plus the engine test.
- `atx-equity-strategy-mine`.
- `atx-impl-strategy-ic-tests`: the source pins.

A `check` of each new TU first: `atx-engine/src/factory/research_ic_fitness.cpp`, `search_driver.cpp`,
`op_catalog.cpp`, `atx-impl/src/strategy_mine*.cpp`, `strategy_research_role.cpp`.

**gtest:**
- `atx-impl-strategy-mine-tests --gtest_filter=SignalFitness*:OpCatalogCfgTest.*:ResearchIcAccessors.*:ResearchIcFitnessTest.*:StrategyMine*`
- `atx-engine-factory-tests --gtest_filter=NsgaSearch.*:FactoryFidelity*:SignalFitness*:OpCatalogCfgTest.*:ResearchIc*`
  (the golden digests: `NsgaSearch.ScalarRaw_ReproducesGoldenDigest` and the rest must stay green).
- `atx-impl-strategy-ic-tests --gtest_filter=StrategyIcRunner.*Pinned*` after the digest re-set below.

**Default-off identity:**
- `SignalFitnessDefaults.ImplicitDefaultsKeepTheGoldenDigest` and
  `...ExplicitDefaultsKeepTheGoldenDigestAtEveryWorkerCount` run the frozen ScalarRaw fixture (seed 777,
  96x6, `kGoldenDigest = 0x889874a3b9b29c55`) with the new members left implicit, then spelled out, at
  1 and 4 workers.
- Every existing factory digest test (NsgaSearch, FactoryFidelity, search progress, research driver) is
  the wider check.
- The default path differs only by the refactor of the two parsimony sites into `set_parsimony`
  (value-identical) and the catalogue rebuild at the top of `run()` (the same build as the constructor).

**Fixture acceptance (`StrategyMineCampaign.*`):**
- `PromotesThePlantedSignalsOnlyInFiveSeeds`, over seeds 1..5:
  - p1, p2 and p3 are each read by an admitted member, and no admitted member lacks a planted field;
  - `rank(copy)` is evaluated with f1 >= hurdle and f2 < hurdle;
  - `n_raw` == evaluated + screen-rejected + racing-rejected, and failed == 0;
  - the ledger line matches the registry head.
- `SameSeedSameChainHeadAtOneAndFourWorkers`:
  - seed 7 at 1, 1 and 4 workers gives the same head, the same trials.csv bytes and the same members;
  - a reuse of the registry without `--registry-head` is refused and writes nothing;
  - with the head, the rerun adds 0 records and keeps the head.
- `RefusesSealedRolesAndWindowsPastTrain`:
  - a role with sessions to 2024-01-02 is refused ("research seal 2024-01-01"), with no output and no
    registry;
  - a confirm end of 2024-01-02 is refused.

**Expected runtime (estimate, not measured):** about 80 trials per campaign, and 9 campaigns across the
three campaign tests.
- Each campaign: about 55 rung reads (8 names) plus 30 to 40 full reads (16 names, 1,074 label rows,
  one marginal kernel per row) plus two small stages.
- Debug: 2 to 6 s per campaign, 20 to 60 s for the three campaign tests. Release: a few seconds.
- The same tests also run in `atx-impl-tests` through its glob.

## Cross-lane edits and pins

- `atx-engine/include/atx/engine/factory/search_driver.hpp`, `src/factory/search_driver.cpp`,
  `search_state.hpp`, `op_catalog.{hpp,cpp}` (no lane owner): additive, as briefed.
- `atx-engine/CMakeLists.txt`: one source line (part 2).
- `atx-impl/CMakeLists.txt` and `atx-impl/tests/CMakeLists.txt` (lane E): six sources in
  `atx-impl-core`, the exe, and the focused test target (not added to the `atx_equity_strategy` label
  loop).
- `atx-impl/src/strategy_ic_signal_cache.cpp` (lane B): merge resolution only.
- `strategy_research_role.cpp` and `strategy_mine_pool.cpp` include `strategy_ic_detail.hpp` (lane B's
  internal header) to call its loaders, and change nothing in it. Its compile-flag-dependent inline
  constants depend on ISA flags only, which are global.

**Pins.** Paths this lane adds to `dsl_vm_sources`:
- `atx-engine/include/atx/engine/data/role_panel.hpp`
- `atx-engine/src/data/role_panel.cpp`

`ic_result_sources` gets no new path. As instructed at the merge, the file carries root's list plus
these two paths (size 33) and root's digest `afbae65d...a181`, so the tripwire fails until root re-pins.

The Python mirror of `expect_sources_pinned` on this tree at `12fc27a9` gives:
- `dsl_vm_sources` (33): `f24cfbbec5404cec34f785b89724f1d0525469823358b2374ac053e009cbf55c`
- `ic_result_sources` (10): `e7a40331a3f2f1a4268feece00d354961ae7ab8215a379733d5855f40f61579a` (unchanged, matches)

Parts 2 and 3 touch no pinned file.

## Fixture statistics (numpy replica, same generator; Bartlett-21 t, not the IC recipe's conservative se)

| expression | discover f1 | discover f2 | rung t (8 names) | confirm marginal t |
|---|---|---|---|---|
| rank(p1) / rank(p2) / rank(p3) | 13.9 / 14.8 / 11.6 | 13.4 / 14.5 / 11.2 | 8.2 / 10.2 / 5.8 | 8.5 / 6.2 / 4.9 |
| rank(copy) | 12.2 | 1.1 | 6.5 (6th of 55) | -0.4 |
| rank(n1) | 0.0 | -0.5 | 0.4 | -2.1 |
| rank(delta(p1, 5)) | 11.1 | 10.7 | 6.4 | 7.7 |

The hurdle is about 3.4 at 80 trials. The fixture keeps half at the rung (`race_keep` 0.5, the 28th
|t| is under 3), so `rank(copy)` reaches the full pass comfortably.

## Open risks

- **Not compiled.** First things to check if the build fails:
  - the `ATX_TRY` uses with ternary Result expressions (`open_registry`);
  - `using namespace mine_detail` at namespace scope in `strategy_mine.cpp`;
  - `std::chrono::year_month_day` / `sys_days` in `strategy_mine.cpp`;
  - nlohmann initializer lists in the campaign JSON;
  - `-Wmissing-field-initializers` on the aggregate inits: all are written in full or default-then-assign.
- **Fixture statistics.** The t values above use a Bartlett-21 se; the engine's IC screen uses a more
  conservative se.
  - The planted margins (f2 11 to 14 against a hurdle of 3.4; confirm 4.9 to 8.5 against 2) leave room.
  - The false-positive gate for noise and the copy is discover f2 >= 3.4 **and** confirm t >= 2 on an
    independent window, about 1e-5 per expression.
  - The data are fixed, so the outcome is deterministic, but it was not observed.
- **Stage-2 search space.** Literature ops plus the deny list are untested on real fields. An op that
  yields an all-NaN signal is screened (IcUndefined), not failed. A VM error is `failed`, and the test
  asserts failed == 0.
- **Memory admission** (`mine_working_bytes`) is an estimate. It assumes 8 VM slots per cell per
  engine; a campaign on a full role should be measured before OD-7 sizes it.
- **Ledger kind.** `backtest_integrity.LEDGER_KINDS` does not list `mining-campaign`. The ledger owner
  must add it, with its N rule, before a campaign line is appended. The line counts registry records,
  not construction trials.
- **The loader is used by the miner only.** The IC runner still binds roles through its own `admit`
  path; moving it onto `ResearchRole` is a follow-up.
