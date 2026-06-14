// atx::engine::data — DataContext unit tests (P2-S6.8).
//
// Suite: DataContext
//
// DataContext is the data-layer facade the BookPipeline consumes. These tests pin
// the THREE accessor contracts independent of the pipeline:
//   * PricePanelLowersFromCatalog    — a price-only context's price_panel() is
//       byte-identical to a direct Panel::create from the same columns.
//   * FactorOverridePresentWhenArtifact — set_factor_model => factor_model_override()
//       returns a value; without it => nullopt.
//   * SignalCandidatesAdmitBeforeMine — a Role::Signal dataset => >=1 candidate with
//       external provenance; no Signal dataset => empty span.

#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"

#include "atx/engine/data/catalog.hpp"
#include "atx/engine/data/context.hpp"
#include "atx/engine/data/dataset.hpp"
#include "atx/engine/data/dataset_schema.hpp"
#include "atx/engine/data/factor_model_artifact.hpp"

#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace atxtest_data_context_test {

using atx::f64;
using atx::u16;
using atx::u32;
using atx::usize;
using atx::engine::WeightPolicy;
using atx::engine::alpha::Panel;
using atx::engine::data::ColumnDType;
using atx::engine::data::DataContext;
using atx::engine::data::Dataset;
using atx::engine::data::DatasetCatalog;
using atx::engine::data::DatasetProvenance;
using atx::engine::data::DatasetSchema;
using atx::engine::data::DateKey;
using atx::engine::data::FactorModelArtifact;
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

namespace linalg = atx::core::linalg;

// ---------------------------------------------------------------------------
//  Fixture builders.
// ---------------------------------------------------------------------------
namespace {

constexpr usize kDates = 12;
constexpr usize kInsts = 4;

[[nodiscard]] ExecutionSimulator frictionless_sim() {
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.1, 0.5, 0.05},
                            CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
                            LatencyCfg{},
                            VolumeCapCfg{1.0}};
}

// A deterministic close column (date-major, kDates*kInsts) with a per-instrument drift.
[[nodiscard]] std::vector<f64> close_col() {
  std::vector<f64> close(kDates * kInsts, 0.0);
  for (usize i = 0; i < kInsts; ++i) {
    f64 px = 100.0;
    for (usize t = 0; t < kDates; ++t) {
      px *= (1.0 + 0.01 - 0.003 * static_cast<f64>(i));
      close[t * kInsts + i] = px;
    }
  }
  return close;
}

// rev = -(close_t/close_{t-1} - 1), the two_field_panel convention.
[[nodiscard]] std::vector<f64> rev_col(const std::vector<f64> &close) {
  std::vector<f64> rev(kDates * kInsts, 0.0);
  for (usize t = 1; t < kDates; ++t) {
    for (usize i = 0; i < kInsts; ++i) {
      const f64 prev = close[(t - 1) * kInsts + i];
      rev[t * kInsts + i] = -(close[t * kInsts + i] / prev - 1.0);
    }
  }
  return rev;
}

// A {"close","rev"} Role::Price Dataset over ascending DateKeys 0..kDates-1, InstKeys 0..kInsts-1.
[[nodiscard]] Dataset price_dataset(const std::vector<f64> &close, const std::vector<f64> &rev) {
  DatasetSchema s;
  s.columns = {"close", "rev"};
  s.dtypes = {ColumnDType::F64, ColumnDType::F64};
  s.role = Role::Price;
  std::vector<DateKey> dates(kDates);
  for (usize t = 0; t < kDates; ++t) {
    dates[t] = static_cast<DateKey>(t);
  }
  std::vector<atx::engine::data::InstKey> insts(kInsts);
  for (usize i = 0; i < kInsts; ++i) {
    insts[i] = static_cast<atx::engine::data::InstKey>(i);
  }
  std::vector<std::vector<f64>> data = {close, rev};
  auto r = Dataset::create(std::move(s), std::move(dates), std::move(insts), std::move(data),
                           /*mask=*/{}, DatasetProvenance{"test:prices", ""});
  EXPECT_TRUE(r.has_value()) << "price dataset must build";
  return std::move(r).value();
}

// A near-foresight Role::Signal Dataset ("score") over the same axis.
[[nodiscard]] Dataset signal_dataset(const std::vector<f64> &rev) {
  DatasetSchema s;
  s.columns = {"score"};
  s.dtypes = {ColumnDType::F64};
  s.role = Role::Signal;
  std::vector<DateKey> dates(kDates);
  for (usize t = 0; t < kDates; ++t) {
    dates[t] = static_cast<DateKey>(t);
  }
  std::vector<atx::engine::data::InstKey> insts(kInsts);
  for (usize i = 0; i < kInsts; ++i) {
    insts[i] = static_cast<atx::engine::data::InstKey>(i);
  }
  // score = next-period rev-rank surrogate: just rev itself (a deterministic column).
  std::vector<std::vector<f64>> data = {rev};
  auto r = Dataset::create(std::move(s), std::move(dates), std::move(insts), std::move(data),
                           /*mask=*/{}, DatasetProvenance{"external:sentiment", ""});
  EXPECT_TRUE(r.has_value()) << "signal dataset must build";
  return std::move(r).value();
}

[[nodiscard]] FactorModelArtifact unit_artifact() {
  FactorModelArtifact a;
  a.X = linalg::MatX::Ones(static_cast<Eigen::Index>(kInsts), 1);       // M×1 all-ones exposure
  a.F = linalg::MatX::Identity(1, 1);                                   // 1×1 SPD
  a.D = linalg::VecX::Constant(static_cast<Eigen::Index>(kInsts), 1.0); // positive specific vars
  a.fit_begin = 0;
  a.fit_end = 1;
  return a;
}

} // namespace

// =============================================================================
//  PricePanelLowersFromCatalog — raw lowering is byte-identical to Panel::create.
// =============================================================================
TEST(DataContext, PricePanelLowersFromCatalog) {
  const std::vector<f64> close = close_col();
  const std::vector<f64> rev = rev_col(close);

  DatasetCatalog catalog;
  ASSERT_TRUE(catalog.register_dataset("prices", price_dataset(close, rev)).has_value());

  auto ctx = DataContext::create(catalog, "prices");
  ASSERT_TRUE(ctx.has_value()) << (ctx ? "" : ctx.error().to_string());

  auto panel_r = ctx->price_panel();
  ASSERT_TRUE(panel_r.has_value()) << (panel_r ? "" : panel_r.error().to_string());
  const Panel &ctx_panel = panel_r.value().get();

  // The reference: a direct Panel::create from the SAME columns + empty mask.
  auto direct = Panel::create(kDates, kInsts, {"close", "rev"}, {close, rev}, {});
  ASSERT_TRUE(direct.has_value());
  const Panel &ref = direct.value();

  ASSERT_EQ(ctx_panel.dates(), ref.dates());
  ASSERT_EQ(ctx_panel.instruments(), ref.instruments());
  ASSERT_EQ(ctx_panel.num_fields(), ref.num_fields());
  for (usize f = 0; f < ref.num_fields(); ++f) {
    EXPECT_EQ(ctx_panel.field_name(static_cast<u32>(f)), ref.field_name(static_cast<u32>(f)));
    const std::span<const f64> a = ctx_panel.field_all(static_cast<u32>(f));
    const std::span<const f64> b = ref.field_all(static_cast<u32>(f));
    ASSERT_EQ(a.size(), b.size());
    for (usize k = 0; k < a.size(); ++k) {
      const bool a_nan = a[k] != a[k];
      const bool b_nan = b[k] != b[k];
      EXPECT_EQ(a_nan, b_nan) << "NaN mismatch at field " << f << " cell " << k;
      if (!a_nan && !b_nan) {
        EXPECT_EQ(a[k], b[k]) << "value mismatch at field " << f << " cell " << k;
      }
    }
  }
}

// =============================================================================
//  FactorOverridePresentWhenArtifact — set_factor_model => value; else nullopt.
// =============================================================================
TEST(DataContext, FactorOverridePresentWhenArtifact) {
  const std::vector<f64> close = close_col();
  const std::vector<f64> rev = rev_col(close);
  DatasetCatalog catalog;
  ASSERT_TRUE(catalog.register_dataset("prices", price_dataset(close, rev)).has_value());

  auto ctx = DataContext::create(catalog, "prices");
  ASSERT_TRUE(ctx.has_value());

  // No artifact set => nullopt.
  auto none = ctx->factor_model_override();
  ASSERT_TRUE(none.has_value()) << (none ? "" : none.error().to_string());
  EXPECT_FALSE(none.value().has_value());

  // Attach an artifact => a lowered FactorModel is present.
  ctx->set_factor_model(unit_artifact());
  auto some = ctx->factor_model_override();
  ASSERT_TRUE(some.has_value()) << (some ? "" : some.error().to_string());
  EXPECT_TRUE(some.value().has_value());
}

// =============================================================================
//  SignalCandidatesAdmitBeforeMine — Signal dataset => >=1 external candidate;
//  no Signal dataset => empty span.
// =============================================================================
TEST(DataContext, SignalCandidatesAdmitBeforeMine) {
  const std::vector<f64> close = close_col();
  const std::vector<f64> rev = rev_col(close);
  const ExecutionSimulator sim = frictionless_sim();
  const WeightPolicy policy{};

  // (a) catalog with a Signal dataset => >=1 candidate, externally flagged.
  {
    DatasetCatalog catalog;
    ASSERT_TRUE(catalog.register_dataset("prices", price_dataset(close, rev)).has_value());
    ASSERT_TRUE(catalog.register_dataset("signal", signal_dataset(rev)).has_value());
    auto ctx = DataContext::create(catalog, "prices");
    ASSERT_TRUE(ctx.has_value());

    auto cands = ctx->signal_admit_candidates(sim, policy, /*as_of*/ kDates - 1U);
    ASSERT_TRUE(cands.has_value()) << (cands ? "" : cands.error().to_string());
    ASSERT_GE(cands.value().size(), 1u) << "a Signal dataset must yield >=1 candidate";
    EXPECT_NE(cands.value()[0].prov.expr_source.find("external"), std::string::npos)
        << "candidate provenance must flag the external source";
  }

  // (b) catalog with NO Signal dataset => empty span.
  {
    DatasetCatalog catalog;
    ASSERT_TRUE(catalog.register_dataset("prices", price_dataset(close, rev)).has_value());
    auto ctx = DataContext::create(catalog, "prices");
    ASSERT_TRUE(ctx.has_value());

    auto cands = ctx->signal_admit_candidates(sim, policy, /*as_of*/ kDates - 1U);
    ASSERT_TRUE(cands.has_value()) << (cands ? "" : cands.error().to_string());
    EXPECT_TRUE(cands.value().empty()) << "no Signal dataset must yield an empty candidate span";
  }
}

} // namespace atxtest_data_context_test
