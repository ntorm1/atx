// `atx-equity-strategy-mine` (platform v8 H-3): configuration, windows, the two search stages, the
// campaign outputs and the CLI. Contracts in strategy_mine.hpp; the trial log and the promotion
// live in strategy_mine_trials.cpp and strategy_mine_promote.cpp.
#include "strategy_mine.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <new>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "atx/core/sha256.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/unparse.hpp"
#include "atx/engine/combine/marginal_rank_ic.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/data/research_window.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/factory/fidelity.hpp"
#include "atx/engine/loop/weight_policy.hpp"
#include "strategy_ic_runner.hpp" // the library identity of a trial recipe (review MINE-3)
#include "strategy_mine_detail.hpp"
#include "strategy_mine_ledger.hpp"

namespace atx::impl::strategy {
using namespace mine_detail;
namespace {
namespace al = atx::engine::alpha;
namespace cb = atx::engine::combine;
namespace dt = atx::engine::data;
namespace fs = std::filesystem;
using steady = std::chrono::steady_clock;

constexpr i64 kDayNs = 86'400'000'000'000LL;
constexpr std::array<int, 5> kTemplateWindows{5, 21, 63, 126, 252};
constexpr u16 kMaxLookback = 252;
// Execution delay 1 + horizon 21: a window of R decision rows has R - 22 mature h 21 labels.
constexpr usize kLabelLag = 22;
// Operators the miner never samples (state machines, regime filters and splitters whose output
// is not a cross-sectional score).
constexpr std::array<std::string_view, 6> kMinerDeny{"trade_when", "hump",   "kalman_level",
                                                     "ou_filter",  "kalman", "split2"};
constexpr std::string_view kCampaignSchema = "atx.mine-campaign/v1";
constexpr std::string_view kMembersSchema = "atx.mined-members/v1";
// The derived terms of mine_memory (review MINE-10, lane MINE-MEM; strategy_mine.hpp).
constexpr u64 kMetadataBytes = 64ULL << 20;
constexpr u64 kIcLabelCellBytes = 48; // 3 horizons x (label + rank) x 8
constexpr u64 kIcCacheCellBytes = kIcLabelCellBytes + 1U; // + the scorer's member row byte
constexpr u64 kIcCacheDateBytes = 32; // per row: eligible names, 3 horizons' label names (usize)
constexpr u64 kBindCellBytes = 6;     // a strided panel's universe 1, member 1, guard 4
constexpr u64 kScratchNameBytes = 128; // IC and marginal row buffers per name
constexpr u64 kScratchDateBytes = 80;  // calendar series and label-row counts per date
constexpr u64 kTrialAllowanceBytes = 16ULL << 10;
constexpr u64 kRegistryGramBytes = 512ULL << 10; // the registry's 256 x 256 f64 Gram
constexpr u64 kRegistryRecordBytes = 128;        // TrialInfo (72 B) and its dedup-set entry
constexpr u64 kPairBytes = 16;     // PairwiseRowCorrelation: a pair's sum (f64) and count (usize)
constexpr u64 kSortPairBytes = 16; // centred_tied_ranks' (value, name) sort buffer per name

f64 seconds_since(steady::time_point from) {
  return std::chrono::duration<f64>(steady::now() - from).count();
}
bool safe_id(std::string_view s) {
  return !s.empty() && s.size() <= 64U && std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
  });
}
std::string padded(unsigned value, usize width) {
  std::string s = std::to_string(value);
  return std::string(width > s.size() ? width - s.size() : 0U, '0') + s;
}
std::string iso_date(i64 ns) {
  const std::chrono::year_month_day ymd{std::chrono::sys_days{std::chrono::days{ns / kDayNs}}};
  return padded(static_cast<unsigned>(static_cast<int>(ymd.year())), 4U) + "-" +
         padded(static_cast<unsigned>(ymd.month()), 2U) + "-" +
         padded(static_cast<unsigned>(ymd.day()), 2U);
}
co::Status write_text(const fs::path &path, const std::string &text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
  out.close();
  if (!out) return co::Err(fail(co::ErrorCode::IoError, "write " + path.string()));
  return co::Ok();
}

// ---- configuration and windows -----------------------------------------------------------------
co::Status check_config(const MineConfig &cfg) {
  // Ruling E-32a (review MINE-7): mined-v1's marginal term and rho check read the book.
  if (cfg.pool_path.empty() || cfg.pool_sha256.empty())
    return co::Err(fail(co::ErrorCode::InvalidArgument,
                        "mined-v1 needs --pool with --pool-sha256 (Ruling E-32a: without the "
                        "book the marginal t is the raw IC t and the rho check meets no member)"));
  std::vector<std::string> fields = cfg.role.fields;
  std::sort(fields.begin(), fields.end());
  const bool distinct = std::adjacent_find(fields.begin(), fields.end()) == fields.end();
  const bool inputs = !cfg.role.manifest.empty() && !cfg.registry_path.empty() &&
                      !cfg.output_directory.empty() && safe_id(cfg.campaign_id) &&
                      !fields.empty() && fields.size() <= 64U && distinct;
  const bool search = cfg.workers >= 1U && cfg.workers <= 64U && cfg.stage2_population >= 2U &&
                      cfg.stage2_population <= 4096U && cfg.stage2_generations <= 256U &&
                      cfg.stage2_seeds <= cfg.stage2_population &&
                      cfg.race_strides.size() <= 2U && cfg.race_keep > 0.0 &&
                      cfg.race_keep <= 1.0;
  // --min-dates >= 8: the IC recipe's own floor (ic_screen validate), refused here before payload.
  const bool rule = cfg.min_names >= 3U && cfg.min_dates >= 8U && cfg.max_promotions >= 1U &&
                    cfg.max_promotions <= 256U && cfg.max_working_bytes >= (64ULL << 20) &&
                    cfg.max_working_bytes <= (64ULL << 30) && cfg.budget >= 1U &&
                    cfg.budget <= kMineMaxBudget;
  if (!inputs || !search || !rule)
    return co::Err(fail(co::ErrorCode::InvalidArgument,
                        "bounded config (needs --role, --registry, --campaign-id [a-z0-9_-]{1,64}, "
                        "--output, 1..64 distinct --fields, --pool with --pool-sha256, --budget "
                        "1..10000000; --workers 1..64; --stage2-seeds <= --stage2-population "
                        "(2..4096); at most 2 --race-strides; --race-keep in (0, 1]; --min-names "
                        ">= 3; --min-dates >= 8; --max-promotions 1..256; --max-memory-mib "
                        "64..65536)"));
  // Ruling PM4-13: the overlap table covers kMinedMaxBudget only; refused before any payload.
  if (cfg.budget > kMinedMaxBudget)
    return co::Err(fail(co::ErrorCode::InvalidArgument,
                        "--budget " + std::to_string(cfg.budget) + " is above kMinedMaxBudget " +
                            std::to_string(kMinedMaxBudget) +
                            " (Ruling PM4-13): the mined-v1 overlap table kMinedOverlapBands is "
                            "validated to that budget only; a larger campaign waits until the "
                            "table is extended at its own Bonferroni level"));
  // Pre-registration rule 10 (Ruling E-32a): the budget is fixed in advance and binds the search.
  const u64 capacity = mine_trial_capacity(cfg);
  if (cfg.budget < capacity)
    return co::Err(fail(co::ErrorCode::InvalidArgument,
                        "--budget " + std::to_string(cfg.budget) +
                            " is below the configuration's trial capacity " +
                            std::to_string(capacity) +
                            " (templates plus stage-2 population x generations): the budget is "
                            "fixed in advance and the search must not be able to exceed it"));
  return co::Ok();
}

// Ruling E-32a (review MINE-7): the pool's manifest names the book -- at least one regressor for
// the marginal term and one member for the rho check -- checked before any payload.
co::Status check_pool(const MinePoolManifest &pool) {
  if (pool.regressors.empty() || pool.members.empty())
    return co::Err(fail(co::ErrorCode::InvalidArgument,
                        "mined-v1 needs --pool with at least one regressor and one member "
                        "(Ruling E-32a); " + pool.path + " has " +
                            std::to_string(pool.regressors.size()) + " and " +
                            std::to_string(pool.members.size())));
  return co::Ok();
}

co::Result<i64> date_ns(const std::string &text, const char *option) {
  const bool shape = text.size() == 10U && text[4] == '-' && text[7] == '-' &&
                     std::all_of(text.begin(), text.end(), [](char c) {
                       return c == '-' || (c >= '0' && c <= '9');
                     });
  const auto number = [&text](usize at, usize length) {
    unsigned value = 0U;
    for (usize i = at; i < at + length; ++i)
      value = value * 10U + static_cast<unsigned>(text[i] - '0');
    return value;
  };
  if (shape) {
    const std::chrono::year_month_day ymd{std::chrono::year{static_cast<int>(number(0U, 4U))},
                                          std::chrono::month{number(5U, 2U)},
                                          std::chrono::day{number(8U, 2U)}};
    if (ymd.ok()) {
      const i64 days = std::chrono::sys_days{ymd}.time_since_epoch().count();
      return co::Ok(days * kDayNs);
    }
  }
  return co::Err(fail(co::ErrorCode::InvalidArgument,
                      std::string(option) + " must be a YYYY-MM-DD date: " + text));
}

struct MineWindows {
  MineWindow discover, confirm;
};

co::Result<MineWindows> parse_windows(const MineConfig &cfg) {
  MineWindows w;
  w.discover.begin_date = cfg.discover_begin;
  w.discover.end_date = cfg.discover_end;
  w.confirm.begin_date = cfg.confirm_begin;
  w.confirm.end_date = cfg.confirm_end;
  ATX_TRY(w.discover.begin_ns, date_ns(cfg.discover_begin, "--discover-begin"));
  ATX_TRY(w.discover.end_ns, date_ns(cfg.discover_end, "--discover-end"));
  ATX_TRY(w.confirm.begin_ns, date_ns(cfg.confirm_begin, "--confirm-begin"));
  ATX_TRY(w.confirm.end_ns, date_ns(cfg.confirm_end, "--confirm-end"));
  const bool ordered = dt::kTrainBeginNs <= w.discover.begin_ns &&
                       w.discover.begin_ns < w.discover.end_ns &&
                       w.discover.end_ns <= w.confirm.begin_ns &&
                       w.confirm.begin_ns < w.confirm.end_ns &&
                       w.confirm.end_ns <= dt::kTrainEndExclusiveNs;
  if (!ordered)
    return co::Err(fail(co::ErrorCode::InvalidArgument,
                        "discover and confirm windows must be chronological, non-overlapping and "
                        "inside TRAIN [" + iso_date(dt::kTrainBeginNs) + ", " +
                            iso_date(dt::kTrainEndExclusiveNs) + ") of " +
                            std::string(dt::kResearchWindowId)));
  return co::Ok(std::move(w));
}

// Binds `w` to the role's decision rows; refuses rows outside the score window and fewer than
// `min_label_rows` mature h 21 labels.
co::Status bind_rows(MineWindow &w, const dt::StrategyRoleData &role, const char *name,
                     usize min_label_rows) {
  const auto &sessions = role.session_keys;
  const auto row = [&sessions](i64 ns) {
    return static_cast<usize>(std::lower_bound(sessions.begin(), sessions.end(), ns) -
                              sessions.begin());
  };
  w.begin = row(w.begin_ns);
  w.end = row(w.end_ns);
  if (w.begin < role.score_begin || w.end > role.score_end)
    return co::Err(fail(co::ErrorCode::InvalidArgument,
                        std::string(name) + " window: rows outside the role's score window"));
  const usize label_rows = w.end > w.begin + kLabelLag ? w.end - w.begin - kLabelLag : 0U;
  if (label_rows < min_label_rows)
    return co::Err(fail(co::ErrorCode::InvalidArgument,
                        std::string(name) + " window: " + std::to_string(label_rows) +
                            " mature h 21 label rows, fewer than the " +
                            std::to_string(min_label_rows) + " the rule reads on it"));
  return co::Ok();
}

// ---- the search ---------------------------------------------------------------------------------
co::Result<ex::FidelityCfg> race_config(const MineConfig &cfg) {
  ex::FidelityCfg race;
  if (cfg.race_strides.empty()) return co::Ok(race);
  ATX_TRY(race.rungs, ex::instrument_rungs(cfg.race_strides));
  race.enabled = true;
  race.eta = cfg.race_keep;
  return co::Ok(race);
}

// Both stages: the signal-fitness path, the decision membership as the cross-section mask, no
// grammar fill or immigrants (every candidate descends from a template), novelty off.
ex::SearchConfig stage_config(const MineConfig &cfg, const ex::FidelityCfg &race,
                              ex::ResearchIcFitness &fitness, std::span<const u8> mask,
                              bool explore) {
  ex::SearchConfig sc;
  sc.n_workers = cfg.workers;
  sc.objective_mode = ex::ObjectiveMode::MultiObjective;
  sc.enable_behavioral_novelty = false;
  sc.seed_from_grammar = false;
  sc.n_immigrants = 0;
  sc.stagnation_patience = 0;
  sc.max_lookback = kMaxLookback;
  sc.gen_cfg.max_lookback = kMaxLookback;
  sc.fidelity = race;
  sc.cross_section_mask = mask;
  sc.signal_fitness = &fitness;
  sc.max_program_slots = kMineMaxProgramSlots; // review MINE-10: the admitted slot pools
  sc.enable_parsimony = explore;
  sc.mutate_seed_copies = explore;
  if (explore) {
    sc.op_catalog.literature_ops = true;
    for (const std::string_view name : kMinerDeny) sc.op_catalog.deny.emplace_back(name);
  }
  return sc;
}

co::Result<StageRun> run_stage(const al::Library &lib, const al::Panel &panel,
                               std::vector<std::string> seeds,
                               const std::vector<std::string> &fields, const ex::SearchConfig &sc,
                               ex::ResearchIcFitness &fitness) {
  const auto started = steady::now();
  const engine::WeightPolicy policy{};
  const engine::exec::ExecutionSimulator sim{};
  const cb::AlphaStore pool{};
  ex::SearchDriver driver{lib, panel, policy, sim, std::move(seeds), fields};
  StageRun out;
  out.result = driver.run(sc, pool);
  if (out.result.signal_path_invalid)
    return co::Err(fail(co::ErrorCode::Internal,
                        "search stage refused or failed: " + out.result.signal_path_error));
  out.trials = fitness.take_trials();
  out.seconds = seconds_since(started);
  return co::Ok(std::move(out));
}

// Both search stages and the discover fitness they score on. Lane MINE-MEM: the fitness -- the
// discover IC cache, the rung scorers and their worker scratch -- lives in run_search only, so the
// promotion never holds it. Stage 1's driver is gone (run_stage) before stage 2's is built.
struct MineSearch {
  StageRun stage1, stage2;
  bool explore{};
};

co::Result<MineSearch> run_search(const MineConfig &cfg, const ResearchRole &role,
                                  const std::vector<std::span<const f64>> &regressors,
                                  const MineWindow &discover, const al::Library &lib,
                                  const std::vector<std::string> &templates,
                                  std::ostream &progress) {
  // Fitness on the discover window against the pool's regressors (borrowed from the pool).
  ex::ResearchIcFitnessInputs inputs;
  inputs.panel = &role.panel();
  inputs.window = ex::ResearchIcWindow{discover.begin, discover.end, cfg.min_names,
                                       cfg.min_dates, cfg.max_working_bytes};
  inputs.member = role.member();
  inputs.guard = role.guard();
  inputs.regressors = regressors;
  ATX_TRY(auto fitness, ex::ResearchIcFitness::prepare(std::move(inputs)));
  ATX_TRY(const auto race, race_config(cfg));

  // Stage 1: the templates, one generation. Stage 2: NSGA-II from the stage-1 front.
  const std::vector<std::string> &fields = cfg.role.fields;
  MineSearch out;
  ex::SearchConfig first = stage_config(cfg, race, fitness, role.member(), false);
  first.master_seed = cfg.seed;
  first.population = templates.size();
  first.generations = 1U;
  ATX_TRY(out.stage1, run_stage(lib, role.panel(), templates, fields, first, fitness));
  progress << "mine: stage=1 templates=" << templates.size()
           << " trials=" << out.stage1.result.trial_count
           << " racing_rejected=" << out.stage1.result.fidelity_rejected
           << " seconds=" << out.stage1.seconds << '\n' << std::flush;
  std::vector<std::string> seeds;
  for (const ex::Genome &g : out.stage1.result.admitted_candidates) {
    if (seeds.size() >= cfg.stage2_seeds) break;
    seeds.push_back(al::unparse(g.ast));
  }
  out.explore = !seeds.empty() && cfg.stage2_generations > 0U;
  if (out.explore) {
    ex::SearchConfig second = stage_config(cfg, race, fitness, role.member(), true);
    second.master_seed = ex::detail::seed_for(cfg.seed, 2U, 0U);
    second.population = cfg.stage2_population;
    second.generations = cfg.stage2_generations;
    ATX_TRY(out.stage2, run_stage(lib, role.panel(), std::move(seeds), fields, second, fitness));
    progress << "mine: stage=2 trials=" << out.stage2.result.trial_count
             << " racing_rejected=" << out.stage2.result.fidelity_rejected
             << " seconds=" << out.stage2.seconds << '\n' << std::flush;
  }
  return co::Ok(std::move(out));
}

Json window_json(const MineWindow &w, usize label_rows) {
  return Json{{"begin", w.begin_date}, {"end", w.end_date},
              {"rows", Json::array({w.begin, w.end})}, {"label_rows", label_rows}};
}

Json stage_json(const StageRun &stage) {
  return Json{{"trials", stage.result.trial_count},
              {"racing_rejected", stage.result.fidelity_rejected},
              {"racing_evaluations", stage.result.fidelity_evals}, {"seconds", stage.seconds}};
}

// A mined-v1 factor table as [[top, factor], ...] (strategy_mine_rule.hpp; lane MINE-STAT).
Json bands_json(std::span<const MinedFactorBand> bands) {
  Json out = Json::array();
  for (const MinedFactorBand &band : bands) out.push_back(Json::array({band.top, band.factor}));
  return out;
}

// The campaign's trial recipe (review MINE-3): what one confirm read of an expression is. It
// carries the factor tables, not the factor of --budget's band, so a re-run under another budget
// is the same identity (lane MINE-STAT).
Json recipe_json(const MineConfig &cfg, const ResearchRole &role, const MinePool &pool,
                 const MineWindows &windows, usize label_rows, usize confirm_rows) {
  const IcCacheVmIdentity vm = ic_cache_vm_identity();
  const IcCacheVmIdentity ic = ic_result_cache_identity();
  return Json{
      {"schema", std::string(kTrialRecipe)},
      {"rule", std::string(kMinedRule)},
      {"role_manifest_sha256", role.data().manifest_sha256},
      {"fields_manifest_sha256", role.fields_sha256()},
      {"pool_sha256", pool.sha256},
      {"library",
       {{"vm_identity", vm.identity},
        {"dsl_vm_sources_sha256", vm.sources_sha256},
        {"ic_identity", ic.identity},
        {"ic_sources_sha256", ic.sources_sha256}}},
      {"window_id", std::string(dt::kResearchWindowId)},
      {"discover", window_json(windows.discover, label_rows)},
      {"confirm", window_json(windows.confirm, confirm_rows)},
      {"ic", "research_window_ic_config (EquivalenceV3, horizons 5/21/63, delay 1, maturity at "
             "the window end), ResearchIcOptions{3, true, 1}, h 21 rank IC; decision "
             "membership and research return guard"},
      {"marginal", "combine::marginal_rank_ic_day on the pool regressors, "
                   "summarize_rank_ic Bartlett lag 21"},
      {"overlap_bands", bands_json(kMinedOverlapBands)}, // F by --budget band
      {"confirm_bands", bands_json(kMinedConfirmBands)}, // Fc by band of confirm reads
      {"max_budget", kMinedMaxBudget}, // Ruling PM4-13: the budget the overlap table covers
      {"min_discover_rows", kMinedMinDiscoverRows},
      {"min_confirm_rows", kMinedMinConfirmRows},
      {"min_names", cfg.min_names},
      {"min_dates", cfg.min_dates}};
}
} // namespace

// ---- public -------------------------------------------------------------------------------------
std::vector<std::string> mine_templates(std::span<const std::string> fields) {
  std::vector<std::string> out;
  for (const std::string &f : fields) {
    out.push_back("rank(" + f + ")");
    for (const int w : kTemplateWindows)
      out.push_back("rank(ts_mean(" + f + ", " + std::to_string(w) + "))");
    for (const int w : kTemplateWindows)
      out.push_back("rank(delta(" + f + ", " + std::to_string(w) + "))");
  }
  return out;
}

u64 mine_trial_capacity(const MineConfig &cfg) {
  const u64 per_field = 1U + 2U * static_cast<u64>(kTemplateWindows.size());
  const u64 templates = static_cast<u64>(cfg.role.fields.size()) * per_field;
  const bool stage2 = cfg.stage2_seeds > 0U && cfg.stage2_generations > 0U;
  return templates +
         (stage2 ? static_cast<u64>(cfg.stage2_population) * cfg.stage2_generations : u64{0});
}

u64 MineMemory::resident() const noexcept {
  return metadata + role + regressors + trial_reads + registry;
}

u64 MineMemory::search() const noexcept {
  const u64 fitness = discover_cache + discover_workspaces + rung_caches + rung_workspaces;
  const u64 race = race_panels + race_engines + race_signals;
  const u64 full = full_engines + full_signals;
  return fitness + std::max({bind_transient, race, full});
}

u64 MineMemory::promotion() const noexcept {
  return members + shortlist + std::max({promotion_engine, rho_rows, confirm_cache});
}

u64 MineMemory::peak() const noexcept { return resident() + std::max(search(), promotion()); }

co::Result<MineMemory> mine_memory(const MineFootprint &f) {
  if (f.workers == 0U || f.workers > 64U || f.rungs > 2U ||
      f.regressors > cb::kMaxMarginalRegressors || f.members > kMaxMinePoolMembers ||
      f.shortlist > 256U || f.trials > (1ULL << 32) || f.prior_records > (1ULL << 40))
    return co::Err(fail(co::ErrorCode::InvalidArgument, "working-bytes geometry"));
  ATX_TRY(const u64 role, research_role_bytes(f.dates, f.names, f.extras));
  // Every factor is bounded above, so no product below can overflow u64.
  const u64 dates = f.dates;
  const u64 names = f.names;
  const u64 half = (names + 1U) / 2U; // a racing rung's names: instrument stride >= 2
  const u64 cells = dates * names;
  const u64 strided = dates * half;
  const u64 fields = 3U + static_cast<u64>(f.extras);
  const u64 workers = f.workers;
  const u64 rungs = f.rungs;
  // A VM slot pool of S panels and the engine's copy of the eligibility mask, per cell.
  const u64 slot_cell = static_cast<u64>(kMineMaxProgramSlots) * sizeof(f64) + 1U;
  const u64 full_workspace = kScratchNameBytes * names + kScratchDateBytes * dates;
  const u64 rung_workspace = kScratchNameBytes * half + kScratchDateBytes * dates;
  const u64 rho = static_cast<u64>(f.members) + f.shortlist;
  MineMemory m;
  m.metadata = kMetadataBytes;
  m.role = role;
  m.regressors = static_cast<u64>(f.regressors) * cells * sizeof(f64);
  m.trial_reads = f.trials * (dates * sizeof(f64) + kTrialAllowanceBytes);
  m.registry = kRegistryGramBytes + (f.prior_records + f.trials) * kRegistryRecordBytes;
  m.discover_cache = kIcCacheCellBytes * cells + kIcCacheDateBytes * dates;
  m.discover_workspaces = workers * full_workspace;
  m.rung_caches = rungs * (kIcLabelCellBytes * strided + kIcCacheDateBytes * dates);
  m.rung_workspaces = workers * rungs * rung_workspace;
  m.bind_transient = rungs == 0U ? u64{0} : strided * (fields * sizeof(f64) + kBindCellBytes);
  m.race_panels = rungs * strided * (fields * sizeof(f64) + 1U);
  m.race_engines = workers * rungs * strided * slot_cell;
  m.race_signals = rungs == 0U ? u64{0} : workers * strided * sizeof(f64);
  m.full_engines = workers * cells * slot_cell;
  m.full_signals = workers * cells * sizeof(f64);
  m.members = static_cast<u64>(f.members) * cells * sizeof(f64);
  // Ruling PM5-9: the rho step runs over every trial above the hurdle, streamed so that at most
  // the cap plus one signals are held at once.
  m.shortlist = (static_cast<u64>(f.shortlist) + 1U) * cells * sizeof(f64);
  m.promotion_engine = cells * slot_cell;
  m.rho_rows = rho * names * sizeof(f64) + rho * rho * kPairBytes + kSortPairBytes * names;
  m.confirm_cache = kIcCacheCellBytes * cells + kIcCacheDateBytes * dates + full_workspace;
  return co::Ok(m);
}

co::Result<u64> mine_working_bytes(const MineFootprint &footprint) {
  ATX_TRY(const MineMemory memory, mine_memory(footprint));
  return co::Ok(memory.peak());
}

co::Status run_mine(const MineConfig &cfg, std::ostream &progress) {
  try {
    const auto started = steady::now();
    ATX_TRY_VOID(check_config(cfg));
    std::error_code ec;
    if (fs::exists(cfg.output_directory, ec) || ec)
      return co::Err(fail(co::ErrorCode::AlreadyExists,
                          "output directory must be new: " + cfg.output_directory));
    // Metadata only up to the role load: windows, registry anchor, role axes, pool manifest.
    ATX_TRY(auto windows, parse_windows(cfg));
    ATX_TRY(const auto anchor, registry_anchor(cfg));
    ATX_TRY(const auto geometry, ResearchRole::geometry(cfg.role));
    ATX_TRY(const auto pool_manifest, read_mine_pool_manifest(cfg.pool_path, cfg.pool_sha256));
    ATX_TRY_VOID(check_pool(pool_manifest));
    MineFootprint footprint;
    footprint.dates = geometry.dates;
    footprint.names = geometry.instruments;
    footprint.extras = cfg.role.fields.size();
    footprint.regressors = pool_manifest.regressors.size();
    footprint.members = pool_manifest.members.size();
    footprint.workers = cfg.workers;
    footprint.rungs = cfg.race_strides.size();
    footprint.shortlist = cfg.max_promotions;
    footprint.trials = mine_trial_capacity(cfg);
    footprint.prior_records = anchor ? anchor->records : u64{0};
    ATX_TRY(const MineMemory memory, mine_memory(footprint));
    const u64 required = memory.peak();
    if (required > cfg.max_working_bytes)
      return co::Err(fail(co::ErrorCode::Unavailable,
                          "required_bytes=" + std::to_string(required) +
                              " exceeds --max-memory-mib before any payload load"));
    progress << "mine: campaign=" << cfg.campaign_id << " fields=" << cfg.role.fields.size()
             << " required_bytes=" << required << '\n' << std::flush;
    ResearchRoleSpec spec = cfg.role;
    spec.max_bytes = cfg.max_working_bytes;
    ATX_TRY(const auto role, ResearchRole::load(spec));
    // Review MINE-6: the overlap factor is derived on discover windows of kMinedMinDiscoverRows
    // label rows and more; review MINE-2: the confirm read is made on at least
    // kMinedMinConfirmRows label rows.
    ATX_TRY_VOID(bind_rows(windows.discover, role->data(), "discover", kMinedMinDiscoverRows));
    ATX_TRY_VOID(bind_rows(windows.confirm, role->data(), "confirm", kMinedMinConfirmRows));
    // Lane MINE-MEM: the regressors now; the members checked now and loaded after the search.
    ATX_TRY(auto pool, bind_mine_pool(pool_manifest, *role));
    const usize label_rows = windows.discover.end - windows.discover.begin - kLabelLag;
    const usize confirm_rows = windows.confirm.end - windows.confirm.begin - kLabelLag;

    // The fitness's and the confirm read's regressors, borrowed from `pool` (whose regressor
    // columns are never resized again: loading the members fills another vector).
    std::vector<std::span<const f64>> regressors;
    for (const auto &column : pool.regressors) regressors.emplace_back(column.values);
    // The Library outlives every genome the search keeps (their ops borrow its rows).
    const al::Library lib{};
    const std::vector<std::string> &fields = cfg.role.fields;
    const std::vector<std::string> templates = mine_templates(fields);
    ATX_TRY(const auto search, run_search(cfg, *role, regressors, windows.discover, lib,
                                          templates, progress));
    std::vector<MinedTrial> trials;
    std::unordered_map<u64, usize> index;
    classify(search.stage1, 1U, trials, index);
    if (search.explore) classify(search.stage2, 2U, trials, index);
    // The capacity check bounds this already; a breach would make the hurdle anti-conservative.
    if (trials.size() > cfg.budget)
      return co::Err(fail(co::ErrorCode::Internal,
                          "the search evaluated " + std::to_string(trials.size()) +
                              " distinct trials, above --budget " + std::to_string(cfg.budget)));

    // The campaign's identity: the scoring recipe every trial is an expression under. Review
    // MINE-3: it binds the confirm window with the role, the fields manifest (which pins every
    // field payload), the library (VM and IC sources) and the pool, so each trial's registry
    // identity is its expression under one confirm read.
    Json payloads = Json::object();
    for (const auto &field : role->extras()) payloads[field.name] = field.sha256;
    const Json recipe = recipe_json(cfg, *role, pool, windows, label_rows, confirm_rows);
    ATX_TRY(const std::string recipe_sha, co::sha256_hex(recipe.dump()));
    // A second confirm read on the same identity is refused, not skipped: no trial of this
    // campaign may be in the registry under this recipe already.
    ATX_TRY(const usize held, registered_trials(cfg, trials, recipe_sha, label_rows, anchor));
    if (held != 0U)
      return co::Err(fail(co::ErrorCode::AlreadyExists,
                          std::to_string(held) + " of the campaign's trials are registered already "
                          "under its recipe " + recipe_sha.substr(0, 16) + " (role, fields, "
                          "library, pool, discover and confirm windows): a second confirm read on "
                          "the same identity is refused"));
    // Lane MINE-MEM: the members, which only the promotion's rho check reads, are loaded now --
    // the search and its fitness are gone -- and before anything is written, so a member that
    // changed since its check is refused with the registry and OUTPUT untouched.
    ATX_TRY_VOID(load_mine_pool_members(pool_manifest, *role, pool));

    // Registry: every distinct expression once; the chain head leaves the log at once.
    const fs::path out_dir(cfg.output_directory);
    if (!fs::create_directory(out_dir, ec))
      return co::Err(fail(co::ErrorCode::AlreadyExists,
                          "output directory must be new; " + ec.message()));
    ATX_TRY(const RegistryReceipt registry,
            record_campaign(cfg, trials, recipe_sha, label_rows, anchor));
    // Ruling E-33a: a campaign's line counts the records it added; one that added none is a
    // re-run of a recorded campaign, which has no line to write and is refused before any read
    // of the confirm window. The identity check above refuses it first; this is the backstop
    // against another writer registering the same trials in between.
    if (registry.inserted == 0U) {
      static_cast<void>(fs::remove(out_dir, ec)); // empty: nothing was written into it
      return co::Err(fail(co::ErrorCode::AlreadyExists,
                          "the campaign added no record to " + cfg.registry_path +
                              ": every trial is registered already (a re-run of a recorded "
                              "campaign)"));
    }
    const u64 n_raw = registry.n_raw;
    ATX_TRY_VOID(ev::write_chain_head(out_dir / "registry_head.txt", registry.chain));
    const Counts counts = count_statuses(trials);
    progress << "mine: registry records=" << registry.chain.records << " n_raw=" << n_raw
             << " new=" << registry.inserted << " sha256=" << registry.sha256 << '\n'
             << std::flush;

    // mined-v1 at the Bonferroni value of the campaign's budget (Ruling E-32a: never the realised
    // count, never the registry's), read on f2 / F, F the overlap factor of the budget's band
    // (review MINE-6; lane MINE-STAT).
    const f64 hurdle = mined_hurdle(cfg.budget);
    PromotionContext context;
    context.role = role.get();
    context.pool = &pool;
    context.regressors = regressors;
    context.discover = &windows.discover;
    context.confirm = &windows.confirm;
    context.min_names = cfg.min_names;
    context.min_dates = cfg.min_dates;
    context.max_promotions = cfg.max_promotions;
    context.max_cache_bytes = cfg.max_working_bytes;
    context.overlap_factor = mined_overlap_factor(cfg.budget);
    ATX_TRY(const auto promotions, promote(trials, hurdle, context));
    const Json members = members_json(trials, promotions);

    // Outputs.
    const Json anchor_json = anchor ? Json{{"records", anchor->records},
                                           {"head", hex16(anchor->head)}}
                                    : Json(nullptr);
    Json field_names = Json::array();
    for (const auto &name : fields) field_names.push_back(name);
    const Json inputs_json{
        {"role", {{"path", cfg.role.manifest}, {"manifest_sha256", role->data().manifest_sha256}}},
        {"fields", {{"directory", cfg.role.fields_directory},
                    {"manifest_sha256", role->fields_sha256()},
                    {"names", field_names},
                    {"payload_sha256", payloads}}},
        {"pool", {{"path", cfg.pool_path},
                  {"sha256", pool.sha256},
                  {"regressors", pool.regressors.size()},
                  {"members", pool.members.size()}}}};
    const Json search_json{
        {"seed", cfg.seed},
        {"workers", cfg.workers},
        {"templates", templates.size()},
        {"capacity", mine_trial_capacity(cfg)},
        {"max_program_slots", kMineMaxProgramSlots},
        {"required_bytes", required},
        {"memory", {{"resident", memory.resident()},
                    {"search", memory.search()},
                    {"promotion", memory.promotion()}}},
        {"stage1", stage_json(search.stage1)},
        {"stage2", search.explore ? stage_json(search.stage2) : Json(nullptr)},
        {"stage2_config", {{"seeds", cfg.stage2_seeds},
                           {"population", cfg.stage2_population},
                           {"generations", cfg.stage2_generations}}},
        {"race", {{"strides", cfg.race_strides}, {"keep", cfg.race_keep}}}};
    const Json campaign{
        {"schema", std::string(kCampaignSchema)},
        {"status", "complete"},
        {"campaign_id", cfg.campaign_id},
        {"rule", std::string(kMinedRule)},
        {"budget", cfg.budget},
        {"research_window", {{"id", std::string(dt::kResearchWindowId)},
                             {"seal_begin", std::string(dt::kSealBeginDate)}}},
        {"inputs", inputs_json},
        {"windows", {{"discover", window_json(windows.discover, label_rows)},
                     {"confirm", window_json(windows.confirm, confirm_rows)}}},
        {"recipe_sha256", recipe_sha},
        {"recipe", recipe},
        {"search", search_json},
        {"trials", {{"distinct", trials.size()},
                    {"evaluated", counts.evaluated},
                    {"screen_rejected", counts.screen_rejected},
                    {"racing_rejected", counts.racing_rejected},
                    {"rung_failed", counts.rung_failed}, // review MINE-16
                    {"failed", counts.failed}}},
        {"registry", {{"path", cfg.registry_path},
                      {"format", "V3"},
                      {"records", registry.chain.records},
                      {"chain", hex16(registry.chain.head)},
                      {"head", registry.sha256},
                      {"bytes", registry.bytes},
                      {"n_raw", n_raw},
                      {"new_records", registry.inserted},
                      {"anchor", anchor_json}}},
        {"hurdle", {{"budget", cfg.budget},
                    {"family_alpha", kMinedFamilyAlpha},
                    {"t", finite_or_null(hurdle)},
                    {"overlap_factor", finite_or_null(context.overlap_factor)},
                    {"max_budget", kMinedMaxBudget},
                    {"reads", "f2 / overlap_factor"}}},
        {"promotions", promotions_json(trials, promotions, pool, context.overlap_factor)},
        {"admitted", members.size()},
        {"seconds", seconds_since(started)}};
    const Json mined{{"schema", std::string(kMembersSchema)},
                     {"campaign_id", cfg.campaign_id},
                     {"rule", std::string(kMinedRule)},
                     {"theme", std::string(kMinedTheme)},
                     {"registry_head", registry.sha256},
                     {"members", members}};
    // Ruling E-33: the line backtest_integrity.campaign_line builds (strategy_mine_ledger.hpp;
    // count 0: it adds no trial to any ledger N; Ruling E-33a: registry.count = the records this
    // campaign added, registry.total = n_raw; review MINE-3: the recipe identity and the confirm
    // window, in the trial_id). `research_cycle.py ledger-campaign` rebuilds it from campaign.json
    // through campaign_line, checks the registry against its head and the recipe against its
    // SHA-256, refuses a difference or a second line on the recipe, and appends it chained.
    MineLedgerLine line;
    line.campaign_id = cfg.campaign_id;
    line.registry_path = cfg.registry_path;
    line.registry_head = registry.sha256;
    line.registry_bytes = registry.bytes;
    line.registry_count = registry.inserted;
    line.registry_total = n_raw;
    line.budget = cfg.budget;
    line.recipe_sha256 = recipe_sha;
    line.confirm_begin = windows.confirm.begin_date;
    line.confirm_end = windows.confirm.end_date;
    line.window_id = std::string(dt::kResearchWindowId);
    ATX_TRY(const std::string ledger, mine_ledger_line(line));
    ATX_TRY_VOID(write_text(out_dir / "trials.csv", trials_csv(trials)));
    ATX_TRY_VOID(write_text(out_dir / "mined_members.json", mined.dump(2) + "\n"));
    ATX_TRY_VOID(write_text(out_dir / "ledger_line.json", ledger + "\n"));
    ATX_TRY_VOID(write_text(out_dir / "campaign.json", campaign.dump(2) + "\n"));
    progress << "mine: admitted=" << members.size() << " hurdle=" << hurdle << " wrote "
             << out_dir.string() << '\n' << std::flush;
    return co::Ok();
  } catch (const std::bad_alloc &) {
    return co::Err(fail(co::ErrorCode::Unavailable, "allocation within the envelope failed"));
  } catch (const std::exception &e) {
    return co::Err(fail(co::ErrorCode::InvalidArgument, e.what()));
  }
}

namespace {
std::vector<std::string> split_list(const std::string &value) {
  std::vector<std::string> out;
  std::string::size_type from = 0;
  while (true) {
    const auto comma = value.find(',', from);
    out.push_back(value.substr(from, comma == std::string::npos ? comma : comma - from));
    if (comma == std::string::npos) return out;
    from = comma + 1U;
  }
}

u64 parse_unsigned(const std::string &key, const std::string &value) {
  if (value.empty() || value.front() == '-')
    throw std::invalid_argument("unsigned integer required: " + key);
  usize used{};
  const auto v = std::stoull(value, &used);
  if (used != value.size()) throw std::invalid_argument("invalid integer: " + key);
  return v;
}

constexpr const char *kUsage =
    "atx-equity-strategy-mine --role MANIFEST --role-sha256 SHA [--role-fields DIR\n"
    "    --role-fields-sha256 SHA] --fields NAME[,NAME...] --discover-begin YYYY-MM-DD\n"
    "    --discover-end YYYY-MM-DD --confirm-begin YYYY-MM-DD --confirm-end YYYY-MM-DD\n"
    "    --registry PATH [--registry-head FILE] --campaign-id ID --output NEWDIR --budget N\n"
    "    --pool MANIFEST --pool-sha256 SHA [--seed N (1)] [--workers N (1)]\n"
    "    [--stage2-seeds N (12)] [--stage2-population N (24)] [--stage2-generations N (4)]\n"
    "    [--race-strides S[,S] (4) | none] [--race-keep F (0.333)] [--min-names N (50)]\n"
    "    [--min-dates N (128)] [--max-promotions N (16)] [--max-memory-mib N (2048)]\n"
    "  Mines the --fields of a pinned research role (platform v8 H-3; real data only under\n"
    "  owner decision OD-7). Windows lie inside TRAIN of research-window-v2; a role with a\n"
    "  session at or after the seal is refused. --budget N fixes the campaign's trial budget in\n"
    "  advance (pre-registration rule 10): N covers the templates plus the stage-2 population\n"
    "  times its generations, and the mined-v1 hurdle is the Bonferroni value at N; N is at\n"
    "  most 10000 (Ruling PM4-13: the overlap table is validated to that budget). --pool is\n"
    "  required and names at least one regressor and one member (Ruling E-32a). Writes\n"
    "  NEWDIR/campaign.json, trials.csv, mined_members.json, ledger_line.json and\n"
    "  registry_head.txt (rule mined-v1).\n";
} // namespace

int dispatch_mine(int argc, char **argv, std::ostream &out, std::ostream &err) {
  MineConfig cfg;
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string key = argv[i];
      if (key == "--help") {
        out << kUsage;
        return 0;
      }
      if (++i >= argc) throw std::invalid_argument("missing option value: " + key);
      const std::string value = argv[i];
      const auto count = [&key, &value]() {
        return static_cast<usize>(parse_unsigned(key, value));
      };
      if (key == "--role") cfg.role.manifest = value;
      else if (key == "--role-sha256") cfg.role.manifest_sha256 = value;
      else if (key == "--role-fields") cfg.role.fields_directory = value;
      else if (key == "--role-fields-sha256") cfg.role.fields_sha256 = value;
      else if (key == "--fields") cfg.role.fields = split_list(value);
      else if (key == "--pool") cfg.pool_path = value;
      else if (key == "--pool-sha256") cfg.pool_sha256 = value;
      else if (key == "--discover-begin") cfg.discover_begin = value;
      else if (key == "--discover-end") cfg.discover_end = value;
      else if (key == "--confirm-begin") cfg.confirm_begin = value;
      else if (key == "--confirm-end") cfg.confirm_end = value;
      else if (key == "--registry") cfg.registry_path = value;
      else if (key == "--registry-head") cfg.registry_head_path = value;
      else if (key == "--campaign-id") cfg.campaign_id = value;
      else if (key == "--output") cfg.output_directory = value;
      else if (key == "--budget") cfg.budget = parse_unsigned(key, value);
      else if (key == "--seed") cfg.seed = parse_unsigned(key, value);
      else if (key == "--workers") cfg.workers = count();
      else if (key == "--stage2-seeds") cfg.stage2_seeds = count();
      else if (key == "--stage2-population") cfg.stage2_population = count();
      else if (key == "--stage2-generations") cfg.stage2_generations = count();
      else if (key == "--min-names") cfg.min_names = count();
      else if (key == "--min-dates") cfg.min_dates = count();
      else if (key == "--max-promotions") cfg.max_promotions = count();
      else if (key == "--race-strides") {
        cfg.race_strides.clear();
        if (value != "none") {
          for (const auto &part : split_list(value)) {
            const u64 stride = parse_unsigned(key, part);
            if (stride > 1024U) throw std::invalid_argument("stride above 1024: " + key);
            cfg.race_strides.push_back(static_cast<u32>(stride));
          }
        }
      } else if (key == "--race-keep") {
        usize used{};
        cfg.race_keep = std::stod(value, &used);
        if (used != value.size()) throw std::invalid_argument("invalid number: " + key);
      } else if (key == "--max-memory-mib") {
        const u64 mib = parse_unsigned(key, value);
        if (mib > 65536U) throw std::invalid_argument("memory limit above 65536 MiB");
        cfg.max_working_bytes = mib << 20;
      } else {
        throw std::invalid_argument("unknown option: " + key);
      }
    }
    const auto status = run_mine(cfg, out);
    if (!status) {
      err << status.error().to_string() << '\n';
      return 1;
    }
    return 0;
  } catch (const std::exception &e) {
    err << e.what() << '\n';
    return 2;
  }
}

} // namespace atx::impl::strategy
