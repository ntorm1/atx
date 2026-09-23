#pragma once

// atx::engine::risk — L7 fundamental style factors + market intercept (Barra USE4-style).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  build_fundamental_exposures(panel, fund, cfg, row, as_of, market_cap, group_id)
//  assembles one date's exposure block X from
//    * the OHLCV PanelView (the P4 panel styles Size/Momentum/Volatility/Beta/
//      Liquidity plus the L7 price styles ResidVol and STReversal), and
//    * a POINT-IN-TIME FundamentalPanel (BookToPrice, EarningsYield, Growth,
//      Profitability, Leverage, DivYield, ShortInterest),
//  selected by a 32-bit StyleMask (bit i == StyleFactor i; bit StyleFactor::Market
//  adds an all-ones market/country intercept column).
//
//  This is a SUPERSET builder next to the legacy build_exposures (exposures.hpp),
//  which is untouched: the legacy 5-factor u8 path stays byte-identical, and this
//  builder is only reached when a caller asks for it.
//
// ===========================================================================
//  PIT — availability, not fiscal period
// ===========================================================================
//  Every FundamentalPanel record is keyed by the day it became PUBLIC (`available`,
//  the filing / release date), never by the fiscal period it describes. A lookup at
//  day `as_of` sees only records with available + cfg.availability_lag <= as_of
//  (upper_bound on a sorted series). Moving a record's availability date past the
//  query date therefore removes it from the exposure — tested directly. Price styles
//  read only panel rows >= `row` (PanelView is newest-first), as in exposures.hpp.
//  Exposures are those KNOWN AT THE END of date `as_of`, and a release dated
//  `available` may come after that day's close. The DEFAULT availability_lag is
//  therefore 1: a record first enters the exposures of day available + 1. That is the
//  first day it can be traded on and the first return it can be regressed against.
//  Set availability_lag = 0 only when `available` is already the first tradable day.
//
// ===========================================================================
//  Raw definitions (cap_i = market_cap[i] when given, else shares·close(row,i))
// ===========================================================================
//    BookToPrice   = BookEquity / cap
//    EarningsYield = EarningsTtm / cap
//    Growth        = (Sales(as_of) − Sales(as_of − growth_lookback)) / |Sales(prior)|
//    Profitability = GrossProfit / TotalAssets                (Novy-Marx)
//    Leverage      = TotalDebt / TotalAssets                  (book leverage)
//    DivYield      = DividendsTtm / cap
//    ShortInterest = ShortInterestShares / SharesOutstanding
//    ResidVol      = population std of the residual of r_i on the equal-weight market
//                    return over the trailing kResidVolWindow rows
//    STReversal    = Σ log(1 + r_i) over the trailing kReversalWindow rows (the 1-month
//                    log return; the reversal PREMIUM is its negative, the risk factor
//                    is sign-agnostic)
//
// ===========================================================================
//  Standardization (USE4 convention)
// ===========================================================================
//  Each style column is standardized over its non-missing entries to CAP-WEIGHTED mean
//  0 and EQUAL-WEIGHTED standard deviation 1 (the std is taken about the cap-weighted
//  mean). When cfg.winsor > 0 the standardized values are clipped to ±winsor and the
//  column is standardized once more. With no positive cap in the cross-section the
//  weights fall back to equal. Missing FUNDAMENTAL entries follow cfg.missing: FillZero
//  (the cap-weighted mean, i.e. "no information") or Drop. A missing PANEL style
//  (Size/Momentum/Volatility/Beta/Liquidity/ResidVol/STReversal) always drops the
//  instrument, exactly like the legacy §3.3 drop rule.
//
//  COLUMN ORDER: [Market] [sector dummies, ascending group id] [styles in enum order].
//
// ===========================================================================
//  Determinism
// ===========================================================================
//  No RNG, no clock, no map iteration; every reduction is ascending (instrument, row).

#include <algorithm> // std::upper_bound, std::clamp
#include <bitset>    // std::bitset (StyleMask)
#include <cmath>     // std::isfinite, std::isnan, std::log, std::log1p, std::sqrt, std::abs
#include <limits>    // std::numeric_limits
#include <span>      // std::span
#include <utility>   // std::move
#include <vector>    // std::vector

#include "atx/core/error.hpp" // Result, Status, Ok, Err, ErrorCode
#include "atx/core/types.hpp" // f64, i64, u8, u32, usize

#include "atx/core/linalg/linalg.hpp" // MatX

#include "atx/engine/loop/panel_types.hpp" // PanelView
#include "atx/engine/risk/exposures.hpp"   // StyleFactor, ColumnTag, ExposureMatrix, detail::*

namespace atx::engine::risk {

// 32-bit style selector: bit i == StyleFactor i (bits >= kAllStyleFactorCount unused).
using StyleMask = std::bitset<32>;

// The StyleMask bit of one StyleFactor.
[[nodiscard]] inline StyleMask style_bit(StyleFactor f) noexcept {
  return StyleMask{1ULL << static_cast<unsigned>(f)};
}

// The legacy P4 u8 style_mask lifted into a StyleMask (bit-for-bit, bits 0..4).
[[nodiscard]] inline StyleMask legacy_style_mask(atx::u8 m) noexcept {
  return StyleMask{static_cast<unsigned long long>(m)};
}

// True for the styles computed from the FundamentalPanel (need a non-null panel).
[[nodiscard]] constexpr bool is_fundamental_style(StyleFactor f) noexcept {
  switch (f) {
  case StyleFactor::BookToPrice:
  case StyleFactor::EarningsYield:
  case StyleFactor::Growth:
  case StyleFactor::Profitability:
  case StyleFactor::Leverage:
  case StyleFactor::DivYield:
  case StyleFactor::ShortInterest:
    return true;
  case StyleFactor::Size:
  case StyleFactor::Momentum:
  case StyleFactor::Volatility:
  case StyleFactor::Beta:
  case StyleFactor::Liquidity:
  case StyleFactor::ResidVol:
  case StyleFactor::STReversal:
  case StyleFactor::Market:
    return false;
  }
  return false; // unreachable (switch exhaustive)
}

// Raw fundamental fields a FundamentalPanel carries.
enum class FundamentalField : atx::u8 {
  BookEquity,
  EarningsTtm,
  SalesTtm,
  GrossProfit,
  TotalAssets,
  TotalDebt,
  DividendsTtm,
  SharesOutstanding,
  ShortInterestShares,
};
inline constexpr atx::usize kFundamentalFieldCount = 9U;

// ===========================================================================
//  FundamentalPanel — per-(instrument, field) availability-keyed series.
//
//  Dates are opaque monotone day numbers (i64; e.g. days since epoch). Records are
//  kept sorted by `available` (insertion keeps order; ties keep insertion order, so a
//  later restatement published the same day wins the lookup). COLD path.
// ===========================================================================
class FundamentalPanel {
public:
  explicit FundamentalPanel(atx::usize n_instruments)
      : n_{n_instruments}, series_(n_instruments * kFundamentalFieldCount) {}

  [[nodiscard]] atx::usize instruments() const noexcept { return n_; }

  // Record `value` for (inst, field), public from day `available`. Err on an
  // out-of-range instrument or a non-finite value (missing data is simply absent).
  [[nodiscard]] atx::core::Status add(atx::usize inst, FundamentalField field, atx::i64 available,
                                      atx::f64 value) {
    if (inst >= n_) {
      return atx::core::Err(atx::core::ErrorCode::OutOfRange,
                            "FundamentalPanel::add: instrument index out of range");
    }
    if (!std::isfinite(value)) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "FundamentalPanel::add: value must be finite");
    }
    std::vector<Rec> &s = series_[slot(inst, field)];
    const auto pos = std::upper_bound(s.begin(), s.end(), available,
                                      [](atx::i64 a, const Rec &r) { return a < r.available; });
    s.insert(pos, Rec{available, value});
    return atx::core::Ok();
  }

  // PIT value of (inst, field) at day `as_of`: the latest record with
  // available + lag <= as_of. NaN when none exists, or when max_staleness > 0 and the
  // record is older than max_staleness days. PRECONDITION inst < instruments().
  [[nodiscard]] atx::f64 as_of(atx::usize inst, FundamentalField field, atx::i64 as_of_day,
                               atx::i64 lag = 0, atx::i64 max_staleness = 0) const noexcept {
    const std::vector<Rec> &s = series_[slot(inst, field)];
    const atx::i64 cutoff = as_of_day - lag; // visible iff available <= as_of − lag
    const auto pos = std::upper_bound(s.begin(), s.end(), cutoff,
                                      [](atx::i64 a, const Rec &r) { return a < r.available; });
    if (pos == s.begin()) {
      return std::numeric_limits<atx::f64>::quiet_NaN();
    }
    const Rec &rec = *(pos - 1);
    if (max_staleness > 0 && cutoff - rec.available > max_staleness) {
      return std::numeric_limits<atx::f64>::quiet_NaN();
    }
    return rec.value;
  }

private:
  struct Rec {
    atx::i64 available;
    atx::f64 value;
  };
  [[nodiscard]] atx::usize slot(atx::usize inst, FundamentalField f) const noexcept {
    return static_cast<atx::usize>(f) * n_ + inst;
  }

  atx::usize n_;
  std::vector<std::vector<Rec>> series_; // [field * n + inst], ascending `available`
};

// Missing-fundamental policy (panel styles always drop — see header).
enum class MissingPolicy : atx::u8 { FillZero, Drop };

struct FundamentalCfg {
  StyleMask mask{};                 // StyleFactor bits; bit Market adds the intercept
  bool sector_factors = true;       // one 0/1 dummy per group id (needs group_id)
  atx::i64 availability_lag = 1;    // days added to every record's availability (see PIT)
  atx::i64 max_staleness = 0;       // days; 0 ⇒ a record never goes stale
  atx::i64 growth_lookback = 365;   // days between the two Sales observations of Growth
  atx::f64 winsor = 3.0;            // |z| clip before re-standardizing; <= 0 disables
  MissingPolicy missing = MissingPolicy::FillZero;
};

namespace detail {

inline constexpr atx::usize kResidVolWindow = 60U;  // ResidVol lookback (rows)
inline constexpr atx::usize kReversalWindow = 21U;  // STReversal lookback (rows)

[[nodiscard]] inline atx::f64 nan_f64() noexcept {
  return std::numeric_limits<atx::f64>::quiet_NaN();
}

// num/den with NaN on a missing input or a non-positive denominator.
[[nodiscard]] inline atx::f64 safe_ratio(atx::f64 num, atx::f64 den) noexcept {
  if (std::isnan(num) || std::isnan(den) || den <= 0.0) {
    return nan_f64();
  }
  return num / den;
}

// Population std of the residual of r_i on the market over the trailing window.
// `mkt[k]` is the market return at row+k (detail::market_returns). NaN when any of
// the window's returns is missing or the market has zero variance.
[[nodiscard]] inline atx::f64 resid_vol(const PanelView &panel, atx::usize row, atx::usize i,
                                        std::span<const atx::f64> mkt) noexcept {
  constexpr atx::usize n = kResidVolWindow;
  if (row + n + 1U > panel.rows() || mkt.size() < n) {
    return nan_f64();
  }
  atx::f64 si = 0.0;
  atx::f64 sm = 0.0;
  for (atx::usize k = 0U; k < n; ++k) {
    const atx::f64 ri = step_return(panel, row + k, i);
    if (std::isnan(ri) || std::isnan(mkt[k])) {
      return nan_f64();
    }
    si += ri;
    sm += mkt[k];
  }
  const atx::f64 nf = static_cast<atx::f64>(n);
  const atx::f64 mi = si / nf;
  const atx::f64 mm = sm / nf;
  atx::f64 cov = 0.0;
  atx::f64 var = 0.0;
  for (atx::usize k = 0U; k < n; ++k) {
    const atx::f64 dm = mkt[k] - mm;
    cov += (step_return(panel, row + k, i) - mi) * dm;
    var += dm * dm;
  }
  if (var <= 0.0) {
    return nan_f64();
  }
  const atx::f64 b = cov / var;
  atx::f64 ss = 0.0;
  for (atx::usize k = 0U; k < n; ++k) {
    const atx::f64 e = (step_return(panel, row + k, i) - mi) - b * (mkt[k] - mm);
    ss += e * e;
  }
  return std::sqrt(ss / nf);
}

// Σ log(1 + r) over the trailing kReversalWindow rows; NaN on any missing return or
// a return <= −1.
[[nodiscard]] inline atx::f64 st_reversal(const PanelView &panel, atx::usize row,
                                          atx::usize i) noexcept {
  if (valid_returns(panel, row, i, kReversalWindow) < kReversalWindow) {
    return nan_f64();
  }
  atx::f64 s = 0.0;
  for (atx::usize k = 0U; k < kReversalWindow; ++k) {
    const atx::f64 r = step_return(panel, row + k, i);
    if (r <= -1.0) {
      return nan_f64();
    }
    s += std::log1p(r);
  }
  return s;
}

// Everything a raw-style evaluation of one instrument needs.
struct StyleCtx {
  const PanelView *panel;
  const FundamentalPanel *fund; // null ⇒ fundamental styles unavailable (caller-validated)
  const FundamentalCfg *cfg;
  atx::usize row;
  atx::i64 as_of;
  std::span<const atx::f64> mkt; // market returns from `row` (empty when not needed)
};

[[nodiscard]] inline atx::f64 fval(const StyleCtx &c, atx::usize i, FundamentalField f,
                                   atx::i64 day) noexcept {
  return c.fund->as_of(i, f, day, c.cfg->availability_lag, c.cfg->max_staleness);
}

// Raw value of style `f` for instrument i with cap `cap` (NaN = missing). EXHAUSTIVE.
[[nodiscard]] inline atx::f64 raw_l7_style(StyleFactor f, const StyleCtx &c, atx::usize i,
                                           atx::f64 cap) noexcept {
  const PanelView &p = *c.panel;
  switch (f) {
  case StyleFactor::Size:
    return (std::isnan(cap) || cap <= 0.0) ? nan_f64() : std::log(cap);
  case StyleFactor::Momentum:
    return momentum(p, c.row, i);
  case StyleFactor::Volatility:
    return volatility(p, c.row, i);
  case StyleFactor::Beta:
    return beta_cached(p, c.row, i, c.mkt);
  case StyleFactor::Liquidity:
    return liquidity(p, c.row, i);
  case StyleFactor::BookToPrice:
    return safe_ratio(fval(c, i, FundamentalField::BookEquity, c.as_of), cap);
  case StyleFactor::EarningsYield:
    return safe_ratio(fval(c, i, FundamentalField::EarningsTtm, c.as_of), cap);
  case StyleFactor::Growth: {
    const atx::f64 now = fval(c, i, FundamentalField::SalesTtm, c.as_of);
    const atx::f64 before =
        fval(c, i, FundamentalField::SalesTtm, c.as_of - c.cfg->growth_lookback);
    if (std::isnan(now) || std::isnan(before) || before == 0.0) {
      return nan_f64();
    }
    return (now - before) / std::abs(before);
  }
  case StyleFactor::Profitability:
    return safe_ratio(fval(c, i, FundamentalField::GrossProfit, c.as_of),
                      fval(c, i, FundamentalField::TotalAssets, c.as_of));
  case StyleFactor::Leverage:
    return safe_ratio(fval(c, i, FundamentalField::TotalDebt, c.as_of),
                      fval(c, i, FundamentalField::TotalAssets, c.as_of));
  case StyleFactor::DivYield:
    return safe_ratio(fval(c, i, FundamentalField::DividendsTtm, c.as_of), cap);
  case StyleFactor::ShortInterest:
    return safe_ratio(fval(c, i, FundamentalField::ShortInterestShares, c.as_of),
                      fval(c, i, FundamentalField::SharesOutstanding, c.as_of));
  case StyleFactor::ResidVol:
    return resid_vol(p, c.row, i, c.mkt);
  case StyleFactor::STReversal:
    return st_reversal(p, c.row, i);
  case StyleFactor::Market:
    return 1.0;
  }
  return nan_f64(); // unreachable (switch exhaustive)
}

// Standardize `v` (NaN = missing) to cap-weighted mean 0 / equal-weighted std 1 over
// its non-missing entries, IN PLACE (missing entries stay NaN). `w` are the cap
// weights (>= 0); an all-zero weight vector falls back to equal weights. A column with
// < 2 entries or zero spread standardizes to 0. Order-fixed.
inline void standardize_cw(std::vector<atx::f64> &v, const std::vector<atx::f64> &w) noexcept {
  atx::f64 sw = 0.0;
  atx::f64 swx = 0.0;
  atx::f64 sx = 0.0;
  atx::usize n = 0U;
  for (atx::usize j = 0U; j < v.size(); ++j) {
    if (std::isnan(v[j])) {
      continue;
    }
    sw += w[j];
    swx += w[j] * v[j];
    sx += v[j];
    ++n;
  }
  if (n == 0U) {
    return;
  }
  const atx::f64 mean = (sw > 0.0) ? swx / sw : sx / static_cast<atx::f64>(n);
  atx::f64 ss = 0.0;
  for (const atx::f64 x : v) {
    if (!std::isnan(x)) {
      ss += (x - mean) * (x - mean);
    }
  }
  const atx::f64 sd = std::sqrt(ss / static_cast<atx::f64>(n));
  const bool degenerate = (n < 2U) || !(sd > 0.0);
  for (atx::f64 &x : v) {
    if (!std::isnan(x)) {
      x = degenerate ? 0.0 : (x - mean) / sd;
    }
  }
}

} // namespace detail

// ===========================================================================
//  build_fundamental_exposures — the L7 per-date exposure builder.
//
//  `row`       : panel row of the cross-section (0 = newest); only rows >= row read.
//  `as_of`     : the day number of `row` (the PIT fundamental query date).
//  `fund`      : may be null iff no fundamental style bit is set.
//  `market_cap`: caps as of `row` (length instruments(), or empty ⇒ cap is
//                SharesOutstanding·close(row, i) from the fundamental panel).
//  `group_id`  : sector id per instrument (length instruments(), or empty ⇒ none).
//  Err on row >= rows(), a span/panel length mismatch, a fundamental style without a
//  panel, or a panel whose instrument count differs from the price panel's.
// ===========================================================================
[[nodiscard]] inline atx::core::Result<ExposureMatrix>
build_fundamental_exposures(const PanelView &panel, const FundamentalPanel *fund,
                            const FundamentalCfg &cfg, atx::usize row, atx::i64 as_of,
                            std::span<const atx::f64> market_cap,
                            std::span<const atx::u32> group_id) {
  using atx::core::Err;
  using atx::core::ErrorCode;
  const atx::usize n_inst = panel.instruments();
  if (row >= panel.rows()) {
    return Err(ErrorCode::OutOfRange, "build_fundamental_exposures: row beyond panel rows");
  }
  if ((!market_cap.empty() && market_cap.size() != n_inst) ||
      (!group_id.empty() && group_id.size() != n_inst)) {
    return Err(ErrorCode::InvalidArgument,
               "build_fundamental_exposures: cap/group span length must equal instruments()");
  }
  if (fund != nullptr && fund->instruments() != n_inst) {
    return Err(ErrorCode::InvalidArgument,
               "build_fundamental_exposures: fundamental panel instrument count mismatch");
  }

  // Emitted styles (enum order, Market handled as its own leading column).
  std::vector<StyleFactor> styles;
  bool need_mkt = false;
  atx::usize mkt_rows = 0U;
  for (atx::usize b = 0U; b < kAllStyleFactorCount; ++b) {
    const auto f = static_cast<StyleFactor>(b);
    if (!cfg.mask.test(b) || f == StyleFactor::Market) {
      continue;
    }
    if (is_fundamental_style(f) && fund == nullptr) {
      return Err(ErrorCode::InvalidArgument,
                 "build_fundamental_exposures: fundamental style requested without a panel");
    }
    if (f == StyleFactor::Beta) {
      need_mkt = true;
      mkt_rows = std::max(mkt_rows, detail::kBetaWindow);
    }
    if (f == StyleFactor::ResidVol) {
      need_mkt = true;
      mkt_rows = std::max(mkt_rows, detail::kResidVolWindow);
    }
    styles.push_back(f);
  }
  const bool market_col = cfg.mask.test(static_cast<atx::usize>(StyleFactor::Market));
  const std::vector<atx::f64> mkt =
      need_mkt ? detail::market_returns(panel, row, mkt_rows) : std::vector<atx::f64>{};
  const detail::StyleCtx ctx{&panel, fund, &cfg, row, as_of, std::span<const atx::f64>{mkt}};

  // Pass 1: raw values + the drop rule.
  const atx::usize n_style = styles.size();
  std::vector<atx::usize> survivors;
  std::vector<atx::f64> caps;             // per survivor (NaN when unknown)
  std::vector<std::vector<atx::f64>> col(n_style); // col[s][survivor]
  std::vector<atx::f64> vals(n_style);
  for (atx::usize i = 0U; i < n_inst; ++i) {
    if (!panel.present(row, i)) {
      continue;
    }
    atx::f64 cap = market_cap.empty() ? detail::nan_f64() : market_cap[i];
    if (market_cap.empty() && fund != nullptr) {
      const atx::f64 sh = detail::fval(ctx, i, FundamentalField::SharesOutstanding, as_of);
      cap = sh * panel.close(row, i);
    }
    bool drop = false;
    for (atx::usize s = 0U; s < n_style && !drop; ++s) {
      vals[s] = detail::raw_l7_style(styles[s], ctx, i, cap);
      const bool missing = std::isnan(vals[s]);
      drop = missing && (!is_fundamental_style(styles[s]) || cfg.missing == MissingPolicy::Drop);
    }
    if (drop) {
      continue;
    }
    survivors.push_back(i);
    caps.push_back(cap);
    for (atx::usize s = 0U; s < n_style; ++s) {
      col[s].push_back(vals[s]);
    }
  }

  // Cap weights for the standardization (>= 0; NaN/non-positive cap ⇒ weight 0).
  std::vector<atx::f64> w(survivors.size());
  for (atx::usize j = 0U; j < survivors.size(); ++j) {
    w[j] = (std::isfinite(caps[j]) && caps[j] > 0.0) ? caps[j] : 0.0;
  }
  for (atx::usize s = 0U; s < n_style; ++s) {
    detail::standardize_cw(col[s], w);
    if (cfg.winsor > 0.0) {
      for (atx::f64 &x : col[s]) {
        if (!std::isnan(x)) {
          x = std::clamp(x, -cfg.winsor, cfg.winsor);
        }
      }
      detail::standardize_cw(col[s], w);
    }
    for (atx::f64 &x : col[s]) {
      x = std::isnan(x) ? 0.0 : x; // FillZero: the cap-weighted mean ("no information")
    }
  }

  // Assemble [Market][sectors][styles].
  const bool have_sectors = cfg.sector_factors && !group_id.empty();
  const std::vector<atx::u32> groups =
      have_sectors ? detail::sector_groups(survivors, group_id) : std::vector<atx::u32>{};
  const atx::usize n_mkt = market_col ? 1U : 0U;
  const atx::usize m = survivors.size();
  atx::core::linalg::MatX x(static_cast<Eigen::Index>(m),
                            static_cast<Eigen::Index>(n_mkt + groups.size() + n_style));
  std::vector<ColumnTag> columns;
  columns.reserve(static_cast<atx::usize>(x.cols()));
  Eigen::Index c = 0;
  if (market_col) {
    x.col(c).setOnes();
    columns.push_back(ColumnTag{ColumnTag::Kind::Style, StyleFactor::Market, 0U});
    ++c;
  }
  for (const atx::u32 gid : groups) {
    for (atx::usize r = 0U; r < m; ++r) {
      x(static_cast<Eigen::Index>(r), c) = (group_id[survivors[r]] == gid) ? 1.0 : 0.0;
    }
    columns.push_back(ColumnTag{ColumnTag::Kind::Sector, StyleFactor{}, gid});
    ++c;
  }
  for (atx::usize s = 0U; s < n_style; ++s) {
    for (atx::usize r = 0U; r < m; ++r) {
      x(static_cast<Eigen::Index>(r), c) = col[s][r];
    }
    columns.push_back(ColumnTag{ColumnTag::Kind::Style, styles[s], 0U});
    ++c;
  }
  return atx::core::Ok(ExposureMatrix{std::move(x), std::move(survivors), std::move(columns)});
}

} // namespace atx::engine::risk
