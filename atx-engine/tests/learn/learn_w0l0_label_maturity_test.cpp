// W0-L0 — label-maturity metadata and the r + H <= t - embargo filter (plan v2 §7
// W0-L0; finding L-02).
//
// select_interactions used to admit every finite label anchored at date <= t - embargo,
// so with embargo < H it read forward returns that are realized AFTER t. Under
// LabelMaturityRule::MaturedV2 (the default) it reads only labels with
// row_date + label_horizons[0] <= t - embargo, and an unannotated matrix selects
// nothing. The legacy FiniteLabelV1 rule is kept (and pinned here as leaking).

#include <limits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/random.hpp" // Xoshiro256pp
#include "atx/core/types.hpp"

#include "atx/engine/learn/feature_matrix.hpp" // FeatureMatrix, label_matured
#include "atx/engine/learn/latent.hpp"         // select_interactions, LabelMaturityRule

namespace atx_test_w0_l0_label_maturity {

using atx::f64;
using atx::u16;
using atx::u32;
using atx::u64;
using atx::u8;
using atx::usize;
namespace learn = atx::engine::learn;
using Pairs = std::vector<std::pair<u32, u32>>;

// ---------------------------------------------------------------------------
//  The filter itself
// ---------------------------------------------------------------------------

TEST(LearnLabelMaturity_Filter, BoundaryIsInclusiveAtCutoff) {
  // r + H <= t - e.
  EXPECT_TRUE(learn::label_matured(/*r=*/5, /*H=*/5, /*t=*/10, /*e=*/0));  // 10 <= 10
  EXPECT_FALSE(learn::label_matured(6, 5, 10, 0));                         // 11 >  10
  EXPECT_TRUE(learn::label_matured(3, 5, 10, 2));                          // 8 <= 8
  EXPECT_FALSE(learn::label_matured(4, 5, 10, 2));                         // 9 >  8
  EXPECT_TRUE(learn::label_matured(0, 0, 0, 0));                           // same-close label
  EXPECT_TRUE(learn::label_matured(10, 0, 10, 0));
}

TEST(LearnLabelMaturity_Filter, NoUnderflowOrOverflow) {
  EXPECT_FALSE(learn::label_matured(0, 1, 0, 0));          // t - e < H
  EXPECT_FALSE(learn::label_matured(0, 0, 3, 4));          // t < e
  EXPECT_FALSE(learn::label_matured(0, std::numeric_limits<u16>::max(), 100, 0));
  const usize big = std::numeric_limits<usize>::max();
  EXPECT_TRUE(learn::label_matured(big - 70000U, std::numeric_limits<u16>::max(), big, 0));
  EXPECT_FALSE(learn::label_matured(big, 1, big, 0)); // r + H would wrap: rejected
}

TEST(LearnLabelMaturity_Filter, AnnotationRequiresOneHorizonPerLabel) {
  learn::FeatureMatrix fm;
  EXPECT_FALSE(fm.has_label_horizons()) << "no labels";
  fm.Y.assign(2U, {});
  EXPECT_FALSE(fm.has_label_horizons()) << "unannotated";
  fm.label_horizons = {1};
  EXPECT_FALSE(fm.has_label_horizons()) << "one horizon for two labels";
  fm.label_horizons = {1, 5};
  EXPECT_TRUE(fm.has_label_horizons());
}

// ---------------------------------------------------------------------------
//  select_interactions reads only matured labels
// ---------------------------------------------------------------------------

// 4 features over 12 dates x 8 instruments with a 5-date forward label. For MATURED
// rows (date <= t - H) the label tracks features 2 and 3; for the rows whose label is
// still unrealized at t (t - H < date <= t) the label tracks features 0 and 1 with a
// large scale. A selector that reads the unrealized labels therefore picks {0,1}.
constexpr usize kDates = 12;
constexpr usize kInst = 8;
constexpr u16 kH = 5;
constexpr usize kT = 11; // decision date (the last date)

[[nodiscard]] learn::FeatureMatrix make_fm(bool annotate, u64 seed) {
  learn::FeatureMatrix fm;
  fm.n_dates = kDates;
  fm.n_instruments = kInst;
  fm.n_features = 4;
  fm.Y.assign(1U, {});
  if (annotate) {
    fm.label_horizons = {kH};
  }
  atx::core::Xoshiro256pp rng{seed};
  for (usize d = 0; d < kDates; ++d) {
    for (usize i = 0; i < kInst; ++i) {
      std::vector<f64> x(4);
      for (f64 &v : x) {
        v = rng.normal();
      }
      fm.row_date.push_back(d);
      fm.row_inst.push_back(i);
      fm.X.insert(fm.X.end(), x.begin(), x.end());
      fm.row_valid.push_back(static_cast<u8>(1));
      const bool matured = d + kH <= kT;
      const f64 y = matured ? (x[2] + 0.9 * x[3] + 0.1 * rng.normal())
                            : (40.0 * (x[0] + x[1]) + 0.1 * rng.normal());
      fm.Y[0].push_back(y);
    }
  }
  return fm;
}

TEST(LearnLabelMaturity_Select, ReadsOnlyMaturedLabels) {
  const learn::FeatureMatrix fm = make_fm(/*annotate=*/true, 17);
  const Pairs v2 = learn::select_interactions(fm, kT, /*embargo=*/0U, /*m=*/2U);
  EXPECT_EQ(v2, (Pairs{{2U, 3U}})) << "the matured rows' leaders";
  const Pairs v1 = learn::select_interactions(fm, kT, 0U, 2U,
                                              learn::LabelMaturityRule::FiniteLabelV1);
  EXPECT_EQ(v1, (Pairs{{0U, 1U}})) << "legacy V1 reads labels realized after t (L-02)";
}

TEST(LearnLabelMaturity_Select, UnrealizedLabelPerturbationCannotChangeSelection) {
  const learn::FeatureMatrix fm = make_fm(/*annotate=*/true, 17);
  const Pairs base = learn::select_interactions(fm, kT, 0U, 2U);
  learn::FeatureMatrix fm_mut = fm;
  usize n_changed = 0;
  for (usize r = 0; r < fm.n_rows(); ++r) {
    if (!learn::label_matured(fm.row_date[r], kH, kT, 0U)) {
      fm_mut.Y[0][r] = -1000.0 * fm.X[r * 4U + 2U]; // any value: it is not yet known
      ++n_changed;
    }
  }
  ASSERT_EQ(n_changed, static_cast<usize>(kH) * kInst);
  EXPECT_EQ(learn::select_interactions(fm_mut, kT, 0U, 2U), base);
  // Non-vacuous: the same perturbation on a MATURED row set moves the selection.
  learn::FeatureMatrix fm_matured_mut = fm;
  for (usize r = 0; r < fm.n_rows(); ++r) {
    if (learn::label_matured(fm.row_date[r], kH, kT, 0U)) {
      fm_matured_mut.Y[0][r] = 50.0 * fm.X[r * 4U + 0U] + fm.X[r * 4U + 1U];
    }
  }
  EXPECT_NE(learn::select_interactions(fm_matured_mut, kT, 0U, 2U), base);
}

TEST(LearnLabelMaturity_Select, EmbargoShiftsTheCutoff) {
  // With embargo e the admissible anchors are date <= t - e - H. At t = 11, e = 2,
  // H = 5 that is date <= 4; perturbing dates 5..11 cannot move the selection, and the
  // matured-row leaders are still {2,3}.
  const learn::FeatureMatrix fm = make_fm(/*annotate=*/true, 29);
  const Pairs base = learn::select_interactions(fm, kT, /*embargo=*/2U, 2U);
  EXPECT_EQ(base, (Pairs{{2U, 3U}}));
  learn::FeatureMatrix fm_mut = fm;
  for (usize r = 0; r < fm.n_rows(); ++r) {
    if (fm.row_date[r] > 4U) {
      fm_mut.Y[0][r] = 77.0 * fm.X[r * 4U + 1U];
    }
  }
  EXPECT_EQ(learn::select_interactions(fm_mut, kT, 2U, 2U), base);
  // A window too short to hold any matured label selects nothing.
  EXPECT_TRUE(learn::select_interactions(fm, /*t=*/4U, /*embargo=*/0U, 2U).empty());
}

TEST(LearnLabelMaturity_Select, UnannotatedMatrixSelectsNothing) {
  const learn::FeatureMatrix fm = make_fm(/*annotate=*/false, 17);
  EXPECT_TRUE(learn::select_interactions(fm, kT, 0U, 2U).empty())
      << "maturity cannot be proven without label_horizons";
  EXPECT_FALSE(learn::select_interactions(fm, kT, 0U, 2U, learn::LabelMaturityRule::FiniteLabelV1)
                   .empty())
      << "the legacy rule still selects (and leaks)";
}

} // namespace atx_test_w0_l0_label_maturity
