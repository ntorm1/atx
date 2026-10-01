// atx::engine::factory — the platform v8 H-3 research-search members of SearchDriver
// (cross_section_mask, signal_fitness, op_catalog), the OpCatalogCfg catalogue, the research IC
// fitness and the part-1 IC accessors it builds on.
//
//   * SignalFitnessDefaults.*   default-off identity: the frozen ScalarRaw boundary pin
//                               (factory_nsga_search_test.cpp kGoldenDigest) with the new
//                               members at their defaults, left implicit or spelled out.
//   * SignalFitnessPath.*       the functor replaces pool_aware_fitness; the mask reaches the
//                               full and the rung engines; refusals.
//   * OpCatalogCfgTest.*        literature rows and the deny list.
//   * ResearchIcAccessors.*     labels() are what evaluate_research_ic correlates; the
//                               research_window_ic_config recipe.
//   * ResearchIcFitnessTest.*   f1/f2 of a planted signal, the spanned screen, rungs, trials.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/random.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/unparse.hpp"
#include "atx/engine/combine/marginal_rank_ic.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/factory/fidelity.hpp"
#include "atx/engine/factory/genome.hpp"
#include "atx/engine/factory/ic_research.hpp"
#include "atx/engine/factory/op_catalog.hpp"
#include "atx/engine/factory/research_ic_fitness.hpp"
#include "atx/engine/factory/search_driver.hpp"
#include "atx/engine/factory/signal_fitness.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace atxtest_factory_signal_fitness {

using atx::f64;
using atx::u8;
using atx::u32;
using atx::u64;
using atx::usize;
using atx::engine::WeightPolicy;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::combine::AlphaStore;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::CommissionMode;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::SlippageMode;
using atx::engine::exec::VolumeCapCfg;
namespace ex = atx::engine::factory;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

// ---- the frozen boundary-pin fixture (factory_nsga_search_test.cpp, verbatim) ------------------

// ScalarRaw + novelty off + seed 777 on this 96x6 fixture (NsgaSearch.ScalarRaw_ReproducesGolden
// Digest). A v8 H-3 edit that moves it breaks the default-off identity.
constexpr u64 kGoldenDigest = 0x889874a3b9b29c55ULL;

[[nodiscard]] ExecutionSimulator frictionless_sim() {
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.0, 0.5, 0.0},
                            CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
                            LatencyCfg{},
                            VolumeCapCfg{1.0}};
}

struct Lcg {
  std::uint64_t s;
  [[nodiscard]] f64 next() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::uint64_t hi = s >> 11U;
    const f64 u = static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U);
    return 2.0 * u - 1.0;
  }
};

[[nodiscard]] Panel golden_panel() {
  constexpr usize dates = 96;
  constexpr usize insts = 6;
  std::vector<f64> drift(insts);
  for (usize j = 0; j < insts; ++j) {
    drift[j] = 0.006 - 0.0024 * static_cast<f64>(j);
  }
  std::vector<f64> close(dates * insts);
  std::vector<f64> px(insts, 100.0);
  Lcg rng{0xA11Cu};
  for (usize t = 0; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      px[j] *= (1.0 + drift[j] + 0.010 * rng.next());
      close[t * insts + j] = px[j];
    }
  }
  std::vector<f64> rev(dates * insts, 0.0);
  for (usize t = 1; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      const f64 prev = close[(t - 1) * insts + j];
      rev[t * insts + j] = -(close[t * insts + j] / prev - 1.0);
    }
  }
  auto r = Panel::create(dates, insts, {"close", "rev"}, {close, rev}, {});
  EXPECT_TRUE(r.has_value());
  return std::move(r.value());
}

[[nodiscard]] std::vector<std::string> golden_seeds() {
  return {"rank(close)",         "rank(rev)", "ts_mean(close, 5)", "ts_mean(rev, 3)",
          "rank(ts_mean(close, 10))", "delta(close, 2)"};
}

[[nodiscard]] ex::SearchConfig legacy_pin_cfg(u64 seed) {
  ex::SearchConfig c;
  c.master_seed = seed;
  c.population = 16;
  c.generations = 5;
  c.elites = 2;
  c.k_tournament = 3;
  c.p_cross = 0.5;
  c.objective_mode = ex::ObjectiveMode::ScalarRaw;
  c.enable_behavioral_novelty = false;
  c.enable_parsimony = false;
  c.seed_from_grammar = false;
  c.n_immigrants = 0;
  c.stagnation_patience = 0;
  c.adaptive_operators = false;
  c.jitter_anneal = false;
  c.enable_wrap_in_op = false;
  c.capacity_objective = false;
  c.turnover_objective = false;
  return c;
}

struct Golden {
  Library lib{};
  Panel panel = golden_panel();
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  [[nodiscard]] ex::SearchResult run(const ex::SearchConfig &cfg,
                                     std::vector<std::string> seeds = golden_seeds()) const {
    ex::SearchDriver driver{lib, panel, policy, sim, std::move(seeds), {"close", "rev"}};
    const AlphaStore pool{};
    return driver.run(cfg, pool);
  }
};

// ---- a recording functor ---------------------------------------------------------------------

// Scores raw = (canon_hash mod 997) + 1, rejects genomes whose DSL reads `reject_field`, and
// records per call whether instrument 0 is NaN on every date (the mask's reach). Worker-local
// records only, merged on demand.
class RecordingFitness final : public ex::SignalFitness {
public:
  struct Call {
    u64 hash{};
    bool full{};
    u32 stride{};
    bool first_name_masked{};
  };
  explicit RecordingFitness(std::string reject_field = {}, bool fail = false)
      : reject_field_{std::move(reject_field)}, fail_{fail} {}

  atx::core::Status bind(const ex::SignalFitnessBinding &binding) override {
    dates_ = binding.dates;
    low_rungs_ = binding.low_rungs.size();
    if (calls_.size() < binding.workers) {
      calls_.resize(binding.workers);
    }
    return atx::core::Ok();
  }
  atx::core::Result<ex::SignalScore> score(const ex::Genome &genome, std::span<const f64> signal,
                                           const ex::SignalLevel &level,
                                           usize worker) override {
    if (fail_) {
      return atx::core::Err(atx::core::ErrorCode::Internal, "recording fitness: planted failure");
    }
    const usize names = signal.size() / dates_;
    bool masked = true;
    for (usize d = 0; d < dates_; ++d) {
      masked = masked && std::isnan(signal[d * names]);
    }
    calls_[worker].push_back(Call{genome.canon_hash, level.full, level.inst_stride, masked});
    ex::SignalScore out;
    const std::string dsl = atx::engine::alpha::unparse(genome.ast);
    if (!reject_field_.empty() && dsl.find(reject_field_) != std::string::npos) {
      out.rejected = true;
      return out;
    }
    out.raw = static_cast<f64>(genome.canon_hash % 997U) + 1.0;
    out.objectives[0] = out.raw;
    out.n_objectives = 1;
    return out;
  }
  [[nodiscard]] std::vector<Call> calls() const {
    std::vector<Call> out;
    for (const auto &worker : calls_) {
      out.insert(out.end(), worker.begin(), worker.end());
    }
    return out;
  }
  [[nodiscard]] usize low_rungs() const noexcept { return low_rungs_; }

private:
  std::string reject_field_;
  bool fail_{};
  usize dates_{1};
  usize low_rungs_{};
  std::vector<std::vector<Call>> calls_;
};

// Every seed is rooted in a cross-sectional op, so a masked name reads NaN on every date.
[[nodiscard]] std::vector<std::string> cs_seeds() {
  return {"rank(close)",           "rank(rev)",           "rank(ts_mean(close, 5))",
          "rank(ts_mean(rev, 3))", "rank(delta(close, 2))", "zscore(close)",
          "rank(ts_mean(close, 10))"};
}

[[nodiscard]] ex::SearchConfig one_generation_cfg(usize population) {
  ex::SearchConfig c = legacy_pin_cfg(99);
  c.population = population;
  c.generations = 1;
  return c;
}

// =============================================================================
//  Default-off identity.
// =============================================================================
TEST(SignalFitnessDefaults, ImplicitDefaultsKeepTheGoldenDigest) {
  const Golden g;
  const ex::SearchResult r = g.run(legacy_pin_cfg(777));
  EXPECT_EQ(r.digest, kGoldenDigest) << "a v8 H-3 member leaked into the default path";
  EXPECT_FALSE(r.signal_path_invalid);
  EXPECT_TRUE(r.fidelity_rejected_hashes.empty());
  EXPECT_TRUE(r.unscored_hashes.empty());
}

TEST(SignalFitnessDefaults, ExplicitDefaultsKeepTheGoldenDigestAtEveryWorkerCount) {
  const Golden g;
  for (const usize workers : {usize{1}, usize{4}}) {
    ex::SearchConfig cfg = legacy_pin_cfg(777);
    cfg.n_workers = workers;
    cfg.cross_section_mask = {};
    cfg.signal_fitness = nullptr;
    cfg.op_catalog = ex::OpCatalogCfg{false, {}};
    const ex::SearchResult r = g.run(cfg);
    EXPECT_EQ(r.digest, kGoldenDigest) << "workers=" << workers;
  }
}

// =============================================================================
//  The functor path.
// =============================================================================
TEST(SignalFitnessPath, FunctorReplacesPoolFitnessAndFilesRejections) {
  const Golden g;
  RecordingFitness fitness{"rev"};
  ex::SearchConfig cfg = one_generation_cfg(cs_seeds().size());
  cfg.signal_fitness = &fitness;
  const ex::SearchResult r = g.run(cfg, cs_seeds());
  ASSERT_FALSE(r.signal_path_invalid) << r.signal_path_error;
  const auto calls = fitness.calls();
  ASSERT_EQ(calls.size(), r.all_scored.size()); // one full-pass score per distinct candidate
  f64 best = -std::numeric_limits<f64>::infinity();
  usize rejected = 0;
  for (const ex::Genome &genome : r.all_scored) {
    const std::string dsl = atx::engine::alpha::unparse(genome.ast);
    const bool reads_rev = dsl.find("rev") != std::string::npos;
    EXPECT_EQ(std::binary_search(r.ic_rejected_hashes.begin(), r.ic_rejected_hashes.end(),
                                 genome.canon_hash),
              reads_rev)
        << dsl;
    if (reads_rev) {
      ++rejected;
    } else {
      best = std::max(best, static_cast<f64>(genome.canon_hash % 997U) + 1.0);
    }
  }
  EXPECT_EQ(rejected, 2U);
  ASSERT_EQ(r.best_fitness_per_gen.size(), 1U);
  EXPECT_EQ(r.best_fitness_per_gen.front(), best);
  for (const ex::Genome &genome : r.admitted_candidates) {
    EXPECT_EQ(atx::engine::alpha::unparse(genome.ast).find("rev"), std::string::npos);
  }
  EXPECT_TRUE(r.unscored_hashes.empty());
}

TEST(SignalFitnessPath, MaskReachesFullAndRungEngines) {
  const Golden g;
  std::vector<u8> mask(g.panel.cells(), 1);
  for (usize d = 0; d < g.panel.dates(); ++d) {
    mask[d * g.panel.instruments()] = 0; // instrument 0 is never eligible
  }
  RecordingFitness fitness;
  ex::SearchConfig cfg = one_generation_cfg(cs_seeds().size());
  cfg.signal_fitness = &fitness;
  cfg.cross_section_mask = mask;
  cfg.fidelity.enabled = true;
  const std::vector<u32> strides{2};
  const auto rungs = ex::instrument_rungs(strides);
  ASSERT_TRUE(rungs.has_value());
  cfg.fidelity.rungs = *rungs;
  const ex::SearchResult r = g.run(cfg, cs_seeds());
  ASSERT_FALSE(r.signal_path_invalid) << r.signal_path_error;
  EXPECT_EQ(fitness.low_rungs(), 1U);
  usize full = 0;
  usize rung = 0;
  for (const auto &call : fitness.calls()) {
    EXPECT_TRUE(call.first_name_masked) << "full=" << call.full;
    full += call.full ? 1U : 0U;
    rung += call.full ? 0U : 1U;
    if (!call.full) {
      EXPECT_EQ(call.stride, 2U);
    }
  }
  EXPECT_EQ(rung, cs_seeds().size());           // every candidate raced once
  EXPECT_EQ(full + r.fidelity_rejected_hashes.size(), r.all_scored.size());
  EXPECT_FALSE(r.fidelity_rejected_hashes.empty()); // eta 1/3 of 7 keeps 3
  EXPECT_TRUE(std::is_sorted(r.fidelity_rejected_hashes.begin(), r.fidelity_rejected_hashes.end()));
}

TEST(SignalFitnessPath, RefusesDateStridesLegacyOverlaysBadMasksAndFunctorErrors) {
  const Golden g;
  RecordingFitness fitness;
  const auto refused = [&](ex::SearchConfig cfg, const std::string &needle) {
    const ex::SearchResult r = g.run(cfg, cs_seeds());
    EXPECT_TRUE(r.signal_path_invalid) << needle;
    EXPECT_NE(r.signal_path_error.find(needle), std::string::npos) << r.signal_path_error;
    EXPECT_TRUE(r.all_scored.empty()) << needle;
  };
  ex::SearchConfig base = one_generation_cfg(cs_seeds().size());
  base.signal_fitness = &fitness;
  auto date_strided = base;
  date_strided.fidelity.enabled = true; // the default ladder strides dates by 4 and 2
  refused(date_strided, "instrument strides");
  auto screened = base;
  screened.ic_screen = ex::equivalence_ic_screen_config();
  refused(screened, "IC screen");
  auto dedup = base;
  dedup.output_dedup = true;
  refused(dedup, "output dedup");
  auto short_mask = legacy_pin_cfg(777); // a malformed mask refuses without a functor too
  const std::vector<u8> three(3, 1);
  short_mask.cross_section_mask = three;
  refused(short_mask, "cross_section_mask");
  RecordingFitness failing{{}, true};
  auto failed = base;
  failed.signal_fitness = &failing;
  const ex::SearchResult r = g.run(failed, cs_seeds());
  EXPECT_TRUE(r.signal_path_invalid);
  EXPECT_NE(r.signal_path_error.find("planted failure"), std::string::npos) << r.signal_path_error;
}

// =============================================================================
//  OpCatalogCfg.
// =============================================================================
// The distinct ops op-swap offers for a delay(x, w) node (Panel, F64, arity 2) over 4000 seeded
// draws, in first-draw order. The names view the static registry tables.
[[nodiscard]] std::vector<std::string_view> panel_binary_swaps(const ex::OpCatalog &catalog,
                                                               const Library &lib) {
  const atx::engine::alpha::OpSig *delay = lib.find("delay");
  EXPECT_NE(delay, nullptr);
  atx::core::Xoshiro256pp rng{0x5eedULL};
  std::vector<std::string_view> names;
  for (usize k = 0; k < 4000; ++k) {
    const auto pick = catalog.sample_compatible(atx::engine::alpha::Shape::Panel,
                                                atx::engine::alpha::DType::F64, 2, delay, rng);
    if (pick && std::find(names.begin(), names.end(), (*pick)->name) == names.end()) {
      names.push_back((*pick)->name);
    }
  }
  return names;
}

TEST(OpCatalogCfgTest, LiteratureRowsAndDenyListShapeTheSwapCatalogue) {
  const Library lib{};
  const ex::OpCatalog plain{lib};
  const ex::OpCatalog defaulted{lib, ex::OpCatalogCfg{}};
  const ex::OpCatalog literature{lib, ex::OpCatalogCfg{true, {}}};
  const ex::OpCatalog denied{lib, ex::OpCatalogCfg{false, {"ts_mean", "trade_when"}}};
  EXPECT_EQ(defaulted.bucket_count(), plain.bucket_count());
  EXPECT_GT(literature.bucket_count(), plain.bucket_count()); // e.g. group_cross (Group out)
  const auto has = [](const std::vector<std::string_view> &names, std::string_view name) {
    return std::find(names.begin(), names.end(), name) != names.end();
  };
  const auto base_names = panel_binary_swaps(plain, lib);
  EXPECT_TRUE(has(base_names, "ts_mean"));
  EXPECT_FALSE(has(base_names, "ts_count_increases"));
  EXPECT_EQ(panel_binary_swaps(defaulted, lib), base_names); // same buckets, same draws
  EXPECT_TRUE(has(panel_binary_swaps(literature, lib), "ts_count_increases"));
  const auto kept = panel_binary_swaps(denied, lib);
  EXPECT_FALSE(has(kept, "ts_mean"));
  EXPECT_TRUE(has(kept, "ts_sum"));
}

// =============================================================================
//  The part-1 IC accessors.
// =============================================================================
[[nodiscard]] Panel rising_panel(usize d, usize n) {
  std::vector<f64> price(d * n);
  for (usize t = 0; t < d; ++t) {
    for (usize i = 0; i < n; ++i) {
      price[t * n + i] = 100.0 * std::exp(0.0001 * static_cast<f64>((i + 1) * t) +
                                          0.01 * std::sin(0.3 * static_cast<f64>(t * (i + 2))));
    }
  }
  auto p = Panel::create(d, n, {"close"}, {std::move(price)}, {});
  EXPECT_TRUE(p.has_value());
  return std::move(*p);
}

TEST(ResearchIcAccessors, LabelsAreWhatEvaluateResearchIcCorrelates) {
  constexpr usize d = 160;
  constexpr usize n = 12;
  const Panel panel = rising_panel(d, n);
  const auto cfg = ex::research_window_ic_config(10, 150, 5, 16, 1ULL << 24);
  auto cache = ex::prepare_research_ic(panel, cfg, {3, true, 1});
  ASSERT_TRUE(cache.has_value());
  auto scratch = ex::prepare_research_ic_scratch(*cache);
  ASSERT_TRUE(scratch.has_value());
  const usize rows = cache->label_rows(ex::kResearchIcHorizon);
  const auto labels = cache->labels(ex::kResearchIcHorizon);
  ASSERT_EQ(rows, 150U - 10U - 22U);
  ASSERT_EQ(labels.size(), rows * n);
  EXPECT_TRUE(cache->labels(3).empty());
  std::vector<f64> signal(d * n, kNaN);
  for (usize row = 0; row < rows; ++row) {
    for (usize i = 0; i < n; ++i) {
      signal[(cache->first_date() + row) * n + i] = labels[row * n + i];
    }
  }
  auto result = ex::evaluate_research_ic(signal, *cache, *scratch);
  ASSERT_TRUE(result.has_value());
  const auto daily = scratch->rank_series(ex::kResearchIcHorizon);
  ASSERT_EQ(daily.size(), rows);
  usize defined = 0;
  for (const f64 ic : daily) {
    if (std::isfinite(ic)) {
      EXPECT_NEAR(ic, 1.0, 1e-12);
      ++defined;
    }
  }
  EXPECT_GT(defined, rows / 2);
}

TEST(ResearchIcAccessors, WindowRecipeFieldValues) {
  const auto cfg = ex::research_window_ic_config(383, 1200, 50, 128, 777);
  EXPECT_EQ(cfg.rule, ex::IcScreenRule::EquivalenceV3);
  EXPECT_EQ(cfg.horizons, (std::array<usize, 4>{{5, 21, 63, 0}}));
  EXPECT_EQ(cfg.execution_delay, 1U);
  EXPECT_EQ(cfg.window_begin, 383U);
  EXPECT_EQ(cfg.window_end, 1200U);
  EXPECT_EQ(cfg.maturity_end, 1200U);
  EXPECT_EQ(cfg.min_names, 50U);
  EXPECT_EQ(cfg.min_dates, 128U);
  EXPECT_EQ(cfg.max_cache_bytes, 777U);
  EXPECT_EQ(cfg.practical_abs_ic, 0.002);
  EXPECT_EQ(cfg.confidence_multiplier, 3.5);
}

// =============================================================================
//  ResearchIcFitness.
// =============================================================================

// n names, d dates; field f i.i.d. uniform; r(t) = .02 f(t-2) + .01 e(t), so f at decision t
// drives the first day of its h 21 label (entry t+1, first return t+2).
struct Planted {
  static constexpr usize d = 300;
  static constexpr usize n = 24;
  Panel panel;
  std::vector<f64> field;
  std::vector<u8> member;
  std::vector<f64> field_rank; // f's centred tied rank over the members (a regressor)
};

[[nodiscard]] Planted planted() {
  constexpr usize d = Planted::d;
  constexpr usize n = Planted::n;
  Lcg rng{0xB0B0ULL};
  std::vector<f64> field(d * n);
  for (f64 &v : field) {
    v = rng.next();
  }
  std::vector<f64> close(d * n, 100.0);
  for (usize t = 1; t < d; ++t) {
    for (usize i = 0; i < n; ++i) {
      const f64 driver = t >= 2 ? field[(t - 2) * n + i] : 0.0;
      close[t * n + i] = close[(t - 1) * n + i] * (1.0 + 0.02 * driver + 0.01 * rng.next());
    }
  }
  auto panel = Panel::create(d, n, {"close", "f"}, {close, field}, {});
  EXPECT_TRUE(panel.has_value());
  std::vector<u8> member(d * n, 1);
  std::vector<f64> ranks(d * n, kNaN);
  std::vector<std::pair<f64, usize>> sorted;
  for (usize t = 0; t < d; ++t) {
    const auto status = atx::engine::combine::centred_tied_ranks(
        std::span<const f64>{field}.subspan(t * n, n),
        std::span<const u8>{member}.subspan(t * n, n), std::span<f64>{ranks}.subspan(t * n, n),
        sorted);
    EXPECT_TRUE(status.has_value());
  }
  return Planted{std::move(*panel), std::move(field), std::move(member), std::move(ranks)};
}

[[nodiscard]] ex::Genome genome_of(const std::string &dsl, const Library &lib, u64 hash) {
  auto ast = atx::engine::alpha::parse_expr(dsl, lib);
  EXPECT_TRUE(ast.has_value()) << dsl;
  auto g = ex::analyze_into(std::move(*ast));
  EXPECT_TRUE(g.has_value()) << dsl;
  g->canon_hash = hash;
  return std::move(*g);
}

[[nodiscard]] ex::ResearchIcWindow planted_window() {
  return ex::ResearchIcWindow{30, Planted::d, 10, 100, 1ULL << 26};
}

TEST(ResearchIcFitnessTest, PlantedSignalScoresAndIsRecorded) {
  const Planted p = planted();
  const Library lib{};
  auto fitness = ex::ResearchIcFitness::prepare(
      ex::ResearchIcFitnessInputs{&p.panel, planted_window(), p.member, {}, {}});
  ASSERT_TRUE(fitness.has_value()) << fitness.error().to_string();
  const std::vector<ex::Rung> low{ex::Rung{1, 2, 0}};
  ASSERT_TRUE(fitness->bind(ex::SignalFitnessBinding{1, Planted::d, Planted::n, low}));
  const ex::Genome g = genome_of("f", lib, 7);
  auto full = fitness->score(g, p.field, ex::SignalLevel{}, 0);
  ASSERT_TRUE(full.has_value()) << full.error().to_string();
  EXPECT_FALSE(full->rejected);
  EXPECT_GT(full->objectives[0], 5.0); // f1 = |IC t|
  EXPECT_GT(full->objectives[1], 5.0); // f2 = sign x marginal t (intercept-only residual)
  EXPECT_EQ(full->raw, std::min(full->objectives[0], full->objectives[1]));
  EXPECT_EQ(full->n_objectives, 2U);
  // The rung reads the IC on every second name only.
  auto strided = ex::strided_cells(std::span<const f64>{p.field}, Planted::d, Planted::n, 1, 2);
  ASSERT_TRUE(strided.has_value());
  auto rung = fitness->score(g, *strided, ex::SignalLevel{0, 2, false}, 0);
  ASSERT_TRUE(rung.has_value()) << rung.error().to_string();
  EXPECT_FALSE(rung->rejected);
  EXPECT_GT(rung->raw, 3.0);
  EXPECT_FALSE(fitness->score(g, *strided, ex::SignalLevel{0, 3, false}, 0).has_value());
  EXPECT_FALSE(fitness->score(g, p.field, ex::SignalLevel{}, 1).has_value()); // unbound worker
  const auto trials = fitness->take_trials();
  ASSERT_EQ(trials.size(), 1U); // rung reads are not trials of the log
  EXPECT_EQ(trials.front().canon_hash, 7U);
  EXPECT_EQ(trials.front().screen, ex::ResearchIcScreen::None);
  EXPECT_EQ(trials.front().daily_rank_ic.size(), Planted::d - 30U - 22U);
  EXPECT_EQ(trials.front().read.sign, 1);
  EXPECT_TRUE(fitness->take_trials().empty());
}

TEST(ResearchIcFitnessTest, CandidateSpannedByTheRegressorsIsScreened) {
  const Planted p = planted();
  const Library lib{};
  std::vector<std::span<const f64>> regressors{p.field_rank};
  auto fitness = ex::ResearchIcFitness::prepare(
      ex::ResearchIcFitnessInputs{&p.panel, planted_window(), p.member, {}, regressors});
  ASSERT_TRUE(fitness.has_value()) << fitness.error().to_string();
  ASSERT_TRUE(fitness->bind(ex::SignalFitnessBinding{2, Planted::d, Planted::n, {}}));
  auto scored = fitness->score(genome_of("f", lib, 9), p.field, ex::SignalLevel{}, 1);
  ASSERT_TRUE(scored.has_value()) << scored.error().to_string();
  EXPECT_TRUE(scored->rejected); // every date spanned: marginal IC 0, its t undefined
  const auto trials = fitness->take_trials();
  ASSERT_EQ(trials.size(), 1U);
  EXPECT_EQ(trials.front().screen, ex::ResearchIcScreen::MarginalUndefined);
  EXPECT_TRUE(trials.front().read.ic_defined);
  EXPECT_EQ(trials.front().read.spanned_dates, trials.front().read.marginal_dates);
  EXPECT_FALSE(fitness->bind(ex::SignalFitnessBinding{1, Planted::d + 1, Planted::n, {}}));
  const std::vector<ex::Rung> dated{ex::Rung{2, 2, 0}};
  EXPECT_FALSE(fitness->bind(ex::SignalFitnessBinding{1, Planted::d, Planted::n, dated}));
}

// Two fields f and g, both i.i.d. uniform, r(t) = .02 (f + g)(t - 2) + .01 e(t); the regressor is
// f's centred tied rank over the members.
struct PlantedPair {
  static constexpr usize d = 300;
  static constexpr usize n = 24;
  Panel panel;
  std::vector<f64> f;
  std::vector<f64> g;
  std::vector<u8> member;
  std::vector<f64> f_rank;
};

[[nodiscard]] PlantedPair planted_pair() {
  constexpr usize d = PlantedPair::d;
  constexpr usize n = PlantedPair::n;
  Lcg rng{0xFA17ULL};
  std::vector<f64> f(d * n);
  std::vector<f64> g(d * n);
  for (f64 &v : f) {
    v = rng.next();
  }
  for (f64 &v : g) {
    v = rng.next();
  }
  std::vector<f64> close(d * n, 100.0);
  for (usize t = 1; t < d; ++t) {
    for (usize i = 0; i < n; ++i) {
      const f64 driver = t >= 2 ? f[(t - 2) * n + i] + g[(t - 2) * n + i] : 0.0;
      close[t * n + i] = close[(t - 1) * n + i] * (1.0 + 0.02 * driver + 0.01 * rng.next());
    }
  }
  auto panel = Panel::create(d, n, {"close", "f", "g"}, {close, f, g}, {});
  EXPECT_TRUE(panel.has_value());
  std::vector<u8> member(d * n, 1);
  std::vector<f64> ranks(d * n, kNaN);
  std::vector<std::pair<f64, usize>> sorted;
  for (usize t = 0; t < d; ++t) {
    const auto status = atx::engine::combine::centred_tied_ranks(
        std::span<const f64>{f}.subspan(t * n, n), std::span<const u8>{member}.subspan(t * n, n),
        std::span<f64>{ranks}.subspan(t * n, n), sorted);
    EXPECT_TRUE(status.has_value());
  }
  return PlantedPair{std::move(*panel), std::move(f), std::move(g), std::move(member),
                     std::move(ranks)};
}

// Review MINE-9: f2 is the discover sign times the marginal HAC t of the candidate's residual on
// the regressors. Against the regressor rank(f), the half-spanned f + g (sign +1; a numpy replica
// reads raw IC t 17.7, marginal t 7.8) and the negative-IC -2 f + g (sign -1, marginal t +8.1)
// score exactly the direct combine::marginal_rank_ic_day + summarize_rank_ic computation on the
// IC recipe's labels: neither the raw IC t nor |marginal t| reproduces both.
TEST(ResearchIcFitnessTest, PartlySpannedAndNegativeCandidatesScoreTheDirectMarginalT) {
  namespace cb = atx::engine::combine;
  constexpr usize d = PlantedPair::d;
  constexpr usize n = PlantedPair::n;
  const PlantedPair p = planted_pair();
  const Library lib{};
  const ex::ResearchIcWindow window{30, d, 10, 100, 1ULL << 26};
  const std::vector<std::span<const f64>> regressors{p.f_rank};
  auto fitness = ex::ResearchIcFitness::prepare(
      ex::ResearchIcFitnessInputs{&p.panel, window, p.member, {}, regressors});
  ASSERT_TRUE(fitness.has_value()) << fitness.error().to_string();
  ASSERT_TRUE(fitness->bind(ex::SignalFitnessBinding{1, d, n, {}}));
  const auto config = ex::research_window_ic_config(window.begin, window.end, window.min_names,
                                                    window.min_dates, window.max_cache_bytes);
  auto cache = ex::prepare_research_ic(p.panel, config, {3, true, 1}, p.member);
  ASSERT_TRUE(cache.has_value()) << cache.error().to_string();
  const usize rows = cache->label_rows(ex::kResearchIcHorizon);
  const std::span<const f64> labels = cache->labels(ex::kResearchIcHorizon);
  ASSERT_EQ(rows, d - 30U - 22U);
  // The K6 kernel row by row on the label rows, then the Bartlett lag-21 summary.
  const auto direct_t = [&](const std::vector<f64> &signal) {
    std::vector<f64> rank_row(n, kNaN);
    std::vector<f64> daily(rows, kNaN);
    std::vector<f64> compact;
    std::vector<std::pair<f64, usize>> sorted;
    cb::MarginalRankIcScratch scratch;
    for (usize row = 0; row < rows; ++row) {
      const usize at = (cache->first_date() + row) * n;
      EXPECT_TRUE(cb::centred_tied_ranks(std::span<const f64>{signal}.subspan(at, n),
                                         std::span<const u8>{p.member}.subspan(at, n), rank_row,
                                         sorted)
                      .has_value());
      const std::array<std::span<const f64>, 1> regressor_rows{
          std::span<const f64>{p.f_rank}.subspan(at, n)};
      const auto day = cb::marginal_rank_ic_day(rank_row, regressor_rows,
                                                labels.subspan(row * n, n), window.min_names,
                                                scratch);
      EXPECT_TRUE(day.has_value());
      if (day.has_value()) {
        daily[row] = day->marginal_ic;
      }
    }
    return cb::summarize_rank_ic(daily, ex::kResearchIcHacLag, compact).hac_t;
  };
  struct Case {
    f64 f_weight;
    f64 g_weight;
    int sign;
    u64 hash;
  };
  for (const Case c : {Case{1.0, 1.0, 1, 21}, Case{-2.0, 1.0, -1, 22}}) {
    std::vector<f64> signal(d * n);
    for (usize k = 0; k < signal.size(); ++k) {
      signal[k] = c.f_weight * p.f[k] + c.g_weight * p.g[k];
    }
    // The genome only names the trial; the fitness scores `signal`.
    auto scored = fitness->score(genome_of("f", lib, c.hash), signal, ex::SignalLevel{}, 0);
    ASSERT_TRUE(scored.has_value()) << scored.error().to_string();
    EXPECT_FALSE(scored->rejected) << c.hash;
    const auto trials = fitness->take_trials();
    ASSERT_EQ(trials.size(), 1U);
    const ex::ResearchIcRead &read = trials.front().read;
    EXPECT_EQ(read.sign, c.sign) << c.hash;
    EXPECT_EQ(read.spanned_dates, 0U) << c.hash;
    const f64 direct = direct_t(signal);
    EXPECT_GT(direct, 3.0) << c.hash; // g's part survives the projection on rank(f)
    EXPECT_NEAR(read.marginal_t, direct, 1e-12) << c.hash;
    EXPECT_NEAR(scored->objectives[1], static_cast<f64>(c.sign) * direct, 1e-12) << c.hash;
    EXPECT_NEAR(ex::research_ic_f2(read), static_cast<f64>(c.sign) * direct, 1e-12) << c.hash;
  }
}

} // namespace atxtest_factory_signal_fitness
