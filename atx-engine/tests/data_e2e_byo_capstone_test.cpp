// data_e2e_byo_capstone_test.cpp — S6.9 CAPSTONE (suite DataE2EByoCapstone).
//
// The FINAL S6 unit: exercise ALL FOUR BYO data plugs SIMULTANEOUSLY through one
// DataContext driving BookPipeline::run_with_context, and prove the run is
// reproducible. ONE DatasetCatalog carries:
//   * a Role::Price Dataset   ({"close","rev"}, the raw lowering)
//   * a Role::Feature Dataset ("sentiment" column — merged into the panel)
//   * a Role::Signal Dataset  ("ext_sig" column — a STRONG planted foresight signal
//                              that clears the LIVE deflated-Sharpe/robustness gate)
//   * a BYO FactorModelArtifact (a valid SPD model sized to the universe), attached
//     via ctx.set_factor_model — the override path that drives optimize.
//
// The capstone proves all four plugs at once:
//   (i)   the external signal IS admitted — lib.n_alphas() grew AND the admitted
//         candidate count > 0 (admission via the live gate, not assumed);
//   (ii)  the feature is used — ctx.price_panel() resolves the "sentiment" field;
//   (iii) the BYO factor model drove optimize — factor_model_override() returns a
//         value AND out.dead_factors == 0 (the override bypasses dead-augmentation,
//         the observable signature of the override path);
//   (iv)  DETERMINISTIC — the WHOLE thing run a SECOND time with FRESH libraries
//         (and a freshly-rebuilt deterministic ctx) yields identical book_digest
//         AND report_digest.
//
// Fixture builders (momentum_close, the rev formula, real_pipeline_cfg, the
// sim/policy/gate/dsl spine) are COPIED file-static from book_pipeline_test.cpp /
// data_boundary_pin_test.cpp.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/linalg/linalg.hpp" // MatX, VecX (BYO factor artifact)
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp" // alpha::Library

#include "atx/engine/combine/combiner.hpp" // CombinerConfig
#include "atx/engine/combine/gate.hpp"     // AlphaGate, GateConfig

#include "atx/engine/exec/execution_sim.hpp"

#include "atx/engine/factory/factory.hpp"         // FactoryConfig
#include "atx/engine/factory/research_driver.hpp" // ResearchConfig

#include "atx/engine/library/library.hpp" // library::Library

#include "atx/engine/loop/weight_policy.hpp"

#include "atx/engine/risk/multi_period.hpp" // RebalanceSchedule

#include "atx/engine/data/catalog.hpp"
#include "atx/engine/data/context.hpp"
#include "atx/engine/data/dataset.hpp"
#include "atx/engine/data/dataset_schema.hpp"
#include "atx/engine/data/factor_model_artifact.hpp"

#include "atx/engine/book/pipeline.hpp" // BookPipeline (run_with_context)

namespace atxtest_data_e2e_byo_capstone_test {

using atx::f64;
using atx::u16;
using atx::u32;
using atx::u64;
using atx::usize;
using atx::engine::WeightPolicy;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::book::BookPipeline;
using atx::engine::book::PipelineConfig;
using atx::engine::book::PipelineReport;
using atx::engine::combine::AlphaGate;
using atx::engine::combine::CombinerConfig;
using atx::engine::combine::GateConfig;
using atx::engine::data::ColumnDType;
using atx::engine::data::DataContext;
using atx::engine::data::Dataset;
using atx::engine::data::DatasetCatalog;
using atx::engine::data::DatasetProvenance;
using atx::engine::data::DatasetSchema;
using atx::engine::data::DateKey;
using atx::engine::data::FactorModelArtifact;
using atx::engine::data::InstKey;
using atx::engine::data::Role;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::CommissionMode;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::SlippageMode;
using atx::engine::exec::VolumeCapCfg;
using atx::engine::factory::FactoryConfig;
using atx::engine::factory::ResearchConfig;
using atx::engine::risk::RebalanceSchedule;

namespace linalg = atx::core::linalg;
namespace lib = atx::engine::library;

// ---- builders (mirrored from book_pipeline_test.cpp / data_boundary_pin_test.cpp) ----
namespace {

[[nodiscard]] ExecutionSimulator frictionless_sim() {
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.1, 0.5, 0.05},
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

constexpr usize kDates = 120U;
constexpr usize kInsts = 8U;
constexpr u64 kLibSeed = 0xC0FFEEu;
constexpr f64 kMinDsr = 0.5;

// A persistent cross-sectional momentum spread: instrument j carries a fixed drift
// (inst 0 best ... inst 7 worst). A foresight signal ranking by that fixed order is
// the right book at EVERY date — so the planted "ext_sig" column ADMITS through the
// live gate, and is genuinely predictive of next-period returns.
[[nodiscard]] std::vector<f64> momentum_close(usize dates, usize insts, std::uint64_t seed) {
  std::vector<f64> drift(insts);
  for (usize j = 0; j < insts; ++j) {
    drift[j] = 0.010 - 0.0040 * static_cast<f64>(j);
  }
  std::vector<f64> close(dates * insts);
  std::vector<f64> px(insts, 100.0);
  Lcg rng{seed};
  for (usize t = 0; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      px[j] *= (1.0 + drift[j] + 0.008 * rng.next());
      close[t * insts + j] = px[j];
    }
  }
  return close;
}

// rev = -(close_t/close_{t-1} - 1), the two_field_panel convention.
[[nodiscard]] std::vector<f64> rev_from_close(usize dates, usize insts,
                                              const std::vector<f64> &close) {
  std::vector<f64> rev(dates * insts, 0.0);
  for (usize t = 1; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      const f64 prev = close[(t - 1) * insts + j];
      rev[t * insts + j] = -(close[t * insts + j] / prev - 1.0);
    }
  }
  return rev;
}

[[nodiscard]] std::vector<std::string> seed_exprs() {
  return {"rank(close)",
          "rank(rev)",
          "ts_mean(close, 5)",
          "ts_mean(rev, 3)",
          "rank(ts_mean(close, 10))",
          "delta(close, 2)"};
}
[[nodiscard]] std::vector<std::string> panel_fields() { return {"close", "rev"}; }
[[nodiscard]] GateConfig default_gate_cfg() { return GateConfig{}; }

[[nodiscard]] FactoryConfig per_run_cfg(usize pop, usize gens) {
  FactoryConfig cfg;
  cfg.search.master_seed = 0;
  cfg.search.population = pop;
  cfg.search.generations = gens;
  cfg.search.elites = 2;
  cfg.search.k_tournament = 3;
  cfg.search.p_cross = 0.5;
  cfg.search.novelty_w = 0.1;
  cfg.search.fitness.trial_count = 4;
  cfg.seed_exprs = seed_exprs();
  cfg.panel_fields = panel_fields();
  cfg.min_dsr = kMinDsr;
  return cfg;
}

[[nodiscard]] ResearchConfig real_signal_research_cfg(u64 master_seed) {
  ResearchConfig cfg;
  cfg.per_run = per_run_cfg(16, 4);
  cfg.max_runs = 2;
  cfg.patience = 0;
  cfg.master_seed = master_seed;
  return cfg;
}

[[nodiscard]] RebalanceSchedule book_schedule() {
  return RebalanceSchedule{{60U, 70U, 80U, 90U, 100U}};
}

[[nodiscard]] PipelineConfig real_pipeline_cfg(u64 research_seed) {
  PipelineConfig cfg;
  cfg.research = real_signal_research_cfg(research_seed);
  cfg.library_master_seed = kLibSeed;
  cfg.schedule = book_schedule();
  cfg.combiner = CombinerConfig{};
  cfg.optimizer.single.gross_leverage = 2.0;
  cfg.optimizer.single.turnover_penalty = 0.0;
  cfg.optimizer.trade_rate = 1.0;
  cfg.optimizer.capacity_bound_gross = true;
  cfg.alloc.max_gross = 1.0;
  cfg.reference_aum = 1.0e5;
  cfg.forward_window = 80U;
  cfg.returns_field = "rev";
  cfg.dead_lifecycle_as_of = 5U;
  cfg.dead_holdings_period = 1U;
  return cfg;
}

[[nodiscard]] std::string tmpdir(const std::string &tag) {
  const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string base = std::string(info != nullptr ? info->name() : "t") + "_" + tag;
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "atx_s6_9_capstone" / base;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

[[nodiscard]] std::vector<DateKey> ascending_dates() {
  std::vector<DateKey> dates(kDates);
  for (usize t = 0; t < kDates; ++t) {
    dates[t] = static_cast<DateKey>(t);
  }
  return dates;
}

[[nodiscard]] std::vector<InstKey> instrument_axis() {
  std::vector<InstKey> insts(kInsts);
  for (usize i = 0; i < kInsts; ++i) {
    insts[i] = static_cast<InstKey>(i);
  }
  return insts;
}

[[nodiscard]] Dataset valid_default_dataset() {
  DatasetSchema s;
  s.columns = {"close", "rev"};
  s.dtypes = {ColumnDType::F64, ColumnDType::F64};
  s.role = Role::Price;
  std::vector<std::vector<f64>> data = {{1.0}, {1.0}};
  auto r = Dataset::create(std::move(s), /*dates=*/{0}, /*instruments=*/{0u}, std::move(data),
                           /*mask=*/{}, DatasetProvenance{"test:default", ""});
  EXPECT_TRUE(r.has_value());
  return std::move(r).value();
}

// A {"close","rev"} Role::Price Dataset over the canonical axis.
[[nodiscard]] Dataset price_dataset(const std::vector<f64> &close, const std::vector<f64> &rev) {
  DatasetSchema s;
  s.columns = {"close", "rev"};
  s.dtypes = {ColumnDType::F64, ColumnDType::F64};
  s.role = Role::Price;
  std::vector<std::vector<f64>> data = {close, rev};
  auto r = Dataset::create(std::move(s), ascending_dates(), instrument_axis(), std::move(data),
                           /*mask=*/{}, DatasetProvenance{"byo:prices", "momentum"});
  if (!r.has_value()) {
    ADD_FAILURE() << "price dataset: " << r.error().message();
    return valid_default_dataset();
  }
  return std::move(r).value();
}

// A Role::Feature Dataset with a "sentiment" column (distinct from price fields, so
// merge_features_into_panel does not error on a name clash). Deterministic content.
[[nodiscard]] Dataset feature_dataset() {
  DatasetSchema s;
  s.columns = {"sentiment"};
  s.dtypes = {ColumnDType::F64};
  s.role = Role::Feature;
  std::vector<f64> sentiment(kDates * kInsts, 0.0);
  for (usize t = 0; t < kDates; ++t) {
    for (usize i = 0; i < kInsts; ++i) {
      sentiment[t * kInsts + i] = static_cast<f64>(kInsts - 1U - i) - static_cast<f64>(t % 3U);
    }
  }
  std::vector<std::vector<f64>> data = {sentiment};
  auto r = Dataset::create(std::move(s), ascending_dates(), instrument_axis(), std::move(data),
                           /*mask=*/{}, DatasetProvenance{"byo:news", "sentiment"});
  if (!r.has_value()) {
    ADD_FAILURE() << "feature dataset: " << r.error().message();
    return valid_default_dataset();
  }
  return std::move(r).value();
}

// A Role::Signal Dataset with a STRONG planted "ext_sig" foresight column: score =
// (kInsts-1-i), HIGHER for the higher-drift instrument (inst 0). Constant in time ->
// a near-constant long-top/short-bottom book that captures the persistent momentum
// spread: positive mean, low turnover -> ADMITS through the live gate.
[[nodiscard]] Dataset signal_dataset() {
  DatasetSchema s;
  s.columns = {"ext_sig"};
  s.dtypes = {ColumnDType::F64};
  s.role = Role::Signal;
  std::vector<f64> ext_sig(kDates * kInsts, 0.0);
  for (usize t = 0; t < kDates; ++t) {
    for (usize i = 0; i < kInsts; ++i) {
      ext_sig[t * kInsts + i] = static_cast<f64>(kInsts - 1U - i);
    }
  }
  std::vector<std::vector<f64>> data = {ext_sig};
  auto r = Dataset::create(std::move(s), ascending_dates(), instrument_axis(), std::move(data),
                           /*mask=*/{}, DatasetProvenance{"external:sentiment_v1", "foresight"});
  if (!r.has_value()) {
    ADD_FAILURE() << "signal dataset: " << r.error().message();
    return valid_default_dataset();
  }
  return std::move(r).value();
}

// A valid SPD BYO factor model sized to the universe (M == kInsts). Two factors:
// an all-ones market exposure + a graded slope; F is a 2x2 SPD; D is positive.
[[nodiscard]] FactorModelArtifact byo_artifact() {
  constexpr Eigen::Index m = static_cast<Eigen::Index>(kInsts);
  FactorModelArtifact a;
  a.X = linalg::MatX::Zero(m, 2);
  for (Eigen::Index i = 0; i < m; ++i) {
    a.X(i, 0) = 1.0;                                            // market
    a.X(i, 1) = static_cast<f64>(i) / static_cast<f64>(kInsts); // graded slope
  }
  a.F = linalg::MatX::Zero(2, 2);
  a.F(0, 0) = 0.04;
  a.F(1, 1) = 0.02;
  a.F(0, 1) = 0.005;
  a.F(1, 0) = 0.005; // symmetric, diagonally dominant -> SPD
  a.D = linalg::VecX::Constant(m, 0.01);
  a.fit_begin = 0;
  a.fit_end = 1;
  return a;
}

// Build the ONE catalog carrying all four plugs (price + feature + signal), then a
// DataContext with the BYO factor override attached. Deterministic — the same arrays
// build an identical context every call (the determinism re-run depends on this).
[[nodiscard]] DatasetCatalog build_catalog(const std::vector<f64> &close,
                                           const std::vector<f64> &rev) {
  DatasetCatalog catalog;
  EXPECT_TRUE(catalog.register_dataset("prices", price_dataset(close, rev)).has_value());
  EXPECT_TRUE(catalog.register_dataset("sentiment", feature_dataset()).has_value());
  EXPECT_TRUE(catalog.register_dataset("signal", signal_dataset()).has_value());
  return catalog;
}

} // namespace

// =============================================================================
//  ByoPriceSignalFactorRunsDeterministic — the four-plug capstone + determinism.
// =============================================================================
TEST(DataE2EByoCapstone, ByoPriceSignalFactorRunsDeterministic) {
  const std::vector<f64> close = momentum_close(kDates, kInsts, 0xA11Cu);
  const std::vector<f64> rev = rev_from_close(kDates, kInsts, close);

  // Shared spine across both runs.
  Library dsl{};
  const ExecutionSimulator sim = frictionless_sim();
  const WeightPolicy policy{};
  const AlphaGate gate{default_gate_cfg()};
  const PipelineConfig cfg = real_pipeline_cfg(/*seed*/ 21);

  // ------------------------------------------------------------------ RUN 1
  const DatasetCatalog catalog1 = build_catalog(close, rev);
  auto ctx1_r = DataContext::create(catalog1, "prices"); // EMPTY adv_windows => raw lowering
  ASSERT_TRUE(ctx1_r.has_value()) << (ctx1_r ? "" : ctx1_r.error().to_string());
  DataContext ctx1 = std::move(ctx1_r).value();
  ctx1.set_factor_model(byo_artifact());

  // (ii) the feature is used — the merged panel resolves the "sentiment" field.
  auto panel1_r = ctx1.price_panel();
  ASSERT_TRUE(panel1_r.has_value()) << (panel1_r ? "" : panel1_r.error().to_string());
  const Panel &panel1 = panel1_r.value().get();
  EXPECT_TRUE(panel1.field_id("close").has_value());
  EXPECT_TRUE(panel1.field_id("rev").has_value());
  EXPECT_TRUE(panel1.field_id("sentiment").has_value())
      << "the BYO feature must be merged into the price panel";

  // (iii) the BYO factor model is present (override path).
  auto ovr1 = ctx1.factor_model_override();
  ASSERT_TRUE(ovr1.has_value()) << (ovr1 ? "" : ovr1.error().to_string());
  EXPECT_TRUE(ovr1.value().has_value()) << "the BYO factor override must be present";

  // (i) the external signal yields >= 1 admitted candidate (admission via the live gate).
  const usize admit_as_of = cfg.schedule.periods.front();
  auto cands1 = ctx1.signal_admit_candidates(sim, policy, admit_as_of);
  ASSERT_TRUE(cands1.has_value()) << (cands1 ? "" : cands1.error().to_string());
  ASSERT_GE(cands1.value().size(), usize{1}) << "the BYO signal must yield >= 1 candidate";

  lib::Library lib1 = lib::Library::open(tmpdir("run1"), default_gate_cfg(), {kLibSeed});
  BookPipeline pipe1{lib1, dsl, panel1, sim, policy, gate};
  const auto rep1_r = pipe1.run_with_context(ctx1, cfg);
  ASSERT_TRUE(rep1_r.has_value()) << (rep1_r ? "" : rep1_r.error().to_string());
  const PipelineReport &rep1 = rep1_r.value();

  // (i) the external signal ACTUALLY admitted into the live library — n_alphas grew
  // beyond the mined count. Compare against a control run with NO signal dataset.
  const u64 n_with_signal = lib1.n_alphas();

  // (iii) the override drove optimize — the override path bypasses dead-augmentation,
  // so dead_factors == 0 is the observable signature.
  EXPECT_EQ(rep1.dead_factors, usize{0})
      << "BYO factor override must bypass dead-factor augmentation (dead_factors == 0)";

  // ------------------------------------------------------------------ RUN 2 (FRESH)
  // Rebuild a fully fresh deterministic catalog + context + library and re-run.
  const DatasetCatalog catalog2 = build_catalog(close, rev);
  auto ctx2_r = DataContext::create(catalog2, "prices");
  ASSERT_TRUE(ctx2_r.has_value()) << (ctx2_r ? "" : ctx2_r.error().to_string());
  DataContext ctx2 = std::move(ctx2_r).value();
  ctx2.set_factor_model(byo_artifact());

  auto panel2_r = ctx2.price_panel();
  ASSERT_TRUE(panel2_r.has_value()) << (panel2_r ? "" : panel2_r.error().to_string());
  const Panel &panel2 = panel2_r.value().get();

  lib::Library lib2 = lib::Library::open(tmpdir("run2"), default_gate_cfg(), {kLibSeed});
  BookPipeline pipe2{lib2, dsl, panel2, sim, policy, gate};
  const auto rep2_r = pipe2.run_with_context(ctx2, cfg);
  ASSERT_TRUE(rep2_r.has_value()) << (rep2_r ? "" : rep2_r.error().to_string());
  const PipelineReport &rep2 = rep2_r.value();

  // (iv) DETERMINISTIC — fresh libraries, identical digests.
  EXPECT_EQ(rep1.book_digest, rep2.book_digest)
      << "book_digest must be byte-identical across fresh BYO runs";
  EXPECT_EQ(rep1.report_digest, rep2.report_digest)
      << "report_digest must be byte-identical across fresh BYO runs";
  EXPECT_EQ(rep1.research.digest, rep2.research.digest);
  EXPECT_NE(rep1.book_digest, u64{0}) << "the run did real work (non-vacuous)";
  EXPECT_EQ(lib1.n_alphas(), lib2.n_alphas());

  // ----------------------------------------------------------- CONTROL (no signal)
  // (i, non-vacuous) a catalog with NO signal dataset admits FEWER alphas — proving
  // the planted external signal genuinely cleared the gate and grew the library.
  {
    DatasetCatalog control;
    ASSERT_TRUE(control.register_dataset("prices", price_dataset(close, rev)).has_value());
    auto ctxc_r = DataContext::create(control, "prices");
    ASSERT_TRUE(ctxc_r.has_value());
    DataContext ctxc = std::move(ctxc_r).value();
    ctxc.set_factor_model(byo_artifact());
    auto panelc_r = ctxc.price_panel();
    ASSERT_TRUE(panelc_r.has_value());
    const Panel &panelc = panelc_r.value().get();

    lib::Library libc = lib::Library::open(tmpdir("control"), default_gate_cfg(), {kLibSeed});
    BookPipeline pipec{libc, dsl, panelc, sim, policy, gate};
    const auto repc_r = pipec.run_with_context(ctxc, cfg);
    ASSERT_TRUE(repc_r.has_value()) << (repc_r ? "" : repc_r.error().to_string());
    EXPECT_GT(n_with_signal, libc.n_alphas())
        << "the planted external signal must admit and grow the library beyond the "
           "no-signal control";
  }
}

} // namespace atxtest_data_e2e_byo_capstone_test
