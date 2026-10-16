#include "atx/vol/api/fitting/quote_fit_policy.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace atx::vol {
namespace {

using atx::core::Err;
using atx::core::Ok;

[[nodiscard]] bool positive(double value) noexcept { return std::isfinite(value) && value > 0.0; }
[[nodiscard]] bool nonnegative(double value) noexcept {
  return std::isfinite(value) && value >= 0.0;
}

[[nodiscard]] std::uint32_t count32(std::size_t count) noexcept {
  return static_cast<std::uint32_t>(
      std::min(count, static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
}

[[nodiscard]] double median(std::vector<double> &values) {
  std::sort(values.begin(), values.end());
  const std::size_t middle = values.size() / 2u;
  return values.size() % 2u != 0u ? values[middle]
                                  : 0.5 * values[middle - 1u] + 0.5 * values[middle];
}

struct RawContract {
  double strike;
  Side side;
  double bid;
  double ask;
};

[[nodiscard]] std::size_t append_expiry_features(const QuoteFitExpirySnapshot &snapshot,
                                                 double band, QuoteFitLiquidityFeatures &out,
                                                 std::vector<double> &relative_spreads) {
  std::vector<RawContract> contracts;
  contracts.reserve(snapshot.quotes.size());
  out.rows_seen += snapshot.quotes.size();
  for (const auto &quote : snapshot.quotes) {
    if (!positive(quote.strike) || (quote.side != Side::Call && quote.side != Side::Put) ||
        !nonnegative(quote.bid) || !positive(quote.ask) || !(quote.ask > quote.bid)) {
      ++out.invalid_rows;
      continue;
    }
    contracts.push_back({quote.strike, quote.side, quote.bid, quote.ask});
  }
  std::sort(contracts.begin(), contracts.end(), [](const auto &a, const auto &b) {
    return a.strike < b.strike || (a.strike == b.strike && a.side < b.side);
  });
  std::size_t distinct_near = 0u;
  double last_near_strike = 0.0;
  const double log_spot = std::log(snapshot.market.spot);
  for (std::size_t i = 0u; i < contracts.size();) {
    RawContract quote = contracts[i++];
    while (i < contracts.size() && contracts[i].strike == quote.strike &&
           contracts[i].side == quote.side) {
      quote.bid = std::min(quote.bid, contracts[i].bid);
      quote.ask = std::max(quote.ask, contracts[i].ask);
      ++i;
    }
    ++out.unique_contracts;
    const bool near = std::fabs(std::log(quote.strike) - log_spot) <= band;
    if (near) {
      ++out.near_money_contracts;
    }
    if (quote.bid == 0.0) {
      ++out.bidless_contracts;
      out.near_money_bidless_contracts += near ? 1u : 0u;
      continue;
    }
    ++out.two_sided_contracts;
    if (near) {
      ++out.near_money_two_sided_contracts;
      // Equivalent to (ask-bid)/mid without overflowing the midpoint sum.
      relative_spreads.push_back((quote.ask - quote.bid) / (0.5 * quote.ask + 0.5 * quote.bid));
      if (quote.strike != last_near_strike) {
        ++distinct_near;
        last_near_strike = quote.strike;
      }
    }
  }
  return distinct_near;
}

[[nodiscard]] Status validate_regimes(const QuoteFitRegimeConfig &r) {
  const auto fraction = [](double x) { return nonnegative(x) && x <= 1.0; };
  if (!positive(r.near_money_log_band) || r.sparse_min_median_strikes < 4u ||
      r.dense_min_median_strikes < r.sparse_min_median_strikes ||
      !positive(r.dense_max_median_relative_spread) ||
      !positive(r.sparse_min_median_relative_spread) ||
      r.dense_max_median_relative_spread >= r.sparse_min_median_relative_spread ||
      r.sparse_min_median_relative_spread > 2.0 || !fraction(r.dense_max_bidless_fraction) ||
      !fraction(r.sparse_min_bidless_fraction) ||
      r.dense_max_bidless_fraction >= r.sparse_min_bidless_fraction) {
    return Err(ErrorCode::InvalidArgument, "quote fit policy: invalid liquidity thresholds");
  }
  return Ok();
}

[[nodiscard]] Status validate_profile(const QuoteFitProfile &profile) {
  const auto &cfg = profile.config;
  const auto &quality = cfg.quality;
  if (profile.name.empty() || static_cast<std::size_t>(profile.kind) >= kProfileKindCount) {
    return Err(ErrorCode::InvalidArgument, "quote fit policy: invalid profile name/kind");
  }
  ATX_TRY_VOID(validate_calib_options(cfg.curve.parametric));
  if (!cfg.curve.parametric.validate_no_arb || !cfg.curve.parametric.lee_bound_project) {
    return Err(ErrorCode::InvalidArgument,
               "quote fit policy: no-arbitrage controls must remain enabled");
  }
  std::size_t minimum = 4u;
  switch (cfg.curve.kind) {
  case VolCurveKind::ConvexDense:
    if (!cfg.curve.convex.bound_slope_below || !nonnegative(cfg.curve.convex.lambda) ||
        cfg.curve.convex.node_cap < 2 || cfg.curve.convex.max_iter < 1) {
      return Err(ErrorCode::InvalidArgument,
                 "quote fit policy: invalid constrained dense configuration");
    }
    break;
  case VolCurveKind::Essvi:
    minimum = std::max<std::size_t>(minimum, cfg.curve.parametric.min_obs_per_slice);
    if (cfg.curve.parametric.essvi_asymmetric_rho || !cfg.curve.parametric.residual_disable) {
      minimum = std::max<std::size_t>(minimum, 8u);
    }
    break;
  case VolCurveKind::Svi:
    minimum = std::max<std::size_t>(6u, cfg.curve.parametric.min_obs_per_slice);
    break;
  case VolCurveKind::C8:
    minimum = std::max<std::size_t>(10u, cfg.curve.parametric.min_obs_per_slice);
    break;
  case VolCurveKind::SplineVol:
    minimum = std::max<std::size_t>(6u, cfg.curve.spline.min_obs);
    break;
  case VolCurveKind::LinearVariance:
    return Err(ErrorCode::InvalidArgument,
               "quote fit policy: linear variance is not a risk-fit family");
  default:
    return Err(ErrorCode::InvalidArgument, "quote fit policy: unknown curve family");
  }
  if (cfg.min_quotes < minimum || !positive(cfg.spread_floor) || !nonnegative(cfg.min_time_value) ||
      !positive(cfg.iv_tolerance) || cfg.max_iv_iterations == 0u ||
      !nonnegative(cfg.atm_log_band) || cfg.discrete_div_steps < 1 ||
      cfg.discrete_div_steps > kDiscreteDivMaxSteps ||
      (cfg.side_policy != QuoteSliceSidePolicy::All &&
       cfg.side_policy != QuoteSliceSidePolicy::OutOfTheMoney) ||
      (cfg.dividend_model != QuoteSliceDividendModel::ContinuousYield &&
       cfg.dividend_model != QuoteSliceDividendModel::AggregatePresentValue &&
       cfg.dividend_model != QuoteSliceDividendModel::AggregateFundingPresentValue &&
       cfg.dividend_model != QuoteSliceDividendModel::DatedCash) ||
      !nonnegative(quality.min_bid) || !nonnegative(quality.max_spread_to_mid) ||
      !nonnegative(quality.max_spread_vol) || !nonnegative(quality.min_vega_weight) ||
      !positive(quality.max_weight)) {
    return Err(ErrorCode::InvalidArgument,
               "quote fit policy: invalid quote quality or model point count");
  }
  const auto grid = cfg.curve.spline.grid;
  if (grid.size() < 4u || !nonnegative(cfg.curve.spline.lambda)) {
    return Err(ErrorCode::InvalidArgument, "quote fit policy: invalid spline configuration");
  }
  for (std::size_t i = 0u; i < grid.size(); ++i) {
    if (!std::isfinite(grid[i]) || (i > 0u && !(grid[i] > grid[i - 1u]))) {
      return Err(ErrorCode::InvalidArgument,
                 "quote fit policy: spline grid must be finite and increasing");
    }
  }
  return Ok();
}

struct OwnedProfile {
  std::vector<double> spline_grid;
  QuoteFitProfile profile;
};

} // namespace

Result<QuoteFitLiquidityFeatures>
quote_fit_liquidity_features(std::span<const QuoteFitExpirySnapshot> snapshots,
                             double near_money_log_band) {
  if (!positive(near_money_log_band)) {
    return Err(ErrorCode::InvalidArgument, "quote fit policy: invalid near-money band");
  }
  std::vector<double> maturities;
  maturities.reserve(snapshots.size());
  for (const auto &snapshot : snapshots) {
    if (!positive(snapshot.market.spot) || !positive(snapshot.market.calendar_years) ||
        !positive(snapshot.market.variance_years)) {
      return Err(ErrorCode::InvalidArgument, "quote fit policy: invalid spot or maturity clocks");
    }
    maturities.push_back(snapshot.market.calendar_years);
  }
  std::sort(maturities.begin(), maturities.end());
  if (std::adjacent_find(maturities.begin(), maturities.end()) != maturities.end()) {
    return Err(ErrorCode::InvalidArgument, "quote fit policy: duplicate calendar maturity");
  }
  QuoteFitLiquidityFeatures out;
  std::vector<double> spreads;
  std::vector<double> counts;
  counts.reserve(snapshots.size());
  std::size_t quoted_expiries = 0u;
  std::size_t identified_expiries = 0u;
  std::size_t front_expiries = 0u;
  for (const auto &snapshot : snapshots) {
    const std::size_t before = out.two_sided_contracts;
    const std::size_t distinct =
        append_expiry_features(snapshot, near_money_log_band, out, spreads);
    counts.push_back(static_cast<double>(distinct));
    quoted_expiries += out.two_sided_contracts > before ? 1u : 0u;
    identified_expiries += distinct >= kMinIdentifiableSliceStrikes ? 1u : 0u;
    front_expiries += snapshot.market.calendar_years <= kFrontExpiryWindowYears ? 1u : 0u;
    out.max_near_money_strikes = std::max(out.max_near_money_strikes, distinct);
  }
  out.bidless_fraction = out.unique_contracts > 0u ? static_cast<double>(out.bidless_contracts) /
                                                         static_cast<double>(out.unique_contracts)
                                                   : 0.0;
  out.near_money_bidless_fraction = out.near_money_contracts > 0u
                                        ? static_cast<double>(out.near_money_bidless_contracts) /
                                              static_cast<double>(out.near_money_contracts)
                                        : 0.0;
  if (!spreads.empty()) {
    out.median_relative_spread = median(spreads);
  }
  if (!counts.empty()) {
    out.median_near_money_strikes = median(counts);
  }
  auto &in = out.classifier_inputs;
  in.n_live_quotes = count32(out.two_sided_contracts);
  in.n_atm_quotes = count32(out.near_money_two_sided_contracts);
  in.n_live_expiries = count32(snapshots.size());
  in.n_quoted_expiries = count32(quoted_expiries);
  in.n_identifiable_expiries = count32(identified_expiries);
  in.max_near_money_strikes = count32(out.max_near_money_strikes);
  in.n_front_expiries = count32(front_expiries);
  in.median_spread_pct =
      out.median_relative_spread.value_or(std::numeric_limits<double>::infinity());
  out.classifier = classify_profile(in);
  return Ok(std::move(out));
}

const char *to_string(QuoteFitPolicySource source) noexcept {
  switch (source) {
  case QuoteFitPolicySource::RawLiquidity:
    return "raw-liquidity";
  case QuoteFitPolicySource::ExactSymbolOverride:
    return "exact-symbol-override";
  }
  return "unknown";
}

Result<QuoteFitLiquidityRegime>
classify_quote_fit_liquidity(const QuoteFitLiquidityFeatures &features,
                             const QuoteFitRegimeConfig &regimes) {
  ATX_TRY_VOID(validate_regimes(regimes));
  const auto spread = features.median_relative_spread;
  if (!nonnegative(features.median_near_money_strikes) ||
      !nonnegative(features.near_money_bidless_fraction) ||
      features.near_money_bidless_fraction > 1.0 ||
      (spread && (!nonnegative(*spread) || *spread > 2.0)) ||
      static_cast<std::size_t>(features.classifier.kind) >= kProfileKindCount) {
    return Err(ErrorCode::InvalidArgument, "quote fit policy: invalid routing features");
  }
  if (!spread || features.max_near_money_strikes < kMinIdentifiableSliceStrikes ||
      features.median_near_money_strikes <
          static_cast<double>(regimes.sparse_min_median_strikes) ||
      *spread >= regimes.sparse_min_median_relative_spread ||
      features.near_money_bidless_fraction >= regimes.sparse_min_bidless_fraction ||
      features.classifier.kind == ProfileKind::IlliquidSmallCap) {
    return Ok(QuoteFitLiquidityRegime::Sparse);
  }
  if (features.median_near_money_strikes >=
          static_cast<double>(regimes.dense_min_median_strikes) &&
      *spread <= regimes.dense_max_median_relative_spread &&
      features.near_money_bidless_fraction <= regimes.dense_max_bidless_fraction) {
    return Ok(QuoteFitLiquidityRegime::Dense);
  }
  return Ok(QuoteFitLiquidityRegime::Ordinary);
}

std::vector<QuoteFitProfile> default_quote_fit_profiles() {
  QuoteFitProfile ordinary{"ordinary", ProfileKind::OrdinarySingleName, {}};
  ordinary.config.quality.max_spread_to_mid = 0.80;
  ordinary.config.quality.max_spread_vol = 0.10;
  ordinary.config.curve.parametric.max_spread_vol = 0.10;
  QuoteFitProfile dense = ordinary;
  dense.name = "dense";
  dense.kind = ProfileKind::IndexEtfUltraLiquid;
  dense.config.curve.kind = VolCurveKind::ConvexDense;
  dense.config.curve.convex.bound_slope_below = true;
  dense.config.curve.convex.node_cap = 48;
  dense.config.curve.convex.lambda = 1.0e-3;
  dense.config.min_quotes = 12;
  dense.config.quality.max_spread_to_mid = 0.30;
  dense.config.quality.max_spread_vol = 0.05;
  dense.config.curve.parametric.max_spread_vol = 0.05;
  QuoteFitProfile sparse = ordinary;
  sparse.name = "sparse";
  sparse.kind = ProfileKind::IlliquidSmallCap;
  sparse.config.min_quotes = 4;
  sparse.config.quality.max_spread_to_mid = 1.50;
  sparse.config.quality.max_spread_vol = 0.25;
  sparse.config.quality.max_weight = 100.0;
  sparse.config.curve.parametric.max_spread_vol = 0.25;
  sparse.config.curve.parametric.max_weight = 100.0;
  sparse.config.curve.parametric.huber_k = 1.0;
  return {std::move(dense), std::move(ordinary), std::move(sparse)};
}

Result<QuoteFitPolicyCatalog>
QuoteFitPolicyCatalog::create(std::span<const QuoteFitProfile> profiles,
                              std::span<const QuoteFitSymbolOverride> overrides,
                              const QuoteFitRegimeConfig &regimes) {
  ATX_TRY_VOID(validate_regimes(regimes));
  QuoteFitPolicyCatalog out;
  out.regimes_ = regimes;
  out.profiles_.reserve(profiles.size());
  for (const auto &profile : profiles) {
    ATX_TRY_VOID(validate_profile(profile));
    auto owned = std::make_shared<OwnedProfile>();
    owned->spline_grid.assign(profile.config.curve.spline.grid.begin(),
                              profile.config.curve.spline.grid.end());
    owned->profile = profile;
    owned->profile.config.curve.spline.grid = owned->spline_grid;
    out.profiles_.emplace_back(owned, &owned->profile);
  }
  std::sort(out.profiles_.begin(), out.profiles_.end(),
            [](const auto &a, const auto &b) { return a->name < b->name; });
  for (std::size_t i = 1u; i < out.profiles_.size(); ++i) {
    if (out.profiles_[i - 1u]->name == out.profiles_[i]->name) {
      return Err(ErrorCode::InvalidArgument, "quote fit policy: duplicate profile name");
    }
  }
  const auto find = [&out](std::string_view name) -> Result<std::size_t> {
    const auto it =
        std::lower_bound(out.profiles_.begin(), out.profiles_.end(), name,
                         [](const auto &p, std::string_view key) { return p->name < key; });
    if (it == out.profiles_.end() || (*it)->name != name) {
      return Err(ErrorCode::NotFound, "quote fit policy: unknown profile " + std::string(name));
    }
    return Ok(static_cast<std::size_t>(it - out.profiles_.begin()));
  };
  ATX_TRY(out.dense_, find(regimes.dense_profile));
  ATX_TRY(out.ordinary_, find(regimes.ordinary_profile));
  ATX_TRY(out.sparse_, find(regimes.sparse_profile));
  for (const auto &entry : overrides) {
    if (entry.symbol.empty()) {
      return Err(ErrorCode::InvalidArgument, "quote fit policy: empty override symbol");
    }
    ATX_TRY(const std::size_t index, find(entry.profile));
    out.overrides_.push_back({entry.symbol, index});
  }
  std::sort(out.overrides_.begin(), out.overrides_.end(),
            [](const auto &a, const auto &b) { return a.symbol < b.symbol; });
  for (std::size_t i = 1u; i < out.overrides_.size(); ++i) {
    if (out.overrides_[i - 1u].symbol == out.overrides_[i].symbol) {
      return Err(ErrorCode::InvalidArgument, "quote fit policy: duplicate symbol override");
    }
  }
  return Ok(std::move(out));
}

Result<ResolvedQuoteFitPolicy>
QuoteFitPolicyCatalog::resolve(std::string_view symbol,
                               std::span<const QuoteFitExpirySnapshot> snapshots) const {
  if (profiles_.empty()) {
    return Err(ErrorCode::InvalidArgument, "quote fit policy: empty or moved-from catalog");
  }
  ATX_TRY(auto features, quote_fit_liquidity_features(snapshots, regimes_.near_money_log_band));
  ATX_TRY(const auto regime, classify_quote_fit_liquidity(features, regimes_));
  std::size_t selected = ordinary_;
  switch (regime) {
  case QuoteFitLiquidityRegime::Sparse:
    selected = sparse_;
    break;
  case QuoteFitLiquidityRegime::Dense:
    selected = dense_;
    break;
  case QuoteFitLiquidityRegime::Ordinary:
    break;
  }
  QuoteFitPolicySource source = QuoteFitPolicySource::RawLiquidity;
  const auto it =
      std::lower_bound(overrides_.begin(), overrides_.end(), symbol,
                       [](const auto &entry, std::string_view key) { return entry.symbol < key; });
  if (it != overrides_.end() && it->symbol == symbol) {
    selected = it->profile;
    source = QuoteFitPolicySource::ExactSymbolOverride;
  }
  return Ok(ResolvedQuoteFitPolicy{profiles_[selected], std::move(features), source});
}

} // namespace atx::vol
