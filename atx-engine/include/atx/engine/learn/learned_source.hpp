#pragma once

// atx::engine::learn — LearnedModel + LearnedSignalSource (S5-3, M6/M7).
//
// =====================================================================
//  What this header is
// =====================================================================
//  Two things, kept in one header because they share ONE feature-row layout:
//
//    * LearnedModel — the deployed, immutable parameters of a fitted learned
//      alpha: a tagged (ModelKind) bundle of per-horizon coefficient vectors, the
//      horizon blend weights, the trailing-fit standardization stats, the
//      LatentAugmentation (PCA basis + interaction pairs) the features were built
//      with, and the deflation trial_count. Designed to be REUSED by S5-4 (GBT)
//      and S5-6 (stacking): adding a ModelKind::Gbt arm there means extending the
//      enum + the predict dispatch's exhaustive switch (no default) — the Linear
//      arm here is the first of several.
//
//    * LearnedSignalSource — the ISignalSource adapter (M6): a learned model IS-A
//      alpha. It holds an immutable LearnedModel + the FeatureSpec, and its
//      evaluate(PanelView) rebuilds the point-in-time feature row per instrument
//      at the panel's current date, augments it, predicts the horizon-blended
//      score, and returns a SignalView cross-section of length n_instruments —
//      exactly like any other alpha the loop runs.
//
// =====================================================================
//  The ONE shared feature-row layout (train/eval parity — load-bearing)
// =====================================================================
//  build_augmented_row() is the SINGLE definition of the augmented feature
//  vector, called BOTH by the trainer (over a FeatureMatrix row) and the
//  evaluator (over a PanelView cross-section). Keeping it in one place is how
//  train-time and eval-time feature layouts CANNOT drift. The layout is:
//
//    [ 0 .. n_base )                standardized base features
//                                   z_f = (x_f - feat_mean[f]) / feat_sd[f]
//    [ n_base .. n_base + k )       latent PCA factor scores
//                                   ((x_base - pca.mean) · pca.components)
//    [ n_base + k .. end )          interaction products, one per aug.interactions
//                                   pair (a,b): z_a * z_b (the standardized cross)
//
//  The coefficient vectors are fit AGAINST this exact augmented layout, so the
//  dot product coeff_h · augmented_row is the per-horizon score. The base-feature
//  standardization stats (feat_mean/feat_sd) are themselves FITTED objects
//  (trailing-fit, applied forward — M2): the model carries them so eval applies
//  the same transform train used.
//
// =====================================================================
//  M7 — evaluate() of a FITTED model allocates ZERO heap per date
// =====================================================================
//  LearnedSignalSource pre-sizes ALL scratch in its constructor: the signal
//  buffer (n_instruments), the base-feature row, the augmented row, and the
//  latent-projection scratch. evaluate() writes into those buffers in place and
//  hands back a SignalView span over the owned signal buffer — no per-date vector
//  allocation. (Fitting, by contrast, is a cold path and may allocate freely.)
//
// Header-only; every member / free function is defined inline.

#include <cmath>   // std::isfinite
#include <limits>  // std::numeric_limits (NaN "no opinion" sentinel)
#include <span>    // std::span (augmented-row view, SignalView values)
#include <string>  // std::string (raw field name resolution)
#include <vector>  // std::vector (owned model params + source scratch)

#include <Eigen/Dense> // Eigen::Index (component / coeff element access)

#include "atx/core/error.hpp" // Result, Ok
#include "atx/core/macro.hpp" // ATX_CHECK
#include "atx/core/types.hpp" // f64, u8, u16, u32, usize

#include "atx/core/linalg/linalg.hpp" // VecX

#include "atx/engine/learn/feature_matrix.hpp" // FeatureSpec, FeatureMatrix
#include "atx/engine/learn/latent.hpp"         // LatentAugmentation, interaction_value
#include "atx/engine/loop/panel_types.hpp"     // PanelView, PanelField
#include "atx/engine/loop/signal_source.hpp"   // ISignalSource, SignalView

namespace atx::engine::learn {

// ===========================================================================
//  ModelKind — the tag distinguishing a LearnedModel's parameter shape.
//
//  S5-3 ships only Linear. S5-4 adds Gbt by extending this enum and the
//  predict dispatch's exhaustive switch (no default) — a compile error there
//  flags the missing arm, which is the intended guard.
// ===========================================================================
enum class ModelKind : atx::u8 { Linear, Gbt };

// ===========================================================================
//  GBT forest representation (S5-4) — the deployed parameters of a histogram
//  gradient-boosted-tree learned alpha. These structs live next to LearnedModel
//  because they ARE part of the deployed model (the fitting algorithm lives in
//  learn/gbt.hpp). Inference walks pre-stored nodes with NO allocation (M7).
//
//  GbtNode — one flat node. For a split: `feature` is the augmented-row column,
//  `threshold` the split point (go left iff value < threshold), `left`/`right`
//  the child node indices (into the owning tree's `nodes`). For a leaf:
//  `is_leaf` is true, `leaf_value` is the additive prediction, and left/right are
//  -1. Storing children as indices (not pointers) keeps the tree relocatable and
//  the bytes hashable for the M1 determinism gate.
// ===========================================================================
struct GbtNode {
  atx::u32 feature{0};  // split feature (augmented-row column); unused at a leaf
  atx::f64 threshold{0.0}; // go left iff augmented_row[feature] < threshold
  atx::f64 leaf_value{0.0}; // additive prediction contribution at a leaf
  atx::i32 left{-1};        // left child node index (-1 at a leaf)
  atx::i32 right{-1};       // right child node index (-1 at a leaf)
  bool is_leaf{true};       // true -> leaf_value is the contribution; no split
};

// One depth-limited regression tree: a flat node array, root at index 0.
struct GbtTree {
  std::vector<GbtNode> nodes;
};

// One boosted forest for a single horizon: an initial constant `base` (the train
// mean) plus a sequence of trees whose (learning-rate-scaled) leaf values sum
// onto it. forest prediction = base + Σ_t tree_t.predict(row).
struct GbtForest {
  atx::f64 base{0.0};
  std::vector<GbtTree> trees;
};

// Evaluate one tree on an augmented row: walk from the root, branching left iff
// row[feature] < threshold, until a leaf, and return its leaf_value. Allocation-
// free (M7): a bounded index walk over pre-stored nodes. An empty tree predicts 0.
[[nodiscard]] inline atx::f64 gbt_tree_predict(const GbtTree &tree,
                                               std::span<const atx::f64> augmented_row) {
  if (tree.nodes.empty()) {
    return 0.0;
  }
  atx::i32 idx = 0;
  // SAFETY: a well-formed tree's child indices are all valid node positions and
  // every path terminates at a leaf; the loop is bounded by the node count so a
  // malformed (cyclic) tree cannot spin forever.
  for (atx::usize steps = 0; steps <= tree.nodes.size(); ++steps) {
    ATX_CHECK(idx >= 0 && static_cast<atx::usize>(idx) < tree.nodes.size());
    const GbtNode &node = tree.nodes[static_cast<atx::usize>(idx)];
    if (node.is_leaf) {
      return node.leaf_value;
    }
    ATX_CHECK(static_cast<atx::usize>(node.feature) < augmented_row.size());
    idx = (augmented_row[node.feature] < node.threshold) ? node.left : node.right;
  }
  ATX_CHECK(false); // a well-formed tree always reaches a leaf within node-count steps
  return 0.0;
}

// Evaluate one forest on an augmented row: base + Σ trees. Allocation-free (M7).
[[nodiscard]] inline atx::f64 gbt_forest_predict(const GbtForest &forest,
                                                 std::span<const atx::f64> augmented_row) {
  atx::f64 acc = forest.base;
  for (const GbtTree &tree : forest.trees) {
    acc += gbt_tree_predict(tree, augmented_row);
  }
  return acc;
}

// ===========================================================================
//  LearnedModel — the immutable deployed parameters of a fitted learned alpha.
//
//  kind        : which arm's parameters are populated (Linear for S5-3).
//  coeffs      : one coefficient vector per horizon, each fit against the shared
//                augmented row layout (Linear).
//  blend_w     : §0.6 horizon blend weights (>= 0, sum 1) — the deployed mixture.
//  horizons    : the H this model was fit for (parallel to coeffs / blend_w).
//  feat_mean   : per-BASE-feature trailing-fit mean (standardization, applied fwd).
//  feat_sd     : per-BASE-feature trailing-fit population std (0 -> column zeroed).
//  trial_count : §0.3 deflation N (distinct fits performed).
//  aug         : the latent/interaction augmentation the feature rows were built
//                with (PCA basis + the selected interaction pairs).
//  n_base_features: the count of BASE features (raw_fields + pool_alphas), for
//                rebuilding the augmented row at eval time.
//  oos_score_series: the per-date OUT-OF-FOLD skill series (e.g. the horizon-0
//                cross-sectional IC of the CPCV out-of-fold predictions, in
//                ascending date order). It is assembled DURING fitting from
//                fold-local (train-only-standardized, train-only-fit) test-row
//                predictions, so it carries NO look-ahead and is the genuine
//                OOS series the deflation gate scores (M3). Defaults to {} so a
//                model arm that does not fill it (a hand-built shell, or a future
//                S5-4/S5-6 model before it fills its own) still constructs.
// ===========================================================================
struct LearnedModel {
  ModelKind kind{ModelKind::Linear};
  std::vector<atx::core::linalg::VecX> coeffs;
  std::vector<atx::f64> blend_w;
  std::vector<atx::u16> horizons;
  std::vector<atx::f64> feat_mean;
  std::vector<atx::f64> feat_sd;
  atx::usize trial_count{0};
  LatentAugmentation aug;
  atx::u32 n_base_features{0};
  std::vector<atx::f64> oos_score_series{};
  // S5-4 GBT deployed forests, one per horizon (parallel to horizons / blend_w).
  // Empty for a Linear model, so existing Linear construction is unchanged; the
  // Gbt predict arm reads these instead of coeffs.
  std::vector<GbtForest> forests{};

  // The augmented feature dimension this model's coeffs operate on:
  // base + latent-k + interaction-pairs. The single source of that arithmetic so
  // the trainer and evaluator size buffers identically.
  [[nodiscard]] atx::usize augmented_dim() const noexcept {
    const atx::usize k = aug.pca.has_value() ? static_cast<atx::usize>(aug.pca->k) : 0U;
    return static_cast<atx::usize>(n_base_features) + k + aug.interactions.size();
  }
};

// ===========================================================================
//  build_augmented_row — THE one feature-row layout (train + eval call this).
//
//  Writes the augmented feature vector for ONE observation into `out` (sized to
//  model.augmented_dim()). `base` is the raw base-feature vector in FeatureMatrix
//  column order (length n_base_features). `latent_scratch` is reused projection
//  scratch sized to >= k (caller-owned; this function does not allocate). All
//  three sections (standardized base, latent scores, interaction products) are
//  laid out exactly as the header documents.
//
//  Returns true iff every written value is finite (an all-finite augmented row);
//  a non-finite base feature (NaN cell) propagates to false so the caller can
//  emit "no opinion".
// ===========================================================================
[[nodiscard]] inline bool build_augmented_row(const LearnedModel &m, std::span<const atx::f64> base,
                                              std::span<atx::f64> latent_scratch,
                                              std::span<atx::f64> out) {
  const atx::usize n_base = static_cast<atx::usize>(m.n_base_features);
  const atx::usize k = m.aug.pca.has_value() ? static_cast<atx::usize>(m.aug.pca->k) : 0U;
  ATX_CHECK(base.size() == n_base);
  ATX_CHECK(out.size() == m.augmented_dim());
  ATX_CHECK(m.feat_mean.size() == n_base && m.feat_sd.size() == n_base);

  bool all_finite = true;

  // 1) standardized base features z_f = (x_f - mean_f) / sd_f (sd 0 -> 0).
  for (atx::usize f = 0; f < n_base; ++f) {
    const atx::f64 sd = m.feat_sd[f];
    const atx::f64 z = (sd == 0.0) ? 0.0 : (base[f] - m.feat_mean[f]) / sd;
    out[f] = z;
    all_finite = all_finite && std::isfinite(z);
  }

  // 2) latent PCA factor scores: ((x_base - pca.mean) . pca.components[:,c]).
  //    Uses the RAW (un-standardized) base, matching apply_latent's fit input.
  if (k > 0U) {
    ATX_CHECK(latent_scratch.size() >= k);
    const auto &model = m.aug.pca->model;
    for (atx::usize c = 0; c < k; ++c) {
      atx::f64 score = 0.0;
      for (atx::usize f = 0; f < n_base; ++f) {
        const atx::f64 centered = base[f] - model.mean(static_cast<Eigen::Index>(f));
        score += centered * model.components(static_cast<Eigen::Index>(f),
                                             static_cast<Eigen::Index>(c));
      }
      latent_scratch[c] = score;
      out[n_base + c] = score;
      all_finite = all_finite && std::isfinite(score);
    }
  }

  // 3) interaction products: z_a * z_b for each selected pair (a, b), reusing the
  //    standardized base values already in out[0..n_base).
  const atx::usize ibase = n_base + k;
  for (atx::usize p = 0; p < m.aug.interactions.size(); ++p) {
    const auto [a, b] = m.aug.interactions[p];
    const atx::f64 za = out[static_cast<atx::usize>(a)];
    const atx::f64 zb = out[static_cast<atx::usize>(b)];
    const atx::f64 v = za * zb;
    out[ibase + p] = v;
    all_finite = all_finite && std::isfinite(v);
  }
  return all_finite;
}

// ===========================================================================
//  predict_blended — the horizon-blended scalar score for one augmented row.
//
//  Sum over horizons of blend_w[h] * (coeff_h . augmented_row). The exhaustive
//  switch on kind has NO default: S5-4 adds the Gbt arm and the compiler flags
//  the missing case. `augmented_row` is already built (build_augmented_row).
// ===========================================================================
[[nodiscard]] inline atx::f64 predict_blended(const LearnedModel &m,
                                              std::span<const atx::f64> augmented_row) {
  switch (m.kind) {
  case ModelKind::Linear: {
    atx::f64 acc = 0.0;
    for (atx::usize h = 0; h < m.coeffs.size(); ++h) {
      const atx::core::linalg::VecX &c = m.coeffs[h];
      ATX_CHECK(static_cast<atx::usize>(c.size()) == augmented_row.size());
      atx::f64 dot = 0.0;
      for (atx::usize j = 0; j < augmented_row.size(); ++j) {
        dot += c(static_cast<Eigen::Index>(j)) * augmented_row[j];
      }
      acc += m.blend_w[h] * dot;
    }
    return acc;
  }
  case ModelKind::Gbt: {
    // Blend each horizon's forest prediction by blend_w (§0.6), exactly as the
    // Linear arm blends its per-horizon dot products. Forest eval is allocation-
    // free (gbt_forest_predict walks pre-stored nodes).
    atx::f64 acc = 0.0;
    for (atx::usize h = 0; h < m.forests.size(); ++h) {
      acc += m.blend_w[h] * gbt_forest_predict(m.forests[h], augmented_row);
    }
    return acc;
  }
  }
  return 0.0; // unreachable: every ModelKind is handled above (no default).
}

// ===========================================================================
//  LearnedSignalSource — the ISignalSource adapter (M6/M7).
//
//  Wraps an immutable LearnedModel + the FeatureSpec it was fit with. evaluate()
//  rebuilds the PIT base feature row at the panel's CURRENT date (row 0, newest)
//  from the raw OHLCV fields named in the spec, augments it through the shared
//  build_augmented_row, predicts the blended score per instrument, and returns a
//  SignalView cross-section of length n_instruments. All scratch is pre-sized in
//  the ctor (M7: zero alloc per evaluate).
//
//  NOTE: this S5-3 adapter consumes ONLY raw Panel fields as base features (the
//  spec's raw_fields, resolved to PanelField at construction). Pool-alpha base
//  features are not reconstructable from a bare PanelView (they live in the
//  AlphaStore, not the loop panel); a spec carrying pool_alphas is rejected at
//  construction so train/eval feature parity is never silently violated. The
//  trainer (linear_alpha.hpp) supports pool features over a FeatureMatrix; the
//  live PanelView adapter is the raw-field subset, by design.
// ===========================================================================
class LearnedSignalSource final : public ISignalSource {
public:
  // Build from a fitted model + the spec it was fit with. PRECONDITION (debug):
  // the spec carries no pool_alphas (see class note) and the model's base count
  // matches the spec's raw_fields. universe_size is the fixed signal length.
  LearnedSignalSource(LearnedModel model, FeatureSpec spec, atx::usize universe_size);

  // Rebuild the PIT cross-section at the panel's current (newest) date and emit
  // the horizon-blended score per instrument. Reads ONLY row 0 (the current date)
  // and earlier rows would be needed only by a lookback feature; the raw-field
  // base reads the current cross-section (M2). Out-of-universe / non-finite cells
  // emit the NaN "no opinion" sentinel. Returns a SignalView over signal_.
  [[nodiscard]] atx::core::Result<SignalView> evaluate(PanelView panel) override {
    const atx::usize inst = panel.instruments();
    const atx::usize n = (inst < universe_size_) ? inst : universe_size_;
    // signal_ is pre-sized to universe_size_; reset to "no opinion" in place.
    for (atx::usize i = 0; i < universe_size_; ++i) {
      signal_[i] = kNoOpinion;
    }
    if (panel.rows() == 0U) {
      return atx::core::Ok(SignalView{std::span<const atx::f64>{signal_}});
    }
    for (atx::usize i = 0; i < n; ++i) {
      if (!panel.present(0U, i)) {
        continue; // out-of-universe at the current date -> no opinion (NaN)
      }
      fill_base_row(panel, i);
      const bool finite = build_augmented_row(model_, std::span<const atx::f64>{base_},
                                              std::span<atx::f64>{latent_},
                                              std::span<atx::f64>{augmented_});
      if (finite) {
        signal_[i] = predict_blended(model_, std::span<const atx::f64>{augmented_});
      }
    }
    return atx::core::Ok(SignalView{std::span<const atx::f64>{signal_}});
  }

  // The trailing history the loop must provision: the spec's declared lookback.
  [[nodiscard]] atx::usize max_lookback() const noexcept override {
    return static_cast<atx::usize>(spec_.max_lookback);
  }

private:
  static constexpr atx::f64 kNoOpinion = std::numeric_limits<atx::f64>::quiet_NaN();

  // Map a raw field name to its PanelField storage slot. ABORTS on an unknown
  // name (a construction-time fixture error, not a per-evaluate runtime path).
  [[nodiscard]] static PanelField panel_field_of(const std::string &name) {
    if (name == "open") {
      return PanelField::Open;
    }
    if (name == "high") {
      return PanelField::High;
    }
    if (name == "low") {
      return PanelField::Low;
    }
    if (name == "close") {
      return PanelField::Close;
    }
    if (name == "volume") {
      return PanelField::Volume;
    }
    ATX_CHECK(false); // unknown raw field name for the live PanelView adapter
    return PanelField::Close;
  }

  // Read one raw field cell at the current date (row 0, newest) for instrument i.
  [[nodiscard]] static atx::f64 read_field(PanelView panel, PanelField f, atx::usize i) {
    switch (f) {
    case PanelField::Open:
      return panel.open(0U, i);
    case PanelField::High:
      return panel.high(0U, i);
    case PanelField::Low:
      return panel.low(0U, i);
    case PanelField::Close:
      return panel.close(0U, i);
    case PanelField::Volume:
      return panel.volume(0U, i);
    }
    return kNoOpinion; // unreachable: every PanelField handled (no default).
  }

  // Fill base_ with the raw-field cross-section for instrument i at the current
  // date, in spec order (matching the FeatureMatrix base column order).
  void fill_base_row(PanelView panel, atx::usize i) {
    for (atx::usize f = 0; f < field_slots_.size(); ++f) {
      base_[f] = read_field(panel, field_slots_[f], i);
    }
  }

  LearnedModel model_;                  // immutable fitted parameters
  FeatureSpec spec_;                    // the spec the model was fit with
  atx::usize universe_size_;            // fixed signal length per evaluate
  std::vector<PanelField> field_slots_; // resolved raw-field slots (cold-path)
  std::vector<atx::f64> signal_;        // [universe_size_] emitted cross-section (borrowed)
  std::vector<atx::f64> base_;          // [n_base_features] base-row scratch (reused)
  std::vector<atx::f64> augmented_;     // [augmented_dim] augmented-row scratch (reused)
  std::vector<atx::f64> latent_;        // [k] latent-projection scratch (reused)
};

} // namespace atx::engine::learn
