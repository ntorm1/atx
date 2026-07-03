// stack_meta_from_positions_test.cpp — p8 S3-1: meta_features_from_pool's
// train/eval parity contract, exercised directly against a hand-built
// combine::AlphaStore pool (every other `fit_stack` fixture in ensemble_test.cpp
// builds a FeatureMatrix by hand, bypassing this frozen builder entirely — this
// is the first direct test of the pool -> meta seam the atx-impl wiring
// (stage_combine.cpp's windowed_pool + build_forward_returns_window) depends on).
//
// Suite: StackMetaFromPositions

#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/combine/metrics.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/learn/ensemble.hpp"
#include "atx/engine/learn/feature_matrix.hpp"

namespace atxtest_stack_meta_from_positions {

using atx::f64;
using atx::u16;
using atx::usize;
namespace combine = atx::engine::combine;
namespace learn = atx::engine::learn;

// A 2-alpha, 3-date, 2-instrument pool with hand-set positions (no NaN — every
// cell finite), so meta_features_from_pool's row_valid is 1 everywhere and the
// full (date,instrument) grid is emitted (3*2 = 6 rows).
TEST(StackMetaFromPositions, ColumnsMatchPoolPositionsAndLabelsMatchSuppliedForwardReturns) {
  combine::AlphaStore pool;
  // Alpha 0 positions: date-major then instrument-minor, [d0i0,d0i1, d1i0,d1i1, d2i0,d2i1].
  const std::vector<f64> pos0{0.10, -0.10, 0.20, -0.20, 0.30, -0.30};
  const std::vector<f64> pos1{-0.05, 0.05, -0.15, 0.15, -0.25, 0.25};
  const std::vector<f64> pnl0{0.0, 0.001, 0.002};
  const std::vector<f64> pnl1{0.0, -0.001, 0.003};
  combine::AlphaMetrics m{};
  ASSERT_TRUE(pool.insert(nullptr, pnl0, pos0, m).has_value());
  ASSERT_TRUE(pool.insert(nullptr, pnl1, pos1, m).has_value());
  ASSERT_EQ(pool.n_periods(), 3U);
  ASSERT_EQ(pool.n_instruments(), 2U);

  // A hand-built forward-return label, period-major then instrument-minor,
  // DISTINCT per cell so a column/row transposition bug would be caught.
  const std::vector<f64> fwd{1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
  const std::vector<u16> horizons{1};

  const learn::FeatureMatrix meta =
      learn::meta_features_from_pool(pool, std::span<const f64>{fwd}, std::span<const u16>{horizons});

  ASSERT_EQ(meta.n_dates, 3U);
  ASSERT_EQ(meta.n_instruments, 2U);
  ASSERT_EQ(meta.n_features, 2U);
  ASSERT_EQ(meta.n_rows(), 6U) << "every (date,instrument) cell is emitted (no universe gating)";
  ASSERT_EQ(meta.Y.size(), 1U);

  // Column f of row r must equal pool alpha f's position at (row_date[r], row_inst[r]).
  for (usize r = 0; r < meta.n_rows(); ++r) {
    const usize d = meta.row_date[r];
    const usize i = meta.row_inst[r];
    for (usize f = 0; f < meta.n_features; ++f) {
      const f64 expected = pool.positions(combine::AlphaId{static_cast<atx::u32>(f)}, d)[i];
      EXPECT_EQ(meta.X[r * meta.n_features + f], expected)
          << "row=" << r << " (d=" << d << ",i=" << i << ") feature=" << f;
    }
    EXPECT_EQ(meta.row_valid[r], 1U) << "every cell here is finite -> row_valid must be 1";
  }

  // Y[0][row] must equal the supplied forward-return label at (date,inst) for
  // every row whose horizon lookahead stays inside the panel (d+1 < n_dates);
  // the LAST date's rows (d==2, d+1>=3) are unknowable -> NaN (S0.6 tail).
  for (usize r = 0; r < meta.n_rows(); ++r) {
    const usize d = meta.row_date[r];
    const usize i = meta.row_inst[r];
    const f64 y = meta.Y[0][r];
    if (d + 1U >= meta.n_dates) {
      EXPECT_TRUE(std::isnan(y)) << "tail row (d=" << d << ") must carry NaN (unknowable forward return)";
    } else {
      const f64 expected = fwd[d * meta.n_instruments + i];
      EXPECT_EQ(y, expected) << "row=" << r << " (d=" << d << ",i=" << i << ")";
    }
  }
}

} // namespace atxtest_stack_meta_from_positions
