// data_boundary_pin_test.cpp — S6.8 THE BOUNDARY PIN (suite DataBoundaryPin).
//
// The sprint's non-negotiable non-regression anchor: a PRICE-ONLY DataContext
// driving BookPipeline::run_with_context must produce a BYTE-IDENTICAL book_digest
// AND report_digest to today's legacy BookPipeline::run over the SAME hand-built
// fixed Panel. We build ONE set of close+rev arrays, lower them BOTH ways:
//   * legacy  : Panel::create({"close","rev"}, ...) -> BookPipeline::run(cfg)
//   * context : Dataset(Role::Price, {"close","rev"}) -> catalog -> DataContext
//               (EMPTY adv_windows => RAW lowering) -> price_panel() ->
//               BookPipeline::run_with_context(ctx, cfg)
// We FIRST assert the two Panels are byte-identical (sanity), THEN assert both
// digests are byte-equal. The pipeline does REAL work (mine + admit + optimize)
// via real_pipeline_cfg, so the pin is non-vacuous.
//
// Fixture builders (momentum_close, the rev formula, real_pipeline_cfg, the
// sim/policy/gate/dsl setup) are COPIED file-static from book_pipeline_test.cpp.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

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

#include "atx/engine/risk/multi_period.hpp" // RebalanceSchedule, MultiPeriodConfig

#include "atx/engine/data/catalog.hpp"
#include "atx/engine/data/context.hpp"
#include "atx/engine/data/dataset.hpp"
#include "atx/engine/data/dataset_schema.hpp"

#include "atx/engine/book/pipeline.hpp" // BookPipeline (run + run_with_context)

namespace atxtest_data_boundary_pin_test {

using atx::f64;
using atx::u32;
using atx::u64;
using atx::usize;
using atx::engine::WeightPolicy;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::book::BookPipeline;
using atx::engine::book::PipelineConfig;
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

namespace lib = atx::engine::library;

// ---- builders (mirrored from book_pipeline_test.cpp) ------------------------
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

constexpr usize kDates = 120U;
constexpr usize kInsts = 8U;
constexpr u64 kLibSeed = 0xC0FFEEu;
constexpr f64 kMinDsr = 0.5;

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

[[nodiscard]] ResearchConfig real_signal_research_cfg(u64 master_seed, usize max_runs,
                                                      usize patience) {
  ResearchConfig cfg;
  cfg.per_run = per_run_cfg(16, 4);
  cfg.max_runs = max_runs;
  cfg.patience = patience;
  cfg.master_seed = master_seed;
  return cfg;
}

[[nodiscard]] RebalanceSchedule book_schedule() {
  return RebalanceSchedule{{60U, 70U, 80U, 90U, 100U}};
}

[[nodiscard]] PipelineConfig real_pipeline_cfg(u64 research_seed) {
  PipelineConfig cfg;
  cfg.research = real_signal_research_cfg(research_seed, /*max_runs*/ 2, /*patience*/ 0);
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
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "atx_s6_8_pin" / base;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

// A {"close","rev"} Role::Price Dataset over ascending DateKeys 0..kDates-1, InstKeys
// 0..kInsts-1, EMPTY mask — the same data the legacy Panel::create receives.
[[nodiscard]] Dataset price_dataset(const std::vector<f64> &close, const std::vector<f64> &rev) {
  DatasetSchema s;
  s.columns = {"close", "rev"};
  s.dtypes = {ColumnDType::F64, ColumnDType::F64};
  s.role = Role::Price;
  std::vector<DateKey> dates(kDates);
  for (usize t = 0; t < kDates; ++t) {
    dates[t] = static_cast<DateKey>(t);
  }
  std::vector<InstKey> insts(kInsts);
  for (usize i = 0; i < kInsts; ++i) {
    insts[i] = static_cast<InstKey>(i);
  }
  std::vector<std::vector<f64>> data = {close, rev};
  auto r = Dataset::create(std::move(s), std::move(dates), std::move(insts), std::move(data),
                           /*mask=*/{}, DatasetProvenance{"pin:prices", ""});
  EXPECT_TRUE(r.has_value()) << "price dataset must build";
  return std::move(r).value();
}

// Byte-compare two panels (NaN-safe): field names, shape, and every cell.
void expect_panels_byte_identical(const Panel &a, const Panel &b) {
  ASSERT_EQ(a.dates(), b.dates());
  ASSERT_EQ(a.instruments(), b.instruments());
  ASSERT_EQ(a.num_fields(), b.num_fields());
  for (usize f = 0; f < a.num_fields(); ++f) {
    EXPECT_EQ(a.field_name(static_cast<u32>(f)), b.field_name(static_cast<u32>(f)));
    const std::span<const f64> ca = a.field_all(static_cast<u32>(f));
    const std::span<const f64> cb = b.field_all(static_cast<u32>(f));
    ASSERT_EQ(ca.size(), cb.size());
    for (usize k = 0; k < ca.size(); ++k) {
      const bool a_nan = ca[k] != ca[k];
      const bool b_nan = cb[k] != cb[k];
      ASSERT_EQ(a_nan, b_nan) << "NaN mismatch at field " << f << " cell " << k;
      if (!a_nan && !b_nan) {
        ASSERT_EQ(ca[k], cb[k]) << "value mismatch at field " << f << " cell " << k;
      }
    }
  }
}

} // namespace

// =============================================================================
//  PriceOnlyContextDigestEqualsLegacy — THE PIN.
// =============================================================================
TEST(DataBoundaryPin, PriceOnlyContextDigestEqualsLegacy) {
  // ONE set of close+rev arrays drives BOTH paths.
  const std::vector<f64> close = momentum_close(kDates, kInsts, 0xA11Cu);
  const std::vector<f64> rev = rev_from_close(kDates, kInsts, close);

  // Shared spine: same dsl / sim / policy / gate / cfg / seed across both pipelines.
  Library dsl{};
  const ExecutionSimulator sim = frictionless_sim();
  const WeightPolicy policy{};
  const AlphaGate gate{default_gate_cfg()};
  const PipelineConfig cfg = real_pipeline_cfg(/*seed*/ 21);

  // ---- Legacy path: hand-built fixed Panel + run() ----
  auto legacy_r = Panel::create(kDates, kInsts, {"close", "rev"}, {close, rev}, {});
  ASSERT_TRUE(legacy_r.has_value());
  const Panel legacy_panel = std::move(legacy_r).value();
  lib::Library libA = lib::Library::open(tmpdir("a"), default_gate_cfg(), {kLibSeed});
  BookPipeline pa{libA, dsl, legacy_panel, sim, policy, gate};
  const auto ra = pa.run(cfg);
  ASSERT_TRUE(ra.has_value()) << (ra ? "" : ra.error().to_string());

  // ---- Context path: Dataset -> catalog -> DataContext (RAW lowering) + run_with_context ----
  DatasetCatalog catalog;
  ASSERT_TRUE(catalog.register_dataset("prices", price_dataset(close, rev)).has_value());
  auto ctx_r = DataContext::create(catalog, "prices"); // EMPTY adv_windows => raw lowering
  ASSERT_TRUE(ctx_r.has_value()) << (ctx_r ? "" : ctx_r.error().to_string());
  DataContext ctx = std::move(ctx_r).value();

  auto ctx_panel_r = ctx.price_panel();
  ASSERT_TRUE(ctx_panel_r.has_value()) << (ctx_panel_r ? "" : ctx_panel_r.error().to_string());
  const Panel &ctx_panel = ctx_panel_r.value().get();

  // SANITY: the two panels are byte-identical BEFORE running the pipeline.
  expect_panels_byte_identical(legacy_panel, ctx_panel);

  lib::Library libB = lib::Library::open(tmpdir("b"), default_gate_cfg(), {kLibSeed});
  BookPipeline pb{libB, dsl, ctx_panel, sim, policy, gate};
  const auto rb = pb.run_with_context(ctx, cfg);
  ASSERT_TRUE(rb.has_value()) << (rb ? "" : rb.error().to_string());

  // THE PIN: byte-identical realized book chain AND report series.
  EXPECT_EQ(ra->book_digest, rb->book_digest)
      << "book_digest must be byte-identical (boundary pin)";
  EXPECT_EQ(ra->report_digest, rb->report_digest)
      << "report_digest must be byte-identical (boundary pin)";
  // Non-vacuity: the pipeline did real work (a non-empty book chain, a real fold).
  EXPECT_EQ(ra->report.pnl_gross.size(), book_schedule().periods.size());
  EXPECT_NE(ra->book_digest, 0u);
  // The price-only context admitted ZERO external signals and supplied NO override, so
  // the research summaries must also match (it ran the identical mine+admit).
  EXPECT_EQ(ra->research.digest, rb->research.digest);
  EXPECT_EQ(rb->dead_factors, ra->dead_factors);
}

} // namespace atxtest_data_boundary_pin_test
