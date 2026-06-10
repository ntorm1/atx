// atx::engine::learn — PIT latent factor + interaction extraction tests (S5-2).
//
// Covers the plan's load-bearing semantics for hidden-feature extraction:
//   * Suite Latent
//       1. PcaFactor_RecoversKnownDirection (M4) — a collinear design (feat1 ==
//          2*feat0) is rank-1; the leading factor explains essentially all the
//          variance (explained_ratio[0] > 0.95).
//       2. PcaBasis_FitOnTrailing_TruncationInvariant (M2 / §0.5) — the basis fit
//          on rows with date <= t-embargo is BYTE-IDENTICAL whether or not rows at
//          dates > t are present (the look-ahead firewall). Pinned by hashing the
//          fitted components + mean f64 bytes via core::hash_bytes.
//       3. Interactions_SelectTopByIC_Deterministic (M1 / §4.2b) — select_interactions
//          is deterministic (same input -> identical pairs, fixed order) AND ranks
//          by |Spearman IC| vs Y[0], not by feature index (the two IC-dominant
//          features are chosen over the two noise features).
//
// Fixtures are FeatureMatrix aggregates built directly in this file (the dense
// public vectors are set; the private (date,inst)->row lookup is not needed —
// latent.hpp reads only row_date / row_valid / X / Y / n_features). Naming:
// Subject_Condition_ExpectedResult.

#include <limits>  // std::numeric_limits (tail NaN label)
#include <utility> // std::pair
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/hash.hpp"   // atx::core::hash_bytes
#include "atx/core/types.hpp"  // f64, u16, u32, usize

#include "atx/engine/learn/feature_matrix.hpp" // FeatureMatrix
#include "atx/engine/learn/latent.hpp"         // fit_latent, apply_latent, select_interactions

namespace {

using atx::f64;
using atx::u16;
using atx::u32;
using atx::usize;
using atx::engine::learn::FeatureMatrix;

// Build a FeatureMatrix from a per-row (date, feature-vector) list plus a single
// horizon-0 label per row. Every row is marked valid. The dense public vectors
// are all that latent.hpp consumes, so the private row lookup is left empty.
[[nodiscard]] FeatureMatrix
make_fm(usize n_features, const std::vector<usize> &row_dates,
        const std::vector<std::vector<f64>> &rows, const std::vector<f64> &y0) {
  FeatureMatrix fm;
  fm.n_features = n_features;
  fm.row_date = row_dates;
  fm.n_dates = 0U;
  for (const usize d : row_dates) {
    fm.n_dates = (d + 1U > fm.n_dates) ? d + 1U : fm.n_dates;
  }
  fm.row_valid.assign(rows.size(), static_cast<atx::u8>(1));
  fm.X.reserve(rows.size() * n_features);
  for (const std::vector<f64> &r : rows) {
    for (usize f = 0; f < n_features; ++f) {
      fm.X.push_back(r[f]);
    }
  }
  fm.Y.assign(1U, y0);
  return fm;
}

// Hash the fitted PCA components + mean f64 bit patterns. NON-VACUOUS: it hashes
// the bytes of the actual fitted matrices, so two fits that differ in any fitted
// coefficient produce different digests.
[[nodiscard]] atx::u64 hash_basis(const atx::engine::learn::LatentBasis &b) {
  const auto &comp = b.model.components;
  const auto &mean = b.model.mean;
  // SAFETY: Eigen MatX/VecX store doubles contiguously; .data() points at
  // size()*sizeof(double) live bytes for the lifetime of the matrix. We hash the
  // raw f64 bit patterns (deterministic within a process) to pin the fit.
  const usize cn = static_cast<usize>(comp.size()) * sizeof(f64);
  const usize mn = static_cast<usize>(mean.size()) * sizeof(f64);
  const atx::u64 hc = atx::core::hash_bytes(comp.data(), cn);
  const atx::u64 hm = atx::core::hash_bytes(mean.data(), mn);
  // Fold the two digests into one (order-sensitive golden-ratio mix); both the
  // component and mean bytes contribute, so the digest is non-vacuous.
  return hc ^ (hm + 0x9E3779B97F4A7C15ULL + (hc << 6U) + (hc >> 2U));
}

// ---- Suite Latent ------------------------------------------------------------

// 1. A collinear design (feat1 == 2*feat0) is rank-1: the leading factor explains
//    essentially all the variance.
TEST(Latent, PcaFactor_RecoversKnownDirection) {
  const usize nf = 2U;
  std::vector<usize> dates;
  std::vector<std::vector<f64>> rows;
  std::vector<f64> y0;
  // 12 rows over a few dates; feat0 spreads, feat1 = 2*feat0 (rank-1 design).
  for (usize i = 0; i < 12U; ++i) {
    const f64 x = static_cast<f64>(i) - 5.5; // centered-ish spread
    dates.push_back(i / 3U);                  // 4 dates, 3 rows each
    rows.push_back({x, 2.0 * x});
    y0.push_back(x);
  }
  const FeatureMatrix fm = make_fm(nf, dates, rows, y0);

  const usize t = fm.n_dates - 1U; // last date; embargo 0 -> all rows in window
  const auto basis = atx::engine::learn::fit_latent(fm, t, /*embargo=*/0U, /*k=*/1U);

  ASSERT_EQ(basis.k, 1U);
  ASSERT_GT(basis.model.explained_ratio.size(), 0);
  EXPECT_GT(basis.model.explained_ratio[0], 0.95); // rank-1 design: first factor dominates
}

// 2. The basis fit on rows with date <= t-embargo is byte-identical whether or not
//    rows at dates > t are present (the M2 / §0.5 look-ahead firewall).
TEST(Latent, PcaBasis_FitOnTrailing_TruncationInvariant) {
  // 8 dates, 2 rows per date. feat values are an arbitrary but deterministic mix.
  auto build = [](usize n_dates) {
    const usize nf = 3U;
    std::vector<usize> dates;
    std::vector<std::vector<f64>> rows;
    std::vector<f64> y0;
    for (usize d = 0; d < n_dates; ++d) {
      for (usize j = 0; j < 2U; ++j) {
        const f64 a = static_cast<f64>(d) + 0.5 * static_cast<f64>(j);
        const f64 b = 2.0 * a - 1.0 + 0.3 * static_cast<f64>(j);
        const f64 c = a * a - 0.25 * static_cast<f64>(d);
        rows.push_back({a, b, c});
        dates.push_back(d);
        y0.push_back(a - b);
      }
    }
    return make_fm(nf, dates, rows, y0);
  };
  const FeatureMatrix full = build(8U);   // dates 0..7
  const FeatureMatrix trunc = build(6U);  // dates 0..5 only (rows > t absent)

  const usize t = 5U; // window: date <= 5 - 0
  const auto b_full = atx::engine::learn::fit_latent(full, t, /*embargo=*/0U, /*k=*/2U);
  const auto b_trunc = atx::engine::learn::fit_latent(trunc, t, /*embargo=*/0U, /*k=*/2U);

  // Non-vacuous: the digest covers the actually-fitted components + mean bytes.
  EXPECT_EQ(hash_basis(b_full), hash_basis(b_trunc));
  // And the fit is non-degenerate (a real 3->2 basis was fit, not an empty model).
  EXPECT_EQ(b_full.model.components.rows(), 3);
  EXPECT_EQ(b_full.model.components.cols(), 2);
}

// Build a 4-feature fixture where the |IC|-leaders are the HIGH-index features
// (feat2, feat3 track Y[0]) and the low-index features (feat0, feat1) are noise.
// An index-order selector would pick {0,1}; an IC-order selector picks {2,3}.
[[nodiscard]] FeatureMatrix make_ranking_fixture() {
  const usize nf = 4U;
  std::vector<usize> dates;
  std::vector<std::vector<f64>> rows;
  std::vector<f64> y0;
  for (usize i = 0; i < 16U; ++i) {
    const f64 t = static_cast<f64>(i);
    const f64 f0 = (i % 2U == 0U) ? 1.0 : -1.0;      // zig-zag noise vs y
    const f64 f1 = static_cast<f64>((i * 7U) % 5U);  // scrambled noise vs y
    const f64 f2 = t;                                 // monotone in y -> |IC| == 1
    const f64 f3 = 0.9 * t + 1.0;                     // monotone in y -> |IC| == 1
    dates.push_back(i / 4U);
    rows.push_back({f0, f1, f2, f3});
    y0.push_back(t); // label tracks f2/f3
  }
  return make_fm(nf, dates, rows, y0);
}

// Build a fixture engineered so the non-finite-label FILTER is load-bearing: the
// final block of rows carries a NaN label (the unknowable tail forward-return).
// On the FINITE rows the IC-leaders are {2,3}; but if the NaN-tail rows are NOT
// filtered out, their ranks lift the noise features {0,1} ABOVE {2,3}. So:
//   * filtered (correct):   select_interactions -> {2,3}
//   * unfiltered (buggy):   would -> {0,1}
// Asserting {2,3} therefore fails on the unfiltered code (non-vacuous).
[[nodiscard]] FeatureMatrix make_tail_nan_fixture() {
  const usize nf = 4U;
  const usize n_rows = 16U;
  const usize nan_from = 12U; // last 4 rows have a NaN (tail) label
  std::vector<usize> dates;
  std::vector<std::vector<f64>> rows;
  std::vector<f64> y0;
  for (usize i = 0; i < n_rows; ++i) {
    const f64 t = static_cast<f64>(i);
    const bool tail = (i >= nan_from);
    // f2/f3 track the label cleanly on the finite block, but BREAK hard on the
    // tail (dropping to large negatives) — so including the tail wrecks their IC.
    const f64 f2 = tail ? -t : t;
    const f64 f3 = tail ? -(0.9 * t) : (0.9 * t + 1.0);
    // f0/f1 are weak noise on the finite block, but on the tail they are large and
    // strictly ordered — so including the tail INFLATES their IC above f2/f3.
    const f64 f0 = tail ? (100.0 + t) : static_cast<f64>((i * 5U) % 4U);
    const f64 f1 = tail ? (200.0 + 0.7 * t) : static_cast<f64>((i * 3U) % 5U);
    dates.push_back(i / 4U);
    rows.push_back({f0, f1, f2, f3});
    y0.push_back(tail ? std::numeric_limits<f64>::quiet_NaN() : t);
  }
  return make_fm(nf, dates, rows, y0);
}

// 3a. select_interactions ranks by |Spearman IC|, NOT by feature index: the
//     high-index IC-leaders {2,3} are chosen over the low-index noise {0,1}.
TEST(Latent, Interactions_RanksByIC_NotIndex) {
  const FeatureMatrix fm = make_ranking_fixture();
  const usize t = fm.n_dates - 1U;
  const auto p = atx::engine::learn::select_interactions(fm, t, /*embargo=*/0U, /*m=*/2U);

  // m=2 -> C(2,2) == 1 pair, and it must be exactly the IC-leaders {2,3} — an
  // index-order selector would have returned {0,1} instead.
  ASSERT_EQ(p.size(), 1U);
  EXPECT_EQ(p[0].first, 2U);
  EXPECT_EQ(p[0].second, 3U);
}

// 3b. A non-finite (tail) Y[0] label in the trailing window is filtered out, so
//     the genuine finite-row IC-leaders {2,3} are ranked correctly. WITHOUT the
//     filter the NaN-tail rows lift the noise features and the selection flips to
//     {0,1} (verified: unfiltered |IC| f0~0.72,f1~0.63 > f2,f3~0.16). Asserting
//     {2,3} therefore fails on the unfiltered code — the filter is load-bearing.
TEST(Latent, Interactions_TailNaNLabel_StillRanks) {
  const FeatureMatrix fm = make_tail_nan_fixture();
  const usize t = fm.n_dates - 1U; // embargo 0 < horizon -> the NaN tail rows are in-window
  const auto p = atx::engine::learn::select_interactions(fm, t, /*embargo=*/0U, /*m=*/2U);

  ASSERT_EQ(p.size(), 1U);
  EXPECT_EQ(p[0].first, 2U);  // finite-row IC-leaders, not the {0,1} the tail would inflate
  EXPECT_EQ(p[0].second, 3U);
}

// 3c. select_interactions is deterministic: same input -> identical pairs, order.
TEST(Latent, Interactions_Deterministic) {
  const FeatureMatrix fm = make_ranking_fixture();
  const usize t = fm.n_dates - 1U;
  const auto p1 = atx::engine::learn::select_interactions(fm, t, /*embargo=*/0U, /*m=*/3U);
  const auto p2 = atx::engine::learn::select_interactions(fm, t, /*embargo=*/0U, /*m=*/3U);

  EXPECT_EQ(p1, p2);
  ASSERT_EQ(p1.size(), 3U); // m=3 -> C(3,2) == 3 pairs
  for (const std::pair<u32, u32> &pr : p1) {
    EXPECT_LT(pr.first, pr.second); // canonical a < b
  }
}

} // namespace
