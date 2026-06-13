#include "atx/engine/learn/latent.hpp"

#include <cmath>     // std::isfinite
#include <cstddef>   // std::ptrdiff_t (chosen-set slice index)
#include <span>      // std::span (the rows view apply_latent projects)
#include <utility>   // std::pair, std::move
#include <vector>    // std::vector

namespace atx::engine::learn {

[[nodiscard]] LatentBasis fit_latent(const FeatureMatrix &fm, atx::usize t,
                                     atx::u16 embargo, atx::u32 k) {
  LatentBasis basis;
  basis.fit_upto_date = t;
  basis.k = 0U;
  if (k == 0U || fm.n_features == 0U) {
    return basis; // PCA disabled or no features to factor
  }
  atx::usize cutoff = 0U;
  if (!detail::trailing_cutoff(t, embargo, cutoff)) {
    return basis; // empty trailing window
  }
  const std::vector<atx::usize> rows = detail::trailing_valid_rows(fm, cutoff);
  if (rows.size() < 2U) {
    return basis; // pca requires at least two samples
  }
  const lin::MatX X = detail::gather_matrix(fm, std::span<const atx::usize>{rows});
  auto res = lin::pca(X, static_cast<atx::i64>(k));
  if (!res.has_value()) {
    return basis; // k beyond feature count etc. -> disabled basis (k stays 0)
  }
  basis.model = std::move(res).value();
  basis.k = static_cast<atx::u32>(basis.model.components.cols());
  return basis;
}

[[nodiscard]] std::vector<std::pair<atx::u32, atx::u32>>
select_interactions(const FeatureMatrix &fm, atx::usize t, atx::u16 embargo, atx::u32 m) {
  std::vector<std::pair<atx::u32, atx::u32>> pairs;
  if (m == 0U || fm.n_features < 2U || fm.Y.empty()) {
    return pairs;
  }
  atx::usize cutoff = 0U;
  if (!detail::trailing_cutoff(t, embargo, cutoff)) {
    return pairs;
  }
  const std::vector<atx::usize> trailing = detail::trailing_valid_rows(fm, cutoff);
  // row_valid means FEATURES are finite, NOT the label: forward_return returns
  // quiet-NaN at the tail (date + horizon >= n_dates), which is reachable in the
  // trailing window whenever embargo < horizon. A single NaN label would make
  // spearman -> pearson return NaN for EVERY feature, collapsing the ranking to
  // blind index order. So drop rows with a non-finite Y[0] here, dropping them
  // from BOTH the label and every feature column consistently (aligned, finite
  // pairs). Deterministic: a single forward walk in row order, no map.
  std::vector<atx::usize> rows;
  rows.reserve(trailing.size());
  for (const atx::usize r : trailing) {
    if (std::isfinite(fm.Y[0][r])) {
      rows.push_back(r);
    }
  }
  if (rows.size() < 2U) {
    return pairs;
  }

  // Per-feature |Spearman IC| against the horizon-0 label over the finite-label
  // trailing rows.
  std::vector<atx::f64> label;
  label.reserve(rows.size());
  for (const atx::usize r : rows) {
    label.push_back(fm.Y[0][r]);
  }
  std::vector<atx::f64> abs_ic(fm.n_features, 0.0);
  for (atx::usize f = 0; f < fm.n_features; ++f) {
    const std::vector<atx::f64> col =
        detail::column(fm, std::span<const atx::usize>{rows}, f);
    const atx::f64 ic = detail::spearman(std::span<const atx::f64>{col},
                                         std::span<const atx::f64>{label});
    abs_ic[f] = (ic < 0.0) ? -ic : ic;
  }

  // Deterministic top-m: order feature indices by descending |IC|, breaking ties
  // by ascending index (first-index wins). Selection-sort the first m positions —
  // m is tiny (a handful), so this O(m * n_features) pass is clear and stable.
  std::vector<atx::u32> idx(fm.n_features);
  for (atx::usize f = 0; f < fm.n_features; ++f) {
    idx[f] = static_cast<atx::u32>(f);
  }
  const atx::usize want = (static_cast<atx::usize>(m) < fm.n_features)
                              ? static_cast<atx::usize>(m)
                              : fm.n_features;
  for (atx::usize s = 0; s < want; ++s) {
    atx::usize best = s;
    for (atx::usize c = s + 1U; c < idx.size(); ++c) {
      const atx::f64 ic_c = abs_ic[idx[c]];
      const atx::f64 ic_b = abs_ic[idx[best]];
      // Strictly-greater |IC| wins; equal |IC| keeps the smaller feature index
      // (idx[best] < idx[c] always here since we never reorder behind us, so a tie
      // leaves `best` unchanged — first-index tie-break).
      if (ic_c > ic_b) {
        best = c;
      }
    }
    const atx::u32 tmp = idx[s];
    idx[s] = idx[best];
    idx[best] = tmp;
  }

  // The chosen feature set, sorted ascending so the crossed pairs come out in a
  // fixed (a < b) order regardless of their IC ranking.
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

} // namespace atx::engine::learn
