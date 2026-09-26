#include "atx/engine/risk/exposures.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/loop/panel_types.hpp"

namespace atx::engine::risk {
namespace detail {

[[nodiscard]] atx::f64 step_return(const PanelView &panel, atx::usize r,
                                          atx::usize i) noexcept {
  const atx::f64 c_new = panel.close(r, i);
  const atx::f64 c_old = panel.close(r + 1U, i);
  if (std::isnan(c_new) || std::isnan(c_old) || c_old <= 0.0) {
    return std::numeric_limits<atx::f64>::quiet_NaN();
  }
  return c_new / c_old - 1.0;
}

[[nodiscard]] atx::usize valid_returns(const PanelView &panel, atx::usize row, atx::usize i,
                                              atx::usize want) noexcept {
  atx::usize n = 0U;
  for (atx::usize k = 0U; k < want; ++k) {
    const atx::usize r = row + k;
    if (r + 1U >= panel.rows() || std::isnan(step_return(panel, r, i))) {
      break;
    }
    ++n;
  }
  return n;
}

[[nodiscard]] atx::f64 momentum(const PanelView &panel, atx::usize row,
                                       atx::usize i) noexcept {
  if (valid_returns(panel, row, i, kMomLong) < kMomLong) {
    return std::numeric_limits<atx::f64>::quiet_NaN();
  }
  atx::f64 long_sum = 0.0;
  atx::f64 short_sum = 0.0;
  for (atx::usize k = 0U; k < kMomLong; ++k) { // ascending row -> order-fixed sum
    const atx::f64 ret = step_return(panel, row + k, i);
    long_sum += ret;
    if (k < kMomShort) {
      short_sum += ret;
    }
  }
  return long_sum - short_sum;
}

[[nodiscard]] atx::f64 volatility(const PanelView &panel, atx::usize row,
                                         atx::usize i) noexcept {
  if (valid_returns(panel, row, i, kVolWindow) < kVolWindow) {
    return std::numeric_limits<atx::f64>::quiet_NaN();
  }
  atx::f64 sum = 0.0;
  for (atx::usize k = 0U; k < kVolWindow; ++k) {
    sum += step_return(panel, row + k, i);
  }
  const atx::f64 mean = sum / static_cast<atx::f64>(kVolWindow);
  atx::f64 ss = 0.0;
  for (atx::usize k = 0U; k < kVolWindow; ++k) {
    const atx::f64 d = step_return(panel, row + k, i) - mean;
    ss += d * d;
  }
  return std::sqrt(ss / static_cast<atx::f64>(kVolWindow)); // population std
}

[[nodiscard]] atx::f64 liquidity(const PanelView &panel, atx::usize row,
                                        atx::usize i) noexcept {
  if (row + kAdvWindow > panel.rows()) {
    return std::numeric_limits<atx::f64>::quiet_NaN();
  }
  atx::f64 sum = 0.0;
  for (atx::usize k = 0U; k < kAdvWindow; ++k) {
    const atx::f64 c = panel.close(row + k, i);
    const atx::f64 v = panel.volume(row + k, i);
    if (std::isnan(c) || std::isnan(v)) {
      return std::numeric_limits<atx::f64>::quiet_NaN();
    }
    sum += c * v;
  }
  const atx::f64 adv = sum / static_cast<atx::f64>(kAdvWindow);
  return (adv <= 0.0) ? std::numeric_limits<atx::f64>::quiet_NaN() : std::log(adv);
}

[[nodiscard]] atx::f64 market_return(const PanelView &panel, atx::usize r) noexcept {
  atx::f64 sum = 0.0;
  atx::usize n = 0U;
  for (atx::usize i = 0U; i < panel.instruments(); ++i) { // ascending instrument
    const atx::f64 ret = step_return(panel, r, i);
    if (!std::isnan(ret)) {
      sum += ret;
      ++n;
    }
  }
  return (n == 0U) ? std::numeric_limits<atx::f64>::quiet_NaN() : sum / static_cast<atx::f64>(n);
}

[[nodiscard]] atx::f64 beta(const PanelView &panel, atx::usize row, atx::usize i) noexcept {
  if (row + kBetaWindow + 1U > panel.rows()) {
    return std::numeric_limits<atx::f64>::quiet_NaN();
  }
  atx::f64 si = 0.0;
  atx::f64 sm = 0.0;
  atx::usize n = 0U;
  for (atx::usize k = 0U; k < kBetaWindow; ++k) {
    const atx::f64 ri = step_return(panel, row + k, i);
    const atx::f64 rm = market_return(panel, row + k);
    if (std::isnan(ri) || std::isnan(rm)) {
      return std::numeric_limits<atx::f64>::quiet_NaN();
    }
    si += ri;
    sm += rm;
    ++n;
  }
  const atx::f64 nf = static_cast<atx::f64>(n);
  const atx::f64 mi = si / nf;
  const atx::f64 mm = sm / nf;
  atx::f64 cov = 0.0;
  atx::f64 var = 0.0;
  for (atx::usize k = 0U; k < kBetaWindow; ++k) {
    const atx::f64 di = step_return(panel, row + k, i) - mi;
    const atx::f64 dm = market_return(panel, row + k) - mm;
    cov += di * dm;
    var += dm * dm;
  }
  return (var <= 0.0) ? std::numeric_limits<atx::f64>::quiet_NaN() : cov / var;
}

[[nodiscard]] std::vector<atx::f64> market_returns(const PanelView &panel, atx::usize row,
                                                          atx::usize n) {
  std::vector<atx::f64> mkt(n, std::numeric_limits<atx::f64>::quiet_NaN());
  for (atx::usize k = 0U; k < n; ++k) {
    if (row + k + 1U < panel.rows()) {
      mkt[k] = market_return(panel, row + k);
    }
  }
  return mkt;
}

[[nodiscard]] atx::f64 beta_cached(const PanelView &panel, atx::usize row, atx::usize i,
                                          std::span<const atx::f64> mkt) noexcept {
  if (row + kBetaWindow + 1U > panel.rows() || mkt.size() < kBetaWindow) {
    return std::numeric_limits<atx::f64>::quiet_NaN();
  }
  atx::f64 si = 0.0;
  atx::f64 sm = 0.0;
  atx::usize n = 0U;
  for (atx::usize k = 0U; k < kBetaWindow; ++k) {
    const atx::f64 ri = step_return(panel, row + k, i);
    const atx::f64 rm = mkt[k];
    if (std::isnan(ri) || std::isnan(rm)) {
      return std::numeric_limits<atx::f64>::quiet_NaN();
    }
    si += ri;
    sm += rm;
    ++n;
  }
  const atx::f64 nf = static_cast<atx::f64>(n);
  const atx::f64 mi = si / nf;
  const atx::f64 mm = sm / nf;
  atx::f64 cov = 0.0;
  atx::f64 var = 0.0;
  for (atx::usize k = 0U; k < kBetaWindow; ++k) {
    const atx::f64 di = step_return(panel, row + k, i) - mi;
    const atx::f64 dm = mkt[k] - mm;
    cov += di * dm;
    var += dm * dm;
  }
  return (var <= 0.0) ? std::numeric_limits<atx::f64>::quiet_NaN() : cov / var;
}

[[nodiscard]] atx::f64 raw_style(StyleFactor f, const PanelView &panel, atx::usize row,
                                        atx::usize i,
                                        std::span<const atx::f64> market_cap) noexcept {
  switch (f) {
  case StyleFactor::Size: {
    const atx::f64 cap = market_cap[i];
    return (std::isnan(cap) || cap <= 0.0) ? std::numeric_limits<atx::f64>::quiet_NaN()
                                           : std::log(cap);
  }
  case StyleFactor::Momentum:
    return momentum(panel, row, i);
  case StyleFactor::Volatility:
    return volatility(panel, row, i);
  case StyleFactor::Beta:
    return beta(panel, row, i);
  case StyleFactor::Liquidity:
    return liquidity(panel, row, i);
  case StyleFactor::BookToPrice:
  case StyleFactor::EarningsYield:
  case StyleFactor::Growth:
  case StyleFactor::Profitability:
  case StyleFactor::Leverage:
  case StyleFactor::DivYield:
  case StyleFactor::ResidVol:
  case StyleFactor::ShortInterest:
  case StyleFactor::STReversal:
  case StyleFactor::Market:
    // L7 fundamental/intercept styles are built by fundamental_factors.hpp; the
    // legacy emit set (bits < kStyleFactorCount) never reaches here.
    return std::numeric_limits<atx::f64>::quiet_NaN();
  }
  return std::numeric_limits<atx::f64>::quiet_NaN(); // unreachable (switch exhaustive)
}

[[nodiscard]] std::vector<StyleFactor> emitted_styles(const FactorModelConfig &cfg,
                                                             bool have_cap) {
  std::vector<StyleFactor> out;
  for (atx::usize b = 0U; b < kStyleFactorCount; ++b) {
    if ((cfg.style_mask & static_cast<atx::u8>(1U << b)) == 0U) {
      continue;
    }
    const auto f = static_cast<StyleFactor>(b);
    if (f == StyleFactor::Size && !have_cap) {
      continue; // cap absent -> Size never fabricated
    }
    out.push_back(f);
  }
  return out;
}

void zscore_column(atx::core::linalg::MatX &x, Eigen::Index col) noexcept {
  const Eigen::Index m = x.rows();
  if (m <= 1) {
    for (Eigen::Index r = 0; r < m; ++r) {
      x(r, col) = 0.0; // single instrument -> degenerate
    }
    return;
  }
  atx::f64 sum = 0.0;
  for (Eigen::Index r = 0; r < m; ++r) {
    sum += x(r, col);
  }
  const atx::f64 mean = sum / static_cast<atx::f64>(m);
  atx::f64 ss = 0.0;
  for (Eigen::Index r = 0; r < m; ++r) {
    const atx::f64 d = x(r, col) - mean;
    ss += d * d;
  }
  const atx::f64 var = ss / static_cast<atx::f64>(m); // population variance
  if (var <= 0.0) {
    for (Eigen::Index r = 0; r < m; ++r) {
      x(r, col) = 0.0; // zero-variance column -> degenerate
    }
    return;
  }
  const atx::f64 inv_std = 1.0 / std::sqrt(var);
  for (Eigen::Index r = 0; r < m; ++r) {
    x(r, col) = (x(r, col) - mean) * inv_std;
  }
}

void zscore_column_v2(atx::core::linalg::MatX &x, Eigen::Index col,
                             std::span<const atx::f64> weights) noexcept {
  const Eigen::Index m = x.rows();
  if (m <= 1) {
    for (Eigen::Index r = 0; r < m; ++r) {
      x(r, col) = 0.0;
    }
    return;
  }
  atx::f64 wsum = 0.0;
  if (weights.size() == static_cast<atx::usize>(m)) {
    for (const atx::f64 w : weights) {
      wsum += (w > 0.0) ? w : 0.0; // NaN compares false ⇒ 0
    }
  }
  const bool cap_weighted = wsum > 0.0;
  const auto weight_of = [&](Eigen::Index r) noexcept -> atx::f64 {
    if (!cap_weighted) {
      return 1.0;
    }
    const atx::f64 w = weights[static_cast<atx::usize>(r)];
    return (w > 0.0) ? w : 0.0;
  };
  const atx::f64 wtot = cap_weighted ? wsum : static_cast<atx::f64>(m);
  // μ_eq, μ_w (cap-weighted) and σ (equal-weight population std about μ_eq).
  atx::f64 mu_eq = 0.0;
  atx::f64 mu_w = 0.0;
  atx::f64 sigma = 0.0;
  const auto moments = [&]() noexcept {
    atx::f64 swx = 0.0;
    atx::f64 sx = 0.0;
    for (Eigen::Index r = 0; r < m; ++r) {
      swx += weight_of(r) * x(r, col);
      sx += x(r, col);
    }
    mu_w = swx / wtot;
    mu_eq = sx / static_cast<atx::f64>(m);
    atx::f64 ss = 0.0;
    for (Eigen::Index r = 0; r < m; ++r) {
      const atx::f64 d = x(r, col) - mu_eq;
      ss += d * d;
    }
    sigma = std::sqrt(ss / static_cast<atx::f64>(m));
  };
  for (atx::usize pass = 0U; pass < kZScoreWinsorPasses; ++pass) {
    moments();
    if (!(sigma > 0.0)) {
      break;
    }
    const atx::f64 lo = mu_eq - kZScoreWinsor * sigma;
    const atx::f64 hi = mu_eq + kZScoreWinsor * sigma;
    bool clipped = false;
    for (Eigen::Index r = 0; r < m; ++r) {
      if (x(r, col) < lo) {
        x(r, col) = lo;
        clipped = true;
      } else if (x(r, col) > hi) {
        x(r, col) = hi;
        clipped = true;
      }
    }
    if (!clipped) {
      break;
    }
  }
  moments();
  if (!(sigma > 0.0)) {
    for (Eigen::Index r = 0; r < m; ++r) {
      x(r, col) = 0.0; // zero-variance column -> degenerate
    }
    return;
  }
  const atx::f64 inv_sigma = 1.0 / sigma;
  for (Eigen::Index r = 0; r < m; ++r) {
    x(r, col) = (x(r, col) - mu_w) * inv_sigma; // no post-centring clip (see contract)
  }
}

[[nodiscard]] std::vector<atx::u32> sector_groups(const std::vector<atx::usize> &survivors,
                                                         std::span<const atx::u32> group_id) {
  std::vector<atx::u32> groups;
  groups.reserve(survivors.size());
  for (const atx::usize inst : survivors) {
    groups.push_back(group_id[inst]);
  }
  std::sort(groups.begin(), groups.end());
  groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
  return groups;
}

[[nodiscard]] atx::core::linalg::VecX
sqrt_cap_weight(std::span<const atx::f64> market_cap,
                const std::vector<atx::usize> &kept_instrument_rows) {
  const Eigen::Index nk = static_cast<Eigen::Index>(kept_instrument_rows.size());
  atx::core::linalg::VecX w(nk);
  atx::f64 sum_root = 0.0; // Σ_j √(cap_j) over positive-cap kept names (order-fixed)
  atx::usize n_pos = 0U;
  for (const atx::usize inst : kept_instrument_rows) {
    const atx::f64 cap = market_cap[inst];
    if (cap > 0.0) {
      sum_root += std::sqrt(cap);
      ++n_pos;
    }
  }
  const atx::f64 mean_root = (n_pos == 0U) ? 0.0 : sum_root / static_cast<atx::f64>(n_pos);
  for (atx::usize j = 0U; j < kept_instrument_rows.size(); ++j) {
    const atx::f64 cap = market_cap[kept_instrument_rows[j]];
    w[static_cast<Eigen::Index>(j)] =
        (cap > 0.0 && mean_root > 0.0) ? std::sqrt(cap) / mean_root : 0.0; // 0.0 ⇒ fallback flag
  }
  return w;
}

void apply_industry_sum_to_zero(atx::core::linalg::MatX &xsr, const ExposureMatrix &xm,
                                       const std::vector<atx::usize> &keep,
                                       std::span<const atx::f64> market_cap) {
  const Eigen::Index nk = xsr.rows();
  if (nk == 0) {
    return;
  }
  atx::core::linalg::VecX nu(nk); // cap weights ν_i, normalized to Σ ν_i = 1
  atx::f64 total = 0.0;
  const bool use_cap = !market_cap.empty();
  for (atx::usize j = 0U; j < keep.size(); ++j) {
    const atx::f64 cap = use_cap ? market_cap[xm.instrument_rows[keep[j]]] : 1.0;
    const atx::f64 root = (cap > 0.0) ? std::sqrt(cap) : 0.0;
    nu[static_cast<Eigen::Index>(j)] = root;
    total += root;
  }
  if (total <= 0.0) {
    return; // no positive cap weight ⇒ constraint undefined ⇒ leave design as-is
  }
  nu /= total;
  for (atx::usize c = 0U; c < xm.columns.size(); ++c) {
    if (xm.columns[c].kind != ColumnTag::Kind::Sector) {
      continue; // only industry/sector dummies are mean-centered
    }
    const Eigen::Index col = static_cast<Eigen::Index>(c);
    atx::f64 wmean = 0.0;
    for (Eigen::Index r = 0; r < nk; ++r) {
      wmean += nu[r] * xsr(r, col);
    }
    for (Eigen::Index r = 0; r < nk; ++r) {
      xsr(r, col) -= wmean;
    }
  }
}

} // namespace detail

[[nodiscard]] atx::core::Result<ExposureMatrix>
build_exposures(const PanelView &panel, const FactorModelConfig &cfg, atx::usize row,
                std::span<const atx::f64> market_cap, std::span<const atx::u32> group_id) {
  if (row >= panel.rows()) {
    return atx::core::Err(atx::core::ErrorCode::OutOfRange,
                          "build_exposures: row offset is beyond the panel's valid rows");
  }
  const atx::usize n_inst = panel.instruments();
  if (!market_cap.empty() && market_cap.size() != n_inst) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "build_exposures: market_cap span length must equal instruments()");
  }
  if (!group_id.empty() && group_id.size() != n_inst) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "build_exposures: group_id span length must equal instruments()");
  }

  const bool have_cap = !market_cap.empty();
  const bool have_sectors = cfg.sector_factors && !group_id.empty();
  const std::vector<StyleFactor> styles = detail::emitted_styles(cfg, have_cap);

  // Pass 1: compute every emitted style's raw value per present instrument, apply
  // the §3.3 drop rule (any required-style NaN drops the instrument), and collect
  // the surviving rows (ascending universe column) + their raw style values.
  std::vector<atx::usize> survivors;
  std::vector<std::vector<atx::f64>> raw; // raw[surviving_row][style_index]
  survivors.reserve(n_inst);
  raw.reserve(n_inst);
  // Beta's market series is shared by every instrument of the cross-section: hoist it
  // (byte-identical values, see detail::market_returns) instead of recomputing it per
  // instrument.
  bool need_mkt = false;
  for (const StyleFactor f : styles) {
    need_mkt = need_mkt || (f == StyleFactor::Beta);
  }
  const std::vector<atx::f64> mkt =
      need_mkt ? detail::market_returns(panel, row, detail::kBetaWindow) : std::vector<atx::f64>{};
  for (atx::usize i = 0U; i < n_inst; ++i) {
    if (!panel.present(row, i)) {
      continue; // no bar at the current date -> not in the cross-section
    }
    if (have_sectors && group_id[i] == kNoGroup) {
      continue; // unclassified at this date -> cannot carry a sector dummy (W0-R0)
    }
    std::vector<atx::f64> vals(styles.size());
    bool drop = false;
    for (atx::usize s = 0U; s < styles.size() && !drop; ++s) {
      vals[s] = (styles[s] == StyleFactor::Beta)
                    ? detail::beta_cached(panel, row, i, mkt)
                    : detail::raw_style(styles[s], panel, row, i, market_cap);
      drop = std::isnan(vals[s]); // missing a required style -> drop the instrument
    }
    if (!drop) {
      survivors.push_back(i);
      raw.push_back(std::move(vals));
    }
  }

  // Column layout: sector dummies first (ascending group id), then style columns.
  std::vector<atx::u32> groups =
      have_sectors ? detail::sector_groups(survivors, group_id) : std::vector<atx::u32>{};
  const atx::usize n_sector = groups.size();
  const atx::usize n_style = styles.size();
  const atx::usize m = survivors.size();

  // SAFETY: m, n_sector, n_style are bounded by instruments()/kStyleFactorCount;
  //         the static_casts to Eigen::Index (signed) cannot overflow on any
  //         realistic universe (<< 2^31). Column-major MatX matches P4-7's WLS.
  atx::core::linalg::MatX x(static_cast<Eigen::Index>(m),
                            static_cast<Eigen::Index>(n_sector + n_style));
  std::vector<ColumnTag> columns;
  columns.reserve(n_sector + n_style);

  // Sector dummy columns (0/1, NOT standardized).
  for (atx::usize g = 0U; g < n_sector; ++g) {
    const atx::u32 gid = groups[g];
    for (atx::usize r = 0U; r < m; ++r) {
      const bool in_group = (group_id[survivors[r]] == gid);
      x(static_cast<Eigen::Index>(r), static_cast<Eigen::Index>(g)) = in_group ? 1.0 : 0.0;
    }
    columns.push_back(ColumnTag{ColumnTag::Kind::Sector, StyleFactor{}, gid});
  }

  // Style columns (raw -> cross-sectional z-score over the surviving set). The V2
  // rule cap-weights the mean with THIS date's cap (the span passed for `row`).
  std::vector<atx::f64> cap_w;
  if (cfg.zscore_rule == ZScoreRule::CapWeightedWinsorV2 && have_cap) {
    cap_w.reserve(m);
    for (const atx::usize inst : survivors) {
      cap_w.push_back(market_cap[inst]);
    }
  }
  for (atx::usize s = 0U; s < n_style; ++s) {
    const Eigen::Index col = static_cast<Eigen::Index>(n_sector + s);
    for (atx::usize r = 0U; r < m; ++r) {
      x(static_cast<Eigen::Index>(r), col) = raw[r][s];
    }
    switch (cfg.zscore_rule) {
    case ZScoreRule::EqualWeightV1:
      detail::zscore_column(x, col);
      break;
    case ZScoreRule::CapWeightedWinsorV2:
      detail::zscore_column_v2(x, col, std::span<const atx::f64>{cap_w});
      break;
    }
    columns.push_back(ColumnTag{ColumnTag::Kind::Style, styles[s], 0U});
  }

  return atx::core::Ok(ExposureMatrix{std::move(x), std::move(survivors), std::move(columns)});
}

[[nodiscard]] atx::core::Result<ExposureMatrix>
build_exposures(const PanelView &panel, const FactorModelConfig &cfg, atx::usize row,
                const PitSideInputs &side) {
  const atx::usize n_inst = panel.instruments();
  ATX_TRY_VOID(side.validate(n_inst));
  if (!side.covers(row)) {
    return atx::core::Err(atx::core::ErrorCode::OutOfRange,
                          "build_exposures: point-in-time side inputs do not cover this row");
  }
  return build_exposures(panel, cfg, row, side.cap_at(row, n_inst), side.group_at(row, n_inst));
}

} // namespace atx::engine::risk
