// risk_w0r0_sector_columns_test.cpp — W0-R0 (R-04): per-date sector columns are
// mapped onto the model's X[0] columns by group id, never by position.
//
// Pre-W0 the WLS pass copied fit->beta[c] for c in [0, K) with K = X[0]'s column count,
// so a date whose cross-section lacked a group (K_s < K) read past the end of beta
// (UB), and a date carrying a group X[0] lacks shifted every later column onto the
// wrong factor. The corrected passes solve each date with its own K_s and scatter the
// coefficients by identity (detail::map_columns).
//
// Sanitizer note (brief lane notes): this repo has no UBSan/ASan preset. The OOB proof
// is the Debug build's live checked indexing — Eigen's eigen_assert on operator[] and
// ATX_ASSERT — shown to abort on the exact pre-W0 access (CheckedIndexingCatchesThe
// PreW0Read), plus explicit shape/index assertions on the corrected path.
//
// Suite: RiskSectorColumnsById.

#include <cmath>
#include <cstdio>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/linalg/regression.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/risk/exposures.hpp"
#include "atx/engine/risk/factor_model.hpp"

#include "risk_w0r0_fixture.hpp"

namespace atx_test_w0_r0_sector_columns {

using atx::f64;
using atx::u32;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::risk::ColumnTag;
using atx::engine::risk::FactorModelBuilder;
using atx::engine::risk::FactorModelConfig;
using atx::engine::risk::PitSideInputs;
using atx::engine::risk::StyleFactor;
using atx_test_w0_r0_fixture::closes_from_returns;
using atx_test_w0_r0_fixture::Grid;
using atx_test_w0_r0_fixture::kNaN;
using atx_test_w0_r0_fixture::Rng;
using atx_test_w0_r0_fixture::StorePanel;

[[nodiscard]] FactorModelConfig sectors_only() {
  FactorModelConfig cfg;
  cfg.sector_factors = true;
  cfg.style_mask = 0U;
  return cfg;
}

// Residual-free sector DGP: r_{i,s} = f[group(i, s)][s], group(i, s) = the group of
// name i at the EXPOSURE row s+1 (`grp_at(row, i)`). Returns (close, f) where
// f[g][s] is the planted factor return of group id g at date s.
struct SectorPanel {
  Grid close;
  Grid volume;
  std::vector<std::vector<f64>> f; // [group id][date]
};
template <class GroupAt>
[[nodiscard]] SectorPanel sector_panel(usize window, usize n_inst, u32 n_groups, GroupAt grp_at,
                                       atx::u64 seed) {
  Rng rng{seed};
  SectorPanel out;
  out.f.assign(n_groups, std::vector<f64>(window));
  for (auto &fg : out.f) {
    for (f64 &v : fg) {
      v = 0.015 * rng.normal();
    }
  }
  Grid ret(window, std::vector<f64>(n_inst));
  for (usize s = 0; s < window; ++s) {
    for (usize i = 0; i < n_inst; ++i) {
      ret[s][i] = out.f[grp_at(s + 1U, i)][s];
    }
  }
  out.close = closes_from_returns(ret);
  out.volume.assign(window + 1U, std::vector<f64>(n_inst, 1.0e4));
  return out;
}

[[nodiscard]] usize col_of_group(const std::vector<ColumnTag> &cols, u32 g) {
  for (usize c = 0; c < cols.size(); ++c) {
    if (cols[c].kind == ColumnTag::Kind::Sector && cols[c].group_id == g) {
      return c;
    }
  }
  return cols.size();
}

// ===========================================================================
//  Acceptance: a group missing at s > 0 builds with no out-of-bounds access, and
//  every factor-return column is the planted return of ITS group.
//  Groups 1 (names 0-2), 2 (3-5), 3 (6-8); group 3's names list at row L = 10 (NaN
//  before), so dates s >= L have only groups {1, 2} (K_s = 2 < K = 3).
// ===========================================================================
TEST(RiskSectorColumnsById, GroupMissingAtOlderDatesBuildsAndMapsById) {
  const usize window = 30U;
  const usize n_inst = 9U;
  const usize listed = 10U;
  auto grp = [](usize /*row*/, usize i) { return static_cast<u32>(1U + i / 3U); };
  SectorPanel p = sector_panel(window, n_inst, 4U, grp, 101U);
  for (usize r = listed + 1U; r <= window; ++r) {
    for (usize i = 6U; i < 9U; ++i) {
      p.close[r][i] = kNaN; // not yet listed
    }
  }
  const StorePanel sp{p.close, p.volume};
  std::vector<u32> gid(n_inst);
  for (usize i = 0; i < n_inst; ++i) {
    gid[i] = grp(0U, i);
  }
  const FactorModelBuilder builder{sectors_only()};
  const PitSideInputs side = PitSideInputs::broadcast({}, std::span<const u32>{gid});

  const auto fr = builder.factor_returns(sp.view(), window, side);
  ASSERT_TRUE(fr.has_value()) << fr.error().to_string();
  // Explicit shape / index assertions (the OOB guard on the corrected path).
  ASSERT_EQ(fr->f.cols(), 3);
  ASSERT_EQ(fr->columns.size(), 3U);
  ASSERT_EQ(fr->missing.size(), 3U);
  ASSERT_EQ(static_cast<usize>(fr->f.rows()), fr->dates.size());
  ASSERT_EQ(fr->dates.size(), window); // every date usable (K_s <= M_s)
  usize older_dates = 0U;
  for (usize u = 0; u < fr->dates.size(); ++u) {
    const usize s = fr->dates[u];
    ASSERT_LT(s, window);
    for (u32 g = 1U; g <= 3U; ++g) {
      const usize c = col_of_group(fr->columns, g);
      ASSERT_LT(c, 3U);
      const f64 want = (g == 3U && s >= listed) ? 0.0 : p.f[g][s];
      EXPECT_NEAR(fr->f(static_cast<Eigen::Index>(u), static_cast<Eigen::Index>(c)), want, 1e-10)
          << "s=" << s << " g=" << g;
    }
    older_dates += (s >= listed) ? 1U : 0U;
  }
  EXPECT_EQ(older_dates, window - listed);
  EXPECT_EQ(fr->missing[col_of_group(fr->columns, 3U)], window - listed);
  EXPECT_EQ(fr->missing[col_of_group(fr->columns, 1U)], 0U);

  // The full build (F, D) succeeds and F is a finite symmetric 3×3.
  const auto comp = builder.build_components(sp.view(), window, side);
  ASSERT_TRUE(comp.has_value()) << comp.error().to_string();
  ASSERT_EQ(comp->F.rows(), 3);
  ASSERT_EQ(comp->F.cols(), 3);
  EXPECT_TRUE(comp->F.allFinite());
  EXPECT_TRUE(comp->F.isApprox(comp->F.transpose()));
  const auto model = builder.build(sp.view(), window, side);
  EXPECT_TRUE(model.has_value()) << (model ? "" : model.error().to_string());
  std::printf("[W0-R0 evidence] group-missing build: K=3, dates=%zu, group-3 missing on %zu "
              "dates, max|f - planted| checked at 1e-10\n",
              fr->dates.size(), fr->missing[col_of_group(fr->columns, 3U)]);
}

// The exact pre-W0 access on this fixture's older date — beta has K_s = 2 entries and
// the old loop read beta[2] — aborts under this build's checked indexing. This is the
// sanitizer substitute: the corrected path above ran the same dates to completion.
TEST(RiskSectorColumnsById, CheckedIndexingCatchesThePreW0Read) {
#ifdef NDEBUG
  GTEST_SKIP() << "checked indexing is a Debug-build property";
#else
  const usize window = 30U;
  const usize n_inst = 9U;
  auto grp = [](usize /*row*/, usize i) { return static_cast<u32>(1U + i / 3U); };
  SectorPanel p = sector_panel(window, n_inst, 4U, grp, 101U);
  for (usize r = 11U; r <= window; ++r) {
    for (usize i = 6U; i < 9U; ++i) {
      p.close[r][i] = kNaN;
    }
  }
  const StorePanel sp{p.close, p.volume};
  std::vector<u32> gid{1U, 1U, 1U, 2U, 2U, 2U, 3U, 3U, 3U};
  const FactorModelBuilder builder{sectors_only()};
  const auto xs = builder.regression_exposures(
      sp.view(), 20U, PitSideInputs::broadcast({}, std::span<const u32>{gid}));
  ASSERT_TRUE(xs.has_value());
  ASSERT_EQ(xs->n_factors(), 2U); // K_s = 2 at the older date
  VecX r(static_cast<Eigen::Index>(xs->n_instruments()));
  for (usize j = 0; j < xs->n_instruments(); ++j) {
    r[static_cast<Eigen::Index>(j)] = 0.01 * static_cast<f64>(j);
  }
  const auto fit = atx::core::linalg::ols(xs->x, r);
  ASSERT_TRUE(fit.has_value());
  const usize k_model = 3U;
  auto pre_w0_copy = [&]() {
    f64 sink = 0.0;
    for (usize c = 0; c < k_model; ++c) { // the pre-W0 loop bound (X[0]'s K)
      sink += fit->beta[static_cast<Eigen::Index>(c)];
    }
    std::printf("%f\n", sink);
  };
  EXPECT_DEATH(pre_w0_copy(), ".*");
#endif
}

// ===========================================================================
//  A group that existed at older dates but not today (all its names delisted) is
//  regressed on at those dates but not carried. Its id (0) sorts FIRST, so the pre-W0
//  positional copy would have written group 0's return into group 1's column.
// ===========================================================================
TEST(RiskSectorColumnsById, VanishedGroupIsRegressedButNotCarried) {
  const usize window = 24U;
  const usize n_inst = 9U;
  const usize delisted = 5U; // names 0-2 (group 0) have NaN closes at rows 0..4
  auto grp = [](usize /*row*/, usize i) { return static_cast<u32>(i / 3U); };
  SectorPanel p = sector_panel(window, n_inst, 3U, grp, 202U);
  for (usize r = 0; r < delisted; ++r) {
    for (usize i = 0; i < 3U; ++i) {
      p.close[r][i] = kNaN;
    }
  }
  const StorePanel sp{p.close, p.volume};
  std::vector<u32> gid(n_inst);
  for (usize i = 0; i < n_inst; ++i) {
    gid[i] = grp(0U, i);
  }
  const FactorModelBuilder builder{sectors_only()};
  const auto fr = builder.factor_returns(sp.view(), window,
                                         PitSideInputs::broadcast({}, std::span<const u32>{gid}));
  ASSERT_TRUE(fr.has_value()) << fr.error().to_string();
  ASSERT_EQ(fr->f.cols(), 2); // X[0] carries groups {1, 2} only
  EXPECT_EQ(fr->columns[0].group_id, 1U);
  EXPECT_EQ(fr->columns[1].group_id, 2U);
  usize with_group0 = 0U;
  for (usize u = 0; u < fr->dates.size(); ++u) {
    const usize s = fr->dates[u];
    EXPECT_NEAR(fr->f(static_cast<Eigen::Index>(u), 0), p.f[1][s], 1e-10) << "s=" << s;
    EXPECT_NEAR(fr->f(static_cast<Eigen::Index>(u), 1), p.f[2][s], 1e-10) << "s=" << s;
    with_group0 += (s >= delisted) ? 1U : 0U;
  }
  EXPECT_GE(with_group0, window - delisted - 1U); // the older dates DID include group 0
  EXPECT_EQ(fr->missing[0], 0U);
  EXPECT_EQ(fr->missing[1], 0U);
}

// ===========================================================================
//  R-04 + R-06: with point-in-time groups, a name that switched sector is regressed
//  on its group AT THAT DATE; the static broadcast mislabels it at older dates.
//  Measured: planted-return recovery error, PIT vs broadcast.
// ===========================================================================
TEST(RiskSectorColumnsById, PitGroupReassignmentRecoversPlantedReturns) {
  const usize window = 40U;
  const usize n_inst = 12U;
  const usize switch_row = 15U; // name 0 is in group 1 at rows > 15, group 2 at rows <= 15
  auto grp = [&](usize row, usize i) -> u32 {
    if (i == 0U) {
      return (row > switch_row) ? 1U : 2U;
    }
    return static_cast<u32>(1U + (i % 3U));
  };
  const SectorPanel p = sector_panel(window, n_inst, 4U, grp, 303U);
  const StorePanel sp{p.close, p.volume};
  std::vector<u32> gid((window + 1U) * n_inst);
  for (usize r = 0; r <= window; ++r) {
    for (usize i = 0; i < n_inst; ++i) {
      gid[r * n_inst + i] = grp(r, i);
    }
  }
  const FactorModelBuilder builder{sectors_only()};
  auto max_err = [&](const PitSideInputs &side) {
    const auto fr = builder.factor_returns(sp.view(), window, side);
    EXPECT_TRUE(fr.has_value());
    f64 e = 0.0;
    if (fr.has_value()) {
      for (usize u = 0; u < fr->dates.size(); ++u) {
        for (usize c = 0; c < fr->columns.size(); ++c) {
          const f64 want = p.f[fr->columns[c].group_id][fr->dates[u]];
          e = std::max(e, std::fabs(fr->f(static_cast<Eigen::Index>(u),
                                          static_cast<Eigen::Index>(c)) - want));
        }
      }
    }
    return e;
  };
  const f64 e_pit = max_err(PitSideInputs::per_date({}, std::span<const u32>{gid}, window + 1U));
  const f64 e_static = max_err(
      PitSideInputs::broadcast({}, std::span<const u32>{gid}.subspan(0U, n_inst)));
  std::printf("[W0-R0 evidence] sector switch: max |f - planted| PIT groups=%.3e, static "
              "(today's) groups=%.3e\n",
              e_pit, e_static);
  EXPECT_LT(e_pit, 1e-10);
  EXPECT_GT(e_static, 1e-4);
}

// Unit contract of detail::map_columns: identity by (kind, style | group id).
TEST(RiskSectorColumnsById, MapColumnsMatchesByIdentity) {
  using atx::engine::risk::detail::kNoColumn;
  using atx::engine::risk::detail::map_columns;
  const std::vector<ColumnTag> model{{ColumnTag::Kind::Sector, StyleFactor{}, 4U},
                                     {ColumnTag::Kind::Sector, StyleFactor{}, 9U},
                                     {ColumnTag::Kind::Style, StyleFactor::Volatility, 0U},
                                     {ColumnTag::Kind::Style, StyleFactor::Liquidity, 0U}};
  const std::vector<ColumnTag> date{{ColumnTag::Kind::Sector, StyleFactor{}, 2U},
                                    {ColumnTag::Kind::Sector, StyleFactor{}, 9U},
                                    {ColumnTag::Kind::Style, StyleFactor::Liquidity, 0U}};
  const std::vector<usize> m = map_columns(date, model);
  ASSERT_EQ(m.size(), 3U);
  EXPECT_EQ(m[0], kNoColumn); // group 2 is not in the model
  EXPECT_EQ(m[1], 1U);        // group 9 -> model column 1 (NOT position 1 of the date = 9 ok)
  EXPECT_EQ(m[2], 3U);        // Liquidity -> model column 3 (positionally it would be 2)
}

} // namespace atx_test_w0_r0_sector_columns
