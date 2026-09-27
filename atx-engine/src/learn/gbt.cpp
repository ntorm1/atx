#include "atx/engine/learn/gbt.hpp"

#include <algorithm> // std::sort, std::fill
#include <cmath>     // std::isfinite, std::sqrt (OOF dispersion floor)
#include <cstddef>   // std::ptrdiff_t (subset slice index)
#include <bit>
#include <array>
#include <string>
#include <limits>    // std::numeric_limits (trace NaN for uncovered rows)
#include <span>      // std::span
#include <utility>   // std::move
#include <vector>    // std::vector

#include <Eigen/Dense> // Eigen::Index, MatX/VecX

#include "atx/core/sha256.hpp"
#include "atx/core/macro.hpp"  // ATX_CHECK
#include "atx/core/random.hpp" // atx::core::Xoshiro256pp

#include "atx/engine/eval/cpcv.hpp"            // eval::CpcvConfig, eval::cpcv_folds, LabelSpan, CpcvFold
#include "atx/engine/learn/feature_matrix.hpp" // FeatureMatrix
#include "atx/engine/learn/latent.hpp"         // LatentAugmentation, detail::pearson
#include "atx/engine/learn/learned_source.hpp" // LearnedModel, ModelKind, GbtForest/Tree/Node, gbt_*_predict
#include "atx/engine/learn/linear_alpha.hpp"   // detail::build_design, fit_standardization, pearson
#include "atx/engine/parallel/det_pool.hpp"
#include "atx/engine/learn/train.hpp"          // seed_for, date_label_spans, expand_date_folds, RowFold

namespace atx::engine::learn {

namespace gbt_detail {

// Compute the bin edges for an (n x p) design over its rows. For each feature,
// sort that column, then take n_bins-1 evenly-spaced quantile positions as cut
// points (dedup-ascending). Deterministic: the sort is a total order; the cut
// positions are a fixed integer division.
BinEdges fit_bin_edges(const gbt_lin::MatX &X, atx::u32 n_bins) {
  const atx::usize n = static_cast<atx::usize>(X.rows());
  const atx::usize p = static_cast<atx::usize>(X.cols());
  BinEdges be;
  be.edges.assign(p, {});
  if (n == 0U || n_bins < 2U) {
    return be; // no rows, or a single bin -> no cut points (everything in bin 0)
  }
  const atx::usize n_cuts = static_cast<atx::usize>(n_bins) - 1U;
  std::vector<atx::f64> col(n);
  for (atx::usize f = 0; f < p; ++f) {
    for (atx::usize i = 0; i < n; ++i) {
      col[i] = X(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(f));
    }
    std::sort(col.begin(), col.end());
    std::vector<atx::f64> cuts;
    cuts.reserve(n_cuts);
    for (atx::usize c = 1; c <= n_cuts; ++c) {
      // The c-th of n_bins quantile boundaries -> the (c*n / n_bins)-th order stat.
      const atx::usize q = (c * n) / static_cast<atx::usize>(n_bins);
      const atx::usize idx = (q >= n) ? (n - 1U) : q;
      const atx::f64 edge = col[idx];
      if (cuts.empty() || edge > cuts.back()) {
        cuts.push_back(edge); // dedup-ascending: collapse repeated quantile values
      }
    }
    be.edges[f] = std::move(cuts);
  }
  return be;
}

// The best split for a node's rows over the given (subsampled) feature set.
// Accumulates (Σg, Σh) per (feature, bin) and scans bins left->right, keeping the
// FIRST-max gain (strictly-greater wins; ties keep the earlier feature/bin — M1).
// Writes the chosen feature, the split BIN (left child = bin < split_bin), and the
// threshold value (the upper edge of the last left bin). `found` is false when no
// candidate clears BOTH the min_child leaf-size floor and the min_split_gain bar
// (the latter the caller passes as the node's gain floor — 0 at the root).
SplitResult best_split(const TreeData &td,
                       const std::vector<atx::usize> &rows,
                       const std::vector<atx::usize> &feats, atx::f64 l2,
                       atx::f64 min_child, atx::f64 min_split_gain) {
  SplitResult out;
  // A split must clear min_split_gain to fire (XGBoost gamma): on noise the best
  // gain stays below the floor, so no split is taken and the node is a leaf.
  atx::f64 best_gain = min_split_gain;
  // Node totals (G, H) — the parent term of the gain.
  atx::f64 g_tot = 0.0;
  atx::f64 h_tot = 0.0;
  for (const atx::usize i : rows) {
    g_tot += td.g[i];
    h_tot += td.h[i];
  }
  const atx::f64 parent = (g_tot * g_tot) / (h_tot + l2);
  std::vector<atx::f64> gh(td.n_bins, 0.0); // per-bin Σg
  std::vector<atx::f64> hh(td.n_bins, 0.0); // per-bin Σh
  for (const atx::usize f : feats) {
    ATX_CHECK(f < td.p); // a subsampled feature indexes within the design (NDEBUG)
    std::fill(gh.begin(), gh.end(), 0.0);
    std::fill(hh.begin(), hh.end(), 0.0);
    for (const atx::usize i : rows) {
      const atx::usize b = td.bins[i * td.p + f];
      ATX_CHECK(b < td.n_bins); // bin index within the histogram width (NDEBUG)
      gh[b] += td.g[i];
      hh[b] += td.h[i];
    }
    // Scan split points left->right: left = bins [0, sb), right = [sb, n_bins).
    atx::f64 gl = 0.0;
    atx::f64 hl = 0.0;
    for (atx::usize sb = 1; sb < td.n_bins; ++sb) {
      gl += gh[sb - 1U];
      hl += hh[sb - 1U];
      const atx::f64 gr = g_tot - gl;
      const atx::f64 hr = h_tot - hl;
      if (hl < min_child || hr < min_child) {
        continue; // a child too small (leaf-size floor)
      }
      const atx::f64 gain =
          0.5 * ((gl * gl) / (hl + l2) + (gr * gr) / (hr + l2) - parent);
      if (gain > best_gain) { // strictly-greater wins -> FIRST-max tie-break (M1)
        best_gain = gain;
        out.feature = f;
        out.split_bin = sb;
        out.found = true;
      }
    }
  }
  if (out.found) {
    // Threshold = the upper edge of the last left bin (split_bin-1). A row goes
    // left iff its value < threshold (== its bin < split_bin). When split_bin-1 is
    // beyond the edge list (the top bin), there is no finite upper edge; that case
    // never produces a usable left/right partition under the bin scan above, so a
    // finite edge always exists here.
    const std::vector<atx::f64> &e = td.be->edges[out.feature];
    if (out.split_bin - 1U < e.size()) {
      out.threshold = e[out.split_bin - 1U];
    } else {
      out.threshold = e.empty() ? 0.0 : e.back();
    }
  }
  return out;
}

// Grow one depth-limited tree by a recursive best-split over row subsets. The
// node array is built breadth-of-recursion; each call appends its node and (when
// it splits) recurses for both children, patching the child indices afterward.
atx::i32 grow_node(GbtTree &tree, const TreeData &td, const std::vector<atx::usize> &rows,
                   const std::vector<atx::usize> &feats, atx::u32 depth_left, atx::f64 l2,
                   atx::f64 min_child, atx::f64 min_split_gain, bool is_root) {
  const atx::i32 self = static_cast<atx::i32>(tree.nodes.size());
  tree.nodes.push_back(GbtNode{}); // placeholder; patched below
  GbtNode node;
  node.is_leaf = true;
  node.leaf_value = leaf_value(td, rows, l2);
  if (depth_left == 0U || rows.size() < 2U) {
    tree.nodes[static_cast<atx::usize>(self)] = node;
    return self;
  }
  // The ROOT split is explored with no gain floor: a PURE interaction (e.g.
  // sign(f0)*sign(f1)) has ZERO marginal gain at the root, so a root-level floor
  // would prune the tree before the interaction can appear in a depth->=1 split.
  // Below the root the floor applies, so on a no-edge panel the depth->=1 children
  // find no qualifying split and stay leaves, while a genuine interaction's
  // depth->=1 split clears the floor and fires. `is_root` is true only for the
  // tree root (the boosting/stump entry points pass true; recursion passes false).
  const atx::f64 node_floor = is_root ? 0.0 : min_split_gain;
  const SplitResult sp = best_split(td, rows, feats, l2, min_child, node_floor);
  if (!sp.found) {
    tree.nodes[static_cast<atx::usize>(self)] = node;
    return self;
  }
  // Partition rows by the chosen split (ascending walk -> stable, deterministic).
  ATX_CHECK(sp.feature < td.p); // the split feature indexes within the design (NDEBUG)
  std::vector<atx::usize> left;
  std::vector<atx::usize> right;
  for (const atx::usize i : rows) {
    if (td.bins[i * td.p + sp.feature] < sp.split_bin) {
      left.push_back(i);
    } else {
      right.push_back(i);
    }
  }
  if (left.empty() || right.empty()) {
    tree.nodes[static_cast<atx::usize>(self)] = node; // degenerate -> leaf
    return self;
  }
  node.is_leaf = false;
  node.feature = static_cast<atx::u32>(sp.feature);
  node.threshold = sp.threshold;
  const atx::i32 lc = grow_node(tree, td, left, feats, depth_left - 1U, l2, min_child,
                                min_split_gain, /*is_root=*/false);
  const atx::i32 rc = grow_node(tree, td, right, feats, depth_left - 1U, l2, min_child,
                                min_split_gain, /*is_root=*/false);
  node.left = lc;
  node.right = rc;
  tree.nodes[static_cast<atx::usize>(self)] = node;
  return self;
}

// Pre-bin a design's rows into a TreeData (bins + the slots for g/h). The g/h are
// filled by the caller per boosting round (they change as F updates).
TreeData make_tree_data(const gbt_lin::MatX &X, const BinEdges &be) {
  TreeData td;
  td.X = &X;
  td.be = &be;
  td.p = static_cast<atx::usize>(X.cols());
  const atx::usize n = static_cast<atx::usize>(X.rows());
  td.bins.assign(n * td.p, 0U);
  atx::usize max_bin = 0U;
  for (atx::usize i = 0; i < n; ++i) {
    for (atx::usize f = 0; f < td.p; ++f) {
      const atx::usize b =
          bin_of(be, f, X(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(f)));
      td.bins[i * td.p + f] = b;
      max_bin = (b > max_bin) ? b : max_bin;
    }
  }
  td.n_bins = max_bin + 1U;
  td.g.assign(n, 0.0);
  td.h.assign(n, 1.0); // squared-error hessian is 1
  return td;
}

// A seeded subset of size round(frac * m) of [0, m), drawn deterministically from
// `rng` (a partial Fisher-Yates over an index permutation), returned ASCENDING so
// every downstream reduction stays order-fixed (M1). frac >= 1 -> all of [0, m).
std::vector<atx::usize> seeded_subset(atx::usize m, atx::f64 frac,
                                      atx::core::Xoshiro256pp &rng) {
  std::vector<atx::usize> idx(m);
  for (atx::usize i = 0; i < m; ++i) {
    idx[i] = i;
  }
  atx::usize keep = (frac >= 1.0) ? m
                                  : static_cast<atx::usize>(frac * static_cast<atx::f64>(m) + 0.5);
  if (keep == 0U && m > 0U) {
    keep = 1U; // always keep at least one (a 0-row/0-feature node is useless)
  }
  if (keep >= m) {
    return idx; // already ascending [0, m)
  }
  // Partial Fisher-Yates: pick `keep` distinct indices into the first slots.
  for (atx::usize i = 0; i < keep; ++i) {
    const atx::usize span = m - i;
    const atx::usize j = i + static_cast<atx::usize>(rng.next_u64() % span);
    const atx::usize tmp = idx[i];
    idx[i] = idx[j];
    idx[j] = tmp;
  }
  std::vector<atx::usize> chosen(idx.begin(), idx.begin() + static_cast<std::ptrdiff_t>(keep));
  std::sort(chosen.begin(), chosen.end()); // ascending -> order-fixed reductions
  return chosen;
}

// Fit a boosted forest on the (n x p) design X with labels y, using TRAIN-only bin
// edges `be`. F starts at base = mean(y); each round computes g = F - y, h = 1,
// grows one depth-limited tree over a seeded row+feature subsample, then
// F += learning_rate * tree.predict(row). Deterministic for a fixed master_seed.
GbtForest fit_forest(const gbt_lin::MatX &X, const gbt_lin::VecX &y,
                     const BinEdges &be, const GbtCfg &cfg,
                     atx::u64 forest_seed_master) {
  GbtForest forest;
  const atx::usize n = static_cast<atx::usize>(X.rows());
  if (n == 0U) {
    return forest;
  }
  TreeData td = make_tree_data(X, be);
  // base = mean(y); F initialized to base.
  atx::f64 base = 0.0;
  for (atx::usize i = 0; i < n; ++i) {
    base += y(static_cast<Eigen::Index>(i));
  }
  base /= static_cast<atx::f64>(n);
  forest.base = base;
  // The squared-error split gain scales with the label residual magnitude (gain ~
  // G^2/(H+l2), G a sum of gradients ~ label units), so the floor is scaled by the
  // train-label MEAN-square residual (Var(y) about base): a split must reduce the
  // node objective by at least min_split_gain * Var(y) to fire. Unit-free in the
  // return scale, and the dominant overfit guard — on a no-edge panel no split
  // clears this fraction-of-variance bar, so the forest stays at its constant base,
  // the OOF cross-section is flat, every per-date IC is 0 (degenerate series), and
  // the deflation gate cleanly rejects it (the tree analog of L1's exact-zeroing).
  // A genuine interaction explains a large fraction of variance and fires.
  atx::f64 var_y = 0.0;
  for (atx::usize i = 0; i < n; ++i) {
    const atx::f64 dv = y(static_cast<Eigen::Index>(i)) - base;
    var_y += dv * dv;
  }
  var_y /= static_cast<atx::f64>(n); // population variance of the label about base
  const atx::f64 gain_floor = cfg.min_split_gain * var_y;
  std::vector<atx::f64> F(n, base);
  for (atx::u32 t = 0; t < cfg.n_trees; ++t) {
    // Squared-error gradient/hessian at the current F.
    for (atx::usize i = 0; i < n; ++i) {
      td.g[i] = F[i] - y(static_cast<Eigen::Index>(i));
      td.h[i] = 1.0;
    }
    // Seeded row + feature subsample (distinct streams via the tag's a/b).
    atx::core::Xoshiro256pp rrng{seed_for(forest_seed_master, "gbt-rows", t, 0U)};
    atx::core::Xoshiro256pp frng{seed_for(forest_seed_master, "gbt-feat", t, 0U)};
    const std::vector<atx::usize> rows = seeded_subset(n, cfg.row_subsample, rrng);
    const std::vector<atx::usize> feats = seeded_subset(td.p, cfg.feature_subsample, frng);
    GbtTree tree;
    grow_node(tree, td, rows, feats, cfg.max_depth, cfg.l2, cfg.min_child, gain_floor,
              /*is_root=*/true);
    // Shrink the tree's leaf values by the learning rate, then update F over ALL
    // rows (subsampling affects which rows the tree was FIT on, not who it scores).
    for (GbtNode &node : tree.nodes) {
      if (node.is_leaf) {
        node.leaf_value *= cfg.learning_rate;
      }
    }
    for (atx::usize i = 0; i < n; ++i) {
      // SAFETY: Eigen X is column-major, so row i is NOT contiguous — copy its p
      // finite cells into a fresh p-element buffer and span THAT (valid for the
      // gbt_tree_predict call); no aliasing of X.
      std::vector<atx::f64> row(td.p);
      for (atx::usize f = 0; f < td.p; ++f) {
        row[f] = X(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(f));
      }
      F[i] += gbt_tree_predict(tree, std::span<const atx::f64>{row});
    }
    forest.trees.push_back(std::move(tree));
  }
  return forest;
}

// The genuine per-date OUT-OF-FOLD IC series for the GBT — fit_linear's
// detail::oof_ic_series, plus ONE numerical-hygiene step the tree model needs and
// the linear model gets for free. A tree forest on a NO-EDGE panel still predicts
// a tiny, evenly-spread per-date dispersion (a weak split fires, then nearly
// cancels), and detail::pearson's only guard is va == 0.0 EXACTLY — so that small
// dispersion becomes an O(1) spurious IC and the deflation gate sees phantom
// skill. The linear model dodges this because L1 drives noise coefficients to
// EXACTLY zero -> an exactly-constant prediction -> IC exactly 0.
//
// The tree analog is a RELATIVE dispersion floor against the GLOBAL OOF
// prediction scale: a no-edge forest spreads its (small) prediction dispersion
// roughly UNIFORMLY across dates, so every date's cross-sectional std is only a
// modest multiple of the global prediction std; a GENUINE interaction
// CONCENTRATES dispersion into the signal dates, whose std is a LARGE multiple of
// global. A date whose std is below rel_floor * global_pred_std carries no usable
// ranking information -> IC 0. On pure noise EVERY date falls below the cutoff ->
// an all-zero (degenerate) series -> the std==0 guard in oos_deflated_sharpe fires
// -> dsr 0 (rejected); a real interaction's signal dates clear it -> genuine skill
// survives. This STRENGTHENS the gate (it only ever zeroes low-information dates).
// Otherwise it is the identical genuine-OOF construction fit_linear uses.
std::vector<atx::f64>
oof_ic_series_floored(const FeatureMatrix &fm, std::span<const atx::f64> oof_sum,
                      std::span<const atx::u32> oof_cnt, atx::f64 rel_floor) {
  const atx::usize nr = fm.n_rows();
  // The prediction-energy scale the per-date floor is relative to: the std of the
  // horizon-0 LABEL over the covered OOF rows. A no-edge forest's OOF predictions
  // carry tiny energy RELATIVE to the label dispersion (weak, learning-rate-shrunk
  // splits), while a genuine interaction's predictions carry a sizeable fraction of
  // it. Scaling by the label std (not the prediction's own std) makes the floor a
  // SIGNAL-STRENGTH test, not a self-referential dispersion ratio (which cannot
  // distinguish noise from signal — both spread evenly across dates). Order-fixed
  // ascending reduction (M1).
  atx::f64 lab_mean = 0.0;
  atx::usize lab_cnt = 0;
  for (atx::usize i = 0; i < nr; ++i) {
    if (oof_cnt[i] != 0U && std::isfinite(fm.Y[0][i])) {
      lab_mean += fm.Y[0][i];
      ++lab_cnt;
    }
  }
  atx::f64 label_std = 0.0;
  if (lab_cnt > 0U) {
    lab_mean /= static_cast<atx::f64>(lab_cnt);
    atx::f64 lv = 0.0;
    for (atx::usize i = 0; i < nr; ++i) {
      if (oof_cnt[i] != 0U && std::isfinite(fm.Y[0][i])) {
        const atx::f64 d = fm.Y[0][i] - lab_mean;
        lv += d * d;
      }
    }
    label_std = std::sqrt(lv / static_cast<atx::f64>(lab_cnt));
  }
  std::vector<atx::f64> series;
  atx::usize r = 0;
  while (r < nr) {
    const atx::usize date = fm.row_date[r];
    std::vector<atx::f64> pv;
    std::vector<atx::f64> lv;
    atx::usize e = r;
    for (; e < nr && fm.row_date[e] == date; ++e) {
      if (oof_cnt[e] == 0U) {
        continue;
      }
      const atx::f64 label = fm.Y[0][e];
      if (!std::isfinite(label)) {
        continue;
      }
      pv.push_back(oof_sum[e] / static_cast<atx::f64>(oof_cnt[e]));
      lv.push_back(label);
    }
    if (pv.size() >= 2U) {
      // Relative dispersion of the prediction cross-section vs the GLOBAL OOF
      // prediction scale. A date whose cross-sectional std is below rel_floor times
      // the global prediction std carries no usable ranking information -> IC 0.
      // This is the tree analog of L1's exact-zeroing: a no-edge forest's per-date
      // dispersion is a small fraction of its global dispersion (uniform noise),
      // while a genuine interaction concentrates dispersion into the signal dates
      // (a large multiple of global). The cutoff cleanly separates the two.
      atx::f64 mn = 0.0;
      for (const atx::f64 v : pv) {
        mn += v;
      }
      mn /= static_cast<atx::f64>(pv.size());
      atx::f64 var = 0.0;
      for (const atx::f64 v : pv) {
        var += (v - mn) * (v - mn);
      }
      const atx::f64 std_dev = std::sqrt(var / static_cast<atx::f64>(pv.size()));
      if (std_dev <= rel_floor * label_std) {
        series.push_back(0.0); // sub-signal-strength dispersion -> no skill
      } else {
        series.push_back(
            detail::pearson(std::span<const atx::f64>{pv}, std::span<const atx::f64>{lv}));
      }
    }
    r = e;
  }
  return series;
}

} // namespace gbt_detail

// ===========================================================================
//  fit_gbt_single_tree — one depth-limited squared-error tree on a raw (X, y).
//
//  Exposed for the M4 stump differential test. Fits ONE tree (no subsampling, all
//  rows + all features) with the squared-error gradient g = mean(y) - y, h = 1
//  and an l2 = 0 / min_child = 1 leaf floor — so the gain-maximizing split is the
//  SSE-minimizing split, matching an independent brute-force best-threshold
//  search to within ~two bin widths (the histogram quantizes the cut point).
// ===========================================================================
GbtTree fit_gbt_single_tree(const gbt_lin::MatX &X, const gbt_lin::VecX &y,
                            atx::u32 max_depth, atx::u32 n_bins) {
  GbtTree tree;
  const atx::usize n = static_cast<atx::usize>(X.rows());
  if (n == 0U) {
    return tree;
  }
  const gbt_detail::BinEdges be = gbt_detail::fit_bin_edges(X, n_bins);
  gbt_detail::TreeData td = gbt_detail::make_tree_data(X, be);
  atx::f64 mean_y = 0.0;
  for (atx::usize i = 0; i < n; ++i) {
    mean_y += y(static_cast<Eigen::Index>(i));
  }
  mean_y /= static_cast<atx::f64>(n);
  for (atx::usize i = 0; i < n; ++i) {
    td.g[i] = mean_y - y(static_cast<Eigen::Index>(i)); // residual gradient about the mean
    td.h[i] = 1.0;
  }
  std::vector<atx::usize> rows(n);
  std::vector<atx::usize> feats(td.p);
  for (atx::usize i = 0; i < n; ++i) {
    rows[i] = i;
  }
  for (atx::usize f = 0; f < td.p; ++f) {
    feats[f] = f;
  }
  gbt_detail::grow_node(tree, td, rows, feats, max_depth, /*l2=*/0.0, /*min_child=*/1.0,
                        /*min_split_gain=*/0.0, /*is_root=*/true);
  return tree;
}

// ===========================================================================
//  fit_gbt — assemble the deployed multi-horizon histogram-GBT learned alpha.
//
//  Mirrors fit_linear exactly so the model plugs into the SAME deflation /
//  predict path: per horizon, walk the CPCV date-folds; on each fold fit
//  TRAIN-only bin edges + a TRAIN-only forest (seeded subsample), predict
//  OUT-OF-FOLD on the test rows (genuine OOS), accumulate the horizon-0 OOF
//  predictions, bump trial_count per fit. The DEPLOYED per-horizon forest is a
//  refit on the full trailing window. Horizon blend = normalize(max(oos_IC, 0)).
//  The OOS skill series is assembled from the OOF predictions (the SAME helper
//  fit_linear uses) so oos_deflated_sharpe is genuinely out-of-fold (M3).
//
//  Standardization stats are full-window (forward-applied — M2); the augmented
//  row layout is the shared build_augmented_row, so train/eval cannot drift and
//  the GBT trains/infers on the exact layout the linear model does.
// ===========================================================================
namespace {

// The fold TRAINING artifact of one GBT fold fit, flattened for LearnFitTrace: fold
// standardization, fold augmentation, then every tree node of the fold forest.
[[nodiscard]] std::vector<atx::f64> gbt_fold_artifact(const LearnedModel &fold_shell,
                                                      const GbtForest &forest) {
  std::vector<atx::f64> a;
  a.insert(a.end(), fold_shell.feat_mean.begin(), fold_shell.feat_mean.end());
  a.insert(a.end(), fold_shell.feat_sd.begin(), fold_shell.feat_sd.end());
  detail::append_augmentation(fold_shell.aug, a);
  a.push_back(forest.base);
  for (const GbtTree &tree : forest.trees) {
    a.push_back(static_cast<atx::f64>(tree.nodes.size()));
    for (const GbtNode &node : tree.nodes) {
      a.push_back(static_cast<atx::f64>(node.feature));
      a.push_back(node.threshold);
      a.push_back(node.leaf_value);
      a.push_back(static_cast<atx::f64>(node.left));
      a.push_back(static_cast<atx::f64>(node.right));
      a.push_back(node.is_leaf ? 1.0 : 0.0);
    }
  }
  return a;
}

} // namespace

namespace {
using atx::f64;
using atx::u64;
using atx::usize;
namespace co = atx::core;
constexpr usize kMissingBin = 255;
constexpr usize kHistogramWidth = 256;
struct HistCell {
  f64 gradient{};
  atx::u32 count{};
};
struct FitBudget {
  u64 maximum{}, used{};
  bool add(u64 count, u64 width) {
    if (width != 0 && count > (maximum - used) / width)
      return false;
    used += count * width;
    return true;
  }
};
co::Result<u64> v2_workspace(usize n, usize p, const GbtCfg &c) {
  if (n == 0 || p == 0 || n > std::numeric_limits<atx::u32>::max() ||
      p > std::numeric_limits<atx::u32>::max() || c.n_bins < 2 || c.n_bins > 255 ||
      c.max_depth > 16 || c.n_trees == 0 || c.workers == 0 || c.workers > 64 ||
      !std::isfinite(c.learning_rate) || c.learning_rate <= 0 || c.learning_rate > 1 ||
      !std::isfinite(c.row_subsample) || c.row_subsample <= 0 || c.row_subsample > 1 ||
      !std::isfinite(c.feature_subsample) || c.feature_subsample <= 0 || c.feature_subsample > 1 ||
      !std::isfinite(c.min_child) || c.min_child < 1 || !std::isfinite(c.l2) || c.l2 < 0 ||
      !std::isfinite(c.min_split_gain) || c.min_split_gain < 0)
    return co::Err(co::ErrorCode::InvalidArgument, "GBT V2: invalid geometry/recipe");
  const auto nodes = (u64{1} << (c.max_depth + 1U)) - 1U;
  FitBudget b{c.max_working_bytes, 0};
  // Owned bytes only; caller designs/labels are charged by their materializer.
  // Parent/smaller/sibling histograms and row vectors can coexist along depth.
  if (!b.add(n, p) || !b.add(n, 64) || !b.add(p, 4096) ||
      !b.add(n, u64{c.max_depth + 2U} * 3U * sizeof(usize)) ||
      !b.add(p, u64{c.max_depth + 2U} * 3U * kHistogramWidth * sizeof(HistCell)) ||
      !b.add(c.n_trees, nodes * sizeof(GbtNode) + sizeof(GbtTree)) ||
      !b.add(c.workers, 64U * 1024U) || !b.add(1, 64U * 1024U))
    return co::Err(co::ErrorCode::OutOfRange, "GBT V2: fit workspace budget exceeded");
  return co::Ok(b.used);
}

struct BinsV2 {
  usize rows{}, features{};
  std::vector<atx::u8> values; // feature-major: values[f*rows+r]
  gbt_detail::BinEdges edges;
};
BinsV2 make_bins_v2(const gbt_lin::MatX &X, atx::u32 requested) {
  BinsV2 out;
  out.rows = static_cast<usize>(X.rows());
  out.features = static_cast<usize>(X.cols());
  out.values.resize(out.rows * out.features);
  out.edges.edges.resize(out.features);
  std::vector<f64> column;
  column.reserve(out.rows);
  for (usize f = 0; f < out.features; ++f) {
    column.clear();
    for (usize r = 0; r < out.rows; ++r) {
      const auto value = X(static_cast<Eigen::Index>(r), static_cast<Eigen::Index>(f));
      if (std::isfinite(value))
        column.push_back(value);
    }
    std::sort(column.begin(), column.end());
    auto &edges = out.edges.edges[f];
    edges.reserve(requested - 1U);
    if (!column.empty())
      for (usize cut = 1; cut < requested; ++cut) {
        const auto value = column[(cut * column.size()) / requested];
        if (edges.empty() || value > edges.back())
          edges.push_back(value);
      }
    for (usize r = 0; r < out.rows; ++r) {
      const auto value = X(static_cast<Eigen::Index>(r), static_cast<Eigen::Index>(f));
      out.values[f * out.rows + r] =
          std::isnan(value)
              ? static_cast<atx::u8>(kMissingBin)
              : static_cast<atx::u8>(std::upper_bound(edges.begin(), edges.end(), value) -
                                     edges.begin());
    }
  }
  return out;
}

using Histogram = std::vector<HistCell>;
struct SplitV2 {
  usize feature_slot{}, bin{};
  f64 gain{};
  bool found{};
};
struct BuilderV2 {
  const BinsV2 &bins;
  const std::vector<f64> &gradient;
  const std::vector<usize> &features;
  const GbtCfg &cfg;
  parallel::DetPool &pool;
  GbtFitDiagnostics &stats;
  std::vector<f64> &gains;

  co::Result<Histogram> histogram(std::span<const usize> rows) {
    Histogram out(features.size() * kHistogramWidth);
    std::vector<atx::u8> invalid(features.size(), 0);
    // Scheduling changes only the feature owner; every sum retains row order.
    pool.parallel_for(features.size(), [&](usize slot, usize) {
      const auto f = features[slot];
      auto *hist = out.data() + slot * kHistogramWidth;
      for (const auto r : rows) {
        auto &cell = hist[bins.values[f * bins.rows + r]];
        cell.gradient += gradient[r];
        ++cell.count;
        if (!std::isfinite(cell.gradient))
          invalid[slot] = 1;
      }
    });
    if (std::find(invalid.begin(), invalid.end(), atx::u8{1}) != invalid.end())
      return co::Err(co::ErrorCode::OutOfRange, "GBT V2: histogram overflow");
    if (features.size() != 0 &&
        rows.size() > (std::numeric_limits<u64>::max() - stats.histogram_rows) / features.size())
      return co::Err(co::ErrorCode::OutOfRange, "GBT V2: histogram counter overflow");
    stats.histogram_rows += rows.size() * features.size();
    return co::Ok(std::move(out));
  }
  co::Result<SplitV2> split(const Histogram &hist, f64 total, usize count, f64 floor) const {
    SplitV2 best;
    best.gain = floor;
    const auto parent = total * total / (static_cast<f64>(count) + cfg.l2);
    if (!std::isfinite(parent))
      return co::Err(co::ErrorCode::OutOfRange, "GBT V2: parent gain overflow");
    for (usize slot = 0; slot < features.size(); ++slot) {
      f64 left = 0;
      usize nleft = 0;
      // The reserved missing bin remains on the right at every split.
      const auto cuts = bins.edges.edges[features[slot]].size();
      for (usize bin = 1; bin <= cuts; ++bin) {
        const auto &cell = hist[slot * kHistogramWidth + bin - 1];
        left += cell.gradient;
        nleft += cell.count;
        if (nleft > count)
          return co::Err(co::ErrorCode::Internal, "GBT V2: histogram count differs");
        const auto nright = count - nleft;
        if (static_cast<f64>(nleft) < cfg.min_child || static_cast<f64>(nright) < cfg.min_child)
          continue;
        const auto right = total - left;
        const auto gain = 0.5 * (left * left / (static_cast<f64>(nleft) + cfg.l2) +
                                 right * right / (static_cast<f64>(nright) + cfg.l2) - parent);
        if (!std::isfinite(gain))
          return co::Err(co::ErrorCode::OutOfRange, "GBT V2: split gain overflow");
        if (gain > best.gain)
          best = {slot, bin, gain, true};
      }
    }
    return co::Ok(best);
  }
  co::Result<atx::i32> grow(GbtTree &tree, const std::vector<usize> &rows, const Histogram &hist,
                            atx::u32 depth, f64 gain_floor, bool root) {
    f64 total = 0;
    for (const auto r : rows)
      total += gradient[r];
    if (!std::isfinite(total))
      return co::Err(co::ErrorCode::OutOfRange, "GBT V2: node sum overflow");
    GbtNode node;
    node.leaf_value = -total / (static_cast<f64>(rows.size()) + cfg.l2);
    if (!std::isfinite(node.leaf_value))
      return co::Err(co::ErrorCode::OutOfRange, "GBT V2: leaf overflow");
    const auto self = static_cast<atx::i32>(tree.nodes.size());
    tree.nodes.push_back(node);
    if (depth == 0 || rows.size() < 2)
      return co::Ok(self);
    ATX_TRY(auto choice, split(hist, total, rows.size(), root ? 0.0 : gain_floor));
    if (!choice.found)
      return co::Ok(self);
    const auto f = features[choice.feature_slot];
    std::vector<usize> left, right;
    left.reserve(rows.size());
    right.reserve(rows.size());
    for (const auto r : rows)
      (bins.values[f * bins.rows + r] < choice.bin ? left : right).push_back(r);
    if (left.empty() || right.empty())
      return co::Ok(self);
    const bool build_left = left.size() <= right.size();
    ATX_TRY(auto smaller,
            histogram(build_left ? std::span<const usize>{left} : std::span<const usize>{right}));
    Histogram sibling(hist.size());
    bool cancellation = false;
    for (usize i = 0; i < hist.size(); ++i) {
      if (smaller[i].count > hist[i].count)
        return co::Err(co::ErrorCode::Internal, "GBT V2: invalid histogram subtraction");
      sibling[i] = {hist[i].gradient - smaller[i].gradient, hist[i].count - smaller[i].count};
      if (sibling[i].count == 0)
        sibling[i].gradient = 0; // no phantom gradient in an empty bin
      else if (std::abs(sibling[i].gradient) <=
               64 * std::numeric_limits<f64>::epsilon() *
                   (std::abs(hist[i].gradient) + std::abs(smaller[i].gradient)))
        cancellation = true;
      if (!std::isfinite(sibling[i].gradient))
        return co::Err(co::ErrorCode::OutOfRange, "GBT V2: histogram subtraction overflow");
    }
    if (cancellation) {
      ATX_TRY(sibling,
              histogram(build_left ? std::span<const usize>{right} : std::span<const usize>{left}));
      ++stats.histogram_rebuilds;
    }
    ++stats.histogram_subtractions;
    const auto &left_hist = build_left ? smaller : sibling;
    const auto &right_hist = build_left ? sibling : smaller;
    node.is_leaf = false;
    node.feature = static_cast<atx::u32>(f);
    node.threshold = bins.edges.edges[f][choice.bin - 1];
    ATX_TRY(node.left, grow(tree, left, left_hist, depth - 1, gain_floor, false));
    ATX_TRY(node.right, grow(tree, right, right_hist, depth - 1, gain_floor, false));
    gains[f] += choice.gain;
    if (!std::isfinite(gains[f]))
      return co::Err(co::ErrorCode::OutOfRange, "GBT V2: importance overflow");
    tree.nodes[static_cast<usize>(self)] = node;
    return co::Ok(self);
  }
};

f64 matrix_tree_predict(const GbtTree &tree, const gbt_lin::MatX &X, usize row) {
  atx::i32 index = 0;
  for (usize steps = 0; steps < tree.nodes.size(); ++steps) {
    const auto &node = tree.nodes[static_cast<usize>(index)];
    if (node.is_leaf)
      return node.leaf_value;
    index =
        X(static_cast<Eigen::Index>(row), static_cast<Eigen::Index>(node.feature)) < node.threshold
            ? node.left
            : node.right;
  }
  return std::numeric_limits<f64>::quiet_NaN();
}
} // namespace

co::Result<GbtForest> fit_gbt_forest_checked(const gbt_lin::MatX &X, const gbt_lin::VecX &y,
                                             const GbtCfg &cfg, u64 seed,
                                             std::span<const usize> row_dates,
                                             GbtFitDiagnostics *diagnostics) {
  if (cfg.rule == GbtRule::LegacyV1) {
    if (X.rows() != y.size())
      return co::Err(co::ErrorCode::InvalidArgument, "GBT: label shape differs");
    const auto edges = gbt_detail::fit_bin_edges(X, cfg.n_bins);
    if (diagnostics)
      *diagnostics = GbtFitDiagnostics{};
    return co::Ok(gbt_detail::fit_forest(X, y, edges, cfg, seed));
  }
  if (cfg.rule != GbtRule::ColumnBinsV2 || X.rows() != y.size())
    return co::Err(co::ErrorCode::InvalidArgument, "GBT V2: rule/label shape");
  const auto n = static_cast<usize>(X.rows()), p = static_cast<usize>(X.cols());
  ATX_TRY(const auto budget, v2_workspace(n, p, cfg));
  if (cfg.demean_loss_by_date &&
      (row_dates.size() != n || !std::is_sorted(row_dates.begin(), row_dates.end())))
    return co::Err(co::ErrorCode::InvalidArgument, "GBT V2: ordered original row dates required");
  for (Eigen::Index r = 0; r < X.rows(); ++r) {
    if (!std::isfinite(y(r)))
      return co::Err(co::ErrorCode::InvalidArgument, "GBT V2: nonfinite used label");
    for (Eigen::Index f = 0; f < X.cols(); ++f)
      if (std::isinf(X(r, f)))
        return co::Err(co::ErrorCode::InvalidArgument, "GBT V2: infinite feature");
  }
  GbtFitDiagnostics stats;
  stats.rule = cfg.rule;
  stats.workspace_bound_bytes = budget;
  stats.bin_bytes = n * p;
  stats.forest_fits = 1;
  auto bins = make_bins_v2(X, cfg.n_bins);
  GbtForest forest;
  forest.trees.reserve(cfg.n_trees);
  if (!cfg.demean_loss_by_date) {
    for (usize r = 0; r < n; ++r)
      forest.base += y(static_cast<Eigen::Index>(r));
    forest.base /= static_cast<f64>(n);
  }
  std::vector<f64> prediction(n, forest.base), gradient(n), gains(p, 0);
  f64 variance = 0;
  for (usize r = 0; r < n; ++r) {
    const auto value = y(static_cast<Eigen::Index>(r)) - forest.base;
    variance += value * value;
  }
  if (cfg.demean_loss_by_date) {
    variance = 0;
    for (usize first = 0; first < n;) {
      usize last = first + 1;
      while (last < n && row_dates[last] == row_dates[first])
        ++last;
      f64 mean = 0;
      for (usize r = first; r < last; ++r)
        mean += y(static_cast<Eigen::Index>(r));
      mean /= static_cast<f64>(last - first);
      for (usize r = first; r < last; ++r) {
        const auto delta = y(static_cast<Eigen::Index>(r)) - mean;
        variance += delta * delta;
      }
      first = last;
    }
  }
  const auto gain_floor = cfg.min_split_gain * (variance / static_cast<f64>(n));
  if (!std::isfinite(forest.base) || !std::isfinite(gain_floor))
    return co::Err(co::ErrorCode::OutOfRange, "GBT V2: label moments overflow");
  parallel::DetPool pool{cfg.workers};
  for (atx::u32 t = 0; t < cfg.n_trees; ++t) {
    for (usize r = 0; r < n; ++r)
      gradient[r] = prediction[r] - y(static_cast<Eigen::Index>(r));
    if (cfg.demean_loss_by_date) {
      for (usize first = 0; first < n;) {
        usize last = first + 1;
        while (last < n && row_dates[last] == row_dates[first])
          ++last;
        f64 mean = 0;
        for (usize r = first; r < last; ++r)
          mean += gradient[r];
        mean /= static_cast<f64>(last - first);
        for (usize r = first; r < last; ++r)
          gradient[r] -= mean;
        first = last;
      }
    }
    for (const auto value : gradient)
      if (!std::isfinite(value))
        return co::Err(co::ErrorCode::OutOfRange, "GBT V2: gradient overflow");
    atx::core::Xoshiro256pp row_rng{seed_for(seed, "gbt-rows", t, 0)},
        feature_rng{seed_for(seed, "gbt-feat", t, 0)};
    const auto rows = gbt_detail::seeded_subset(n, cfg.row_subsample, row_rng);
    const auto features = gbt_detail::seeded_subset(p, cfg.feature_subsample, feature_rng);
    BuilderV2 builder{bins, gradient, features, cfg, pool, stats, gains};
    ATX_TRY(auto hist, builder.histogram(rows));
    GbtTree tree;
    tree.nodes.reserve(static_cast<usize>((u64{1} << (cfg.max_depth + 1U)) - 1U));
    ATX_TRY(auto root, builder.grow(tree, rows, hist, cfg.max_depth, gain_floor, true));
    (void)root;
    for (auto &node : tree.nodes)
      if (node.is_leaf)
        node.leaf_value *= cfg.learning_rate;
    for (usize r = 0; r < n; ++r) {
      prediction[r] += matrix_tree_predict(tree, X, r);
      if (!std::isfinite(prediction[r]))
        return co::Err(co::ErrorCode::OutOfRange, "GBT V2: prediction overflow");
    }
    forest.trees.push_back(std::move(tree));
  }
  f64 total = 0;
  for (auto gain : gains)
    total += gain;
  if (!std::isfinite(total))
    return co::Err(co::ErrorCode::OutOfRange, "GBT V2: total importance overflow");
  if (total > 0)
    for (auto &gain : gains)
      gain /= total;
  stats.split_gain_total = total;
  stats.gain_importance = std::move(gains);
  if (diagnostics)
    *diagnostics = std::move(stats);
  return co::Ok(std::move(forest));
}

namespace {
co::Result<usize> v2_augmented_columns(usize base, const LatentAugmentation &aug) {
  const auto latent = aug.pca ? aug.pca->k : 0U;
  const auto maximum = std::numeric_limits<atx::u32>::max();
  if (base == 0 || base > maximum || latent > maximum - base ||
      aug.interactions.size() > maximum - base - latent)
    return co::Err(co::ErrorCode::OutOfRange, "GBT V2: augmented column geometry");
  if (aug.pca && latent != 0 &&
      (aug.pca->model.mean.size() != static_cast<Eigen::Index>(base) ||
       aug.pca->model.components.rows() != static_cast<Eigen::Index>(base) ||
       aug.pca->model.components.cols() != static_cast<Eigen::Index>(latent) ||
       !aug.pca->model.mean.allFinite() || !aug.pca->model.components.allFinite()))
    return co::Err(co::ErrorCode::InvalidArgument, "GBT V2: malformed PCA basis");
  for (auto [a, b] : aug.interactions)
    if (a >= base || b >= base)
      return co::Err(co::ErrorCode::InvalidArgument, "GBT V2: interaction outside feature axis");
  return co::Ok(base + latent + aug.interactions.size());
}
co::Result<u64> v2_model_workspace(usize rows, usize base, const LatentAugmentation &aug,
                                   const GbtCfg &cfg, bool trace) {
  if (cfg.cpcv.rule != eval::CpcvRule::DateV2 ||
      cfg.protocol.fold_aug != FoldAugRule::FoldLocalV2 || cfg.horizons.empty() ||
      cfg.horizons.size() > 256 || cfg.cpcv.n_groups > 20 || cfg.cpcv.n_groups < 2 ||
      cfg.cpcv.n_test_groups == 0 || cfg.cpcv.n_test_groups >= cfg.cpcv.n_groups)
    return co::Err(co::ErrorCode::InvalidArgument, "GBT V2: DateV2/fold-local recipe required");
  ATX_TRY(auto columns, v2_augmented_columns(base, aug));
  ATX_TRY(auto backend, v2_workspace(rows, columns, cfg));
  const auto nodes = (u64{1} << (cfg.max_depth + 1U)) - 1U;
  const auto forest_bytes = u64{cfg.n_trees} * (nodes * sizeof(GbtNode) + sizeof(GbtTree));
  FitBudget b{cfg.max_working_bytes, 0};
  // Includes simultaneous train/test f64 designs, OOF/row scratch, augmentation
  // covariance work, retained deployed forests/shell copies and the separately
  // checked CPCV plan/expanded rows/metadata ceilings. Caller FeatureMatrix is
  // not owned here; the dataset bridge charges its materialization separately.
  if (!b.add(backend, 1) || !b.add(rows, columns * 16U + 256U) || !b.add(base, base * 64U) ||
      !b.add(cfg.cpcv.max_working_bytes, 3U) || !b.add(cfg.horizons.size(), forest_bytes * 3U))
    return co::Err(co::ErrorCode::OutOfRange, "GBT V2: model fit aggregate budget");
  if (trace) {
    const auto folds = eval::detail::binomial(cfg.cpcv.n_groups, cfg.cpcv.n_test_groups);
    FitBudget per_fold{cfg.max_working_bytes, 0};
    if (!per_fold.add(rows, 32U) || !per_fold.add(forest_bytes, 2U) ||
        !per_fold.add(base, base * 16U + 64U) || !b.add(folds * cfg.horizons.size(), per_fold.used))
      return co::Err(co::ErrorCode::OutOfRange, "GBT V2: retained trace budget");
  }
  return co::Ok(b.used);
}
co::Status accumulate_fit_stats(GbtFitDiagnostics &out, const GbtFitDiagnostics &in,
                                bool deployed) {
  out.rule = GbtRule::ColumnBinsV2;
  out.bin_bytes = std::max(out.bin_bytes, in.bin_bytes);
  out.workspace_bound_bytes = std::max(out.workspace_bound_bytes, in.workspace_bound_bytes);
  const auto add = [](u64 &a, u64 b) {
    if (b > std::numeric_limits<u64>::max() - a)
      return false;
    a += b;
    return true;
  };
  if (!add(out.forest_fits, in.forest_fits) || !add(out.histogram_rows, in.histogram_rows) ||
      !add(out.histogram_subtractions, in.histogram_subtractions) ||
      !add(out.histogram_rebuilds, in.histogram_rebuilds))
    return co::Err(co::ErrorCode::OutOfRange, "GBT V2: fit diagnostic counter overflow");
  if (deployed) {
    if (out.gain_importance.empty())
      out.gain_importance.assign(in.gain_importance.size(), 0);
    if (out.gain_importance.size() != in.gain_importance.size())
      return co::Err(co::ErrorCode::Internal, "GBT V2: deployed importance geometry");
    for (usize i = 0; i < in.gain_importance.size(); ++i) {
      out.gain_importance[i] += in.gain_importance[i] * in.split_gain_total;
      if (!std::isfinite(out.gain_importance[i]))
        return co::Err(co::ErrorCode::OutOfRange, "GBT V2: aggregate importance overflow");
    }
    out.split_gain_total += in.split_gain_total;
    if (!std::isfinite(out.split_gain_total))
      return co::Err(co::ErrorCode::OutOfRange, "GBT V2: aggregate split gain overflow");
  }
  return co::Ok();
}
// Canonical little-endian words, row-major matrix coordinates, no locale or
// Eigen storage-order dependence. Incremental hashing retains constant scratch.
co::Result<std::string> augmentation_identity(const LatentAugmentation &aug) {
  co::Sha256 hash;
  const auto word = [&](u64 value) -> co::Status {
    std::array<std::byte, 8> bytes{};
    for (usize i = 0; i < bytes.size(); ++i)
      bytes[i] = static_cast<std::byte>((value >> (8U * i)) & 255U);
    return hash.update(bytes);
  };
  const auto matrix = [&](const auto &values) -> co::Status {
    ATX_TRY_VOID(word(static_cast<u64>(values.rows())));
    ATX_TRY_VOID(word(static_cast<u64>(values.cols())));
    for (Eigen::Index row = 0; row < values.rows(); ++row)
      for (Eigen::Index col = 0; col < values.cols(); ++col)
        ATX_TRY_VOID(word(std::bit_cast<u64>(values(row, col))));
    return co::Ok();
  };
  ATX_TRY_VOID(word(1)); // augmentation identity format
  ATX_TRY_VOID(word(aug.pca.has_value()));
  if (aug.pca) {
    ATX_TRY_VOID(word(aug.pca->k));
    ATX_TRY_VOID(word(aug.pca->fit_upto_date));
    ATX_TRY_VOID(matrix(aug.pca->model.mean));
    ATX_TRY_VOID(matrix(aug.pca->model.components));
    ATX_TRY_VOID(matrix(aug.pca->model.explained_variance));
    ATX_TRY_VOID(matrix(aug.pca->model.explained_ratio));
  }
  ATX_TRY_VOID(word(aug.interactions_fixed));
  ATX_TRY_VOID(word(aug.interactions.size()));
  for (const auto &[a, b] : aug.interactions) {
    ATX_TRY_VOID(word(a));
    ATX_TRY_VOID(word(b));
  }
  ATX_TRY(auto digest, hash.finalize());
  constexpr char hex[] = "0123456789abcdef";
  std::string text;
  text.reserve(64);
  for (auto byte : digest) {
    const auto value = std::to_integer<unsigned>(byte);
    text.push_back(hex[value >> 4U]);
    text.push_back(hex[value & 15U]);
  }
  return co::Ok(std::move(text));
}
std::string gbt_v2_recipe(const GbtCfg &c) {
  std::string out =
      "gbt-column-bins-v2;missing255-fixed-right;hist-subtract-cancel-rebuild-v1;early-stop=none";
  const auto word = [&](u64 v) {
    out += ';';
    out += std::to_string(v);
  };
  for (u64 v : {static_cast<u64>(c.rule), u64{c.n_trees}, u64{c.max_depth}, u64{c.n_bins},
                c.master_seed, u64{c.demean_loss_by_date}, static_cast<u64>(c.protocol.fold_aug),
                static_cast<u64>(c.protocol.trials), static_cast<u64>(c.protocol.blend_ic),
                eval::cpcv_recipe_identity(c.cpcv)})
    word(v);
  for (f64 v :
       {c.learning_rate, c.row_subsample, c.feature_subsample, c.min_child, c.l2, c.min_split_gain})
    word(std::bit_cast<u64>(v));
  for (auto h : c.horizons)
    word(h);
  return out; // worker count is not a numerical recipe input
}
} // namespace

co::Result<DatasetGbtFit> fit_gbt_dataset(const PanelDataset &dataset, usize begin, usize end,
                                          usize asof, u64 max_bytes, const LatentAugmentation &aug,
                                          const GbtCfg &cfg, LearnFitTrace *trace) {
  const auto &source = dataset.config();
  if (cfg.rule != GbtRule::ColumnBinsV2 || begin >= end || end > source.session_keys.size() ||
      asof >= source.session_keys.size() || end - 1 > asof ||
      (aug.pca && aug.pca->fit_upto_date > asof))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "dataset GBT: V2/bounded asof/nonfuture PCA required");
  if (cfg.horizons.size() != source.holding_horizons.size())
    return co::Err(co::ErrorCode::InvalidArgument, "dataset GBT: horizon identity differs");
  for (usize h = 0; h < cfg.horizons.size(); ++h) {
    const auto endpoint = usize{source.holding_horizons[h]} + source.execution_delay;
    if (endpoint > std::numeric_limits<atx::u16>::max() || cfg.horizons[h] != endpoint)
      return co::Err(co::ErrorCode::InvalidArgument,
                     "dataset GBT: holding-plus-delay endpoint differs");
  }
  const auto count = source.instrument_ids.size(), features = source.feature_names.size() * 2U;
  if (count != 0 && end - begin > std::numeric_limits<usize>::max() / count)
    return co::Err(co::ErrorCode::OutOfRange, "dataset GBT: row bound overflow");
  const auto rows = (end - begin) * count;
  ATX_TRY(auto fit_bytes, v2_model_workspace(rows, features, aug, cfg, trace != nullptr));
  FitBudget combined{cfg.max_working_bytes, fit_bytes};
  if (!combined.add(1, 32U * 1024U * 1024U) || !combined.add(source.max_mapped_bytes, 1U) ||
      !combined.add(rows, u64{features + cfg.horizons.size()} * 8U + 192U))
    return co::Err(co::ErrorCode::OutOfRange, "dataset GBT: combined materialization/fit budget");
  ATX_TRY(auto fm, read_dataset_features(dataset, begin, end, asof, max_bytes));
  GbtFitDiagnostics diagnostics;
  ATX_TRY(auto model, fit_gbt_checked(fm, aug, cfg, trace, &diagnostics));
  diagnostics.workspace_bound_bytes = combined.used;
  ATX_TRY(auto augmentation_sha, augmentation_identity(aug));
  return co::Ok(DatasetGbtFit{std::move(model), fm.dataset_manifest_sha256, fm.dataset_recipe,
                              gbt_v2_recipe(cfg) + ";augmentation-sha256=" + augmentation_sha, std::move(diagnostics)});
}

LearnedModel fit_gbt(const FeatureMatrix &fm, const LatentAugmentation &aug, const GbtCfg &cfg) {
  return fit_gbt(fm, aug, cfg, nullptr);
}

LearnedModel fit_gbt(const FeatureMatrix &fm, const LatentAugmentation &aug, const GbtCfg &cfg,
                     LearnFitTrace *trace) {
  auto result = fit_gbt_checked(fm, aug, cfg, trace);
  ATX_CHECK(result.has_value());
  return std::move(*result);
}

atx::core::Result<LearnedModel> fit_gbt_checked(const FeatureMatrix &fm,
                                                const LatentAugmentation &aug, const GbtCfg &cfg,
                                                LearnFitTrace *trace,
                                                GbtFitDiagnostics *diagnostics) {
  ATX_TRY_VOID(validate_date_cpcv_inputs(fm, cfg.horizons, cfg.cpcv));
  GbtFitDiagnostics fit_stats;
  if (cfg.rule == GbtRule::ColumnBinsV2) {
    ATX_TRY(fit_stats.workspace_bound_bytes,
            v2_model_workspace(fm.n_rows(), fm.n_features, aug, cfg, trace != nullptr));
    fit_stats.rule = cfg.rule;
  } else if (cfg.rule != GbtRule::LegacyV1) {
    return co::Err(co::ErrorCode::InvalidArgument, "GBT: unknown fit rule");
  }
  LearnedModel m;
  std::vector<eval::CpcvMetadata> cpcv_metadata;
  m.kind = ModelKind::Gbt;
  m.aug = aug;
  m.n_base_features = static_cast<atx::u32>(fm.n_features);
  m.horizons = cfg.horizons;
  m.trial_count = 0;
  atx::usize n_fold_fits = 0; // successful fold fits (TrialCountRule input, L-08)
  if (trace != nullptr) {
    *trace = LearnFitTrace{}; // a reused trace starts empty
  }

  // Deployed full-window standardization (the forward-applied transform, M2).
  std::vector<atx::usize> all_valid;
  for (atx::usize r = 0; r < fm.n_rows(); ++r) {
    if (fm.row_valid[r] != 0) {
      all_valid.push_back(r);
    }
  }
  detail::fit_standardization(fm, std::span<const atx::usize>{all_valid}, m.feat_mean, m.feat_sd);

  m.forests.assign(cfg.horizons.size(), GbtForest{});
  m.blend_w.assign(cfg.horizons.size(), 0.0);
  std::vector<atx::f64> oos_ic_h(cfg.horizons.size(), 0.0);

  // Horizon-0 OOF accumulators keyed by FeatureMatrix row (average across folds).
  std::vector<atx::f64> oof_pred_sum(fm.n_rows(), 0.0);
  std::vector<atx::u32> oof_pred_cnt(fm.n_rows(), 0U);

  for (atx::usize h = 0; h < cfg.horizons.size(); ++h) {
    ATX_TRY(auto plan, learn_cpcv_plan(fm, cfg.horizons[h], cfg.cpcv));
    ATX_TRY(auto folds, expand_date_folds_checked(plan.folds, fm, cfg.cpcv));
    if (cfg.cpcv.rule == eval::CpcvRule::DateV2)
      ATX_TRY_VOID(retain_cpcv_metadata(cpcv_metadata, std::move(plan.metadata),
                                        cfg.cpcv.max_working_bytes));

    std::vector<atx::f64> oos_pred;
    std::vector<atx::f64> oos_label;
    std::vector<atx::f64> oof_sum_h(fm.n_rows(), 0.0); // horizon-h OOF (MeanDateIcV2)
    std::vector<atx::u32> oof_cnt_h(fm.n_rows(), 0U);
    const atx::usize sel_label = fold_selection_label(std::span<const atx::u16>{cfg.horizons}, h);
    atx::usize fold_idx = 0;
    for (const RowFold &f : folds) {
      // Fold-local standardization on the TRAIN rows only (M2), applied forward to
      // both the train and OOS test design.
      LearnedModel fold_shell = m;
      detail::fit_standardization(fm, std::span<const atx::usize>{f.train_rows},
                                  fold_shell.feat_mean, fold_shell.feat_sd);
      // L-03 firewall: refit the augmentation recipe on the fold's TRAIN rows only
      // (FoldLocalV2) instead of copying the caller's full-window fit.
      fold_shell.aug = fold_augmentation(fm, aug, std::span<const atx::usize>{f.train_rows},
                                         sel_label, cfg.protocol.fold_aug);
      gbt_lin::MatX Xtr;
      gbt_lin::VecX ytr;
      std::vector<atx::usize> kept_train;
      const atx::usize ntr =
          detail::build_design(fm, fold_shell, std::span<const atx::usize>{f.train_rows}, h, Xtr,
                               ytr, cfg.rule == GbtRule::ColumnBinsV2 ? &kept_train : nullptr);
      if (ntr == 0U) {
        ++fold_idx;
        continue;
      }
      // TRAIN-only bin edges, then a fold-local boosted forest (seeded per fold so
      // distinct folds draw distinct subsamples but are reproducible — M1).
      const atx::u64 fold_seed = seed_for(cfg.master_seed, "gbt-fold", h, fold_idx);
      GbtForest forest;
      if (cfg.rule == GbtRule::LegacyV1) {
        const auto be = gbt_detail::fit_bin_edges(Xtr, cfg.n_bins);
        forest = gbt_detail::fit_forest(Xtr, ytr, be, cfg, fold_seed);
      } else {
        for (auto &row : kept_train)
          row = fm.row_date[row];
        GbtFitDiagnostics local;
        ATX_TRY(forest, fit_gbt_forest_checked(Xtr, ytr, cfg, fold_seed, kept_train, &local));
        ATX_TRY_VOID(accumulate_fit_stats(fit_stats, local, false));
      }
      ++n_fold_fits;

      gbt_lin::MatX Xte;
      gbt_lin::VecX yte;
      std::vector<atx::usize> te_rows;
      const atx::usize nte = detail::build_design(
          fm, fold_shell, std::span<const atx::usize>{f.test_rows}, h, Xte, yte, &te_rows);
      std::vector<atx::f64> row(static_cast<atx::usize>(Xte.cols()));
      LearnFoldRecord rec;
      for (atx::usize i = 0; i < nte; ++i) {
        for (atx::usize j = 0; j < row.size(); ++j) {
          row[j] = Xte(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j));
        }
        const atx::f64 pred = gbt_forest_predict(forest, std::span<const atx::f64>{row});
        oos_pred.push_back(pred);
        oos_label.push_back(yte(static_cast<Eigen::Index>(i)));
        oof_sum_h[te_rows[i]] += pred;
        oof_cnt_h[te_rows[i]] += 1U;
        if (h == 0U) {
          oof_pred_sum[te_rows[i]] += pred;
          oof_pred_cnt[te_rows[i]] += 1U;
        }
        if (trace != nullptr) {
          rec.test_keys.push_back(te_rows[i]);
          rec.test_pred.push_back(pred);
        }
      }
      if (trace != nullptr) {
        rec.horizon_idx = h;
        rec.fold_idx = fold_idx;
        rec.artifact = gbt_fold_artifact(fold_shell, forest);
        rec.fit_keys = f.train_rows;
        trace->folds.push_back(std::move(rec));
      }
      ++fold_idx;
    }
    oos_ic_h[h] = (cfg.protocol.blend_ic == HorizonBlendIc::PooledPearsonV1)
                      ? detail::pearson(std::span<const atx::f64>{oos_pred},
                                        std::span<const atx::f64>{oos_label})
                      : detail::oof_mean_date_ic(std::span<const atx::usize>{fm.row_date},
                                                 std::span<const atx::f64>{fm.Y[h]},
                                                 std::span<const atx::f64>{oof_sum_h},
                                                 std::span<const atx::u32>{oof_cnt_h});

    // Deployed per-horizon forest: refit on the full trailing window with m's
    // full-window standardization + full-window bin edges (forward-applied, M2).
    gbt_lin::MatX Xfull;
    gbt_lin::VecX yfull;
    std::vector<atx::usize> kept_full;
    const atx::usize nfull =
        detail::build_design(fm, m, all_valid, h, Xfull, yfull,
                             cfg.rule == GbtRule::ColumnBinsV2 ? &kept_full : nullptr);
    if (nfull > 0U) {
      const atx::u64 deploy_seed = seed_for(cfg.master_seed, "gbt-deploy", h, 0U);
      if (cfg.rule == GbtRule::LegacyV1) {
        const auto be = gbt_detail::fit_bin_edges(Xfull, cfg.n_bins);
        m.forests[h] = gbt_detail::fit_forest(Xfull, yfull, be, cfg, deploy_seed);
      } else {
        for (auto &row : kept_full)
          row = fm.row_date[row];
        GbtFitDiagnostics local;
        ATX_TRY(m.forests[h],
                fit_gbt_forest_checked(Xfull, yfull, cfg, deploy_seed, kept_full, &local));
        ATX_TRY_VOID(accumulate_fit_stats(fit_stats, local, true));
      }
    }
  }

  // Genuine per-date OOS skill series from the horizon-0 OOF predictions — the same
  // genuine-OOF construction fit_linear uses, with the tree-model dispersion floor
  // (see gbt_detail::oof_ic_series_floored) so a no-edge cross-section whose
  // prediction energy is below signal strength scores IC 0. Frozen for the gate (M3).
  m.oos_score_series = gbt_detail::oof_ic_series_floored(
      fm, std::span<const atx::f64>{oof_pred_sum}, std::span<const atx::u32>{oof_pred_cnt},
      /*rel_floor=*/kOofDispersionFloor);
  m.trial_count = detail::protocol_trial_count(cfg.protocol.trials, n_fold_fits);
  if (trace != nullptr) {
    trace->oof_cnt = oof_pred_cnt;
    trace->oof_pred.assign(fm.n_rows(), std::numeric_limits<atx::f64>::quiet_NaN());
    for (atx::usize r = 0; r < fm.n_rows(); ++r) {
      if (oof_pred_cnt[r] > 0U) {
        trace->oof_pred[r] = oof_pred_sum[r] / static_cast<atx::f64>(oof_pred_cnt[r]);
      }
    }
  }

  // §0.6 horizon blend: normalize(max(oos_IC_h, 0)). All non-positive -> uniform.
  atx::f64 sum = 0.0;
  for (atx::usize h = 0; h < cfg.horizons.size(); ++h) {
    const atx::f64 w = (oos_ic_h[h] > 0.0) ? oos_ic_h[h] : 0.0;
    m.blend_w[h] = w;
    sum += w;
  }
  if (sum > 0.0) {
    for (atx::f64 &w : m.blend_w) {
      w /= sum;
    }
  } else {
    const atx::f64 u =
        (cfg.horizons.empty()) ? 0.0 : 1.0 / static_cast<atx::f64>(cfg.horizons.size());
    for (atx::f64 &w : m.blend_w) {
      w = u;
    }
  }
  if (cfg.rule == GbtRule::ColumnBinsV2 && fit_stats.split_gain_total > 0)
    for (auto &gain : fit_stats.gain_importance)
      gain /= fit_stats.split_gain_total;
  if (diagnostics)
    *diagnostics = std::move(fit_stats);
  m.cpcv_metadata = std::move(cpcv_metadata);
  return atx::core::Ok(std::move(m));
}

} // namespace atx::engine::learn
