#include "atx/engine/learn/latent.hpp"

#include <algorithm> // std::sort, std::unique (interaction_top_m)
#include <cmath>     // std::isfinite
#include <cstddef>   // std::ptrdiff_t (chosen-set slice index)
#include <span>      // std::span (the rows view apply_latent projects)
#include <utility>   // std::pair, std::move
#include <vector>    // std::vector

namespace atx::engine::learn {

namespace detail {

LatentBasis fit_latent_on_rows(const FeatureMatrix &fm, std::span<const atx::usize> rows,
                               atx::u32 k, atx::usize fit_upto_date) {
  LatentBasis basis;
  basis.fit_upto_date = fit_upto_date;
  basis.k = 0U;
  if (k == 0U || fm.n_features == 0U || rows.size() < 2U) {
    return basis; // PCA disabled, no features to factor, or pca needs >= 2 samples
  }
  const lin::MatX X = gather_matrix(fm, rows);
  auto res = lin::pca(X, static_cast<atx::i64>(k));
  if (!res.has_value()) {
    return basis; // k beyond feature count etc. -> disabled basis (k stays 0)
  }
  basis.model = std::move(res).value();
  basis.k = static_cast<atx::u32>(basis.model.components.cols());
  return basis;
}

std::vector<std::pair<atx::u32, atx::u32>>
select_interactions_on_rows(const FeatureMatrix &fm, std::span<const atx::usize> rows_in,
                            atx::usize label_idx, atx::u32 m) {
  std::vector<std::pair<atx::u32, atx::u32>> pairs;
  if (m == 0U || fm.n_features < 2U || label_idx >= fm.Y.size()) {
    return pairs;
  }
  // row_valid means FEATURES are finite, NOT the label: forward_return returns quiet-NaN
  // at the tail. A single NaN label would make spearman -> pearson return NaN for EVERY
  // feature, collapsing the ranking to blind index order. So drop rows with a non-finite
  // label, from BOTH the label and every feature column consistently (aligned pairs).
  // Deterministic: a single forward walk in the caller's row order, no map.
  const std::vector<atx::f64> &y = fm.Y[label_idx];
  std::vector<atx::usize> rows;
  rows.reserve(rows_in.size());
  for (const atx::usize r : rows_in) {
    if (std::isfinite(y[r])) {
      rows.push_back(r);
    }
  }
  if (rows.size() < 2U) {
    return pairs;
  }

  // Per-feature |Spearman IC| against the label over the finite-label rows.
  std::vector<atx::f64> label;
  label.reserve(rows.size());
  for (const atx::usize r : rows) {
    label.push_back(y[r]);
  }
  std::vector<atx::f64> abs_ic(fm.n_features, 0.0);
  for (atx::usize f = 0; f < fm.n_features; ++f) {
    const std::vector<atx::f64> col = column(fm, std::span<const atx::usize>{rows}, f);
    const atx::f64 ic =
        spearman(std::span<const atx::f64>{col}, std::span<const atx::f64>{label});
    abs_ic[f] = (ic < 0.0) ? -ic : ic;
  }

  // Deterministic top-m: order feature indices by descending |IC|, breaking ties by
  // ascending index (first-index wins). Selection-sort the first m positions — m is
  // tiny (a handful), so this O(m * n_features) pass is clear and stable.
  std::vector<atx::u32> idx(fm.n_features);
  for (atx::usize f = 0; f < fm.n_features; ++f) {
    idx[f] = static_cast<atx::u32>(f);
  }
  const atx::usize want =
      (static_cast<atx::usize>(m) < fm.n_features) ? static_cast<atx::usize>(m) : fm.n_features;
  for (atx::usize s = 0; s < want; ++s) {
    atx::usize best = s;
    for (atx::usize c = s + 1U; c < idx.size(); ++c) {
      // Strictly-greater |IC| wins; equal |IC| keeps the smaller feature index
      // (idx[best] < idx[c] always here since we never reorder behind us, so a tie
      // leaves `best` unchanged — first-index tie-break).
      if (abs_ic[idx[c]] > abs_ic[idx[best]]) {
        best = c;
      }
    }
    const atx::u32 tmp = idx[s];
    idx[s] = idx[best];
    idx[best] = tmp;
  }

  // The chosen feature set, sorted ascending so the crossed pairs come out in a fixed
  // (a < b) order regardless of their IC ranking.
  std::vector<atx::u32> chosen(idx.begin(), idx.begin() + static_cast<std::ptrdiff_t>(want));
  for (atx::usize i = 0; i + 1U < chosen.size(); ++i) {
    for (atx::usize j = i + 1U; j < chosen.size(); ++j) {
      if (chosen[j] < chosen[i]) {
        const atx::u32 tmp = chosen[i];
        chosen[i] = chosen[j];
        chosen[j] = tmp;
      }
    }
  }
  for (atx::usize i = 0; i + 1U < chosen.size(); ++i) {
    for (atx::usize j = i + 1U; j < chosen.size(); ++j) {
      pairs.emplace_back(chosen[i], chosen[j]); // a < b by construction
    }
  }
  return pairs;
}

atx::u32 interaction_top_m(const std::vector<std::pair<atx::u32, atx::u32>> &pairs) {
  std::vector<atx::u32> feats;
  feats.reserve(2U * pairs.size());
  for (const auto &[a, b] : pairs) {
    feats.push_back(a);
    feats.push_back(b);
  }
  std::sort(feats.begin(), feats.end());
  feats.erase(std::unique(feats.begin(), feats.end()), feats.end());
  return static_cast<atx::u32>(feats.size());
}

void append_augmentation(const LatentAugmentation &aug, std::vector<atx::f64> &out) {
  const bool has_pca = aug.pca.has_value();
  out.push_back(has_pca ? static_cast<atx::f64>(aug.pca->k) : -1.0);
  if (has_pca) {
    const lin::VecX &mean = aug.pca->model.mean;
    for (Eigen::Index i = 0; i < mean.size(); ++i) {
      out.push_back(mean(i));
    }
    const lin::MatX &comp = aug.pca->model.components;
    for (Eigen::Index c = 0; c < comp.cols(); ++c) {
      for (Eigen::Index r = 0; r < comp.rows(); ++r) {
        out.push_back(comp(r, c));
      }
    }
  }
  out.push_back(static_cast<atx::f64>(aug.interactions.size()));
  for (const auto &[a, b] : aug.interactions) {
    out.push_back(static_cast<atx::f64>(a));
    out.push_back(static_cast<atx::f64>(b));
  }
}

} // namespace detail

LatentBasis fit_latent(const FeatureMatrix &fm, atx::usize t, atx::u16 embargo, atx::u32 k) {
  atx::usize cutoff = 0U;
  if (k == 0U || fm.n_features == 0U || !detail::trailing_cutoff(t, embargo, cutoff)) {
    LatentBasis basis;
    basis.fit_upto_date = t;
    return basis; // PCA disabled, no features, or an empty trailing window
  }
  const std::vector<atx::usize> rows = detail::trailing_valid_rows(fm, cutoff);
  return detail::fit_latent_on_rows(fm, std::span<const atx::usize>{rows}, k, t);
}

std::vector<std::pair<atx::u32, atx::u32>>
select_interactions(const FeatureMatrix &fm, atx::usize t, atx::u16 embargo, atx::u32 m,
                    LabelMaturityRule rule) {
  if (m == 0U || fm.n_features < 2U || fm.Y.empty()) {
    return {};
  }
  atx::usize cutoff = 0U;
  if (!detail::trailing_cutoff(t, embargo, cutoff)) {
    return {};
  }
  std::vector<atx::usize> rows = detail::trailing_valid_rows(fm, cutoff);
  switch (rule) {
  case LabelMaturityRule::FiniteLabelV1:
    break; // legacy: the finite-label filter below is the only label check
  case LabelMaturityRule::MaturedV2: {
    if (!fm.has_label_horizons()) {
      return {}; // unannotated labels: maturity cannot be proven, so read none
    }
    const atx::u16 h0 = fm.label_horizons[0];
    std::vector<atx::usize> matured;
    matured.reserve(rows.size());
    for (const atx::usize r : rows) {
      if (label_matured(fm.row_date[r], h0, t, embargo)) {
        matured.push_back(r);
      }
    }
    rows = std::move(matured);
    break;
  }
  }
  return detail::select_interactions_on_rows(fm, std::span<const atx::usize>{rows}, 0U, m);
}

LatentAugmentation fit_fold_augmentation(const FeatureMatrix &fm,
                                         const LatentAugmentation &deployed,
                                         std::span<const atx::usize> train_rows,
                                         atx::usize label_idx) {
  LatentAugmentation out;
  out.interactions_fixed = deployed.interactions_fixed;
  atx::usize last_date = 0U;
  for (const atx::usize r : train_rows) {
    last_date = (fm.row_date[r] > last_date) ? fm.row_date[r] : last_date;
  }
  if (deployed.pca.has_value() && deployed.pca->k > 0U) {
    out.pca = detail::fit_latent_on_rows(fm, train_rows, deployed.pca->k, last_date);
  }
  if (deployed.interactions_fixed) {
    out.interactions = deployed.interactions; // explicit label-free pairs: no refit
  } else if (!deployed.interactions.empty()) {
    const atx::u32 m = detail::interaction_top_m(deployed.interactions);
    out.interactions = detail::select_interactions_on_rows(fm, train_rows, label_idx, m);
  }
  return out;
}

} // namespace atx::engine::learn
