#include "spx_wilmott_repro_support.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "atx/vol/arb.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/data.hpp"
#include "atx/vol/implied_vol.hpp"
#include "atx/vol/opra_panel.hpp"

namespace atx::vol::spx_wilmott {
namespace {

struct PairForward {
  double parity_gap{0.0};
  double strike{0.0};
  double forward{0.0};
};

[[nodiscard]] double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  const std::size_t middle = values.size() / 2u;
  if ((values.size() & 1u) != 0u) {
    return values[middle];
  }
  return 0.5 * (values[middle - 1u] + values[middle]);
}

[[nodiscard]] bool chain_has_pair_axes(const Chain &chain) noexcept {
  if (chain.n_strikes() > 65'536u) {
    return false;
  }
  const std::size_t required = chain.n_strikes() * 2u;
  return chain.bids.size() >= required && chain.asks.size() >= required;
}

[[nodiscard]] std::vector<PairForward> collect_pair_forwards(const Chain &chain, double T,
                                                             double r) {
  std::vector<PairForward> pairs;
  pairs.reserve(chain.n_strikes());
  const double carry = std::exp(r * T);
  for (std::size_t strike_index = 0; strike_index < chain.n_strikes(); ++strike_index) {
    const auto source_index = static_cast<std::uint16_t>(strike_index);
    const std::size_t call_index = chain_index(source_index, Side::Call);
    const std::size_t put_index = chain_index(source_index, Side::Put);
    const double call_bid = chain.bids[call_index];
    const double call_ask = chain.asks[call_index];
    const double put_bid = chain.bids[put_index];
    const double put_ask = chain.asks[put_index];
    if (!(call_bid > 0.0 && call_ask > call_bid && put_bid > 0.0 && put_ask > put_bid)) {
      continue;
    }
    const double call_mid = 0.5 * (call_bid + call_ask);
    const double put_mid = 0.5 * (put_bid + put_ask);
    const double strike = chain.strikes[strike_index];
    const double forward = (call_mid - put_mid) * carry + strike;
    if (std::isfinite(forward) && forward > 0.0) {
      pairs.push_back(PairForward{std::fabs(call_mid - put_mid), strike, forward});
    }
  }
  return pairs;
}

struct IvBand {
  double bid{std::numeric_limits<double>::quiet_NaN()};
  double ask{std::numeric_limits<double>::quiet_NaN()};
  bool valid{false};
};

[[nodiscard]] IvBand quote_iv_band(const FitObs &observation, double T) {
  const double half_spread = 0.5 * observation.spread;
  const double bid_price = observation.mid - half_spread;
  const double ask_price = observation.mid + half_spread;
  if (!(bid_price >= 0.0) || !(ask_price > bid_price)) {
    return {};
  }
  const auto bid_iv =
      implied_vol(bid_price, observation.F, observation.K, T, observation.df, observation.side);
  const auto ask_iv =
      implied_vol(ask_price, observation.F, observation.K, T, observation.df, observation.side);
  if (!bid_iv.has_value() || !ask_iv.has_value()) {
    return {};
  }
  return IvBand{(std::min)(*bid_iv, *ask_iv), (std::max)(*bid_iv, *ask_iv), true};
}

struct ReproductionRow {
  FitObs observation{};
  bool accepted{false};
  bool zero_bid{false};
};

[[nodiscard]] ReproductionRow make_reproduction_row(const Chain &chain, std::uint16_t source_index,
                                                    double F, double T, double df,
                                                    double max_weight) {
  const double strike = chain.strikes[source_index];
  const Side side = strike >= F ? Side::Call : Side::Put;
  const std::size_t quote_index = chain_index(source_index, side);
  const double bid = chain.bids[quote_index];
  const double ask = chain.asks[quote_index];
  if (chain.flags[quote_index] != 0u || !std::isfinite(bid) || !std::isfinite(ask) ||
      !(bid >= 0.0) || !(ask > bid)) {
    return {};
  }
  const double mid = 0.5 * (bid + ask);
  const auto sigma = implied_vol(mid, F, strike, T, df, side);
  if (!sigma.has_value() || !(*sigma > 0.005 && *sigma < 5.0)) {
    return {};
  }
  const auto value_vega = black76_value_and_vega(F, strike, T, *sigma, df, side);
  const double spread = ask - bid;
  if (!(value_vega.vega > 1.0e-12) || !std::isfinite(value_vega.vega)) {
    return {};
  }
  const double weight_sigma = (value_vega.vega / spread) * (value_vega.vega / spread);
  const double variance_jacobian = 2.0 * *sigma * T;
  double weight_w = (std::min)(max_weight, weight_sigma / (variance_jacobian * variance_jacobian));
  if (bid == 0.0) {
    weight_w = (std::max)(1.0e-8, weight_w * kZeroBidWeightScale);
  }
  FitObs observation;
  observation.k = std::log(strike / F);
  observation.sigma_mkt = *sigma;
  observation.w_mkt = *sigma * *sigma * T;
  observation.weight_w = weight_w;
  observation.active_weight_w = weight_w;
  observation.K = strike;
  observation.F = F;
  observation.df = df;
  observation.mid = mid;
  observation.spread = spread;
  observation.vega = value_vega.vega;
  observation.noise_sigma = spread / value_vega.vega;
  observation.side = side;
  observation.source_strike_index = source_index;
  observation.score_sigma_mkt = *sigma;
  return ReproductionRow{observation, true, bid == 0.0};
}

} // namespace

Result<ForwardEstimate> estimate_european_forward(const Chain &chain, double T, double r,
                                                  std::size_t n_atm_pairs) {
  if (!(T > 0.0) || !std::isfinite(r) || n_atm_pairs == 0u) {
    return Err(ErrorCode::InvalidArgument,
               "estimate_european_forward: require T > 0, finite r, and pair budget > 0");
  }
  if (!chain_has_pair_axes(chain)) {
    return Err(ErrorCode::InvalidArgument,
               "estimate_european_forward: chain quote axes are malformed");
  }
  std::vector<PairForward> pairs = collect_pair_forwards(chain, T, r);
  if (pairs.empty()) {
    return Err(ErrorCode::NotFound, "estimate_european_forward: no two-sided co-terminal pairs");
  }
  std::sort(pairs.begin(), pairs.end(), [](const PairForward &lhs, const PairForward &rhs) {
    if (lhs.parity_gap != rhs.parity_gap) {
      return lhs.parity_gap < rhs.parity_gap;
    }
    return lhs.strike < rhs.strike;
  });
  const std::size_t count = (std::min)(n_atm_pairs, pairs.size());
  std::vector<double> selected;
  selected.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    selected.push_back(pairs[index].forward);
  }
  const double forward = median(selected);
  for (double &value : selected) {
    value = std::fabs(value - forward);
  }
  return atx::core::Ok(ForwardEstimate{forward, median(std::move(selected)), count});
}

double interpolate_atm_sigma(std::span<const FitObs> observations, double T) noexcept {
  if (!(T > 0.0)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const FitObs *below = nullptr;
  const FitObs *above = nullptr;
  for (const FitObs &observation : observations) {
    if (!std::isfinite(observation.k) || !(observation.sigma_mkt > 0.0) ||
        !std::isfinite(observation.sigma_mkt)) {
      continue;
    }
    if (observation.k <= 0.0 && (below == nullptr || observation.k > below->k)) {
      below = &observation;
    }
    if (observation.k >= 0.0 && (above == nullptr || observation.k < above->k)) {
      above = &observation;
    }
  }
  if (below != nullptr && above != nullptr) {
    if (below == above || below->k == above->k) {
      return below->sigma_mkt;
    }
    const double upper_weight = -below->k / (above->k - below->k);
    return below->sigma_mkt + upper_weight * (above->sigma_mkt - below->sigma_mkt);
  }
  const FitObs *nearest = below != nullptr ? below : above;
  return nearest != nullptr ? nearest->sigma_mkt : std::numeric_limits<double>::quiet_NaN();
}

Result<ReproductionObsSet> build_figure_reproduction_observations(const Chain &chain, double F,
                                                                  double T, double df,
                                                                  double sigma0,
                                                                  double max_weight) {
  if (!(F > 0.0) || !(T > 0.0) || !(df > 0.0) || !(sigma0 > 0.0) || !std::isfinite(max_weight) ||
      !(max_weight > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "build_figure_reproduction_observations: invalid model inputs");
  }
  if (chain.n_strikes() > 65'536u) {
    return Err(ErrorCode::InvalidArgument,
               "build_figure_reproduction_observations: malformed chain axes");
  }
  const std::size_t required = chain.n_strikes() * 2u;
  if (chain.bids.size() < required || chain.asks.size() < required ||
      chain.flags.size() < required) {
    return Err(ErrorCode::InvalidArgument,
               "build_figure_reproduction_observations: malformed chain axes");
  }
  ReproductionObsSet out;
  out.obs.reserve(chain.n_strikes());
  const double normalization = sigma0 * std::sqrt(T);
  for (std::size_t strike_index = 0; strike_index < chain.n_strikes(); ++strike_index) {
    const double strike = chain.strikes[strike_index];
    if (!(strike > 0.0) || !std::isfinite(strike)) {
      ++out.n_rejected;
      continue;
    }
    const double k_log = std::log(strike / F);
    const double z = k_log / normalization;
    if (z < kFigureMarketZMin || z > kFigureMarketZMax) {
      ++out.n_outside_domain;
      continue;
    }
    const auto source_index = static_cast<std::uint16_t>(strike_index);
    ReproductionRow row = make_reproduction_row(chain, source_index, F, T, df, max_weight);
    if (!row.accepted) {
      ++out.n_rejected;
      continue;
    }
    if (row.zero_bid) {
      ++out.n_zero_bid;
    }
    out.obs.push_back(row.observation);
    if (out.obs.size() == 1u) {
      out.z_min = z;
      out.z_max = z;
    } else {
      out.z_min = (std::min)(out.z_min, z);
      out.z_max = (std::max)(out.z_max, z);
    }
  }
  return atx::core::Ok(std::move(out));
}

PriceConeScore check_convex_price_cone(const ConvexSliceFit &fit, double tolerance) noexcept {
  PriceConeScore score;
  if (!(fit.F > 0.0) || !(fit.T > 0.0) || !(fit.df > 0.0) || !std::isfinite(tolerance) ||
      tolerance < 0.0 || fit.u.size() < 2u || fit.C.size() != fit.u.size()) {
    score.price_bound_violations = 1u;
    return score;
  }
  double previous_slope = 0.0;
  bool have_previous_slope = false;
  for (std::size_t index = 0; index < fit.u.size(); ++index) {
    const double strike = fit.u[index];
    const double price = fit.C[index];
    const double lower = fit.df * (std::max)(fit.F - strike, 0.0);
    const double upper = fit.df * fit.F;
    if (!(strike > 0.0) || !std::isfinite(price) || price < lower - tolerance ||
        price > upper + tolerance) {
      ++score.price_bound_violations;
    }
    if (index == 0u) {
      continue;
    }
    const double strike_width = strike - fit.u[index - 1u];
    if (!(strike_width > 0.0)) {
      ++score.convexity_violations;
      continue;
    }
    const double slope = (price - fit.C[index - 1u]) / strike_width;
    if (!std::isfinite(slope) || slope > tolerance || slope < -fit.df - tolerance) {
      ++score.monotonicity_violations;
    }
    if (have_previous_slope && previous_slope - slope > tolerance) {
      ++score.convexity_violations;
      score.max_slope_decrease = (std::max)(score.max_slope_decrease, previous_slope - slope);
    }
    previous_slope = slope;
    have_previous_slope = true;
  }
  return score;
}

FitScore score_curve(std::span<const FitObs> observations, const IVolCurve &curve) {
  FitScore score;
  double squared_error = 0.0;
  for (const FitObs &observation : observations) {
    const double fitted_iv = curve.iv(observation.k);
    if (!std::isfinite(fitted_iv) || !std::isfinite(observation.sigma_mkt)) {
      continue;
    }
    const double error = fitted_iv - observation.sigma_mkt;
    squared_error += error * error;
    score.max_abs_iv_error = (std::max)(score.max_abs_iv_error, std::fabs(error));
    ++score.n_scored;
    const IvBand band = quote_iv_band(observation, curve.T());
    if (!band.valid) {
      continue;
    }
    ++score.n_band_scored;
    if (fitted_iv >= band.bid && fitted_iv <= band.ask) {
      ++score.n_in_band;
    }
  }
  if (score.n_scored > 0u) {
    score.rmse_iv = std::sqrt(squared_error / static_cast<double>(score.n_scored));
  }
  if (score.n_band_scored > 0u) {
    score.in_band_percent =
        100.0 * static_cast<double>(score.n_in_band) / static_cast<double>(score.n_band_scored);
  }
  return score;
}

} // namespace atx::vol::spx_wilmott

#ifndef ATX_SPX_WILMOTT_REPRO_NO_MAIN
namespace {

using atx::vol::arb_check_butterfly;
using atx::vol::build_observations;
using atx::vol::CalibOpts;
using atx::vol::Chain;
using atx::vol::CurveConfig;
using atx::vol::data_install;
using atx::vol::ExerciseStyle;
using atx::vol::ExpiryCloseConvention;
using atx::vol::fit_slice_curve;
using atx::vol::FitObs;
using atx::vol::implied_vol;
using atx::vol::IVolCurve;
using atx::vol::load_opra_cbbo_parquet;
using atx::vol::ns_to_iso_date;
using atx::vol::OpraLoadSpec;
using atx::vol::Side;
using atx::vol::to_string;
using atx::vol::Universe;
using atx::vol::VolCurveKind;
using atx::vol::spx_wilmott::build_figure_reproduction_observations;
using atx::vol::spx_wilmott::check_convex_price_cone;
using atx::vol::spx_wilmott::estimate_european_forward;
using atx::vol::spx_wilmott::FitScore;
using atx::vol::spx_wilmott::ForwardEstimate;
using atx::vol::spx_wilmott::interpolate_atm_sigma;
using atx::vol::spx_wilmott::kFigureCurveZMax;
using atx::vol::spx_wilmott::kFigureCurveZMin;
using atx::vol::spx_wilmott::PriceConeScore;
using atx::vol::spx_wilmott::ReproductionObsSet;
using atx::vol::spx_wilmott::score_curve;

constexpr std::string_view kSnapshot = "2019-08-26T19:30:00Z";
constexpr std::string_view kExpiry = "2019-09-20";
constexpr double kRate = 0.0209;
constexpr std::string_view kDefaultInput = "C:/atx-data/spx-wilmott-2019-08-26/fit_slice/"
                                           "SPX_2019-08-26T1930Z_2019-09-20.parquet";

struct CliOptions {
  std::string input{std::string(kDefaultInput)};
  std::string output{};
  double spot_override{0.0};
};

struct FitAttempt {
  VolCurveKind kind{VolCurveKind::Essvi};
  std::unique_ptr<IVolCurve> curve{};
  FitScore visual_score{};
  FitScore strict_score{};
  std::size_t butterfly_observed{0};
  std::size_t butterfly_figure{0};
  std::size_t butterfly_observed_nonfinite{0};
  std::size_t butterfly_figure_nonfinite{0};
  PriceConeScore price_cone{};
  bool price_cone_applicable{false};
  std::int64_t fit_us{0};
  std::string error{};
  std::string arb_error{};
};

[[nodiscard]] bool parse_positive_double(std::string_view text, double &value) noexcept {
  const char *const begin = text.data();
  const char *const end = begin + text.size();
  const std::from_chars_result parsed = std::from_chars(begin, end, value);
  return parsed.ec == std::errc{} && parsed.ptr == end && std::isfinite(value) && value > 0.0;
}

[[nodiscard]] bool parse_cli(int argc, char **argv, CliOptions &options) {
  std::size_t positional = 0u;
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--spot") {
      if (++index >= argc || !parse_positive_double(argv[index], options.spot_override)) {
        return false;
      }
      continue;
    }
    if (arg == "--help" || arg == "-h" || (!arg.empty() && arg.front() == '-')) {
      return false;
    }
    if (positional == 0u) {
      options.input = std::string(arg);
    } else if (positional == 1u) {
      options.output = std::string(arg);
    } else {
      return false;
    }
    ++positional;
  }
  return true;
}

[[nodiscard]] std::vector<FitAttempt> fit_families(std::span<const FitObs> visual_observations,
                                                   std::span<const FitObs> strict_observations,
                                                   double F, double T, double df, double sigma0,
                                                   double observed_z_min, double observed_z_max) {
  constexpr std::array<VolCurveKind, 5> kKinds{VolCurveKind::Essvi, VolCurveKind::Svi,
                                               VolCurveKind::C8, VolCurveKind::ConvexDense,
                                               VolCurveKind::LinearVariance};
  std::vector<FitAttempt> attempts;
  attempts.reserve(kKinds.size());
  for (const VolCurveKind kind : kKinds) {
    CurveConfig config;
    config.kind = kind;
    const auto start = std::chrono::steady_clock::now();
    auto fitted = fit_slice_curve(config, visual_observations, F, T, df);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    FitAttempt attempt;
    attempt.kind = kind;
    attempt.fit_us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    if (fitted.has_value()) {
      attempt.curve = std::move(*fitted);
      attempt.visual_score = score_curve(visual_observations, *attempt.curve);
      attempt.strict_score = score_curve(strict_observations, *attempt.curve);
      const double normalization = sigma0 * std::sqrt(T);
      const auto observed_arb = arb_check_butterfly(*attempt.curve, observed_z_min * normalization,
                                                    observed_z_max * normalization, 512u);
      const auto figure_arb = arb_check_butterfly(*attempt.curve, kFigureCurveZMin * normalization,
                                                  kFigureCurveZMax * normalization, 512u);
      if (observed_arb.has_value() && figure_arb.has_value()) {
        attempt.butterfly_observed = observed_arb->size();
        attempt.butterfly_figure = figure_arb->size();
        attempt.butterfly_observed_nonfinite = static_cast<std::size_t>(
            std::count_if(observed_arb->begin(), observed_arb->end(),
                          [](const auto &violation) { return !std::isfinite(violation.slack); }));
        attempt.butterfly_figure_nonfinite = static_cast<std::size_t>(
            std::count_if(figure_arb->begin(), figure_arb->end(),
                          [](const auto &violation) { return !std::isfinite(violation.slack); }));
      } else {
        attempt.arb_error = observed_arb.has_value() ? figure_arb.error().message()
                                                     : observed_arb.error().message();
      }
      if (attempt.curve->kind() == VolCurveKind::ConvexDense) {
        const auto *const convex =
            dynamic_cast<const atx::vol::ConvexDenseCurve *>(attempt.curve.get());
        if (convex != nullptr) {
          attempt.price_cone = check_convex_price_cone(convex->fit());
          attempt.price_cone_applicable = true;
        }
      }
    } else {
      attempt.error = fitted.error().message();
    }
    attempts.push_back(std::move(attempt));
  }
  return attempts;
}

void write_meta(std::ostream &out, const ForwardEstimate &forward, double T, double sigma0,
                std::size_t strict_count, const ReproductionObsSet &visual) {
  out << std::fixed << std::setprecision(10);
  out << "#META snapshot=" << kSnapshot << ",expiry=" << kExpiry << ",exercise=European"
      << ",settlement=AM,r=" << kRate << ",T=" << T << ",F=" << forward.forward
      << ",pcp_pairs=" << forward.n_pairs << ",pcp_mad=" << forward.mad << ",sigma0=" << sigma0
      << ",recommended_family=convex-dense,observations=" << visual.obs.size()
      << ",strict_observations=" << strict_count << ",visual_observations=" << visual.obs.size()
      << ",visual_zero_bid=" << visual.n_zero_bid << ",visual_rejected=" << visual.n_rejected
      << ",visual_outside_domain=" << visual.n_outside_domain << ",visual_z_min=" << visual.z_min
      << ",visual_z_max=" << visual.z_max << '\n';
}

void write_metrics(std::ostream &out, const std::vector<FitAttempt> &attempts) {
  out << "#SUMMARY family,visual_rmse_iv,visual_max_abs_iv,visual_in_band_pct,"
         "visual_n_in,visual_n_band,strict_rmse_iv,strict_max_abs_iv,strict_in_band_pct,"
         "strict_n_in,strict_n_band,butterfly_observed,butterfly_figure,"
         "butterfly_observed_nonfinite,butterfly_figure_nonfinite,price_cone_applicable,price_"
         "bounds,"
         "price_monotonicity,price_convexity,max_slope_decrease\n";
  for (const FitAttempt &attempt : attempts) {
    if (attempt.curve == nullptr) {
      continue;
    }
    out << to_string(attempt.kind) << ',' << attempt.visual_score.rmse_iv << ','
        << attempt.visual_score.max_abs_iv_error << ',' << attempt.visual_score.in_band_percent
        << ',' << attempt.visual_score.n_in_band << ',' << attempt.visual_score.n_band_scored << ','
        << attempt.strict_score.rmse_iv << ',' << attempt.strict_score.max_abs_iv_error << ','
        << attempt.strict_score.in_band_percent << ',' << attempt.strict_score.n_in_band << ','
        << attempt.strict_score.n_band_scored << ',' << attempt.butterfly_observed << ','
        << attempt.butterfly_figure << ',' << attempt.butterfly_observed_nonfinite << ','
        << attempt.butterfly_figure_nonfinite << ',' << (attempt.price_cone_applicable ? 1 : 0)
        << ',' << attempt.price_cone.price_bound_violations << ','
        << attempt.price_cone.monotonicity_violations << ','
        << attempt.price_cone.convexity_violations << ',' << attempt.price_cone.max_slope_decrease
        << '\n';
  }
}

void write_curve_rows(std::ostream &out, const std::vector<FitAttempt> &attempts, double sigma0,
                      double T) {
  out << "#CURVE family,z,fitted_iv\n";
  constexpr int kFirstHundredth = static_cast<int>(kFigureCurveZMin * 100.0);
  constexpr int kLastHundredth = static_cast<int>(kFigureCurveZMax * 100.0);
  constexpr int kStepHundredth = 2;
  const double normalization = sigma0 * std::sqrt(T);
  for (const FitAttempt &attempt : attempts) {
    if (attempt.curve == nullptr) {
      continue;
    }
    for (int z_hundredths = kFirstHundredth; z_hundredths <= kLastHundredth;
         z_hundredths += kStepHundredth) {
      const double z = static_cast<double>(z_hundredths) / 100.0;
      const double fitted_iv = attempt.curve->iv(z * normalization);
      if (std::isfinite(fitted_iv)) {
        out << to_string(attempt.kind) << ',' << z << ',' << fitted_iv << '\n';
      }
    }
  }
}

void write_quote_rows(std::ostream &out, std::span<const FitObs> observations,
                      const std::vector<FitAttempt> &attempts, double sigma0, double T) {
  out << "#QUOTES family,z,strike,side,market_iv,bid_iv,ask_iv,fitted_iv,residual_iv,in_band\n";
  const double normalization = sigma0 * std::sqrt(T);
  for (const FitAttempt &attempt : attempts) {
    if (attempt.curve == nullptr) {
      continue;
    }
    for (const FitObs &observation : observations) {
      const double fitted_iv = attempt.curve->iv(observation.k);
      const auto bid_iv = implied_vol(observation.mid - 0.5 * observation.spread, observation.F,
                                      observation.K, T, observation.df, observation.side);
      const auto ask_iv = implied_vol(observation.mid + 0.5 * observation.spread, observation.F,
                                      observation.K, T, observation.df, observation.side);
      if (!std::isfinite(fitted_iv) || !bid_iv.has_value() || !ask_iv.has_value()) {
        continue;
      }
      const double lower = (std::min)(*bid_iv, *ask_iv);
      const double upper = (std::max)(*bid_iv, *ask_iv);
      out << to_string(attempt.kind) << ',' << observation.k / normalization << ',' << observation.K
          << ',' << (observation.side == Side::Call ? 'C' : 'P') << ',' << observation.sigma_mkt
          << ',' << lower << ',' << upper << ',' << fitted_iv << ','
          << fitted_iv - observation.sigma_mkt << ','
          << (fitted_iv >= lower && fitted_iv <= upper ? 1 : 0) << '\n';
    }
  }
}

void report_attempts(const std::vector<FitAttempt> &attempts) {
  for (const FitAttempt &attempt : attempts) {
    if (attempt.curve == nullptr) {
      std::fprintf(stderr, "%s: fit failed after %lld us: %s\n", to_string(attempt.kind),
                   static_cast<long long>(attempt.fit_us), attempt.error.c_str());
      continue;
    }
    std::fprintf(
        stderr,
        "%s: fit_us=%lld visual_rmse=%.8f visual_in_band=%.2f%% (%zu/%zu) "
        "strict_rmse=%.8f strict_in_band=%.2f%% (%zu/%zu) "
        "butterfly=%zu[%zu-nf]/%zu[%zu-nf] price_cone=%s%zu/%zu/%zu%s%s\n",
        to_string(attempt.kind), static_cast<long long>(attempt.fit_us),
        attempt.visual_score.rmse_iv, attempt.visual_score.in_band_percent,
        attempt.visual_score.n_in_band, attempt.visual_score.n_band_scored,
        attempt.strict_score.rmse_iv, attempt.strict_score.in_band_percent,
        attempt.strict_score.n_in_band, attempt.strict_score.n_band_scored,
        attempt.butterfly_observed, attempt.butterfly_observed_nonfinite, attempt.butterfly_figure,
        attempt.butterfly_figure_nonfinite,
        attempt.price_cone_applicable ? "" : "n/a:", attempt.price_cone.price_bound_violations,
        attempt.price_cone.monotonicity_violations, attempt.price_cone.convexity_violations,
        attempt.arb_error.empty() ? "" : " arb_error=",
        attempt.arb_error.empty() ? "" : attempt.arb_error.c_str());
  }
}

[[nodiscard]] const Chain *find_target_chain(const Universe &universe, atx::vol::Uid uid) {
  const auto underlying = universe.get_underlying(uid);
  if (!underlying.has_value()) {
    return nullptr;
  }
  for (const Chain &chain : (*underlying)->chains) {
    if (ns_to_iso_date(chain.expiry_ns) == kExpiry) {
      return &chain;
    }
  }
  return nullptr;
}

} // namespace

int main(int argc, char **argv) {
  CliOptions options;
  if (!parse_cli(argc, argv, options)) {
    std::fprintf(stderr, "usage: spx_wilmott_repro [input.parquet] [output.csv] [--spot S]\n");
    return 2;
  }
  OpraLoadSpec spec;
  spec.path = options.input;
  spec.underlying = "SPX";
  spec.snapshot_iso = std::string(kSnapshot);
  spec.r = kRate;
  spec.spot_override = options.spot_override;
  spec.expiry_close = ExpiryCloseConvention::UsIndexAmOpen;
  spec.exercise_style = ExerciseStyle::European;
  auto panel = load_opra_cbbo_parquet(spec);
  if (!panel.has_value()) {
    std::fprintf(stderr, "load failed: %s\n", panel.error().message().c_str());
    return 1;
  }
  Universe universe;
  const auto uid = data_install(universe, panel->frame);
  if (!uid.has_value()) {
    std::fprintf(stderr, "install failed: %s\n", uid.error().message().c_str());
    return 1;
  }
  const Chain *const chain = find_target_chain(universe, *uid);
  if (chain == nullptr || chain->exercise_style != ExerciseStyle::European) {
    std::fprintf(stderr, "European target expiry %s not found\n", kExpiry.data());
    return 1;
  }
  const double T = chain->T;
  const double df = std::exp(-kRate * T);
  const auto forward = estimate_european_forward(*chain, T, kRate);
  if (!forward.has_value()) {
    std::fprintf(stderr, "forward failed: %s\n", forward.error().message().c_str());
    return 1;
  }
  const auto observations = build_observations(*chain, forward->forward, T, df, CalibOpts{});
  if (!observations.has_value()) {
    std::fprintf(stderr, "observation build failed: %s\n", observations.error().message().c_str());
    return 1;
  }
  const double sigma0 = interpolate_atm_sigma(observations->obs, T);
  if (!(sigma0 > 0.0) || !std::isfinite(sigma0)) {
    std::fprintf(stderr, "ATM normalization failed\n");
    return 1;
  }
  const auto visual =
      build_figure_reproduction_observations(*chain, forward->forward, T, df, sigma0);
  if (!visual.has_value() || visual->obs.size() < 5u) {
    std::fprintf(stderr, "visual observation build failed%s%s\n", visual.has_value() ? "" : ": ",
                 visual.has_value() ? "" : visual.error().message().c_str());
    return 1;
  }
  const std::vector<FitAttempt> attempts =
      fit_families(visual->obs, observations->obs, forward->forward, T, df, sigma0, visual->z_min,
                   visual->z_max);
  report_attempts(attempts);

  std::ofstream file;
  std::ostream *output = &std::cout;
  if (!options.output.empty()) {
    file.open(options.output, std::ios::out | std::ios::trunc);
    if (!file.is_open()) {
      std::fprintf(stderr, "cannot open output: %s\n", options.output.c_str());
      return 1;
    }
    output = &file;
  }
  write_meta(*output, *forward, T, sigma0, observations->obs.size(), *visual);
  write_metrics(*output, attempts);
  write_curve_rows(*output, attempts, sigma0, T);
  write_quote_rows(*output, visual->obs, attempts, sigma0, T);
  if (!output->good()) {
    std::fprintf(stderr, "CSV write failed\n");
    return 1;
  }
  return std::any_of(attempts.begin(), attempts.end(),
                     [](const FitAttempt &attempt) { return attempt.curve != nullptr; })
             ? 0
             : 1;
}
#endif
