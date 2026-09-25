// W0-D0 — DataContext rejects a reused as_of instead of returning future-realized
// candidates (D-08).
//
// Suite: DataContextAsOf_W0d0
//
// signal_admit_candidates caches candidates realized as-of the FIRST call. A later
// call with another as_of was guarded by ATX_ASSERT, a no-op in release builds, so
// an earlier as_of silently received candidates realized over later data. It is now
// an Err in every build type. These tests run in whatever configuration the test
// binary was built with and do not depend on assertions being enabled.

#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/data/catalog.hpp"
#include "atx/engine/data/context.hpp"
#include "atx/engine/data/dataset.hpp"
#include "atx/engine/data/dataset_schema.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace atx_test_w0_d0_context_asof {

using atx::f64;
using atx::usize;
using atx::core::ErrorCode;
using atx::engine::WeightPolicy;
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

[[nodiscard]] Dataset make(Role role, std::vector<std::string> columns,
                           std::vector<std::vector<f64>> data, const char *source) {
  DatasetSchema s;
  s.dtypes.assign(columns.size(), ColumnDType::F64);
  s.columns = std::move(columns);
  s.role = role;
  std::vector<DateKey> dates(kDates);
  for (usize t = 0; t < kDates; ++t) {
    dates[t] = static_cast<DateKey>(t);
  }
  std::vector<InstKey> insts(kInsts);
  for (usize i = 0; i < kInsts; ++i) {
    insts[i] = static_cast<InstKey>(i);
  }
  auto r = Dataset::create(std::move(s), std::move(dates), std::move(insts), std::move(data), {},
                           DatasetProvenance{source, ""});
  EXPECT_TRUE(r.has_value());
  return std::move(r).value();
}

// A catalog with a price and a signal dataset (close drifts per instrument; the
// signal is the one-period reversal, as in data_context_test).
[[nodiscard]] DatasetCatalog catalog_with_signal() {
  std::vector<f64> close(kDates * kInsts);
  std::vector<f64> rev(kDates * kInsts, 0.0);
  for (usize i = 0; i < kInsts; ++i) {
    f64 px = 100.0;
    for (usize t = 0; t < kDates; ++t) {
      px *= (1.0 + 0.01 - 0.003 * static_cast<f64>(i));
      close[t * kInsts + i] = px;
      if (t > 0) {
        rev[t * kInsts + i] = -(px / close[(t - 1) * kInsts + i] - 1.0);
      }
    }
  }
  DatasetCatalog catalog;
  EXPECT_TRUE(catalog.register_dataset("prices", make(Role::Price, {"close", "rev"},
                                                      {close, rev}, "test:prices"))
                  .has_value());
  EXPECT_TRUE(
      catalog.register_dataset("signal", make(Role::Signal, {"score"}, {rev}, "external:sig"))
          .has_value());
  return catalog;
}

} // namespace

// An EARLIER as_of on a reused context is an error, not the cached (later) epoch.
TEST(DataContextAsOf_W0d0, EarlierAsOfOnAReusedContextReturnsErr) {
  const DatasetCatalog catalog = catalog_with_signal();
  const ExecutionSimulator sim = frictionless_sim();
  const WeightPolicy policy{};
  auto ctx = DataContext::create(catalog, "prices");
  ASSERT_TRUE(ctx.has_value());

  auto late = ctx->signal_admit_candidates(sim, policy, kDates - 1U);
  ASSERT_TRUE(late.has_value()) << late.error().to_string();
  const usize n_late = late->size();
  ASSERT_GE(n_late, 1U);

  auto early = ctx->signal_admit_candidates(sim, policy, 5U);
  ASSERT_FALSE(early.has_value()) << "an earlier as_of must not get the later epoch's candidates";
  EXPECT_EQ(early.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(early.error().to_string().find("as_of"), std::string::npos);

  // A later as_of is rejected too; the cache itself is untouched.
  auto later = ctx->signal_admit_candidates(sim, policy, kDates + 3U);
  ASSERT_FALSE(later.has_value());
  auto again = ctx->signal_admit_candidates(sim, policy, kDates - 1U);
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(again->size(), n_late);
}

// A fresh context for the earlier as_of works (the supported walk-forward pattern).
TEST(DataContextAsOf_W0d0, FreshContextServesTheEarlierAsOf) {
  const DatasetCatalog catalog = catalog_with_signal();
  const ExecutionSimulator sim = frictionless_sim();
  const WeightPolicy policy{};
  auto ctx = DataContext::create(catalog, "prices");
  ASSERT_TRUE(ctx.has_value());
  auto early = ctx->signal_admit_candidates(sim, policy, 5U);
  ASSERT_TRUE(early.has_value()) << early.error().to_string();
  EXPECT_GE(early->size(), 1U);
}

// A moved-from context reports an error from every Result accessor instead of
// dereferencing a null catalog.
TEST(DataContextAsOf_W0d0, MovedFromContextReturnsErrInsteadOfCrashing) {
  const DatasetCatalog catalog = catalog_with_signal();
  auto ctx = DataContext::create(catalog, "prices");
  ASSERT_TRUE(ctx.has_value());
  DataContext moved_to = std::move(ctx).value();
  DataContext &moved_from = *ctx; // NOLINT(bugprone-use-after-move): the contract under test
  const ExecutionSimulator sim = frictionless_sim();
  EXPECT_FALSE(moved_from.price_panel().has_value());
  EXPECT_FALSE(moved_from.signal_admit_candidates(sim, WeightPolicy{}, 1U).has_value());
  EXPECT_FALSE(moved_from.reference_spans_at(DateKey{1}).has_value());
  EXPECT_TRUE(moved_to.price_panel().has_value());
}

} // namespace atx_test_w0_d0_context_asof
