#include "atx/engine/factory/search_driver.hpp"

#include <algorithm>     // std::clamp, std::sort, std::max, std::min
#include <bit>           // std::bit_cast (screen checkpoint identity)
#include <cmath>         // std::isfinite (mean_raw telemetry, NaN/inf-safe)
#include <cstddef>       // std::size_t (hash_combine seed type)
#include <cstdint>       // std::uint8_t (compiled[] flag vector)
#include <limits>        // std::numeric_limits (L3 rung NaN rejection)
#include <memory>        // std::unique_ptr, std::make_unique
#include <optional>      // std::optional (per-child single-writer reproduce slots, Tier 5)
#include <span>          // std::span
#include <string>        // std::string (v8 H-3 refusal / functor errors)
#include <unordered_map> // std::unordered_map (score_j_of_ptr: Genome* -> j)
#include <unordered_set> // std::unordered_set (seen_this_gen dedup)
#include <utility>       // std::move
#include <vector>

#include "atx/core/hash.hpp" // atx::core::hash_combine

#include "atx/engine/alpha/bytecode.hpp"  // alpha::compile, alpha::Program
#include "atx/engine/alpha/typecheck.hpp" // alpha::analyze
#include "atx/engine/alpha/unparse.hpp"   // alpha::unparse (population serialization)
#include "atx/engine/alpha/vm.hpp"        // alpha::Engine (digest eval, fresh per program)

#include "atx/engine/parallel/digest.hpp"    // parallel::signal_set_digest
#include "atx/engine/parallel/scheduler.hpp" // parallel::Scheduler, ShardId (Tier 1 LPT dispatch)

namespace atx::engine::factory {

namespace {
[[nodiscard]] atx::u64 ic_config_identity(const IcScreenConfig &cfg,
                                        const alpha::Panel &panel) noexcept {
  // Explicit little-endian FNV words: independent of padding, native enum size,
  // std::hash, allocation order and machine addresses. All effective knobs bind.
  atx::u64 value = 14695981039346656037ULL;
  const auto mix = [&value](atx::u64 word) {
    for (atx::usize byte = 0; byte < 8U; ++byte) {
      value ^= word & 0xffU;
      value *= 1099511628211ULL;
      word >>= 8U;
    }
  };
  mix(static_cast<atx::u64>(cfg.rule));
  mix(panel.dates());
  mix(panel.instruments());
  for (const atx::usize h : cfg.horizons) {
    mix(h);
  }
  mix(cfg.execution_delay);
  mix(cfg.window_begin);
  mix(cfg.window_end);
  mix(cfg.maturity_end);
  mix(cfg.min_names);
  mix(cfg.min_dates);
  mix(std::bit_cast<atx::u64>(cfg.practical_abs_ic));
  mix(std::bit_cast<atx::u64>(cfg.confidence_multiplier));
  mix(cfg.max_cache_bytes);
  return value;
}

// S-quality parsimony (objectives[kObjParsimony] = -node count), shared by every score path.
void set_parsimony(CachedScore &score, const Genome &g) {
  score.objectives[kObjParsimony] = -static_cast<atx::f64>(g.ast.nodes().size());
  score.n_objectives =
      static_cast<atx::u8>(std::max<atx::usize>(score.n_objectives, kObjParsimony + 1U));
}

// ---- platform v8 H-3: cross-section mask and signal-fitness path ---------------------------

// Why the v8 H-3 members of `cfg` cannot run; empty when accepted. With the defaults (no mask,
// default catalogue, no functor, no slot bound) this returns after four checks, so the legacy
// path is untouched.
[[nodiscard]] std::string signal_path_refusal(const SearchConfig &cfg, const alpha::Panel &panel,
                                              bool weak_panel, const combine::AlphaStore &pool,
                                              bool checkpointing, bool injected_ic_cache,
                                              bool execution_context) {
  const std::span<const atx::u8> mask = cfg.cross_section_mask;
  if (!mask.empty()) {
    if (mask.size() != panel.cells() ||
        std::any_of(mask.begin(), mask.end(), [](atx::u8 v) { return v > 1U; })) {
      return "cross_section_mask must be empty or panel cells() flags in {0, 1}";
    }
    if (weak_panel) {
      return "cross_section_mask has the search panel's geometry; a weak panel would run unmasked";
    }
  }
  // Review MINE-11: neither the mask nor the op catalogue is in the checkpoint identity, so a run
  // that sets either takes no progress sink and no resume, on any fitness path.
  const bool catalogue = cfg.op_catalog.literature_ops || !cfg.op_catalog.deny.empty();
  if ((!mask.empty() || catalogue) && checkpointing) {
    return "cross_section_mask and op_catalog are outside the checkpoint identity: no progress "
           "sink and no resume";
  }
  if (cfg.max_program_slots != 0U && cfg.signal_fitness == nullptr) {
    return "max_program_slots bounds the signal-fitness path only";
  }
  if (cfg.signal_fitness == nullptr) {
    return {};
  }
  const FitnessCfg &f = cfg.fitness;
  if (f.objective_rule != FitnessObjectiveRule::LegacyV1 ||
      f.execution.rule != ExecutionObjectiveRule::LegacyStreamsV1 || execution_context ||
      f.execution_context != nullptr) {
    return "signal fitness replaces the residual and execution objectives";
  }
  if (pool.n_alphas() != 0 || weak_panel) {
    return "signal fitness scores the signal alone: no pool, no weak panel";
  }
  if (cfg.ic_screen.rule != IcScreenRule::DisabledV1 || injected_ic_cache) {
    return "signal fitness screens itself: the IC screen must be DisabledV1";
  }
  if (cfg.output_dedup || cfg.deflate_selection || cfg.capacity_objective ||
      cfg.turnover_objective) {
    return "signal fitness refuses output dedup, deflation and capacity/turnover objectives";
  }
  if (checkpointing) {
    return "signal fitness runs take no progress sink and no resume";
  }
  if (f.cpcv.rule != eval::CpcvRule::ObservationV1) {
    return "signal fitness uses no CPCV plan (DateV2 refused)";
  }
  if (cfg.fidelity.enabled && !instrument_only(cfg.fidelity)) {
    return "signal fitness races on instrument strides only (date_stride 1, n_folds 0)";
  }
  return {};
}

// Copies the run's eligibility mask into `engine`, strided to `rung`'s sub-panel of `panel`
// (the full rung copies it as is). An empty mask leaves the engine untouched.
[[nodiscard]] atx::core::Status apply_mask(alpha::Engine &engine, std::span<const atx::u8> mask,
                                           const alpha::Panel &panel, const Rung &rung) {
  if (mask.empty()) {
    return atx::core::Ok();
  }
  if (rung.full()) {
    return engine.set_cross_section_mask(std::vector<atx::u8>(mask.begin(), mask.end()));
  }
  ATX_TRY(auto strided, strided_cells(mask, panel.dates(), panel.instruments(),
                                      rung.date_stride, rung.inst_stride));
  return engine.set_cross_section_mask(std::move(strided));
}

// One full-pass engine of the run: bound to `panel` with the run's eligibility mask (a no-op
// without one). run() builds one per worker and the signal-fitness path rebuilds them after each
// race (lane MINE-MEM) through this one function, so both sites construct the same engine.
[[nodiscard]] atx::core::Result<std::unique_ptr<alpha::Engine>>
make_full_engine(const alpha::Panel &panel, std::span<const atx::u8> mask) {
  auto engine = std::make_unique<alpha::Engine>(panel);
  ATX_TRY_VOID(apply_mask(*engine, mask, panel, Rung{}));
  return atx::core::Ok(std::move(engine));
}

// The full-pass score of a representative on the signal-fitness path. The slot keeps the
// unscored sentinel for an empty signal set or a non-finite score; a functor Err goes to `error`.
void score_signal(const Genome &g, const alpha::SignalSet &signals, const SearchConfig &cfg,
                  atx::usize wid, CachedScore &slot, std::string &error) {
  if (signals.alphas.empty()) {
    return;
  }
  auto scored = cfg.signal_fitness->score(g, signals.alphas.front().values, SignalLevel{}, wid);
  if (!scored) {
    error = scored.error().to_string();
    return;
  }
  if (scored->rejected) {
    slot = ic_rejected_score();
    return;
  }
  if (!std::isfinite(scored->raw) || scored->n_objectives > kMaxObjectives) {
    return;
  }
  CachedScore full{};
  full.raw = scored->raw;
  full.objectives = scored->objectives;
  full.n_objectives = scored->n_objectives;
  if (cfg.enable_parsimony) {
    set_parsimony(full, g);
  }
  slot = std::move(full);
}

// A racing rung's score of `g` on the signal-fitness path: evaluate on the rung's engine, then
// the functor's raw. NaN (a rung rejection) for a compile or VM failure, a rejected or
// non-finite score, and a functor Err (the race's evaluator carries a score only).
[[nodiscard]] atx::f64 signal_rung_score(const Genome &g, alpha::Engine &engine,
                                         SignalFitness &fitness, const SignalLevel &level,
                                         atx::usize wid) {
  constexpr atx::f64 kReject = std::numeric_limits<atx::f64>::quiet_NaN();
  auto prog = alpha::compile(g.ast, g.analysis);
  if (!prog.has_value()) {
    return kReject;
  }
  auto ss = engine.evaluate(*prog);
  if (!ss.has_value() || ss->alphas.empty()) {
    return kReject;
  }
  auto scored = fitness.score(g, ss->alphas.front().values, level, wid);
  if (!scored || scored->rejected || !std::isfinite(scored->raw)) {
    return kReject;
  }
  return scored->raw;
}
} // namespace

SearchDriver::SearchDriver(const alpha::Library &lib, const alpha::Panel &panel,
                           const WeightPolicy &policy, const exec::ExecutionSimulator &sim,
                           std::vector<std::string> seed_exprs,
                           std::vector<std::string> panel_fields, const alpha::Panel *weak_panel,
                           std::vector<std::string> numeric_excluded_fields,
                           std::vector<std::string> extra_group_fields)
    : lib_{lib}, panel_{panel}, weak_panel_{weak_panel}, policy_{policy}, sim_{sim},
      catalog_{lib}, seed_exprs_{std::move(seed_exprs)}, panel_fields_{std::move(panel_fields)} {
  // R1: build O(k) lookup sets for the exclusion/extra-group lists (k == list sizes,
  // typically tiny). Deterministic: no RNG, stable iteration over panel_fields_.
  // When both lists are empty these branches are dead and the partition is IDENTICAL
  // to the pre-R1 code -> byte-identical digest (kGoldenDigest pin unchanged).
  const std::vector<std::string> num_excl = std::move(numeric_excluded_fields);
  const std::vector<std::string> grp_extra = std::move(extra_group_fields);
  auto in_excl = [&](std::string_view name) noexcept {
    for (const auto &s : num_excl) { if (s == name) return true; }
    return false;
  };
  auto in_extra_grp = [&](std::string_view name) noexcept {
    for (const auto &s : grp_extra) { if (s == name) return true; }
    return false;
  };

  panel_field_views_.reserve(panel_fields_.size());
  for (const std::string &f : panel_fields_) {
    panel_field_views_.push_back(f);
    const std::string_view sv = panel_field_views_.back();
    // R1: a field in the exclusion list is removed from the numeric pool regardless
    // of is_group_field; an extra-group field is added to the group pool even if
    // is_group_field is false. When both lists are empty, behaviour is IDENTICAL to
    // the original Task 3.1 partition (byte-identical by construction).
    const bool is_group = alpha::detail::is_group_field(f) || in_extra_grp(sv);
    const bool is_numeric_excluded = in_excl(sv);
    if (is_group) {
      group_field_views_.push_back(sv);
    } else if (!is_numeric_excluded) {
      numeric_field_views_.push_back(sv);
    }
    // A field that is neither group nor numeric (i.e. excluded from numeric, not
    // routed to group) remains accessible via panel_field_views_ for eval validity
    // but cannot appear as a numeric or group leaf in the grammar.
  }
}

[[nodiscard]] SearchResult SearchDriver::run(const SearchConfig &input,
                                             const combine::AlphaStore &pool,
                                             SearchProgressSink *sink,
                                             const SearchResumeState *resume,
                                             const IcScreenCache *prepared_ic_screen,
                                             const ExecutionObjectiveContext *execution_context) {
  SearchResult res;
  res.seed = input.master_seed;
  // v8 H-3: this run's op-swap catalogue (the default cfg rebuilds the constructor's exactly),
  // then the preconditions of the mask and the signal-fitness path (none on the default path).
  catalog_ = OpCatalog{lib_, input.op_catalog};
  if (auto refusal = signal_path_refusal(input, panel_, weak_panel_ != nullptr, pool,
                                         sink != nullptr || resume != nullptr,
                                         prepared_ic_screen != nullptr,
                                         execution_context != nullptr);
      !refusal.empty()) {
    res.signal_path_invalid = true;
    res.signal_path_error = std::move(refusal);
    return res;
  }
  const bool residual_on = input.fitness.objective_rule == FitnessObjectiveRule::ResidualHacIcV2;
  const auto fail_residual = [&](std::string message) {
    res.residual_invalid = true; res.residual_error = std::move(message);
  };
  if (input.fitness.objective_rule != FitnessObjectiveRule::LegacyV1 && !residual_on) {
    fail_residual("unknown fitness objective rule"); return res;
  }
  if (residual_on) {
    const auto *binding = input.fitness.residual_binding;
    if (!binding || !binding->matches(panel_) || input.fitness.residual_scratch != nullptr) {
      fail_residual("residual objective needs a prepared immutable Panel binding and owned worker scratch");
      return res;
    }
    if (input.ic_screen.rule != IcScreenRule::DisabledV1 || prepared_ic_screen != nullptr) {
      fail_residual("residual objective requires the raw IC screen explicitly DisabledV1"); return res;
    }
    if (input.objective_mode != ObjectiveMode::ScalarRaw || input.output_dedup || input.deflate_selection ||
        input.fidelity.enabled || resume || sink || weak_panel_ || pool.n_alphas() != 0 ||
        execution_context || input.fitness.execution_context ||
        input.fitness.execution.rule != ExecutionObjectiveRule::LegacyStreamsV1 ||
        input.capacity_objective || input.turnover_objective || input.fitness.capacity_objective ||
        input.fitness.turnover_objective || input.fitness.target_aum != 0 ||
        input.fitness.cost_selection.impact_in_selection || input.fitness.turnover_penalty_slope != 0) {
      fail_residual("residual IC-only scoring refuses unbound pools, checkpoints, fidelity, dedup and legacy overlays");
      return res;
    }
    res.residual_context_sha256 = binding->context().identity_sha256();
    for (const char ch : std::string{"ResidualHacIcV2-signed-equal-mean:"} + res.residual_context_sha256)
      res.digest = (res.digest ^ static_cast<unsigned char>(ch)) * 1099511628211ULL;
  }
  const bool execution_on = input.fitness.execution.rule == ExecutionObjectiveRule::DelayedSurfaceV2;
  std::optional<SearchConfig> execution_cfg;
  const auto fail_execution = [&](std::string message) {
    res.execution_invalid = true; res.execution_error = std::move(message);
  };
  if (input.fitness.execution.rule != ExecutionObjectiveRule::LegacyStreamsV1 && !execution_on) {
    fail_execution("unknown execution objective rule"); return res;
  }
  if (execution_on) {
    const auto* context = execution_context != nullptr ? execution_context : input.fitness.execution_context;
    if (context == nullptr || (execution_context != nullptr && input.fitness.execution_context != nullptr &&
        execution_context != input.fitness.execution_context) ||
        !execution_objective_matches(*context, panel_, policy_, input.fitness.execution)) {
      fail_execution("execution context/policy/panel mismatch"); return res;
    }
    // Legacy raw*DSR assumes a nonnegative fitness. V2 net Sharpe is signed:
    // shrinking a negative value toward zero would improve its selection rank.
    if (input.deflate_selection) {
      fail_execution("execution V2 does not support the legacy multiplicative DSR overlay");
      return res;
    }
    if (resume != nullptr || sink != nullptr || input.fidelity.enabled || weak_panel_ != nullptr ||
        input.capacity_objective || input.turnover_objective || input.fitness.target_aum != 0.0 ||
        input.fitness.cost_selection.impact_in_selection || input.fitness.turnover_penalty_slope != 0.0 ||
        pool.n_alphas() != 0) {
      fail_execution("execution V2 does not support unbound pools, checkpoints, fidelity or legacy cost/weak overlays");
      return res;
    }
    execution_cfg = input;
    execution_cfg->fitness.execution_context = context;
    res.execution_context_sha256 = context->identity_sha256();
    // Immutable execution identity contributes before any candidate digest.
    for (const char ch : res.execution_context_sha256)
      res.digest = (res.digest ^ static_cast<unsigned char>(ch)) * 1099511628211ULL;
  }
  const SearchConfig& cfg = execution_cfg ? *execution_cfg : input;
  std::optional<atx::u64> cpcv_identity = execution_on || residual_on || cfg.fitness.cpcv.rule == eval::CpcvRule::ObservationV1
      ? std::nullopt : std::optional<atx::u64>{(eval::cpcv_recipe_identity(cfg.fitness.cpcv) ^
          (static_cast<atx::u64>(panel_.dates()) * 1099511628211ULL))};
  if (cpcv_identity) {
    const auto bind = [&](atx::u64 v) { *cpcv_identity = (*cpcv_identity ^ v) * 1099511628211ULL; };
    bind(cfg.fitness.cpcv_session_stride);
    bind(static_cast<atx::u64>(cfg.fidelity.enabled));
    if (cfg.fidelity.enabled) {
      for (const auto& rung : cfg.fidelity.rungs) {
        bind(rung.date_stride); bind(rung.inst_stride); bind(rung.n_folds);
      }
    }
    CpcvCache validation_cache;
    auto plan = validation_cache.get_or_build_checked(
        panel_.dates(), cfg.fitness.cpcv, cfg.fitness.cpcv_session_stride);
    if (!plan) { res.cpcv_invalid = true; return res; }
    if (cfg.fidelity.enabled) {
      for (const auto& rung : cfg.fidelity.rungs) {
        if (rung.full()) break;
        if (rung.date_stride == 0U || rung.inst_stride == 0U) {
          res.cpcv_invalid = true; return res;
        }
        auto rung_cfg = cfg.fitness.cpcv;
        if (rung.n_folds > 1U) {
          rung_cfg.n_groups = rung.n_folds;
          rung_cfg.n_test_groups = std::min<atx::usize>(rung_cfg.n_test_groups, rung.n_folds - 1U);
        }
        const auto periods = panel_.dates() / rung.date_stride +
            static_cast<atx::usize>(panel_.dates() % rung.date_stride != 0U);
        auto rung_plan = validation_cache.get_or_build_checked(periods, rung_cfg, rung.date_stride);
        if (!rung_plan) { res.cpcv_invalid = true; return res; }
      }
    }
    res.cpcv_metadata = (*plan)->metadata;
    res.digest = *cpcv_identity;
    if (resume != nullptr && resume->cache_blob.empty()) {
      res.cpcv_resume_mismatch = true;
      return res;
    }
  }

  if (cfg.ic_screen.rule != IcScreenRule::DisabledV1 && prepared_ic_screen != nullptr) {
    if (!ic_screen_cache_matches(*prepared_ic_screen, panel_, cfg.ic_screen)) {
      res.ic_screen_cache_mismatch = true;
      return res; // never screen against another training window or recipe
    }
    if (resume != nullptr) {
      res.ic_screen_resume_mismatch = true;
      return res; // caller membership/guard identity is absent from checkpoints
    }
  }
  res.best_fitness_per_gen.reserve(cfg.generations);
  // L3 per-run state: the canonical key config and the fingerprint index start
  // clean on every run() so a same-seed replay is byte-identical (F1).
  canon_cfg_ = cfg.canon;
  fp_index_ = FingerprintIndex{};

  CanonSet canon; // F6 dedup: distinct structures scored so far
  // Run-LOCAL fitness cache keyed by canon_hash (F6 throughput: an equivalent
  // structure is scored ONCE per run). Declared per-run (not a member) so a
  // second run() with the same seed replays from a clean slate (F1). Caches the
  // raw scalar AND the S4.1 multi-objective vector (CachedScore).
  std::unordered_map<atx::u64, CachedScore> fitness_cache;
  std::vector<ObjectiveIcScratch> residual_scratch;
  if (residual_on) {
    const auto &context = cfg.fitness.residual_binding->context();
    const auto budget = context.config().max_working_bytes;
    const auto owned = context.bytes(), per_worker = context.per_signal_working_bytes();
    // Admission must precede DetPool's constructor: it immediately launches
    // threads. This explicit mode refuses auto sizing instead of resolving a
    // machine-dependent worker count after resources have already been spent.
    if (!cfg.n_workers || owned > budget || !per_worker || cfg.n_workers > context.config().workers ||
        cfg.n_workers > (budget - owned) / per_worker) {
      fail_residual("residual context plus declared worker scratch exceeds budget"); return res;
    }
    residual_scratch.reserve(cfg.n_workers);
    for (atx::usize w = 0; w < cfg.n_workers; ++w) {
      auto scratch = prepare_objective_ic_scratch(context);
      if (!scratch) { fail_residual(scratch.error().to_string()); return res; }
      residual_scratch.push_back(std::move(*scratch));
    }
  }
  parallel::DetPool det_pool{cfg.n_workers};
  if (execution_on) {
    const auto& context = *cfg.fitness.execution_context;
    const auto maximum = context.config().max_working_bytes;
    const auto owned = context.bytes(), per_worker = context.per_signal_working_bytes();
    // Execution-only payload budget; existing per-worker VM and search caches
    // are outside it. Check before their construction and any candidate work.
    if (owned > maximum || per_worker == 0 || det_pool.n_workers() > (maximum - owned) / per_worker) {
      fail_execution("execution context plus worker scratch exceeds budget"); return res;
    }
  }
  // v8 H-3: the functor sizes its worker state for this pool and learns the racing rungs
  // (serial, before any candidate work).
  if (cfg.signal_fitness != nullptr) {
    const atx::usize n_low = cfg.fidelity.enabled ? first_full_rung(cfg.fidelity) : 0U;
    const auto bound = cfg.signal_fitness->bind(
        SignalFitnessBinding{det_pool.n_workers(), panel_.dates(), panel_.instruments(),
                             std::span<const Rung>{cfg.fidelity.rungs.data(), n_low}});
    if (!bound) {
      res.signal_path_invalid = true;
      res.signal_path_error = bound.error().to_string();
      return res;
    }
  }

  std::optional<IcScreenCache> owned_ic_cache;
  const std::optional<atx::u64> ic_identity =
      cfg.ic_screen.rule == IcScreenRule::DisabledV1
          ? std::nullopt : std::optional<atx::u64>{ic_config_identity(cfg.ic_screen, panel_)};
  if (resume != nullptr && ic_identity && resume->cache_blob.empty()) {
    res.ic_screen_resume_mismatch = true;
    return res; // population-only legacy checkpoints cannot certify active-screen state
  }
  const IcScreenCache *ic_cache = nullptr;
  std::vector<IcScreenScratch> ic_scratch;
  if (cfg.ic_screen.rule != IcScreenRule::DisabledV1) {
    ic_cache = prepared_ic_screen;
    if (ic_cache == nullptr) {
      auto prepared = prepare_ic_screen(panel_, cfg.ic_screen);
      if (prepared) {
        owned_ic_cache.emplace(std::move(*prepared));
        ic_cache = &*owned_ic_cache;
      } else {
        ++res.ic_screen_unavailable;
      }
    }
    if (ic_cache != nullptr) {
      ic_scratch.reserve(det_pool.n_workers());
      for (atx::usize w = 0; w < det_pool.n_workers(); ++w) {
        auto scratch = prepare_ic_screen_scratch(*ic_cache);
        if (!scratch) {
          ++res.ic_screen_unavailable;
          ic_cache = nullptr; // uncertain preparation never discards a candidate
          ic_scratch.clear();
          break;
        }
        ic_scratch.push_back(std::move(*scratch));
      }
    }
  }

  // One reusable Engine per worker, bound to panel_, built ONCE per run() and reused
  // across EVERY generation (Tier 4). Engine holds a const Panel& (not move-assignable)
  // -> unique_ptr so the vector never assigns on growth (mirrors parallel_evaluate,
  // batch_eval.cpp:89-93). Engine::evaluate is idempotent (output depends only on
  // (program, panel_)) and panel_ is constant for the whole run, so reusing each engine
  // across generations is byte-identical to a fresh-per-generation engine — the SlotPool
  // simply grows ONCE to its peak and is reused, eliminating per-generation reallocation.
  // Run-LOCAL (not a member) so a second run() with the same seed replays from a clean
  // slate (F1) — the same lifecycle discipline as det_pool / fitness_cache / behavior_archive.
  std::vector<std::unique_ptr<alpha::Engine>> engines;
  engines.reserve(det_pool.n_workers());
  for (atx::usize w = 0; w < det_pool.n_workers(); ++w) {
    // v8 H-3: the mask is a no-op without a cross_section_mask (validated by
    // signal_path_refusal).
    auto engine = make_full_engine(panel_, cfg.cross_section_mask);
    if (!engine) {
      res.signal_path_invalid = true;
      res.signal_path_error = engine.error().to_string();
      return res;
    }
    engines.push_back(std::move(*engine));
  }

  // S4.2 behavioral archive: a per-RUN ring of past-elite descriptors (declared
  // here, not a member, so a second run() with the same seed replays from a clean
  // slate — F1). Empty + unused when the behavioral objective is inactive. `nbr`
  // is the per-generation k-nearest population scratch, sized once and reused (no
  // hot-path alloc in the inner generation loop).
  BehavioralArchive behavior_archive{cfg.behavior_archive_cap, cfg.archive_eviction,
                                     cfg.behavior_metric};
  std::vector<std::span<const atx::f64>> nbr;

  // Initial population + start generation. Off-path (resume == nullptr) this is
  // EXACTLY the legacy `pop = init_population(cfg)` path. On a well-formed resume
  // the loop starts at resume->start_generation from the checkpoint population
  // (the canonical DSL strings captured in a prior GenerationSnapshot). The
  // resume->start_generation must be a real interior generation [1, generations);
  // a corrupt/incompatible blob (deserialize Err) returns an empty well-formed
  // result rather than silently restarting from gen 0. The impl-side validates the
  // blob hash before calling, so a valid resume always succeeds (deserialize Ok).
  std::vector<Genome> pop;
  atx::usize gen_start = 0;
  bool resumed_state = false;
  if (resume != nullptr && resume->start_generation > 0 &&
      resume->start_generation < cfg.generations && !resume->population.empty()) {
    auto restored = deserialize_population(resume->population);
    if (!restored) { // corrupt/incompatible checkpoint -> fail loud, do NOT silently restart
      SearchResult err_res;
      err_res.seed = cfg.master_seed;
      return err_res; // empty, well-formed result
    }
    pop = std::move(*restored);
    gen_start = resume->start_generation;
    resumed_state = true;
  } else {
    pop = init_population(cfg);
  }
  res.candidates_generated += pop.size();

  // RESUME: restore the FULL cross-generation accumulated state ENTERING gen_start so
  // gens [gen_start..generations) replay BYTE-IDENTICALLY to an uninterrupted run
  // (F1). Without this, canon / fitness_cache / behavior_archive start EMPTY and
  // res.digest / counters start at 0, so the resumed trajectory (ranking -> selection
  // -> reproduction -> admission) and the folded digest diverge. The blobs are bit-
  // exact, deterministically ordered, and lossless (search_progress.hpp codecs). A
  // malformed blob is a corrupt checkpoint -> fail loud (well-formed empty result),
  // never a silent partial restore. Off-path (resume == nullptr) this whole block is
  // skipped and the loop is the byte-identical legacy path.
  if (resumed_state) {
    auto canon_keys = deserialize_canon(resume->canon_blob);
    auto archive_entries = deserialize_archive(resume->archive_blob);
    auto best_pg = deserialize_f64_list(resume->best_per_gen_blob);
    std::vector<atx::u64> cache_keys;
    std::vector<CachedScore> cache_vals;
    std::optional<atx::u64> restored_ic_identity, restored_cpcv_identity;
    auto cache_st = deserialize_cache(resume->cache_blob, cache_keys, cache_vals,
                                      &restored_ic_identity, &restored_cpcv_identity);
    if (!canon_keys || !archive_entries || !best_pg || !cache_st) {
      SearchResult err_res; // corrupt accumulated-state blob -> fail loud
      err_res.seed = cfg.master_seed;
      return err_res;
    }
    if (restored_cpcv_identity != cpcv_identity) {
      res.cpcv_resume_mismatch = true;
      return res;
    }
    const bool has_ic_rejection = std::any_of(
        cache_vals.begin(), cache_vals.end(),
        [](const CachedScore &score) { return score.origin == ScoreOrigin::IcRejected; });
    if (std::any_of(cache_vals.begin(), cache_vals.end(), [](const CachedScore &score) {
          return score.origin == ScoreOrigin::ResidualUnavailable;
        })) {
      fail_residual("residual score checkpoints are not supported"); return res;
    }
    if (restored_ic_identity != ic_identity || (has_ic_rejection && !restored_ic_identity)) {
      SearchResult incompatible;
      incompatible.seed = cfg.master_seed;
      incompatible.ic_screen_resume_mismatch = true;
      return incompatible;
    }
    for (atx::u64 h : *canon_keys) {
      canon.insert(h);
    }
    for (atx::usize i = 0; i < cache_keys.size(); ++i) {
      if (cache_vals[i].origin == ScoreOrigin::IcRejected) {
        res.ic_rejected_hashes.push_back(cache_keys[i]);
      }
      fitness_cache.emplace(cache_keys[i], std::move(cache_vals[i]));
    }
    std::sort(res.ic_rejected_hashes.begin(), res.ic_rejected_hashes.end());
    res.ic_rejected_hashes.erase(
        std::unique(res.ic_rejected_hashes.begin(), res.ic_rejected_hashes.end()),
        res.ic_rejected_hashes.end());
    // Replay archive inserts in ring order (oldest first) so the FIFO contents — and
    // therefore every future novelty() neighbourhood — match the uninterrupted run.
    for (const std::vector<atx::f64> &e : *archive_entries) {
      behavior_archive.insert(std::span<const atx::f64>{e});
    }
    res.best_fitness_per_gen = std::move(*best_pg);
    res.digest = resume->digest;
    // Overwrite (NOT accumulate) candidates_generated with the value ENTERING
    // gen_start: the `+= pop.size()` above counted the restored population, but the
    // persisted counter already includes the full [0..gen_start) accrual.
    res.candidates_generated = resume->candidates_generated;
  }

  std::vector<Scored> scored; // current generation's scored population
  // Run-local mean-fitness history — parallel to res.best_fitness_per_gen —
  // used by the stagnation early-stop to guard against false positives on
  // non-collapsed populations (best_raw is non-decreasing BY CONSTRUCTION due
  // to elitism, so a best-raw plateau does NOT signal convergence; mean_raw
  // collapsing is the genuine signal of population homogeneity).
  std::vector<atx::f64> mean_fitness_per_gen;
  mean_fitness_per_gen.reserve(cfg.generations);

  // ----- Task 5: adaptive operator selection (run-local credit state) ---------
  // `op_weights` (op_swap, field_swap, jitter) bias each generation's mutation-
  // operator draw. They are updated SERIALLY here (before reproduce) from the
  // realized fitness gain of the PREVIOUS generation's children, so every child of
  // a generation draws against the SAME fixed weights -> worker-count-invariant
  // (F1). Inert (stays uniform, never read) when cfg.adaptive_operators is false.
  //
  // DATA FLOW per generation g (g > the generation that produced the current pop):
  //   1. reproduce(g-1) filled `prev_child_ops`[p] with the operator id (0/1/2, or
  //      0xFF for crossover/immigrant/elite-clone) that made child slot p, written
  //      by the single shard that owns slot p (no race).
  //   2. Those children ARE this generation's population slots [prev_n_elites..),
  //      and evaluate_generation assembles `scored` in population order, so
  //      scored[prev_n_elites + p] is exactly the child from slot p.
  //   3. mean_gain[o] = mean over mutation-children of operator o of
  //      (child_raw - prev_parent_best), where prev_parent_best is the best raw of
  //      the generation that PRODUCED them. op_weights[o] = max(0.05,
  //      op_weights[o] + mean_gain[o]) so no operator starves.
  // All RNG-free and computed from `scored` in population order -> deterministic.
  // NOTE (resume): on a resume the credit history is not persisted, so a resumed
  // adaptive run is NOT guaranteed byte-identical to an uninterrupted one; the
  // resume-identity determinism tests pin adaptive_operators=false for that reason.
  constexpr atx::f64 kOpWeightFloor = 0.05;
  std::array<atx::f64, 3> op_weights{1.0, 1.0, 1.0};
  std::vector<atx::u8> prev_child_ops;  // operator id per child slot of the last reproduce
  atx::usize prev_n_elites = 0;         // population offset of those children
  atx::f64 prev_parent_best = 0.0;      // best raw of the generation that produced them
  bool have_prev_children = false;

  for (atx::usize gen = gen_start; gen < cfg.generations; ++gen) {
    // Capture the ACCUMULATED state ENTERING this generation (BEFORE
    // evaluate_generation / update_archive mutate canon / fitness_cache /
    // behavior_archive / res.digest). This is the exact state a resume must restore
    // to be byte-identical, and it is consistent with the `pop` population snapshot
    // (also the generation INPUT). Captured only when a sink is attached — off-path
    // (sink == nullptr) this is skipped entirely (no work, byte-identical legacy
    // loop). Serialized lazily into the snapshot below so a no-resume sink still pays
    // only the serialize cost it already incurs for the population blob.
    std::string entering_canon_blob;
    std::string entering_cache_blob;
    std::string entering_archive_blob;
    std::string entering_best_pg_blob;
    const atx::u64 entering_digest = res.digest;
    const atx::usize entering_candidates = res.candidates_generated;
    if (sink != nullptr) {
      entering_canon_blob = serialize_canon(canon);
      // fitness_cache serialized in sorted-by-canon-hash key order (deterministic).
      {
        std::vector<atx::u64> ck;
        ck.reserve(fitness_cache.size());
        for (const auto &kv : fitness_cache) {
          ck.push_back(kv.first);
        }
        std::sort(ck.begin(), ck.end());
        std::vector<CachedScore> cv;
        cv.reserve(ck.size());
        for (atx::u64 k : ck) {
          cv.push_back(fitness_cache.at(k));
        }
        entering_cache_blob = serialize_cache(ck, cv, ic_identity, cpcv_identity);
      }
      entering_archive_blob = serialize_archive(behavior_archive.entries());
      entering_best_pg_blob = serialize_f64_list(res.best_fitness_per_gen);
    }

    // (a)-(c): evaluate the fresh (not-yet-seen) candidates of `pop`, fold the
    // determinism digest, and score each via pool_aware_fitness (cached by canon).
    scored = evaluate_generation(pop, cfg, gen, pool, canon, fitness_cache, det_pool, engines, res,
                                 ic_cache, ic_scratch, residual_scratch);
    if (res.execution_invalid || res.residual_invalid || res.signal_path_invalid) return res;

    // (d2) S4.2 behavioral-novelty pass: write the population-relative phenotypic
    // novelty into objectives[3] (n_objectives -> 4) BEFORE ranking, but ONLY when
    // the objective is active (MultiObjective && enable_behavioral_novelty). A no-op
    // otherwise -> n_objectives stays 3 and the boundary pin is byte-untouched.
    behavioral_novelty_pass(scored, behavior_archive, cfg, nbr);

    // (d') NSGA-II rank + crowding (S4.1), assigned ONCE per generation in
    // canonical-id order BEFORE any reproduction RNG draw (F1/F2). In ScalarRaw
    // this collapses to a total order by RAW fitness (the pre-S4 path); in
    // MultiObjective it is the Pareto front rank + per-front crowding distance.
    // Reads n_objectives dynamically -> the 4th (behavioral) column enters here
    // automatically when active.
    const std::vector<atx::usize> canon_order = canon_ordered_indices(scored);
    assign_pareto_ranks(scored, canon_order, cfg);

    // (d3) S4.2 archive update: insert this generation's Pareto FRONT (rank-0
    // elites) in canonical-id order, evict oldest past C. After ranks are known,
    // before reproduction. No-op when the objective is inactive.
    update_archive(scored, canon_order, cfg, behavior_archive);

    // Track the best RAW fitness this generation (the maximized search signal,
    // NOT the novelty-penalized selection score). A best-raw elite is carried
    // verbatim into the next gen and re-scores to the same cached raw value, so
    // this sequence is non-decreasing by construction (the ElitismKeepsBest
    // guarantee).
    res.best_fitness_per_gen.push_back(best_raw(scored));
    mean_fitness_per_gen.push_back(mean_raw(scored));

    // Progress sink (resumable-discover). Off-path (sink == nullptr) this is a
    // single null-pointer check — no work, byte-identical legacy loop. When set,
    // hand the sink a snapshot of the population that ENTERED this generation
    // (`pop` is NOT mutated by evaluate_generation — it scores into
    // scored/canon/fitness_cache only — so serializing it here is correct without
    // a loop-top copy). An Err return (real I/O failure or an injected test crash)
    // aborts cleanly: finalize the current scored set into a well-formed partial
    // result and return. canon.size() is the distinct-scored count (the CanonSet
    // local). This call runs BEFORE reproduce, so the snapshot population is the
    // exact blob a resume feeds back via SearchResumeState.
    if (sink != nullptr) {
      GenerationSnapshot snap;
      snap.generation = gen;
      snap.population = serialize_population(pop); // population that ENTERED gen `gen`
      snap.best_fitness = best_raw(scored);
      snap.mean_fitness = mean_raw(scored);
      snap.n_evaluated = canon.size();
      snap.n_unique = pop.size();
      // Accumulated state ENTERING gen `gen` (captured above, consistent with the
      // population snapshot) — the full payload a resume restores for byte-identity.
      snap.canon_blob = std::move(entering_canon_blob);
      snap.cache_blob = std::move(entering_cache_blob);
      snap.archive_blob = std::move(entering_archive_blob);
      snap.best_per_gen_blob = std::move(entering_best_pg_blob);
      snap.digest = entering_digest;
      snap.candidates_generated = entering_candidates;
      auto st = sink->on_generation(snap);
      if (!st) { // sink-requested abort -> clean stop with a well-formed partial result
        finalize(scored, canon, res);
        return res;
      }
    }

    // Stagnation early-stop (pure fn of best_fitness_per_gen + mean_fitness_per_gen;
    // F1-safe). 0 disables. Placed AFTER the sink checkpoint and BEFORE reproduce.
    //
    // WHY BOTH: best_raw is NON-DECREASING BY CONSTRUCTION (elitism carries the
    // best genome verbatim and the canon cache re-scores it identically), so a
    // best-raw plateau is a NORMAL elitist-GA state, NOT genuine convergence. Mean
    // fitness collapsing is the genuine signal of population homogeneity (all
    // genomes converged to the same score). Requiring BOTH guards against early
    // termination on healthy, diverse populations.
    //
    // Small epsilon (1e-9) instead of strict `>` to avoid float-equality flakiness
    // from NaN propagation or benign rounding. The break falls through to the
    // post-loop finalize, returning a well-formed result on the current scored set.
    if (cfg.stagnation_patience > 0 &&
        res.best_fitness_per_gen.size() > cfg.stagnation_patience) {
      const atx::usize n = res.best_fitness_per_gen.size();
      const atx::f64 best_recent   = res.best_fitness_per_gen[n - 1];
      const atx::f64 best_baseline = res.best_fitness_per_gen[n - 1 - cfg.stagnation_patience];
      const atx::f64 mean_recent   = mean_fitness_per_gen[n - 1];
      const atx::f64 mean_baseline = mean_fitness_per_gen[n - 1 - cfg.stagnation_patience];
      constexpr atx::f64 kEps = 1e-9;
      const bool best_flat = !(best_recent > best_baseline + kEps);
      const bool mean_flat = !(mean_recent > mean_baseline + kEps);
      if (best_flat && mean_flat) { break; }
    }

    // Task 5: credit the PREVIOUS generation's operators from the realized fitness
    // gain of the children they produced (now scored, in population order), then
    // bias this generation's operator weights toward what worked. SERIAL (before the
    // parallel reproduce) so every child draws against FIXED weights. Pure fn of
    // `scored` + the recorded operator ids -> RNG-free, worker-count-invariant. All
    // gated behind adaptive_operators so the legacy path leaves op_weights uniform.
    // An all-IC-rejected generation has best_raw == -inf. It supplies no
    // realized fitness baseline: crediting a later survivor against it would
    // create infinite operator weights and corrupt future mutation draws.
    if (cfg.adaptive_operators && have_prev_children && std::isfinite(prev_parent_best)) {
      std::array<atx::f64, 3> gain_sum{0.0, 0.0, 0.0};
      std::array<atx::usize, 3> gain_cnt{0, 0, 0};
      for (atx::usize p = 0; p < prev_child_ops.size(); ++p) {
        const atx::u8 o = prev_child_ops[p];
        if (o > 2) {
          continue; // 0xFF: crossover / immigrant / elite-clone -> uncredited
        }
        const atx::usize idx = prev_n_elites + p; // population slot of this child
        if (idx >= scored.size()) {
          continue; // defensive (population shrank) — never expected
        }
        if (is_rejected_score(scored[idx].origin)) {
          continue; // L3: never fully scored (sentinel raw) -> no realized gain to credit
        }
        gain_sum[o] += scored[idx].fitness - prev_parent_best;
        ++gain_cnt[o];
      }
      for (atx::usize o = 0; o < 3; ++o) {
        if (gain_cnt[o] > 0) {
          const atx::f64 mean_gain = gain_sum[o] / static_cast<atx::f64>(gain_cnt[o]);
          op_weights[o] = std::max(kOpWeightFloor, op_weights[o] + mean_gain);
        }
      }
    }

    // (e)-(g) reproduce into the next population (skip on the final generation —
    // the last scored set is the result).
    if (gen + 1 < cfg.generations) {
      const atx::usize n_elites_this = std::min(cfg.elites, scored.size());
      pop = reproduce(scored, cfg, gen, det_pool, res, op_weights, prev_child_ops);
      // Stash the bookkeeping the NEXT generation needs to credit these children.
      prev_n_elites = n_elites_this;
      prev_parent_best = best_raw(scored);
      have_prev_children = cfg.adaptive_operators;
    }
  }

  finalize(scored, canon, res);
  return res;
}

// ----- (1) init_population -------------------------------------------------
// Parse + analyze each seed expression into an F5-valid Genome, set its
// canon_hash, then pad up to cfg.population by cycling the valid seeds (a
// deterministic, in-grammar fill — every member is analyze-valid by
// construction). At least one seed must parse; an all-invalid seed set yields an
// empty population and an empty (but well-formed) result.
[[nodiscard]] std::vector<Genome> SearchDriver::init_population(const SearchConfig &cfg) const {
  std::vector<Genome> seeds;
  for (const std::string &src : seed_exprs_) {
    auto ast = alpha::parse_expr(src, lib_);
    if (!ast.has_value()) {
      continue;
    }
    auto info = alpha::analyze(*ast);
    if (!info.has_value()) {
      continue;
    }
    Genome g{std::move(*ast), std::move(*info), 0};
    g.canon_hash = canon_key(g);
    seeds.push_back(std::move(g));
  }
  std::vector<Genome> pop;
  pop.reserve(cfg.population);
  if (seeds.empty()) {
    return pop; // degenerate: no valid seed -> empty (well-formed) run
  }

  // S3.5 GENERATION WIRE (plan §0.5/§0.6), GATED. With seed_from_grammar OFF (the
  // boundary-pin / ScalarRaw path) gen-0 is filled EXACTLY as pre-S4 — cycle the
  // valid seeds over every slot — so the frozen digest is byte-identical. With it
  // ON, the seeds fill the first slots and any REMAINING slots are sampled from
  // the type-correct grammar (generate_genome); a generation failure (Err — a
  // sampler bug, ~never) falls back to the cyclic seed fill for that slot, so the
  // population size is always held and every member stays analyze-valid.
  //
  // S3-2 kMutateSeedAxis: a distinct seed_for axis used ONLY by mutate_seed_copies
  // so its RNG draws do NOT perturb any other axis's stream. Chosen to not collide
  // with kGenSeedAxis (0xFFFFFFFF…) or kImmigrantAxis (0xA5A5A5A5…).
  constexpr atx::u64 kMutateSeedAxis = 0x5E5E5E5E5E5E5E5EULL;

  if (!cfg.seed_from_grammar) {
    for (atx::usize i = 0; i < cfg.population; ++i) {
      const Genome &src = seeds[i % seeds.size()];
      // S3-2: tag EVERY seed-derived genome with from_seed=true so
      // protect_seed_elites can identify the seed lineage across generations.
      // Default false (protect_seed_elites off) -> this tag is never read, so
      // the default path is byte-identical (no fitness/RNG change).
      if (i == 0 || !cfg.mutate_seed_copies) {
        // Slot 0 always gets the unmodified seed original. When mutate_seed_copies
        // is OFF, all slots get identical clones (the legacy pre-S3-2 behavior).
        pop.push_back(src.clone());
        pop.back().canon_hash = src.canon_hash;
        pop.back().from_seed = true; // tag: derived from a seed expression
      } else {
        // S3-2 mutate_seed_copies=true: apply ONE seeded mutation to this clone
        // slot (i > 0) so the cycled copies are structurally distinct.
        // The seed is a pure fn of (master_seed, kMutateSeedAxis, i) — a new axis
        // that never collides with gen/idx draws (F1 preserved). No crossover:
        // only a single operator is applied, keeping each copy a seed descendant.
        // If the mutation fails (Err), fall back to the unmutated clone so the
        // population size is always held.
        Xoshiro256pp mut_rng{detail::seed_for(cfg.master_seed, kMutateSeedAxis, i)};
        atx::u8 op_used = 0xFF;
        // mutate_one needs access to catalog_/field_views_ — but init_population
        // is const. We inline the three mutation paths to avoid a const violation.
        // The operator draw uses the SAME fixed-modulus-3 convention as the legacy
        // path so kMutateSeedAxis draws stay fully isolated.
        const atx::u64 which = mut_rng.next_u64() % 3U;
        JitterCfg jc;
        jc.max_lookback = cfg.max_lookback;
        atx::core::Result<Genome> child = atx::core::Err(atx::core::ErrorCode::NotFound,
                                                         "mutate_seed_copies: not attempted");
        if (which == 0 && cfg.enable_op_swap) {
          child = op_swap(src, catalog_, mut_rng);
          if (child.has_value()) { op_used = 0; }
        } else if (which == 1) {
          child = field_swap(src, std::span<const std::string_view>{numeric_field_views_},
                             mut_rng);
          if (child.has_value()) { op_used = 1; }
        }
        if (op_used == 0xFF) {
          // Default / fallback: jitter — the most broadly applicable mutation.
          child = jitter_const(src, mut_rng, jc);
          if (child.has_value()) { op_used = 2; }
        }
        static_cast<void>(op_used); // used only for the fallback logic above
        if (child.has_value()) {
          child->canon_hash = canon_key(*child);
          child->from_seed = true; // seed descendant: inherits the tag
          pop.push_back(std::move(*child));
        } else {
          // Mutation failed (degenerate genome) — fall back to unmutated clone.
          pop.push_back(src.clone());
          pop.back().canon_hash = src.canon_hash;
          pop.back().from_seed = true;
        }
      }
    }
    return pop;
  }

  const atx::usize n_seed_slots = std::min(cfg.population, seeds.size());
  for (atx::usize i = 0; i < n_seed_slots; ++i) {
    pop.push_back(seeds[i].clone());
    pop.back().canon_hash = seeds[i].canon_hash;
    pop.back().from_seed = true; // S3-2: tag seed-derived genomes for protect_seed_elites
  }
  // Track gen-0 structures so the grammar fill maximizes DISTINCT members.
  std::unordered_set<atx::u64> seen0;
  for (const Genome &g : pop) { seen0.insert(g.canon_hash); }
  constexpr atx::u64 kGenSeedAxis = 0xFFFFFFFFFFFFFFFFULL;
  constexpr atx::usize kMaxResample = 8U; // bounded retries; deterministic
  const atx::usize dmax = std::max<atx::usize>(cfg.gen_cfg.max_depth, cfg.init_min_depth);
  // Base gen config for grammar fill: always use the driver's panel fields as the
  // numeric field source so grammar-generated expressions only reference fields the
  // Panel can actually evaluate. group_fields are also set to panel fields — Group-
  // typed ops that pick a panel field as their group arg will fail type-check inside
  // generate_genome (F64 where Group is required), so they return Err and the
  // attempt-loop continues; pick_field(group_fields, rng) requires a non-empty list
  // so we must supply one. The panel_field_views_ are string_views into the owned
  // panel_fields_ vector (driver lifetime), valid for the fill duration. Other gen_cfg
  // knobs (max_lookback, max_depth, etc.) come from cfg.
  GenConfig base_gc = cfg.gen_cfg;
  // Task 3.1: use dtype-partitioned field views so numeric ops only draw from
  // F64 fields and group ops only draw from classifier fields.
  base_gc.numeric_fields = numeric_field_views_;
  base_gc.group_fields = group_field_views_;
  for (atx::usize i = n_seed_slots; i < cfg.population; ++i) {
    Genome chosen = seeds[i % seeds.size()].clone(); // deterministic fallback
    chosen.canon_hash = seeds[i % seeds.size()].canon_hash;
    for (atx::usize attempt = 0; attempt < kMaxResample; ++attempt) {
      // Ramped depth: slot i and the attempt index both perturb the seed AND the
      // sampler depth, so distinct slots explore distinct shapes. Pure fn of
      // (master_seed, axis, i, attempt) -> F1 deterministic.
      GenConfig gc = base_gc;
      const atx::usize span = (dmax >= cfg.init_min_depth) ? (dmax - cfg.init_min_depth + 1U) : 1U;
      gc.max_depth = cfg.init_min_depth + ((i + attempt) % span);
      Xoshiro256pp rng{detail::seed_for(cfg.master_seed,
                                        kGenSeedAxis ^ static_cast<atx::u64>(attempt), i)};
      auto gen = generate_genome(gc, lib_, rng);
      if (!gen.has_value()) { continue; }
      gen->canon_hash = canon_key(*gen);
      if (seen0.insert(gen->canon_hash).second) { chosen = std::move(*gen); break; }
      // collision: keep the last valid sample as a fallback even if not distinct
      chosen = std::move(*gen);
    }
    pop.push_back(std::move(chosen));
  }
  return pop;
}

// ----- (2) evaluate_generation --------------------------------------------
// Collect the fresh (un-seen) genomes (F6), compile them to single-root Programs,
// evaluate each on its per-worker `engines[wid]` (owned by run(), reused across
// every generation — see the EVAL-PATH NOTE below) and fold its worker-invariant
// digest into the run digest (F2), then score each NEW population member via
// pool_aware_fitness (dedup-hits reuse the cached score). A fresh candidate that
// fails to compile is dropped from the digest (F5 backstop); every distinct
// structure is recorded in all_scored + the CanonSet.
[[nodiscard]] std::vector<Scored>
SearchDriver::evaluate_generation(const std::vector<Genome> &pop, const SearchConfig &cfg,
                                  atx::usize gen, const combine::AlphaStore &pool, CanonSet &canon,
                                  std::unordered_map<atx::u64, CachedScore> &fitness_cache,
                                  parallel::DetPool &det_pool,
                                  std::vector<std::unique_ptr<alpha::Engine>> &engines,
                                  SearchResult &res, const IcScreenCache *ic_cache,
                                  std::span<IcScreenScratch> ic_scratch,
                                  std::span<ObjectiveIcScratch> residual_scratch) {
  // -----------------------------------------------------------------------
  // Phase 1 (serial): dedup + plan
  //
  // `fresh` = all population members not yet in canon, sorted by canonical-id
  // order so the digest fold is order-stable and replayable (F1/F2). Intra-
  // generation duplicates are kept: the current serial path folded them too, so
  // their digest contribution is preserved.
  //
  // `to_score` = pointers to the DISTINCT-new genomes in population first-
  // occurrence order (the order they are inserted into canon/fitness_cache and
  // pushed to all_scored).
  // -----------------------------------------------------------------------
  std::vector<const Genome *> fresh;
  for (const Genome &g : pop) {
    if (!canon.contains(g.canon_hash)) {
      fresh.push_back(&g);
    }
  }
  std::sort(fresh.begin(), fresh.end(),
            [](const Genome *a, const Genome *b) { return detail::canon_less(*a, *b); });

  std::vector<const Genome *> to_score;
  {
    std::unordered_set<atx::u64> seen_this_gen;
    for (const Genome &g : pop) {
      if (!canon.contains(g.canon_hash) && seen_this_gen.insert(g.canon_hash).second) {
        to_score.push_back(&g);
      }
    }
  }

  // -----------------------------------------------------------------------
  // Phase 2+3 (PARALLEL): single merged eval pass — digest + fitness share
  // ONE evaluate per genome.
  //
  // Each fresh[k] is compiled and evaluated ONCE. The digest is folded as
  // before (serial, canonical order). When fresh[k] is a to_score
  // representative (exact pointer match via score_j_of_ptr), fitness is
  // computed from the SAME SignalSet produced by that evaluate — eliminating
  // the redundant second evaluate that Phase 3 previously performed.
  //
  // SAFETY: `digest_slot`, `compiled`, and `score_slot` are pre-sized before
  // the parallel region; shard k writes ONLY digest_slot[k] and compiled[k]
  // (disjoint single-writer slots). When fresh[k] matches to_score[j], shard
  // k ALSO writes score_slot[j] — each j has EXACTLY ONE matching k (a pop
  // object's address appears in fresh at most once, and to_score contains only
  // one entry per canon_hash), so score_slot[j] is also a disjoint
  // single-writer slot. `panel_` is shared CONST (never mutated). `fresh[k]`
  // is a const pointer to a const Genome (read-only). `alpha::compile` and
  // `alpha::Engine::evaluate` are reentrant (no static/thread_local mutable
  // state). Worker `wid` touches ONLY `engines[wid]` (disjoint single-owner):
  // no cross-worker Engine access. No shared-mutable write occurs inside the
  // parallel region. The serial fold below reads digest_slot + compiled in
  // fixed canonical index order, so the accumulated digest is byte-identical
  // across all worker counts — worker scheduling CANNOT affect the fold order
  // or any slot value.
  //
  // EVAL-PATH NOTE: each worker reuses its own `engines[wid]` Engine across
  // all programs it processes (Engine::evaluate is IDEMPOTENT — output depends
  // ONLY on (program, panel_), never on prior engine state: field_remap_
  // reassigned per call, output buffers re-assigned per call, SlotPool slots
  // written-before-read, recurrence state_ seeded at t==0 on every call). The
  // SlotPool inside each Engine therefore grows monotonically to the peak
  // allocation for that worker and is reused thereafter. The digest is worker-
  // count-invariant BY CONSTRUCTION: slot k's value depends only on
  // (fresh[k], panel_), and the fold is serial in fixed canonical order.
  // No cross-genome substitution: fresh[k] digests from its OWN eval; fitness
  // for to_score[j] uses that SAME genome's SignalSet.
  // -----------------------------------------------------------------------
  atx::usize n_fresh = fresh.size();
  std::vector<atx::u64> digest_slot(n_fresh, atx::u64{0});
  std::vector<std::uint8_t> compiled(n_fresh, std::uint8_t{0});
  const bool execution_on = cfg.fitness.execution.rule == ExecutionObjectiveRule::DelayedSurfaceV2;
  const bool residual_on = cfg.fitness.objective_rule == FitnessObjectiveRule::ResidualHacIcV2;
  std::vector<std::string> execution_errors(execution_on ? n_fresh : 0);
  std::vector<std::string> residual_errors(residual_on ? n_fresh : 0);
  // v8 H-3 signal-fitness path: one functor Err slot per fresh candidate (canonical slot k).
  const bool signal_on = cfg.signal_fitness != nullptr;
  std::vector<std::string> signal_errors(signal_on ? n_fresh : 0);

  // One stateless Scheduler for the merged parallel region's Tier 1 LPT
  // dispatch order. Default-constructed: the single-node fallback topology, NO
  // OS query; `dispatch_order` reads ONLY its cost-hint argument (a PURE
  // function — no clock, no thread id, no topology), so it cannot touch a
  // result bit.
  const parallel::Scheduler lpt{};

  // -----------------------------------------------------------------------
  // Tier 1 LPT (longest-processing-time-first) dispatch order for the merged
  // Phase 2+3 pass.
  //
  // `order_fresh` is a PURE permutation of [0, n_fresh) sorted by DESCENDING
  // genome size — the AST node count is a finite, NaN-free monotone proxy for
  // compile+eval cost. Dispatching the heaviest candidates first overlaps their
  // long tail with the many short ones, shrinking the generation's makespan.
  //
  // DETERMINISM: the permutation only remaps WHICH claim-position p runs WHICH
  // canonical slot k (k == order_fresh[p]). Slot k is still written by exactly
  // one shard, and the serial fold below reads digest_slot in canonical k-order,
  // so the accumulated digest is byte-identical to ANY dispatch order (the
  // Scheduler §4.4 "no bit contact" proof + the DetPool single-writer contract).
  // A uniform-cost population degenerates to the ascending-id identity order, i.e.
  // exactly the prior Tier 0 dispatch.
  // -----------------------------------------------------------------------
  std::vector<atx::f64> cost_fresh(n_fresh);
  for (atx::usize k = 0; k < n_fresh; ++k) {
    cost_fresh[k] = static_cast<atx::f64>(fresh[k]->ast.nodes().size());
  }
  std::vector<parallel::ShardId> order_fresh = lpt.dispatch_order(cost_fresh);

  // `engines` (one reusable Engine per worker, bound to panel_) is owned by run()
  // and passed in: it is built ONCE per run and reused across every generation
  // (Tier 4), so its SlotPool grows once to peak instead of being reallocated each
  // generation. engines.size() == det_pool.n_workers() (run() sizes it from the
  // same pool). Worker `wid` touches ONLY engines[wid] (disjoint single-owner).
  // Reuse is byte-identical because Engine::evaluate is idempotent (output depends
  // only on (program, panel_)) and panel_ is constant across the run -> slot k's
  // value is independent of which worker ran it or what it evaluated before, so
  // worker-count + Tier 1 LPT-order invariance hold.

  // Pointer -> to_score index map (serial, built after to_score is finalized).
  // Each to_score[j] is the SAME pop object as exactly one fresh[k] (a pop
  // object's address is unique -> appears in `fresh` at most once). This lets
  // the single merged pass score a representative from the SAME SignalSet it
  // evaluated for the digest, eliminating Phase 3's redundant evaluate.
  // score_slot[j] therefore has exactly one writer (the unique matching shard)
  // -> the disjoint single-writer contract holds.
  std::unordered_map<const Genome *, atx::usize> score_j_of_ptr;
  score_j_of_ptr.reserve(to_score.size());
  for (atx::usize j = 0; j < to_score.size(); ++j) {
    score_j_of_ptr.emplace(to_score[j], j);
  }

  const atx::usize n_to_score = to_score.size();
  std::vector<CachedScore> score_slot(n_to_score);
  std::vector<ResidualCandidateScore> residual_slot(residual_on ? n_to_score : 0);
  if (residual_on)
    std::fill(score_slot.begin(), score_slot.end(), residual_unavailable_score());
  // A representative the functor never scores (compile/VM failure) stays an unscored trial.
  if (signal_on)
    std::fill(score_slot.begin(), score_slot.end(), unscored_score());
  // Disjoint per-representative decisions: 0 untested, 1 keep, 2 reject, 3 error.
  // Only the serial merge updates counters and persistent rejection identities.
  std::vector<atx::u8> ic_status(ic_cache != nullptr ? n_to_score : 0U, atx::u8{0});
  std::vector<atx::u64> ic_prepass_digest;

  // R4 — per-generation deflation N (Piece 1, determinism-safe).
  //
  // canon.size() is captured HERE (serial, before the parallel_for barrier) so
  // every worker in this generation sees the SAME N — it is the count of distinct
  // candidates scored in ALL PRIOR generations (the canon.insert seam at
  // search_driver.cpp Phase 4 runs serially, after the parallel barrier, so no
  // per-candidate atomic inside the parallel region is needed or allowed).
  //
  // gen 0: canon.size()==0 -> N=1 (same as default trial_count=1), so gen-0 DSR
  // values equal the off-path; divergence is driven by the objective/raw seam
  // below, not by N at gen 0.
  //
  // When deflate_selection is OFF: gen_fit == cfg.fitness exactly (trial_count
  // stays at its cfg.fitness default) -> zero new computation -> byte-identical.
  //
  // S5-2: cfg.prior_trial_count folds in the CROSS-RUN cumulative N from a
  // persistent library (0 for a fresh library / the non-library mine() path, so
  // this collapses to the exact pre-S5-2 expression). Both terms were already
  // captured/known before this point (canon.size() here; prior_trial_count by the
  // Factory caller before driver.run() even started) -> still a single serial
  // scalar, so the seq==parallel invariant is unaffected.
  FitnessCfg gen_fit = cfg.fitness;
  if (cfg.deflate_selection) {
    gen_fit.trial_count = cfg.prior_trial_count + std::max<atx::usize>(1U, canon.size());
  }
  // S4-3: thread the SearchConfig-level objective gates into the per-generation
  // FitnessCfg -- fitness_core/finish_report (which alone have strm/panel in scope)
  // read the FitnessCfg mirror, never SearchConfig directly. Both default false on
  // BOTH structs, so this copy is a no-op unless the caller set the SearchConfig
  // flag (byte-identical off-path).
  gen_fit.capacity_objective = cfg.capacity_objective;
  gen_fit.turnover_objective = cfg.turnover_objective;

  // Drops every fresh candidate whose canonical hash is in `dropped` (no full pass, no digest
  // fold) and re-plans the LPT dispatch over the kept ones, in canonical order.
  const auto drop_fresh = [&](const std::unordered_set<atx::u64> &dropped) {
    std::vector<const Genome *> kept;
    kept.reserve(fresh.size());
    std::vector<atx::f64> kept_cost;
    kept_cost.reserve(fresh.size());
    for (const Genome *g : fresh) {
      if (dropped.find(g->canon_hash) == dropped.end()) {
        kept.push_back(g);
        kept_cost.push_back(static_cast<atx::f64>(g->ast.nodes().size()));
      }
    }
    fresh = std::move(kept);
    n_fresh = fresh.size();
    digest_slot.assign(n_fresh, atx::u64{0});
    compiled.assign(n_fresh, std::uint8_t{0});
    order_fresh = lpt.dispatch_order(kept_cost);
  };

  // v8 review MINE-10: the program slot bound (signal-fitness path only; 0, the default, skips
  // this block). A representative whose program needs more VM slots than the bound is refused
  // before the race and the full pass: it keeps the unscored sentinel in score_slot, so the
  // merge below still files it as a trial.
  std::unordered_set<atx::u64> slot_refused;
  if (cfg.max_program_slots != 0U) {
    std::vector<atx::u8> over(n_to_score, atx::u8{0});
    det_pool.parallel_for(n_to_score, [&](atx::usize j, atx::usize) {
      const auto prog = alpha::compile(to_score[j]->ast, to_score[j]->analysis);
      over[j] = (prog.has_value() && prog->num_slots > cfg.max_program_slots) ? atx::u8{1}
                                                                               : atx::u8{0};
    });
    for (atx::usize j = 0; j < n_to_score; ++j) {
      if (over[j] != atx::u8{0}) {
        slot_refused.insert(to_score[j]->canon_hash);
        res.slot_refused_hashes.push_back(to_score[j]->canon_hash);
      }
    }
    if (!slot_refused.empty()) {
      drop_fresh(slot_refused);
    }
  }

  // IC must precede even the low-rung backtests. With both flags on, evaluate
  // distinct representatives once for the IC decision, retain only their small
  // decisions/digests, then race survivors. A surviving candidate may evaluate
  // the VM again in the full pass; retaining population*panel SignalSets would
  // defeat the bounded-memory goal. The IC-only path below evaluates once.
  if (ic_cache != nullptr && cfg.fidelity.enabled) {
    ic_prepass_digest.resize(n_to_score, atx::u64{0});
    std::vector<atx::u8> vm_evaluated(n_to_score, atx::u8{0});
    det_pool.parallel_for(n_to_score, [&](atx::usize j, atx::usize wid) {
      auto prog = alpha::compile(to_score[j]->ast, to_score[j]->analysis);
      if (!prog) {
        return;
      }
      vm_evaluated[j] = atx::u8{1};
      auto ss = engines[wid]->evaluate(*prog);
      if (!ss || ss->alphas.empty()) {
        return;
      }
      ic_prepass_digest[j] = parallel::signal_set_digest(*ss);
      auto screened = screen_ic(ss->alphas.front().values, *ic_cache, ic_scratch[wid]);
      ic_status[j] = !screened ? atx::u8{3}
                              : screened->reject ? atx::u8{2} : atx::u8{1};
      if (screened && screened->reject) {
        score_slot[j] = ic_rejected_score();
      }
    });
    for (const atx::u8 evaluated : vm_evaluated) {
      res.ic_prepass_vm_evaluations += evaluated;
    }
  }

  // L3 multi-fidelity race (opt-in). The distinct fresh candidates are raced on
  // strided sub-panels; the rejected ones leave `fresh` (no full eval, no digest
  // fold) and get the worst-case sentinel score (rejected_score(): raw -inf,
  // ScoreOrigin::FidelityRejected) in score_slot, but stay in `to_score` so the
  // Phase 4 merge still inserts them into canon (a rejected candidate IS a trial)
  // and all_scored. The sentinel ranks them last in ScalarRaw, keeps them off the
  // NSGA-II fronts of evaluated candidates and out of admitted_candidates (a
  // default all-zero score would outrank negative-raw candidates and sit on
  // front 0 whenever a negative objective such as parsimony is live).
  // Off (the default) -> `rejected` is empty -> no-op.
  if (cfg.fidelity.enabled) {
    std::vector<const Genome *> ic_survivors;
    const std::vector<const Genome *> *race_candidates = &to_score;
    if (!ic_prepass_digest.empty()) {
      ic_survivors.reserve(to_score.size());
      for (atx::usize j = 0; j < n_to_score; ++j) {
        if (ic_status[j] != atx::u8{2}) {
          ic_survivors.push_back(to_score[j]);
        }
      }
      race_candidates = &ic_survivors;
    }
    std::vector<const Genome *> slot_survivors; // v8 MINE-10: refused programs never race
    if (!slot_refused.empty()) {
      slot_survivors.reserve(race_candidates->size());
      for (const Genome *g : *race_candidates) {
        if (slot_refused.find(g->canon_hash) == slot_refused.end()) {
          slot_survivors.push_back(g);
        }
      }
      race_candidates = &slot_survivors;
    }
    // Lane MINE-MEM (signal-fitness path only): the race and the full pass never hold each
    // other's buffers. The full-pass engines are released for the race (their grown slot pools
    // and mask copies) and rebuilt for the full pass by make_full_engine, as run() builds them;
    // the strided rung panels are released after the race and rebuilt by the next one
    // (strided_panel is a pure copy of panel_). Engine::evaluate depends only on (program,
    // panel_, mask) -- the EVAL-PATH NOTE above -- so a rebuilt engine computes the bits the
    // reused one would, and no ordering changes: every release and rebuild is serial, between
    // the parallel regions.
    if (signal_on) {
      for (std::unique_ptr<alpha::Engine> &engine : engines) {
        engine.reset();
      }
    }
    const std::vector<atx::u64> rejected =
        fidelity_reject(*race_candidates, cfg, gen_fit, det_pool, res);
    if (res.signal_path_invalid) {
      return {}; // review MINE-12: a rung could not be masked; nothing from this generation
    }
    if (signal_on) {
      rung_panels_.clear();
      rung_keys_.clear();
      for (std::unique_ptr<alpha::Engine> &engine : engines) {
        auto rebuilt = make_full_engine(panel_, cfg.cross_section_mask);
        if (!rebuilt) {
          res.signal_path_invalid = true; // unreachable: run() built the same engines
          res.signal_path_error = rebuilt.error().to_string();
          return {};
        }
        engine = std::move(*rebuilt);
      }
    }
    // v8 H-3: every racing rejection's identity (sorted at the merge below).
    res.fidelity_rejected_hashes.insert(res.fidelity_rejected_hashes.end(), rejected.begin(),
                                        rejected.end());
    if (!rejected.empty()) {
      const std::unordered_set<atx::u64> rej(rejected.begin(), rejected.end());
      for (atx::usize j = 0; j < n_to_score; ++j) {
        if (rej.find(to_score[j]->canon_hash) != rej.end()) {
          score_slot[j] = rejected_score();
        }
      }
      drop_fresh(rej);
    }
  }

  // L3 output-fingerprint dedup (opt-in): per-representative fingerprint slots,
  // filled inside the parallel region (single writer per j). `fp_index_` is only
  // READ inside the region (it holds prior generations' fingerprints) and is
  // updated serially after the barrier -> worker-count invariant.
  // Gated on the Rank transform: the fingerprint is rank-quantized (monotone-
  // invariant), so under ZScore/Raw weights x and f(x) trade different books and
  // must not share a score.
  const bool fp_on = cfg.output_dedup && policy_.transform == Transform::Rank;
  std::vector<atx::u64> fp_slot(fp_on ? n_to_score : 0U, atx::u64{0});
  std::vector<std::uint8_t> fp_valid(fp_on ? n_to_score : 0U, std::uint8_t{0});
  std::vector<atx::u64> fp_owner(fp_on ? n_to_score : 0U, atx::u64{0});
  std::vector<std::uint8_t> fp_hit(fp_on ? n_to_score : 0U, std::uint8_t{0});
  const std::vector<atx::usize> fp_rows =
      fp_on ? probe_rows(panel_.dates(), cfg.fingerprint_rows) : std::vector<atx::usize>{};

  // S3-1 PERF: create a shared CpcvCache for this generation.  All workers in the
  // parallel_for share it (its internal mutex serialises the rare cold insert).
  // After the first genome is scored, every subsequent call for the same
  // (n_periods, cpcv) is O(1) and allocates no spans or folds.  Lifetime: local to
  // evaluate_generation, safely outlives the parallel_for barrier below.
  CpcvCache cpcv_cache{};

  det_pool.parallel_for(n_fresh, [&](atx::usize p, atx::usize wid) {
    const atx::usize k = order_fresh[p]; // LPT remap -> canonical slot k
    const auto it = score_j_of_ptr.find(fresh[k]);
    if (it != score_j_of_ptr.end() && !ic_prepass_digest.empty() &&
        ic_status[it->second] == atx::u8{2}) {
      // The same representative was already evaluated in the prepass; retain
      // its own signal digest, but do not evaluate/backtest the reject again.
      digest_slot[k] = ic_prepass_digest[it->second];
      compiled[k] = std::uint8_t{1};
      return;
    }
    auto prog = alpha::compile(fresh[k]->ast, fresh[k]->analysis);
    if (!prog.has_value()) {
      return; // compiled[k] stays 0 (F5 backstop): no digest, no fitness for this k
    }
    auto ss = engines[wid]->evaluate(*prog); // reused per-worker engine (idempotent)
    digest_slot[k] = ss.has_value() ? parallel::signal_set_digest(*ss) : atx::u64{0};
    compiled[k] = std::uint8_t{1};
    // Representative? Score it from the SAME SignalSet (no second evaluate). On
    // eval-failure (!ss) leave score_slot[j] default — matches the prior code where
    // Phase 3's own eval would fail to a default score.
    if (it != score_j_of_ptr.end() && ss.has_value()) {
      const atx::usize j = it->second;
      if (signal_on) {
        // v8 H-3: the functor replaces pool_aware_fitness on the same SignalSet.
        score_signal(*to_score[j], *ss, cfg, wid, score_slot[j], signal_errors[k]);
        return;
      }
      if (ic_cache != nullptr && ic_status[j] == atx::u8{0} && !ss->alphas.empty()) {
        auto screened = screen_ic(ss->alphas.front().values, *ic_cache, ic_scratch[wid]);
        ic_status[j] = !screened ? atx::u8{3}
                                : screened->reject ? atx::u8{2} : atx::u8{1};
        if (screened && screened->reject) {
          score_slot[j] = ic_rejected_score();
          return; // no extract_streams, CPCV, transaction costs or weak-panel fit
        }
      }
      if (fp_on && !ss->alphas.empty()) {
        thread_local FingerprintScratch fp_scratch;
        fp_slot[j] = signal_fingerprint(ss->alphas.front().values, panel_.instruments(),
                                        fp_rows, fp_scratch, cfg.fingerprint_quant);
        fp_valid[j] = std::uint8_t{1};
        if (const atx::u64 *owner = fp_index_.find(fp_slot[j]); owner != nullptr) {
          fp_owner[j] = *owner; // reuse the prior-generation owner's score (serial merge)
          fp_hit[j] = std::uint8_t{1};
          return;
        }
      }
      auto worker_fit = gen_fit;
      if (residual_on) worker_fit.residual_scratch = &residual_scratch[wid];
      auto rep = pool_aware_fitness(*to_score[j], pool, panel_, policy_, sim_, worker_fit,
                                   /*weak_panel=*/weak_panel_, /*engine=*/engines[wid].get(),
                                   /*signals=*/&*ss, /*cpcv_cache=*/&cpcv_cache);
      if (residual_on) {
        if (!rep) { residual_errors[k] = rep.error().to_string(); return; }
        residual_slot[j].status = rep->residual_available ? ResidualScoreStatus::Available
                                                         : ResidualScoreStatus::InsufficientEvidence;
        residual_slot[j].score = rep->raw;
        residual_slot[j].diagnostics = std::move(rep->residual_ic);
        if (!rep->residual_available) return; // keep explicit sentinel, never finite zero
        score_slot[j].origin = ScoreOrigin::Full;
      }
      if (!rep && execution_on) {
        execution_errors[k] = rep.error().to_string();
        return; // unpriceable execution must never become a successful zero score
      }
      if (rep.has_value()) {
        score_slot[j].raw = rep->raw;
        score_slot[j].objectives = rep->objectives; // S4.1: cache the objectives
        score_slot[j].n_objectives = rep->n_objectives;
        score_slot[j].descriptor = std::move(rep->descriptor); // S4.2: canon-cache phenotype
      }
      // On Err: score_slot[j] stays default-constructed (raw=0, empty descriptor),
      // matching the prior code's behaviour for a fitness-error candidate.
      // S-quality: parsimony objective (slot 5). Set from the genome's node count
      // for the representative regardless of fitness success (node count is a pure
      // structural value, canon-cacheable). Bump n_objectives to cover slot 5; the
      // intervening slots (3 novelty, 4 cost) stay at their inert defaults until
      // their own passes fill them. MultiObjective-only effect (ScalarRaw ignores
      // objectives). Errored genomes get a node-count value too, but cannot be
      // ADMITTED (factory drops un-evaluable candidates), so no perverse incentive.
      if (cfg.enable_parsimony) {
        set_parsimony(score_slot[j], *to_score[j]);
      }
      // R4 — deflated-Sharpe selection pressure (Pieces 2 + 3), opt-in.
      //
      // Piece 2 (NSGA objective): add dsr as objectives[kObjDeflation] so
      // MultiObjective ranking rewards candidates with higher deflated edge.
      // n_objectives is bumped to cover slot 6 so the ObjMatrix includes it.
      // Piece 3 (raw haircut): multiply raw by dsr so the elitism/ScalarRaw
      // signal also reflects deflation risk. rep->dsr in [0,1] (PSR/probability)
      // so this shrinks raw toward 0 as the deflation bar bites.
      //
      // OFF-PATH: when deflate_selection is false this block is skipped — raw is
      // unchanged, n_objectives is unchanged, objectives[6] stays at its zero
      // default, and the NSGA ObjMatrix never sees slot 6. Byte-identical.
      //
      // GUARD: only write when fitness succeeded (rep.has_value() checked above;
      // this block is inside the `if (rep.has_value())` scope via the outer seam
      // structure). The write is to score_slot[j], a disjoint single-writer slot.
      if (cfg.deflate_selection && rep.has_value()) {
        score_slot[j].objectives[kObjDeflation] = rep->dsr; // maximization: higher deflated edge ranks better
        score_slot[j].n_objectives = static_cast<atx::u8>(
            std::max<atx::usize>(score_slot[j].n_objectives, kObjDeflation + 1U));
        score_slot[j].raw = rep->raw * rep->dsr; // deflation haircut for elitism / ScalarRaw
      }
    }
  });

  for (const auto& error : execution_errors) {
    if (!error.empty()) {
      res.execution_invalid = true; res.execution_error = error;
      return {}; // deterministic first canonical error; no admission from this generation
    }
  }
  for (const auto &error : residual_errors) {
    if (!error.empty()) {
      res.residual_invalid = true; res.residual_error = error;
      return {}; // hard config/binding/scratch failure, no partial admission
    }
  }
  for (const auto &error : signal_errors) {
    if (!error.empty()) {
      res.signal_path_invalid = true; res.signal_path_error = error;
      return {}; // first canonical functor Err; no admission from this generation
    }
  }

  for (const atx::u8 status : ic_status) {
    if (status != atx::u8{0}) {
      ++res.ic_screen_evaluations;
      res.ic_screen_unavailable += status == atx::u8{3} ? 1U : 0U;
    }
  }

  // Serial fold in canonical-id index order (0..n_fresh): skip non-compilable
  // genomes (compiled[k]==0), exactly matching the prior sequential loop's
  // `continue` on compile failure. This reproduces the byte-identical digest.
  // Each genome's digest comes from its OWN eval (no cross-genome substitution);
  // the serial fold order is the same as before — golden digest preserved.
  for (atx::usize k = 0; k < n_fresh; ++k) {
    if (compiled[k] != std::uint8_t{0}) {
      res.digest = static_cast<atx::u64>(
          atx::core::hash_combine(static_cast<std::size_t>(res.digest), gen, digest_slot[k]));
    }
  }

  // -----------------------------------------------------------------------
  // Phase 4 (serial): merge scores into canon/fitness_cache/all_scored, then
  // assemble the Scored output vector in population order.
  //
  // score_slot[j] was written inside the merged Phase 2+3 pass (above) by the
  // unique shard that evaluated the corresponding fresh[k] representative;
  // fitness was computed from that same evaluate's SignalSet. Merge is in
  // to_score order (== pop first-occurrence order), identical to the prior
  // sequential implementation's per-pop-member insertion sequence.
  // -----------------------------------------------------------------------
  if (fp_on) {
    // Serial, to_score order: resolve fingerprint hits from the (prior-generation)
    // owner's cached score, then publish this generation's new fingerprints with
    // first-occurrence ownership.
    for (atx::usize j = 0; j < n_to_score; ++j) {
      if (fp_hit[j] != std::uint8_t{0}) {
        const auto it = fitness_cache.find(fp_owner[j]);
        if (it != fitness_cache.end()) {
          score_slot[j] = it->second;
          score_slot[j].origin = ScoreOrigin::FingerprintBorrowed; // approximate
          // Structural objective is per-genome: recompute parsimony from THIS
          // genome's node count instead of inheriting the owner's.
          if (cfg.enable_parsimony) {
            set_parsimony(score_slot[j], *to_score[j]);
          }
        }
        ++res.fingerprint_hits;
      } else if (fp_valid[j] != std::uint8_t{0}) {
        static_cast<void>(fp_index_.insert(fp_slot[j], to_score[j]->canon_hash));
      }
    }
  }
  for (atx::usize j = 0; j < n_to_score; ++j) {
    const atx::u64 hash = to_score[j]->canon_hash;
    if (residual_on) {
      residual_slot[j].canon_hash = hash;
      res.residual_scores.push_back(std::move(residual_slot[j]));
      if (score_slot[j].origin == ScoreOrigin::ResidualUnavailable)
        res.residual_unavailable_hashes.push_back(hash);
    }
    if (score_slot[j].origin == ScoreOrigin::IcRejected) {
      res.ic_rejected_hashes.push_back(hash);
    }
    if (score_slot[j].origin == ScoreOrigin::Unscored) {
      res.unscored_hashes.push_back(hash);
    }
    canon.insert(hash);
    fitness_cache.emplace(hash, std::move(score_slot[j]));
    res.all_scored.push_back(to_score[j]->clone());
    res.all_scored.back().canon_hash = hash;
  }
  std::sort(res.ic_rejected_hashes.begin(), res.ic_rejected_hashes.end());
  std::sort(res.residual_unavailable_hashes.begin(), res.residual_unavailable_hashes.end());
  res.ic_rejected_hashes.erase(
      std::unique(res.ic_rejected_hashes.begin(), res.ic_rejected_hashes.end()),
      res.ic_rejected_hashes.end());
  // v8 H-3 identity lists (empty on the legacy path except the racing rejections).
  std::sort(res.unscored_hashes.begin(), res.unscored_hashes.end());
  std::sort(res.slot_refused_hashes.begin(), res.slot_refused_hashes.end());
  std::sort(res.fidelity_rejected_hashes.begin(), res.fidelity_rejected_hashes.end());
  res.fidelity_rejected_hashes.erase(
      std::unique(res.fidelity_rejected_hashes.begin(), res.fidelity_rejected_hashes.end()),
      res.fidelity_rejected_hashes.end());

  // Assemble output in population order (every hash is now present in
  // fitness_cache after the merge above; cached hits reuse the stored score).
  std::vector<Scored> out;
  out.reserve(pop.size());
  for (const Genome &g : pop) {
    CachedScore cs{};
    const auto it = fitness_cache.find(g.canon_hash);
    if (it != fitness_cache.end()) {
      cs = it->second; // reuse cached score (raw + objectives + descriptor), F6 dedup
    }
    Scored s{g.clone(), cs.raw, cs.raw};
    s.objectives = cs.objectives;
    s.n_objectives = cs.n_objectives;
    s.descriptor = std::move(cs.descriptor); // S4.2: phenotype for the novelty pass
    s.origin = cs.origin;                    // L3: Full / FingerprintBorrowed / FidelityRejected
    s.genome.canon_hash = g.canon_hash;
    out.push_back(std::move(s));
  }
  return out;
}

// ----- (3b) behavioral_novelty_pass (S4.2) ---------------------------------

// The single S4.2 activation gate (referenced by every S4.2 seam): the behavioral
// objective is live ONLY in MultiObjective mode with enable_behavioral_novelty set.
// ScalarRaw, or enable_behavioral_novelty==false -> off -> n_objectives stays 3 -> boundary pin holds.
[[nodiscard]] bool SearchDriver::behavioral_active(const SearchConfig &cfg) noexcept {
  return cfg.objective_mode == ObjectiveMode::MultiObjective && cfg.enable_behavioral_novelty;
}

// Write each genome's population-relative behavioral novelty into objectives[3]
// and bump n_objectives to 4 — phenotypic diversity, DISTINCT from the marginal-
// corr `diversify` objective (objectives[1]). For genome i: novelty = mean
// behavioral_distance to the k nearest descriptors in (population minus self) ∪
// archive. `nbr` is the reused population-span scratch (sized once per generation;
// no inner-loop alloc). Deterministic, RNG-free. No-op when inactive (the boundary
// pin / ScalarRaw path is byte-untouched).
void SearchDriver::behavioral_novelty_pass(std::vector<Scored> &scored,
                                           const BehavioralArchive &archive,
                                           const SearchConfig &cfg,
                                           std::vector<std::span<const atx::f64>> &nbr) const {
  if (!behavioral_active(cfg)) {
    return; // gate closed: objectives[3] untouched, n_objectives stays at 3
  }

  // S3-3: opt-in viable-only novelty floor (min_viable_raw > 0).
  //
  // WHY: a junk genome (raw ≈ 0, near-zero PnL) produces a near-zero descriptor
  // whose pairwise_complete_corr against ANY viable peer is ≈ 0 (zero-variance
  // leg -> degenerate corr 0 -> behavioral_distance 1.0). BehavioralArchive::novelty
  // then scores junk as "maximally novel", promoting it in NSGA-II objective space
  // and at the same time pulling viable genomes' distances downward (junk is always
  // a distance-1 neighbor, inflating the k-nearest mean for viable peers that happen
  // to rank junk among their k nearest). Zeroing the descriptor makes the math
  // transparent: junk's submitted descriptor is all-zeros (zero-variance, distance=1
  // against all), so the archive learns nothing from it, and viable descriptors
  // no longer compete against an inert distance-1 junk neighbor.
  //
  // ZEROING CONVENTION: we reproduce the SAME degenerate-descriptor that a
  // zero-variance genome already produces (behavior.hpp §166-170): a non-empty vector
  // of all zeros, length == scored[i].descriptor.size(). A zero-filled descriptor
  // has zero variance -> pairwise_complete_corr returns 0 -> distance = 1.0 against
  // all peers. This is distinct from an empty descriptor (which behavioral_novelty_pass
  // skips entirely); we intentionally keep the genome IN the loop so it receives a
  // novelty score of 1.0 (maximally novel against itself) without contaminating peers.
  //
  // DEFAULT PATH (min_viable_raw == 0.0): the condition `raw < 0.0` is never true
  // for non-negative fitness values, so the zeroing branch is dead -> byte-identical
  // to the pre-S3-3 behavior. The three golden-digest tests confirm this.
  const bool apply_viable_floor = (cfg.min_viable_raw > 0.0);

  // Zero-descriptor scratch for junk genomes. Sized lazily to the descriptor
  // length of the first non-empty genome we encounter (all descriptors for one
  // run share the same OOS-window length, so one allocation suffices). Reused
  // across the inner loop. Empty on the default path (apply_viable_floor==false):
  // the lambda below returns the real descriptor span and never touches this.
  std::vector<atx::f64> zero_desc_buf;

  // Returns the descriptor span to submit for genome j: the real descriptor if j
  // is viable (or the floor is off), a zero-filled span of the same length otherwise.
  // WHY a lambda: keeps the hottest path (floor off) as a single branch at function
  // entry without duplicating the inner loop body.
  auto desc_for = [&](atx::usize j) -> std::span<const atx::f64> {
    const std::vector<atx::f64> &d = scored[j].descriptor;
    if (apply_viable_floor && scored[j].fitness < cfg.min_viable_raw) {
      // Junk genome: submit a zero-variance descriptor so behavioral_distance
      // to ALL peers is 1.0 (distance == 1 - |corr(zeros, x)| == 1 - 0 == 1).
      // Lazy-size the scratch buffer to match the descriptor length.
      if (zero_desc_buf.size() != d.size()) {
        zero_desc_buf.assign(d.size(), 0.0);
      }
      return std::span<const atx::f64>{zero_desc_buf};
    }
    return std::span<const atx::f64>{d};
  };

  const atx::usize n = scored.size();
  nbr.reserve(n); // size the scratch once; cleared + refilled per genome (no realloc)
  for (atx::usize i = 0; i < n; ++i) {
    // Skip genomes whose fitness errored (empty descriptor): they have no behavioral
    // information and cannot be compared. Leave objectives[3] at its default (0).
    if (scored[i].descriptor.empty()) {
      continue;
    }
    // Population neighbourhood = every OTHER genome's descriptor (exclude self so a
    // genome's own 0-distance never collapses its novelty). Reuse `nbr`'s capacity.
    // Also skip neighbours with empty descriptors (fitness errors) — they lack a
    // meaningful behavioral profile and pairwise_complete_corr requires equal-length
    // spans (correlation.hpp:78).
    // S3-3: use desc_for(j) to substitute the zero descriptor for junk neighbors
    // when the viable floor is active. On the default path desc_for is a no-op alias.
    nbr.clear();
    for (atx::usize j = 0; j < n; ++j) {
      if (j == i || scored[j].descriptor.empty()) {
        continue;
      }
      nbr.push_back(desc_for(j));
    }
    // S3-3: use desc_for(i) for genome i's own submission. When i is below the floor,
    // a zero descriptor makes its novelty score 1.0 (max-novel vs all peers) without
    // contaminating any peer's neighborhood with a spuriously close junk entry.
    const atx::f64 nov = archive.novelty(desc_for(i),
                                         std::span<const std::span<const atx::f64>>{nbr},
                                         cfg.behavior_k, cfg.behavior_metric);
    scored[i].objectives[3] = nov; // the 4th NSGA-II objective (maximization)
    // Fixed-slot bookkeeping (S4.3): novelty owns slot 3; cost owns slot 4. Bump
    // n_objectives to COVER slot 3 without CLOBBERING a higher active slot — when
    // the cost objective is on, finish_report already set n_objectives to 5, and
    // max() preserves it so assign_pareto_ranks keeps reading all 5 columns. With
    // cost off this is exactly 4 (the pre-S4.3 behavior).
    scored[i].n_objectives = static_cast<atx::u8>(std::max<atx::usize>(scored[i].n_objectives, 4U));
  }
}

// Insert this generation's Pareto FRONT (rank-0 elites) into the behavioral archive
// in CANONICAL-ID order, evicting oldest past C. After assign_pareto_ranks (reads
// .rank), before reproduction. No-op when the objective is inactive (archive stays
// empty). The canonical-id insert order makes the archive contents + FIFO eviction
// byte-deterministic and worker-count-invariant.
void SearchDriver::update_archive(const std::vector<Scored> &scored,
                                  const std::vector<atx::usize> &canon_order,
                                  const SearchConfig &cfg, BehavioralArchive &archive) {
  if (!behavioral_active(cfg)) {
    return;
  }
  for (const atx::usize i : canon_order) {
    if (scored[i].rank == 0) { // the non-dominated front: the generation's elites
      archive.insert(std::span<const atx::f64>{scored[i].descriptor});
    }
  }
}

// ----- (4) reproduce -------------------------------------------------------
// Produce the next population: carry the top `elites` by RAW fitness, then fill
// the rest by id-seeded crossover/mutation over canonical-id-ordered parents
// (F1/F2). Every child is funneled through the operators' analyze backstop, so an
// Err child (F5) is simply dropped; if a child cannot be produced, an elite is
// cloned in its place to hold the population size stable (still F5-valid).
//
// ELITISM IS STRUCTURAL: elites are ranked by RAW .fitness (the maximized search
// signal), NOT the novelty-penalized .selection. Carrying the best-raw genome
// verbatim — combined with the canon-keyed fitness_cache that re-scores it to the
// SAME raw value next gen — makes best_fitness_per_gen non-decreasing BY
// CONSTRUCTION. (Tournament parent selection below still uses .selection, the
// explore/anti-collapse pressure — only the elite carry switches to raw.)
[[nodiscard]] std::vector<Genome> SearchDriver::reproduce(const std::vector<Scored> &scored,
                                                          const SearchConfig &cfg, atx::usize gen,
                                                          parallel::DetPool &det_pool,
                                                          SearchResult &res,
                                                          const std::array<atx::f64, 3> &op_weights,
                                                          std::vector<atx::u8> &child_ops) {
  child_ops.clear();
  if (scored.empty()) {
    return {};
  }
  // Canonical-id order BEFORE any RNG draw (F2). Index into this order for the
  // value-based parent pool; rank elites by NSGA-II survivor order (S4.1) — in
  // ScalarRaw this is exactly the pre-S4 raw-descending order. Both are pure,
  // RNG-free permutations of `scored`, computed ONCE here and read-only thereafter.
  const std::vector<atx::usize> canon_order = canon_ordered_indices(scored);
  const std::vector<atx::usize> elite_order = pareto_ordered_indices(scored);

  const atx::usize n_elites = std::min(cfg.elites, scored.size());
  const atx::usize n_children = (cfg.population > n_elites) ? cfg.population - n_elites : 0;

  // (e) reproduction (PARALLEL): id-seeded children fill the non-elite slots.
  //
  // SAFETY: child slot p (population index i = n_elites + p) is written by EXACTLY
  // one shard -> disjoint single-writer slots (the DetPool contract proven for
  // evaluate_generation). Its RNG is Xoshiro256pp{seed_for(master_seed, gen, i)} —
  // a PURE function of (master_seed, gen, i), never worker/thread/claim-order (F1).
  // The make_child path (tournament_pick + crossover/mutation + analyze +
  // canonical_hash) and the F5-reject elite-clone fallback (elite_order[i %
  // max(n_elites,1)] — pure in i, NO shared counter / NO order dependence) read
  // ONLY shared-CONST state (scored, canon_order, elite_order, cfg, catalog_,
  // panel_field_views_, lib_) plus per-call locals — audited reentrant, with no
  // static/thread_local/mutable/shared-scratch state. So child_slot[p] depends only
  // on (i, scored, cfg), independent of worker count and claim order -> byte-identical
  // across {1,2,4} workers. No LPT remap: per-child cost is not known a priori (the
  // tournament parent is drawn inside make_child), and determinism rests on the
  // single-writer slots + per-index seed, NOT on dispatch order.
  std::vector<std::optional<Genome>> child_slot(n_children);
  // Task 5 (adaptive operators): per-child operator id, written by the single shard
  // that produced slot p (disjoint single-writer -> no race, worker-count-invariant).
  // 0xFF == "not a mutation child" (crossover / elite-clone fallback) -> excluded
  // from credit. Sized to n_children; left all-0xFF when adaptation is off.
  child_ops.assign(n_children, atx::u8{0xFF});
  det_pool.parallel_for(n_children, [&](atx::usize p, atx::usize /*wid*/) {
    const atx::usize i = n_elites + p;
    Xoshiro256pp rng{detail::seed_for(cfg.master_seed, gen, i)};
    atx::u8 op_used = 0xFF;
    auto child = make_child(scored, canon_order, cfg, rng, op_weights, gen, op_used);
    if (child.has_value()) {
      child->canon_hash = canon_key(*child);
      child_slot[p] = std::move(*child);
      child_ops[p] = op_used; // 0xFF for a crossover child; 0/1/2 for a mutation child
    } else {
      // F5 reject -> hold population size with an elite clone (deterministic, pure in i).
      const atx::usize fallback = elite_order[i % std::max<atx::usize>(n_elites, 1)];
      Genome clone = scored[fallback].genome.clone();
      clone.canon_hash = scored[fallback].genome.canon_hash;
      child_slot[p] = std::move(clone);
      // child_ops[p] stays 0xFF (elite clone is not credited to any operator).
    }
  });

  // candidates_generated is order-INDEPENDENT: exactly n_children children are
  // produced regardless of which shard ran which slot. Folded as a closed-form add
  // OUTSIDE the parallel region (the old per-child `++` was a shared-counter write).
  res.candidates_generated += n_children;

  // Diversity injection: overwrite the LAST n_imm non-elite slots with fresh
  // grammar genomes (deterministic seed per (gen, slot)). Skips when disabled or
  // when grammar sampling fails (keeps the crossover/mutation child). F1-safe:
  // seed is a pure fn of (master_seed, gen, slot). SERIAL (after the parallel_for)
  // so the overwrite is race-free and byte-deterministic. Elites are never
  // displaced — immigrants only touch child slots.
  constexpr atx::u64 kImmigrantAxis = 0xA5A5A5A5A5A5A5A5ULL;
  const atx::usize n_imm = std::min(cfg.n_immigrants, n_children);
  for (atx::usize t = 0; t < n_imm; ++t) {
    const atx::usize p = n_children - 1U - t; // last slots
    // Restrict the sampler to the driver's panel fields EXACTLY as init_population
    // does (numeric_fields/group_fields = panel_field_views_) so the immigrant only
    // references fields the Panel can evaluate — without this the sampler emits
    // fields absent from narrow panels and the immigrant crashes at eval.
    GenConfig imm_gc = cfg.gen_cfg;
    // Task 3.1: partition immigrant sampler to dtype-correct field catalogs.
    imm_gc.numeric_fields = numeric_field_views_;
    imm_gc.group_fields = group_field_views_;
    Xoshiro256pp rng{detail::seed_for(cfg.master_seed,
                                      kImmigrantAxis ^ static_cast<atx::u64>(gen),
                                      n_elites + p)};
    auto imm = generate_genome(imm_gc, lib_, rng);
    if (imm.has_value()) {
      imm->canon_hash = canon_key(*imm);
      child_slot[p] = std::move(*imm);
      child_ops[p] = atx::u8{0xFF}; // immigrant overwrites the mutation child -> uncredited
    }
  }

  // Serial assembly in POPULATION order (elites first, then children in slot order)
  // — byte-identical to the prior sequential push_back sequence. Elite clones are
  // cheap and stay serial (no parallel benefit; avoids cloning into the slot vector).
  std::vector<Genome> next;
  next.reserve(cfg.population);
  for (atx::usize e = 0; e < n_elites; ++e) {
    next.push_back(scored[elite_order[e]].genome.clone());
    next.back().canon_hash = scored[elite_order[e]].genome.canon_hash;
  }
  for (atx::usize p = 0; p < n_children; ++p) {
    next.push_back(std::move(*child_slot[p]));
  }

  // S3-2: protect_seed_elites — guarantee the top-ranked from_seed genome
  // survives into the next generation's population up to protect_until_gen.
  //
  // WHY AFTER THE CANONICAL SORT: the canon_ordered_indices + pareto_ordered_indices
  // sorts above establish F2 canonical-order BEFORE any RNG draw (the load-bearing
  // determinism proof). Inserting before those sorts would corrupt the canonical
  // ordering invariant. Inserting here — into the already-assembled `next` vector,
  // AFTER all sorting is complete — is the only safe point: no sort has been run
  // on `next` yet (it is the INPUT to the next generation, not the current one).
  //
  // The protection is a POST-selection insertion, NOT a tournament modification:
  // it does not change the tournament draw, the canonical sort, or the reproduction
  // RNG stream — only the final assembled population is adjusted. Default false ->
  // this entire block is dead -> byte-identical to the pre-S3-2 path.
  if (cfg.protect_seed_elites && gen < cfg.protect_until_gen && !scored.empty()) {
    // Find the top-ranked from_seed genome in NSGA-II survivor order.
    // pareto_ordered_indices was already called above (elite_order); iterate in
    // that order to find the best-ranked from_seed candidate.
    atx::usize best_seed_idx = scored.size(); // sentinel: none found
    for (const atx::usize i : elite_order) {
      if (scored[i].genome.from_seed) {
        best_seed_idx = i;
        break; // elite_order is sorted best->worst; first hit is top-ranked
      }
    }
    if (best_seed_idx < scored.size()) {
      // Check whether this genome is already present in the elite slots of `next`
      // (it will be if it naturally survived selection). Compare by canon_hash.
      const atx::u64 seed_hash = scored[best_seed_idx].genome.canon_hash;
      bool already_in_elites = false;
      for (atx::usize e = 0; e < n_elites && e < next.size(); ++e) {
        if (next[e].canon_hash == seed_hash) {
          already_in_elites = true;
          break;
        }
      }
      if (!already_in_elites && !next.empty()) {
        // Replace the last non-elite slot (worst child) with the seed genome.
        // If there are no child slots, replace the last elite slot instead
        // (still better than losing the seed genome entirely).
        const atx::usize replace_pos = next.size() - 1U;
        next[replace_pos] = scored[best_seed_idx].genome.clone();
        next[replace_pos].canon_hash = scored[best_seed_idx].genome.canon_hash;
      }
    }
  }

  return next;
}

// Produce one child from the canonical-id-ordered parent pool with a single
// id-seeded rng: bernoulli(p_cross) ? crossover(two tournament picks) :
// mutate(one tournament pick). The operators' analyze backstop guarantees an
// Ok child is F5-valid; an Err propagates (the caller substitutes an elite).
[[nodiscard]] atx::core::Result<Genome>
SearchDriver::make_child(const std::vector<Scored> &scored,
                         const std::vector<atx::usize> &canon_order, const SearchConfig &cfg,
                         Xoshiro256pp &rng, const std::array<atx::f64, 3> &op_w, atx::usize gen,
                         atx::u8 &op_used) {
  op_used = 0xFF; // default: a crossover child credits no mutation operator
  const Genome &p1 = tournament_pick(scored, canon_order, cfg, rng);
  if (rng.bernoulli(cfg.p_cross)) {
    const Genome &p2 = tournament_pick(scored, canon_order, cfg, rng);
    return subtree_crossover(p1, p2, rng, CrossoverCfg{cfg.max_lookback});
  }
  return mutate_one(p1, cfg, rng, op_w, gen, op_used);
}

// Pick one type-safe mutation by a seeded draw, fallback-cascading so a
// degenerate genome (e.g. no literal to jitter) still yields a child when ANY
// operator applies. Each operator self-validates (analyze backstop, F5).
//
// op_swap is gated behind cfg.enable_op_swap (default ON since S3.4 fixed the
// root cause — analyze's validate_node_contract + the materialized-arity buckets
// make analyze-valid ⟹ VM-safe). The seeded draw uses a FIXED modulus (3)
// regardless of the gate so the RNG stream — and therefore the replay (F1) —
// does not shift when the gate flips; a drawn-but-disabled op_swap simply falls
// through to jitter_const.
[[nodiscard]] atx::core::Result<Genome>
SearchDriver::mutate_one(const Genome &g, const SearchConfig &cfg, Xoshiro256pp &rng,
                         const std::array<atx::f64, 3> &op_w, atx::usize gen, atx::u8 &op_used) {
  // ----- operator draw (flag-branched for RNG-stream parity) -----------------
  // Both branches consume EXACTLY ONE rng word, so the stream length is identical.
  // OFF: the literal legacy `% 3` draw — bit-identical to the pre-Task-5 path, so
  // the ScalarRaw boundary golden is untouched. ON: a weighted draw over op_w
  // (uniform01 from the top 53 bits x total weight; pick by cumulative weight),
  // which is intentionally NOT bit-identical to `% 3` even under uniform weights —
  // which is exactly why the two draws are branched, never unified.
  atx::u64 which;
  if (!cfg.adaptive_operators) {
    which = rng.next_u64() % 3; // LEGACY stream — bit-identical
  } else {
    const atx::f64 total = op_w[0] + op_w[1] + op_w[2];
    const atx::f64 u = (static_cast<atx::f64>(rng.next_u64() >> 11U) /
                        static_cast<atx::f64>(1ULL << 53U)) *
                       total;
    which = (u < op_w[0]) ? 0U : (u < op_w[0] + op_w[1]) ? 1U : 2U;
  }
  // ----- jitter sigma (flag-branched anneal) ---------------------------------
  // base_sigma is the current JitterCfg default (0.5); OFF keeps it constant, ON
  // scales it by jitter_anneal_decay^gen (coarse early, fine late). Pure fn of gen
  // -> no wall-clock / RNG; OFF leaves the jitter draw byte-identical.
  JitterCfg jc;
  jc.max_lookback = cfg.max_lookback;
  if (cfg.jitter_anneal) {
    const atx::f64 base_sigma = JitterCfg{}.sigma;
    jc.sigma = base_sigma * std::pow(cfg.jitter_anneal_decay, static_cast<atx::f64>(gen));
  }
  // ----- W1b wrap_in_op (opt-in; ZERO new RNG draws when disabled) -----------
  // The wrap attempt is a post-draw bernoulli sampled ONLY inside this flag guard,
  // so the disabled path's RNG stream — and therefore the kGoldenDigest boundary
  // pin — is byte-identical to the pre-W1b path. The operator draw `which`, the
  // modulus 3, and op_weights are untouched here. A drawn-and-fired wrap that
  // returns Err falls through to the originally-drawn operator below (so the
  // population never stalls and op_used reflects the operator that actually built
  // the child). op_used = 3 marks a wrap child (0=op_swap,1=field_swap,2=jitter).
  if (cfg.enable_wrap_in_op && rng.bernoulli(cfg.wrap_in_op_prob)) {
    WrapCfg wcfg;
    wcfg.max_depth = cfg.gen_cfg.max_depth;
    auto r = wrap_in_op(g, catalog_, std::span<const std::string_view>{group_field_views_}, rng,
                        wcfg);
    if (r.has_value()) {
      op_used = 3; // wrap_in_op made the child
      // S3-2 gate #4: propagate the seed-lineage tag from parent to child so
      // protect_seed_elites can track seed descendants across the wrap path.
      r->from_seed = g.from_seed;
      return r;
    }
    // Err: fall through to the originally-drawn operator (no stall).
  }
  if (which == 0 && cfg.enable_op_swap) {
    auto r = op_swap(g, catalog_, rng);
    if (r.has_value()) {
      op_used = 0; // op_swap made the child
      // S3-2 gate #4: propagate seed-lineage tag.
      r->from_seed = g.from_seed;
      return r;
    }
  } else if (which == 1) {
    // Task 3.1: restrict field_swap to the numeric partition; group classifier
    // leaves are left alone (the 3.2 analyze backstop catches any bad swap).
    auto r = field_swap(g, std::span<const std::string_view>{numeric_field_views_}, rng);
    if (r.has_value()) {
      op_used = 1; // field_swap made the child
      // S3-2 gate #4: propagate seed-lineage tag.
      r->from_seed = g.from_seed;
      return r;
    }
  }
  // Default / fallback: jitter a literal (the most broadly-applicable mutation).
  op_used = 2; // jitter_const made the child (drawn, or fallback from op_swap/field_swap)
  auto r = jitter_const(g, rng, jc);
  // S3-2 gate #4: propagate seed-lineage tag through the jitter path.
  if (r.has_value()) {
    r->from_seed = g.from_seed;
  }
  return r;
}

// ----- selection helpers ---------------------------------------------------

// Indices of `scored` in canonical-id order (value-based, RNG-free). Established
// before any draw so the seeded tournament is replayable (F2).
[[nodiscard]] std::vector<atx::usize>
SearchDriver::canon_ordered_indices(const std::vector<Scored> &scored) {
  std::vector<atx::usize> idx(scored.size());
  for (atx::usize i = 0; i < idx.size(); ++i) {
    idx[i] = i;
  }
  std::sort(idx.begin(), idx.end(), [&scored](atx::usize a, atx::usize b) {
    return detail::canon_less(scored[a].genome, scored[b].genome);
  });
  return idx;
}

// Indices of `scored` ranked by DESCENDING selection fitness (penalized score);
// ties broken by canonical order so the rank is deterministic (F1). NOTE: this is
// the .selection ranking — it is no longer used for elitism (which switched to
// raw_ordered_indices, below); retained for any selection-pressure consumer.
[[nodiscard]] std::vector<atx::usize>
SearchDriver::elite_ordered_indices(const std::vector<Scored> &scored) {
  std::vector<atx::usize> idx(scored.size());
  for (atx::usize i = 0; i < idx.size(); ++i) {
    idx[i] = i;
  }
  std::sort(idx.begin(), idx.end(), [&scored](atx::usize a, atx::usize b) {
    if (scored[a].selection != scored[b].selection) {
      return scored[a].selection > scored[b].selection;
    }
    return detail::canon_less(scored[a].genome, scored[b].genome);
  });
  return idx;
}

// Indices of `scored` ranked by DESCENDING RAW fitness (the maximized search
// signal); ties broken by canonical order so the rank is deterministic (F1).
// This is the elitism + admitted-candidate ordering: carrying the best-raw genome
// verbatim each gen (and re-scoring it from the canon-keyed cache to the SAME raw
// value) makes best_fitness_per_gen non-decreasing BY CONSTRUCTION.
[[nodiscard]] std::vector<atx::usize>
SearchDriver::raw_ordered_indices(const std::vector<Scored> &scored) {
  std::vector<atx::usize> idx(scored.size());
  for (atx::usize i = 0; i < idx.size(); ++i) {
    idx[i] = i;
  }
  std::sort(idx.begin(), idx.end(), [&scored](atx::usize a, atx::usize b) {
    if (scored[a].fitness != scored[b].fitness) {
      return scored[a].fitness > scored[b].fitness;
    }
    return detail::canon_less(scored[a].genome, scored[b].genome);
  });
  return idx;
}

// S4.1: assign NSGA-II rank + crowding to every Scored, ONCE per generation, in
// canonical-id order, BEFORE any reproduction RNG draw (F1/F2).
//
//  ScalarRaw (the pre-S4 path): collapse to a TOTAL order by RAW fitness — rank ==
//  the position in raw_ordered_indices (raw desc + canon tie-break), crowding == 0.
//  Then pareto_ordered_indices (sort by rank asc) reproduces raw_ordered_indices
//  EXACTLY, and the tournament's .selection comparison is untouched, so the
//  boundary pin stays byte-identical.
//
//  MultiObjective: build an ObjMatrix over the first n_objectives columns (in
//  scored index order), run fast_nondominated_sort(canon_order) -> per-genome
//  rank, then for each front run crowding_distance(front_members, canon_order) ->
//  per-genome crowding. All ordering inside pareto.hpp is canonical-id stable.
void SearchDriver::assign_pareto_ranks(std::vector<Scored> &scored,
                                       const std::vector<atx::usize> &canon_order,
                                       const SearchConfig &cfg) {
  const atx::usize n = scored.size();
  if (n == 0) {
    return;
  }
  if (cfg.objective_mode == ObjectiveMode::ScalarRaw) {
    const std::vector<atx::usize> raw_order = raw_ordered_indices(scored);
    for (atx::usize pos = 0; pos < raw_order.size(); ++pos) {
      scored[raw_order[pos]].rank = static_cast<atx::u16>(pos);
      scored[raw_order[pos]].crowding = 0.0;
    }
    return;
  }

  // MultiObjective: how many objective columns are live (uniform across a run; a
  // 0-objective scored set degenerates to a single front, crowding 0).
  atx::usize k = 0;
  for (const Scored &s : scored) {
    k = std::max<atx::usize>(k, s.n_objectives);
  }
  if (k == 0) {
    for (Scored &s : scored) {
      s.rank = 0;
      s.crowding = 0.0;
    }
    return;
  }

  // L3: fidelity-rejected rows (sentinel score, never fully evaluated) are kept
  // OUT of the non-dominated sort — an all-zero / sentinel objective row would
  // otherwise be mutually non-dominated with real candidates (e.g. a negative
  // parsimony column). The sort runs over the "live" rows only, compacted in
  // index order (so with no rejected rows local id == global id and the result
  // is byte-identical to the pre-L3 path); rejected rows take the trailing front
  // max_front + 1 with crowding 0. If EVERY row is rejected they form front 0.
  std::vector<atx::usize> live;
  live.reserve(n);
  std::vector<atx::usize> local_of(n, n);
  for (atx::usize i = 0; i < n; ++i) {
    if (!is_rejected_score(scored[i].origin)) {
      local_of[i] = live.size();
      live.push_back(i);
    }
  }
  const atx::usize m = live.size();
  if (m == 0) {
    for (Scored &s : scored) {
      s.rank = 0;
      s.crowding = 0.0;
    }
    return;
  }
  std::vector<atx::usize> local_canon;
  local_canon.reserve(m);
  for (const atx::usize i : canon_order) {
    if (local_of[i] != n) {
      local_canon.push_back(local_of[i]);
    }
  }

  // Flat row-major [m * k] objective buffer, sized once (cold per-generation path).
  std::vector<atx::f64> flat(m * k);
  for (atx::usize l = 0; l < m; ++l) {
    for (atx::usize o = 0; o < k; ++o) {
      flat[l * k + o] = scored[live[l]].objectives[o];
    }
  }
  const ObjMatrix obj{flat, m, k};
  const std::vector<atx::u16> front_of = fast_nondominated_sort(obj, local_canon);
  atx::u16 max_front = 0;
  for (const atx::u16 f : front_of) {
    max_front = std::max(max_front, f);
  }
  for (atx::usize i = 0; i < n; ++i) {
    scored[i].rank = (local_of[i] != n) ? front_of[local_of[i]]
                                        : static_cast<atx::u16>(max_front + 1U);
    scored[i].crowding = 0.0;
  }
  // Per-front crowding distance. Group ids by front (in canon_order so each
  // front's member list is canonical-id stable), then score each group.
  for (atx::u16 f = 0; f <= max_front; ++f) {
    std::vector<atx::usize> members;
    for (const atx::usize l : local_canon) {
      if (front_of[l] == f) {
        members.push_back(l);
      }
    }
    if (members.empty()) {
      continue;
    }
    const std::vector<atx::f64> cd = crowding_distance(obj, members, local_canon);
    for (const atx::usize l : members) {
      scored[live[l]].crowding = cd[l];
    }
  }
}

// S4.1: indices of `scored` in NSGA-II survivor order — (rank asc, crowding desc,
// canon_less). Elitism keeps the first front then crowding up to `elites`; the
// admitted-candidate list reads the same order. In ScalarRaw this reduces to the
// exact pre-S4 raw_ordered_indices (rank == raw-descending position, crowding 0,
// canonical tie-break). Requires assign_pareto_ranks to have run.
[[nodiscard]] std::vector<atx::usize>
SearchDriver::pareto_ordered_indices(const std::vector<Scored> &scored) {
  std::vector<atx::usize> idx(scored.size());
  for (atx::usize i = 0; i < idx.size(); ++i) {
    idx[i] = i;
  }
  std::sort(idx.begin(), idx.end(), [&scored](atx::usize a, atx::usize b) {
    if (scored[a].rank != scored[b].rank) {
      return scored[a].rank < scored[b].rank; // lower front first
    }
    if (scored[a].crowding != scored[b].crowding) {
      return scored[a].crowding > scored[b].crowding; // larger crowding first
    }
    return detail::canon_less(scored[a].genome, scored[b].genome);
  });
  return idx;
}

// k-tournament over the canonical-id-ordered parent pool: draw k indices into
// `canon_order` with the seeded rng, return the BETTER candidate. MultiObjective
// compares (rank asc, crowding desc) — the NSGA-II crowded operator; ScalarRaw
// compares .selection > (the EXACT pre-S4 rule). Ties -> the earlier draw / lower
// canonical-id slot (deterministic). The candidate set is iterated in fixed
// canonical order and the RNG DRAW SEQUENCE is identical in both modes (only the
// comparison differs), so the draw is fully replayable (F1/F2).
[[nodiscard]] const Genome &
SearchDriver::tournament_pick(const std::vector<Scored> &scored,
                              const std::vector<atx::usize> &canon_order, const SearchConfig &cfg,
                              Xoshiro256pp &rng) {
  const atx::usize n = canon_order.size();
  const atx::usize k = cfg.k_tournament;
  atx::usize best = canon_order[static_cast<atx::usize>(rng.next_u64() % n)];
  for (atx::usize t = 1; t < std::max<atx::usize>(k, 1); ++t) {
    const atx::usize cand = canon_order[static_cast<atx::usize>(rng.next_u64() % n)];
    const bool better = (cfg.objective_mode == ObjectiveMode::MultiObjective)
                            ? crowded_better(scored[cand], scored[best])
                            : (scored[cand].selection > scored[best].selection);
    if (better) {
      best = cand;
    }
  }
  return scored[best].genome;
}

// The NSGA-II crowded-comparison operator <_n (Deb §III-C): `a` is better than
// `b` iff it has a strictly lower front rank, or an equal rank with strictly
// greater crowding distance. No canonical tie-break here (the ordered-index +
// tournament callers iterate in fixed canonical order, so equal (rank, crowding)
// resolves to the earlier slot deterministically). RNG-free, noexcept.
[[nodiscard]] bool SearchDriver::crowded_better(const Scored &a, const Scored &b) noexcept {
  if (a.rank != b.rank) {
    return a.rank < b.rank;
  }
  return a.crowding > b.crowding;
}

// ----- result assembly -----------------------------------------------------

// Best RAW fitness in the scored set (the maximized search signal, NOT the
// novelty-penalized .selection). This is what best_fitness_per_gen tracks; the
// structural elite carry guarantees it is non-decreasing across generations.
[[nodiscard]] atx::f64 SearchDriver::best_raw(const std::vector<Scored> &scored) {
  atx::f64 best = 0.0;
  bool any = false;
  for (const Scored &s : scored) {
    if (!any || s.fitness > best) {
      best = s.fitness;
      any = true;
    }
  }
  return best;
}

// Mean RAW fitness over the scored set (telemetry for the progress sink; NOT part
// of the digest or admission). NaN/inf-safe: skips non-finite members; empty -> 0.
[[nodiscard]] atx::f64 SearchDriver::mean_raw(const std::vector<Scored> &scored) {
  atx::f64 sum = 0.0;
  atx::usize n = 0;
  for (const auto &s : scored) {
    if (std::isfinite(s.fitness)) {
      sum += s.fitness;
      ++n;
    }
  }
  return n ? sum / static_cast<atx::f64>(n) : 0.0;
}

// dedup_pct + admitted candidates (top survivors of the final generation by
// RAW fitness — same ordering as the structural elite carry, so the run's
// reported best matches what was preserved across generations). trial_count ==
// the distinct structures scored (CanonSet).
void SearchDriver::finalize(const std::vector<Scored> &scored, const CanonSet &canon,
                            SearchResult &res) const {
  res.trial_count = canon.size();
  if (res.candidates_generated > 0) {
    res.dedup_pct = 1.0 - static_cast<atx::f64>(res.trial_count) /
                              static_cast<atx::f64>(res.candidates_generated);
    res.dedup_pct = std::clamp(res.dedup_pct, 0.0, 1.0);
  }
  // Admitted in NSGA-II survivor order (S4.1): first front then crowding; in
  // ScalarRaw this is exactly the pre-S4 raw-descending order, so the boundary-pin
  // result's admitted list is byte-identical.
  std::vector<atx::usize> order = pareto_ordered_indices(scored);
  for (const atx::usize i : order) {
    const Genome &g = scored[i].genome;
    // L3: only a genome with a real full-fidelity score is emitted; a fidelity-
    // rejected (sentinel) or fingerprint-borrowed (approximate) score is not.
    if (scored[i].origin != ScoreOrigin::Full) {
      continue;
    }
    // Task 3.3: a tradeable alpha root must be F64. Reject bare Group (e.g.
    // a naked `sector` root) or Mask roots — they are not numeric signals.
    // Grammar partition (3.1) + dtype guard (3.2) prevent these from arising
    // in normal operation; this is a belt-and-suspenders final gate.
    if (!g.ast.roots().empty()) {
      const alpha::ExprId root_id = g.ast.roots().front().root;
      if (g.analysis.info(root_id).dtype != alpha::DType::F64) {
        continue; // skip non-F64 root — degenerate genome, not admittable
      }
    }
    res.admitted_candidates.push_back(g.clone());
    res.admitted_candidates.back().canon_hash = g.canon_hash;
  }
}

// ----- population checkpoint helpers (resumable-discover, Task 1) ------------

std::vector<std::string>
SearchDriver::serialize_population(const std::vector<Genome> &pop) const {
  std::vector<atx::usize> order(pop.size());
  for (atx::usize i = 0; i < pop.size(); ++i) {
    order[i] = i;
  }
  std::sort(order.begin(), order.end(),
            [&](atx::usize a, atx::usize b) { return detail::canon_less(pop[a], pop[b]); });
  std::vector<std::string> out;
  out.reserve(pop.size());
  for (atx::usize i : order) {
    out.push_back(alpha::unparse(pop[i].ast));
  }
  return out;
}

atx::core::Result<std::vector<Genome>>
SearchDriver::deserialize_population(const std::vector<std::string> &exprs) const {
  std::vector<Genome> out;
  out.reserve(exprs.size());
  for (const auto &src : exprs) {
    ATX_TRY(auto ast, alpha::parse_expr(src, lib_));
    ATX_TRY(auto g, analyze_into(std::move(ast)));
    g.canon_hash = canon_key(g);
    out.push_back(std::move(g));
  }
  return atx::core::Ok(std::move(out));
}

// ----- L3: canonical key + multi-fidelity race ---------------------------------

[[nodiscard]] atx::u64 SearchDriver::canon_key(const Genome &g) const {
  return canonical_hash(g, canon_cfg_); // default CanonCfg == structural hash
}

// Race the distinct fresh candidates through the LOW rungs (those before the
// first full-fidelity rung) on strided sub-panels and return the canon hashes
// that do not survive to the full pass. The low-rung score is the same scalar
// the search maximizes (pool_aware_fitness raw) at reduced fidelity; a fitness
// error scores NaN (a rung rejection). Fail-open: a sub-panel that cannot be
// built, or a race that would reject EVERY candidate, rejects nothing.
//
// DETERMINISM: candidates are raced in to_score order (population first-
// occurrence order, itself a pure function of the seeded search), each (cand,
// rung) evaluation writes a single slot, and promotion sorts by a total value
// key (fidelity.hpp) -> worker-count invariant. The sub-panels depend only on
// (panel_, rung strides).
[[nodiscard]] std::vector<atx::u64>
SearchDriver::fidelity_reject(const std::vector<const Genome *> &to_score,
                              const SearchConfig &cfg, const FitnessCfg &gen_fit,
                              parallel::DetPool &det_pool, SearchResult &res) {
  const FidelityCfg &fc = cfg.fidelity;
  const atx::usize n_low = first_full_rung(fc);
  if (n_low == 0 || to_score.size() < std::max<atx::usize>(fc.min_batch, 2U)) {
    return {};
  }
  std::vector<const alpha::Panel *> rp(n_low, nullptr);
  for (atx::usize r = 0; r < n_low; ++r) {
    const Rung &rung = fc.rungs[r];
    for (atx::usize k = 0; k < rung_keys_.size(); ++k) {
      if (rung_keys_[k].date_stride == rung.date_stride &&
          rung_keys_[k].inst_stride == rung.inst_stride) {
        rp[r] = &rung_panels_[k];
      }
    }
    if (rp[r] == nullptr) {
      auto sub = strided_panel(panel_, rung.date_stride, rung.inst_stride);
      if (!sub.has_value()) {
        return {};
      }
      rung_keys_.push_back(rung);
      rung_panels_.push_back(std::move(*sub));
      // Re-resolve every pointer: push_back may have reallocated rung_panels_.
      for (atx::usize q = 0; q <= r; ++q) {
        for (atx::usize k = 0; k < rung_keys_.size(); ++k) {
          if (rung_keys_[k].date_stride == fc.rungs[q].date_stride &&
              rung_keys_[k].inst_stride == fc.rungs[q].inst_stride) {
            rp[q] = &rung_panels_[k];
          }
        }
      }
    }
  }

  // One reusable Engine per (low rung, worker), bound to that rung's sub-panel
  // (rung_panels_ is not resized below, so the bound references stay valid).
  // Engine::evaluate is idempotent, so reuse is byte-identical to fresh engines.
  std::vector<std::vector<std::unique_ptr<alpha::Engine>>> rung_engines(n_low);
  for (atx::usize r = 0; r < n_low; ++r) {
    rung_engines[r].reserve(det_pool.n_workers());
    for (atx::usize w = 0; w < det_pool.n_workers(); ++w) {
      rung_engines[r].push_back(std::make_unique<alpha::Engine>(*rp[r]));
      // v8 H-3: the run's eligibility, strided like the rung panel (no-op without a mask).
      if (!apply_mask(*rung_engines[r].back(), cfg.cross_section_mask, panel_, fc.rungs[r])) {
        // Review MINE-12: never race (or fully score) unmasked. run() validated the mask, so
        // this is unreachable; evaluate_generation stops the run on the flag.
        res.signal_path_invalid = true;
        res.signal_path_error = "racing rung: the cross_section_mask could not be applied";
        return {};
      }
    }
  }
  CpcvCache rung_cpcv{}; // thread-safe; shared by every rung evaluation below
  // Low rungs score the POOL-INDEPENDENT fitness: the run's pool stores full-panel
  // PnL streams, while a rung candidate's oos_pnl covers only the date-strided
  // sub-panel, so corr_to_pool against the real pool would compare unequal-length
  // (misaligned) streams (ATX_ASSERT abort in Debug). An empty pool -> redundancy 0
  // -> diversify 1: the rung ranks on wq * robust alone; the full-fidelity pass
  // still applies the real pool's diversification to every survivor.
  const combine::AlphaStore no_pool{};

  std::vector<Genome> cands;
  cands.reserve(to_score.size());
  for (const Genome *g : to_score) {
    cands.push_back(g->clone());
    cands.back().canon_hash = g->canon_hash;
  }
  // SAFETY (reentrancy): each call reads only const shared state (rp panels,
  // no_pool, policy_, sim_, gen_fit copy); worker `wid` touches only
  // rung_engines[r][wid] (disjoint single owner); rung_cpcv serializes its own
  // cold inserts behind its mutex.
  const RungEvaluator legacy_eval = [&](const Genome &g, atx::usize r, const Rung &rung,
                                        atx::usize wid) -> atx::f64 {
    FitnessCfg f = gen_fit;
    if (f.cpcv.rule == eval::CpcvRule::DateV2) f.cpcv_session_stride = rung.date_stride;
    f.capacity_objective = false;
    f.turnover_objective = false;
    if (rung.n_folds > 1) {
      f.cpcv.n_groups = rung.n_folds;
      f.cpcv.n_test_groups = std::min<atx::usize>(f.cpcv.n_test_groups, rung.n_folds - 1U);
    }
    auto rep = pool_aware_fitness(g, no_pool, *rp[r], policy_, sim_, f, /*weak_panel=*/nullptr,
                                  rung_engines[r][wid].get(), /*signals=*/nullptr, &rung_cpcv);
    if (!rep.has_value() || !std::isfinite(rep->raw)) {
      return std::numeric_limits<atx::f64>::quiet_NaN();
    }
    return rep->raw;
  };
  // v8 H-3: on the signal-fitness path the functor scores the rung signal instead
  // (same reentrancy: worker `wid` touches only rung_engines[r][wid] and its own
  // functor state).
  const RungEvaluator signal_eval = [&](const Genome &g, atx::usize r, const Rung &rung,
                                        atx::usize wid) -> atx::f64 {
    return signal_rung_score(g, *rung_engines[r][wid], *cfg.signal_fitness,
                             SignalLevel{r, rung.inst_stride, false}, wid);
  };
  const RaceResult rr = race(cands, fc, cfg.signal_fitness != nullptr ? signal_eval : legacy_eval,
                             n_low, &det_pool, /*promote_after_last=*/true);
  res.fidelity_evals += rr.n_evals;
  if (rr.survivors.empty()) {
    return {}; // fail-open: never starve the full pass
  }
  std::vector<std::uint8_t> alive(cands.size(), std::uint8_t{0});
  for (const GenomeId id : rr.survivors) {
    alive[id] = std::uint8_t{1};
  }
  std::vector<atx::u64> rejected;
  rejected.reserve(rr.n_rejected);
  for (atx::usize i = 0; i < cands.size(); ++i) {
    if (alive[i] == std::uint8_t{0}) {
      rejected.push_back(cands[i].canon_hash);
    }
  }
  res.fidelity_rejected += rejected.size();
  return rejected;
}

} // namespace atx::engine::factory
