// atx::engine::learn — build_sequences (S5-1) tests.
//
// TDD pins for the PIT trailing sequence-window tensor builder. The unit under
// test bridges the tabular FeatureMatrix to a (sample x time x feature) tensor by
// adding ONLY trailing windowing — so these tests assert exactly that: window
// pack values + order (happy path), the load-bearing trailing/no-look-ahead
// property (truncation invariance), the L==1 degenerate reduction to the tabular
// matrix, history/gap incompleteness in both drop modes, verbatim NaN carry (no
// survivorship / no zero-fill), the two argument errors, and multi-horizon label
// carry.
//
// Suite name is `LearnSequenceFeatures` so `ctest -R LearnSequenceFeatures`
// selects them. Test names follow Suite_Condition_ExpectedResult.
//
// FIXTURE STRATEGY: we construct FeatureMatrix DIRECTLY via push_row + filling
// X/Y/row_valid for a controlled multi-date, multi-instrument panel. That gives
// full control over row presence/gaps and the exact feature/label bytes, which is
// what the hand-computed assertions need. We do NOT go through build_features —
// it would couple these tests to Panel/AlphaStore construction we do not need.

#include <cmath>   // std::isnan, std::isfinite
#include <limits>  // std::numeric_limits
#include <utility> // std::pair
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp" // ErrorCode
#include "atx/core/types.hpp" // f64, u8, usize

#include "atx/engine/learn/feature_matrix.hpp"   // FeatureMatrix
#include "atx/engine/learn/nn/tensor.hpp"         // nn::Seq3
#include "atx/engine/learn/sequence_features.hpp" // SeqFeatureSpec, SequenceTensor, build_sequences

namespace atxtest_learn_sequence_features_test {

using atx::f64;
using atx::u8;
using atx::usize;
using atx::engine::learn::build_sequences;
using atx::engine::learn::FeatureMatrix;
using atx::engine::learn::SeqFeatureSpec;
using atx::engine::learn::SequenceTensor;

// One synthetic feature-matrix cell to push: which (date,inst), its F feature
// values, its per-horizon labels, and whether the row is valid.
struct Cell {
  usize date;
  usize inst;
  std::vector<f64> feats;  // length F
  std::vector<f64> labels; // length n_horizons (one per Y[h])
  bool valid;
};

// Build a FeatureMatrix directly from an explicit cell list, IN the given order
// (the caller supplies cells already in ascending (date,inst) order, mirroring
// build_features' emit order). n_features / n_horizons are inferred from the
// first cell; n_dates / n_instruments are the caller-declared panel extents.
FeatureMatrix make_fm(usize n_dates, usize n_instruments, usize n_features, usize n_horizons,
                      const std::vector<Cell> &cells) {
  FeatureMatrix fm;
  fm.n_dates = n_dates;
  fm.n_instruments = n_instruments;
  fm.n_features = n_features;
  fm.Y.assign(n_horizons, {});
  for (const Cell &c : cells) {
    const usize row = fm.push_row(c.date, c.inst);
    fm.X.resize((row + 1) * n_features);
    for (usize f = 0; f < n_features; ++f) {
      fm.X[row * n_features + f] = c.feats[f];
    }
    for (usize h = 0; h < n_horizons; ++h) {
      fm.Y[h].push_back(c.labels[h]);
    }
    fm.row_valid.push_back(static_cast<u8>(c.valid ? 1 : 0));
  }
  return fm;
}

// A dense 2-instrument x 5-date panel, F=2, 1 horizon. Feature f of (date d,
// inst i) = d*10 + i + f*0.5 (so every cell is unique and hand-computable). The
// label is the forward-1 return placeholder d*100 + i (unique per row).
std::vector<Cell> dense_cells(usize n_dates, usize n_instruments, usize n_features,
                              usize n_horizons) {
  std::vector<Cell> cells;
  for (usize d = 0; d < n_dates; ++d) {
    for (usize i = 0; i < n_instruments; ++i) {
      Cell c;
      c.date = d;
      c.inst = i;
      c.valid = true;
      for (usize f = 0; f < n_features; ++f) {
        c.feats.push_back(static_cast<f64>(d * 10 + i) + static_cast<f64>(f) * 0.5);
      }
      for (usize h = 0; h < n_horizons; ++h) {
        c.labels.push_back(static_cast<f64>(d * 100 + i) + static_cast<f64>(h));
      }
      cells.push_back(c);
    }
  }
  return cells;
}

// Expected feature value in the dense panel (mirrors dense_cells).
f64 dense_feat(usize d, usize i, usize f) {
  return static_cast<f64>(d * 10 + i) + static_cast<f64>(f) * 0.5;
}

// =====================================================================
//  Happy path: dense 2-instrument x several-date matrix, L=3.
// =====================================================================
TEST(LearnSequenceFeatures, DenseMatrix_Lookback3_PacksTrailingWindowsInOrder) {
  const usize n_dates = 5, n_inst = 2, F = 2, H = 1, L = 3;
  const FeatureMatrix fm = make_fm(n_dates, n_inst, F, H, dense_cells(n_dates, n_inst, F, H));

  SeqFeatureSpec spec;
  spec.lookback = L;
  spec.drop_incomplete = true;

  const auto r = build_sequences(fm, spec);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  const SequenceTensor &st = *r;

  // Anchors need 3 trailing rows => valid anchor dates are 2,3,4 (t+1 >= L).
  // For each such date, both instruments qualify => 3 dates * 2 inst = 6 samples.
  EXPECT_EQ(st.n_samples, 6U);
  EXPECT_EQ(st.lookback, L);
  EXPECT_EQ(st.n_features, F);
  EXPECT_EQ(st.x.size(), st.n_samples * L * F);

  // Ascending (date, inst) emit order: (2,0),(2,1),(3,0),(3,1),(4,0),(4,1).
  const std::vector<std::pair<usize, usize>> expect_anchors{{2, 0}, {2, 1}, {3, 0},
                                                            {3, 1}, {4, 0}, {4, 1}};
  ASSERT_EQ(st.date_of.size(), expect_anchors.size());
  for (usize s = 0; s < expect_anchors.size(); ++s) {
    EXPECT_EQ(st.date_of[s], expect_anchors[s].first) << "sample " << s;
    EXPECT_EQ(st.inst_of[s], expect_anchors[s].second) << "sample " << s;
    EXPECT_EQ(st.sample_valid[s], 1U) << "dense window must be valid, sample " << s;
  }

  // Hand-computed window for the first sample, anchor (t=2, i=0): rows for dates
  // 0,1,2 of inst 0 in trailing order l=0..2.
  const atx::engine::learn::nn::Seq3 v = st.view();
  EXPECT_EQ(v.B, st.n_samples);
  EXPECT_EQ(v.T, L);
  EXPECT_EQ(v.F, F);
  for (usize l = 0; l < L; ++l) {
    const usize d = 2 - (L - 1) + l; // 0,1,2
    for (usize f = 0; f < F; ++f) {
      EXPECT_DOUBLE_EQ(v.at(0, l, f), dense_feat(d, 0, f)) << "l=" << l << " f=" << f;
    }
  }

  // And the last sample, anchor (t=4, i=1): rows for dates 2,3,4 of inst 1.
  for (usize l = 0; l < L; ++l) {
    const usize d = 4 - (L - 1) + l; // 2,3,4
    for (usize f = 0; f < F; ++f) {
      EXPECT_DOUBLE_EQ(v.at(5, l, f), dense_feat(d, 1, f)) << "l=" << l << " f=" << f;
    }
  }

  // Labels carried from the ANCHOR row (date t, inst i), one horizon.
  ASSERT_EQ(st.y.size(), 1U);
  ASSERT_EQ(st.y[0].size(), st.n_samples);
  for (usize s = 0; s < expect_anchors.size(); ++s) {
    const usize t = expect_anchors[s].first, i = expect_anchors[s].second;
    EXPECT_DOUBLE_EQ(st.y[0][s], static_cast<f64>(t * 100 + i)) << "sample " << s;
  }
}

// =====================================================================
//  R2 truncation invariance (load-bearing): a trailing window for an anchor with
//  date <= t_cut is BYTE-IDENTICAL whether or not later dates exist in the source.
// =====================================================================
TEST(LearnSequenceFeatures, TruncatedFutureDates_TrailingWindow_ByteIdentical) {
  const usize n_dates = 6, n_inst = 2, F = 2, H = 1, L = 3, t_cut = 3;

  const std::vector<Cell> full = dense_cells(n_dates, n_inst, F, H);
  const FeatureMatrix fm_full = make_fm(n_dates, n_inst, F, H, full);

  // Truncated: drop every cell with date > t_cut. Forward labels would become NaN
  // at the boundary in a real build, but the WINDOW (trailing) is what we pin.
  std::vector<Cell> trunc;
  for (const Cell &c : full) {
    if (c.date <= t_cut) {
      trunc.push_back(c);
    }
  }
  const FeatureMatrix fm_trunc = make_fm(t_cut + 1, n_inst, F, H, trunc);

  SeqFeatureSpec spec;
  spec.lookback = L;
  spec.drop_incomplete = true;

  const auto rf = build_sequences(fm_full, spec);
  const auto rt = build_sequences(fm_trunc, spec);
  ASSERT_TRUE(rf.has_value());
  ASSERT_TRUE(rt.has_value());
  const SequenceTensor &sf = *rf;
  const SequenceTensor &st = *rt;

  // For every sample in the TRUNCATED build (all have date_of <= t_cut), find the
  // same anchor in the full build and assert the window x-slice is byte-identical.
  for (usize s = 0; s < st.n_samples; ++s) {
    const usize t = st.date_of[s], i = st.inst_of[s];
    ASSERT_LE(t, t_cut);
    // locate matching anchor in full build
    usize sf_idx = sf.n_samples; // sentinel
    for (usize k = 0; k < sf.n_samples; ++k) {
      if (sf.date_of[k] == t && sf.inst_of[k] == i) {
        sf_idx = k;
        break;
      }
    }
    ASSERT_LT(sf_idx, sf.n_samples) << "anchor (" << t << "," << i << ") missing in full build";
    const usize window = L * F;
    for (usize j = 0; j < window; ++j) {
      const f64 a = st.x[s * window + j];
      const f64 b = sf.x[sf_idx * window + j];
      // byte-identical (no NaN in this dense fixture, so == is exact)
      EXPECT_EQ(a, b) << "window byte differs at anchor (" << t << "," << i << ") j=" << j;
    }
    // Label equality holds when date_of + horizon (=1 here) <= t_cut, i.e. the
    // forward return is still knowable post-truncation. In THIS direct fixture the
    // truncated build still carries the same label bytes (we did not NaN them), so
    // assert equality only where it is contractually guaranteed.
    if (t + 1 <= t_cut) {
      EXPECT_DOUBLE_EQ(st.y[0][s], sf.y[0][sf_idx]) << "label differs, anchor date " << t;
    }
  }
}

// =====================================================================
//  L=1 reduces to the tabular FeatureMatrix (R7 degenerate pin).
// =====================================================================
TEST(LearnSequenceFeatures, Lookback1_WindowEqualsFeatureMatrixRow_RowForRow) {
  const usize n_dates = 4, n_inst = 2, F = 3, H = 1, L = 1;
  const FeatureMatrix fm = make_fm(n_dates, n_inst, F, H, dense_cells(n_dates, n_inst, F, H));

  SeqFeatureSpec spec;
  spec.lookback = L;
  spec.drop_incomplete = true;

  const auto r = build_sequences(fm, spec);
  ASSERT_TRUE(r.has_value());
  const SequenceTensor &st = *r;

  // L=1 => every emitted row is a valid anchor => one sample per fm row, same order.
  ASSERT_EQ(st.n_samples, fm.n_rows());
  ASSERT_EQ(st.x.size(), fm.X.size());
  for (usize s = 0; s < st.n_samples; ++s) {
    const usize fm_row = fm.row_of(st.date_of[s], st.inst_of[s]);
    for (usize f = 0; f < F; ++f) {
      EXPECT_DOUBLE_EQ(st.x[s * F + f], fm.X[fm_row * F + f]) << "sample " << s << " f=" << f;
    }
  }
}

// =====================================================================
//  Insufficient history: an anchor with t < L-1, BOTH drop modes.
// =====================================================================
TEST(LearnSequenceFeatures, InsufficientHistory_DropTrue_EmitsNoSample) {
  // 2 dates, 1 instrument, L=3 => no anchor can have 3 trailing rows.
  const usize n_dates = 2, n_inst = 1, F = 2, H = 1, L = 3;
  const FeatureMatrix fm = make_fm(n_dates, n_inst, F, H, dense_cells(n_dates, n_inst, F, H));

  SeqFeatureSpec spec;
  spec.lookback = L;
  spec.drop_incomplete = true;

  const auto r = build_sequences(fm, spec);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->n_samples, 0U) << "no complete window => no samples when drop_incomplete";
  EXPECT_EQ(r->x.size(), 0U);
  ASSERT_EQ(r->y.size(), 1U);
  EXPECT_EQ(r->y[0].size(), 0U);
}

TEST(LearnSequenceFeatures, InsufficientHistory_DropFalse_EmitsNaNWindowValidZeroLabelCarried) {
  const usize n_dates = 2, n_inst = 1, F = 2, H = 1, L = 3;
  const FeatureMatrix fm = make_fm(n_dates, n_inst, F, H, dense_cells(n_dates, n_inst, F, H));

  SeqFeatureSpec spec;
  spec.lookback = L;
  spec.drop_incomplete = false; // emit NaN-filled "no opinion" windows

  const auto r = build_sequences(fm, spec);
  ASSERT_TRUE(r.has_value());
  const SequenceTensor &st = *r;

  // Anchors = every emitted row (both dates of the single instrument) since with
  // drop_incomplete=false we emit a sample per anchor row regardless of history.
  ASSERT_EQ(st.n_samples, fm.n_rows());
  EXPECT_EQ(st.x.size(), st.n_samples * L * F);

  for (usize s = 0; s < st.n_samples; ++s) {
    EXPECT_EQ(st.sample_valid[s], 0U) << "incomplete window must be invalid, sample " << s;
    // entire window NaN-filled (no opinion)
    for (usize j = 0; j < L * F; ++j) {
      EXPECT_TRUE(std::isnan(st.x[s * L * F + j])) << "expected NaN fill at sample " << s;
    }
    // label still carried from the anchor row
    const usize t = st.date_of[s], i = st.inst_of[s];
    EXPECT_DOUBLE_EQ(st.y[0][s], static_cast<f64>(t * 100 + i)) << "label carried, sample " << s;
  }
}

// A GAP inside the window (instrument missing one date) is also "incomplete".
TEST(LearnSequenceFeatures, RowGapInsideWindow_DropTrue_EmitsNoSampleForGappedAnchor) {
  // 4 dates, 1 instrument, but date 1 is MISSING for the instrument (a universe
  // dropout). L=3. The anchor at t=3 needs rows {1,2,3}; row 1 is absent => gap.
  const usize n_dates = 4, n_inst = 1, F = 2, H = 1, L = 3;
  std::vector<Cell> cells;
  for (usize d = 0; d < n_dates; ++d) {
    if (d == 1) {
      continue; // drop the instrument on date 1
    }
    Cell c;
    c.date = d;
    c.inst = 0;
    c.valid = true;
    c.feats = {static_cast<f64>(d * 10), static_cast<f64>(d * 10) + 0.5};
    c.labels = {static_cast<f64>(d * 100)};
    cells.push_back(c);
  }
  const FeatureMatrix fm = make_fm(n_dates, n_inst, F, H, cells);

  SeqFeatureSpec spec;
  spec.lookback = L;
  spec.drop_incomplete = true;

  const auto r = build_sequences(fm, spec);
  ASSERT_TRUE(r.has_value());
  const SequenceTensor &st = *r;
  // Anchor t=2 needs {0,1,2}: row 1 absent => gap => dropped.
  // Anchor t=3 needs {1,2,3}: row 1 absent => gap => dropped.
  // No other anchor has 3 trailing dates => zero samples.
  EXPECT_EQ(st.n_samples, 0U);
}

// =====================================================================
//  NaN feature carried verbatim ("no opinion", no survivorship / no zero-fill).
// =====================================================================
TEST(LearnSequenceFeatures, NaNFeatureInWindow_CarriedVerbatim_SampleInvalidNotZeroed) {
  // 3 dates, 1 instrument, L=3. One feature cell (date 1, feature 0) is NaN and
  // the row is flagged invalid (mirrors row_valid). The single anchor t=2 packs
  // rows {0,1,2}; the NaN must propagate into the window and set sample_valid=0,
  // and must NOT be replaced by 0.
  const usize n_dates = 3, n_inst = 1, F = 2, H = 1, L = 3;
  const f64 kNaN = std::numeric_limits<f64>::quiet_NaN();
  std::vector<Cell> cells;
  for (usize d = 0; d < n_dates; ++d) {
    Cell c;
    c.date = d;
    c.inst = 0;
    c.feats = {static_cast<f64>(d * 10), static_cast<f64>(d * 10) + 0.5};
    c.valid = true;
    if (d == 1) {
      c.feats[0] = kNaN; // poison one feature cell
      c.valid = false;   // row_valid reflects the NaN
    }
    c.labels = {static_cast<f64>(d * 100)};
    cells.push_back(c);
  }
  const FeatureMatrix fm = make_fm(n_dates, n_inst, F, H, cells);

  SeqFeatureSpec spec;
  spec.lookback = L;
  spec.drop_incomplete = true; // window is COMPLETE (all rows present) — not dropped

  const auto r = build_sequences(fm, spec);
  ASSERT_TRUE(r.has_value());
  const SequenceTensor &st = *r;

  ASSERT_EQ(st.n_samples, 1U) << "window is complete (all 3 rows present) => emitted";
  EXPECT_EQ(st.sample_valid[0], 0U) << "a NaN in the window => sample invalid";

  // The NaN sits at l=1 (date 1), f=0. Verify it is NaN, not 0, and the rest are
  // the verbatim feature bytes.
  const atx::engine::learn::nn::Seq3 v = st.view();
  EXPECT_TRUE(std::isnan(v.at(0, 1, 0))) << "NaN must be carried verbatim, not zeroed";
  EXPECT_DOUBLE_EQ(v.at(0, 1, 1), static_cast<f64>(1 * 10) + 0.5); // sibling feature intact
  EXPECT_DOUBLE_EQ(v.at(0, 0, 0), 0.0);                            // date 0 feature 0
  EXPECT_DOUBLE_EQ(v.at(0, 2, 0), static_cast<f64>(2 * 10));       // date 2 feature 0
}

// =====================================================================
//  Errors.
// =====================================================================
TEST(LearnSequenceFeatures, LookbackZero_ReturnsInvalidArgument) {
  const FeatureMatrix fm = make_fm(3, 1, 2, 1, dense_cells(3, 1, 2, 1));
  SeqFeatureSpec spec;
  spec.lookback = 0;
  const auto r = build_sequences(fm, spec);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(LearnSequenceFeatures, ZeroFeatures_ReturnsInvalidArgument) {
  FeatureMatrix fm;
  fm.n_dates = 3;
  fm.n_instruments = 1;
  fm.n_features = 0; // no features
  fm.Y.assign(1, {});
  SeqFeatureSpec spec;
  spec.lookback = 2;
  const auto r = build_sequences(fm, spec);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

// =====================================================================
//  Multi-horizon labels: fm.Y of size 3 => SequenceTensor.y has 3 rows, each
//  carrying the anchor row's per-horizon label.
// =====================================================================
TEST(LearnSequenceFeatures, MultiHorizonLabels_CarriesEachHorizonFromAnchor) {
  const usize n_dates = 4, n_inst = 2, F = 2, H = 3, L = 2;
  const FeatureMatrix fm = make_fm(n_dates, n_inst, F, H, dense_cells(n_dates, n_inst, F, H));

  SeqFeatureSpec spec;
  spec.lookback = L;
  spec.drop_incomplete = true;

  const auto r = build_sequences(fm, spec);
  ASSERT_TRUE(r.has_value());
  const SequenceTensor &st = *r;

  ASSERT_EQ(st.y.size(), 3U) << "one label row per horizon";
  for (usize h = 0; h < 3; ++h) {
    ASSERT_EQ(st.y[h].size(), st.n_samples) << "horizon " << h;
  }
  // dense label = d*100 + i + h for horizon h.
  for (usize s = 0; s < st.n_samples; ++s) {
    const usize t = st.date_of[s], i = st.inst_of[s];
    for (usize h = 0; h < 3; ++h) {
      EXPECT_DOUBLE_EQ(st.y[h][s], static_cast<f64>(t * 100 + i) + static_cast<f64>(h))
          << "sample " << s << " horizon " << h;
    }
  }
}

}  // namespace atxtest_learn_sequence_features_test
