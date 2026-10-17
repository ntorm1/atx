#include "atx/vol/api/fitting/american_markov_policy.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "atx/vol/api/pricing/american_markov_batch.hpp"
#include "fitting/american_markov_fit_controls.hpp"
#include "fitting/american_markov_time_basis.hpp"

namespace atx::vol {
namespace {
using atx::core::Err;
using atx::core::Ok;

bool positive(double x) noexcept { return std::isfinite(x) && x > 0.0; }
bool nonnegative(double x) noexcept { return std::isfinite(x) && x >= 0.0; }

// The validators shared with the fitter speak as "Markov fit:"; a catalog
// refusal keeps the "Markov policy:" prefix of every other profile check.
// Code and message body are unchanged.
[[nodiscard]] Status as_policy_refusal(Status status) {
  constexpr std::string_view fit_prefix = "Markov fit: ";
  if (status || !status.error().message().starts_with(fit_prefix)) return status;
  return Err(status.error().code(),
             "Markov policy: " + status.error().message().substr(fit_prefix.size()));
}

bool valid_name(std::string_view name) noexcept {
  return !name.empty() && name.size() <= kAmericanMarkovPolicyMaxNameBytes &&
         std::none_of(name.begin(), name.end(), [](char character) {
           const auto byte = static_cast<unsigned char>(character);
           return byte <= 0x20U || byte == 0x7fU;
         });
}

Status validate_model(const AmericanMarkovPolicyProfile &profile) {
  const auto &m = profile.model_config;
  if (m.max_states < 3 || m.max_states > kAmericanMarkovMaxStates ||
      m.grid_nodes < 3 || m.grid_nodes > m.max_states || !positive(m.upper_spot_multiple) ||
      m.upper_spot_multiple <= 1.0 || !positive(m.max_calendar_step) ||
      !positive(m.max_variance_step) || !positive(m.cfl_fraction) || m.cfl_fraction > 1.0 ||
      m.max_transitions == 0 || m.max_transitions > kAmericanMarkovMaxTransitions ||
      m.max_stored_rows == 0 || m.max_stored_rows > kAmericanMarkovMaxStoredRows ||
      m.max_transition_work == 0 || m.max_transition_work > kAmericanMarkovMaxTransitionWork ||
      (m.diffusion_method != AmericanMarkovDiffusionMethod::ExplicitEuler &&
       m.diffusion_method != AmericanMarkovDiffusionMethod::VarianceResolvent) ||
      m.max_stored_volatility_values == 0 ||
      m.max_stored_volatility_values > kAmericanMarkovMaxStoredVolatilityValues) {
    return Err(ErrorCode::InvalidArgument, "Markov policy: invalid model resource configuration");
  }
  ATX_TRY(const auto grid, make_american_markov_sinh_grid(1.0, profile.grid_config));
  if (grid.states.size() > m.max_states || grid.states.size() > m.max_stored_rows ||
      grid.states.size() > m.max_transition_work /
          (m.diffusion_method == AmericanMarkovDiffusionMethod::VarianceResolvent ? 2U : 1U) ||
      grid.states.size() >= m.max_stored_volatility_values) {
    return Err(ErrorCode::OutOfRange, "Markov policy: grid exceeds model storage capacity");
  }
  return Ok();
}

Status validate_fit(const AmericanMarkovPolicyProfile &profile) {
  const auto &c = profile.fit_config;
  if (c.max_parameters == 0 || c.max_parameters > kAmericanMarkovFitMaxParameters ||
      c.max_quotes == 0 || c.max_quotes > kAmericanMarkovFitMaxQuotes ||
      c.max_evaluations == 0 || c.max_evaluations > kAmericanMarkovFitMaxEvaluations ||
      c.max_sweeps == 0 || c.max_sweeps > kAmericanMarkovFitMaxSweeps ||
      c.batch_options == 0 || c.batch_options > kAmericanMarkovBatchMaxOptions ||
      c.max_batch_values == 0 || c.max_batch_values > kAmericanMarkovBatchMaxValues ||
      c.max_rollback_work == 0 || c.max_rollback_work > kAmericanMarkovFitMaxRollbackWork ||
      (c.time_shape != AmericanMarkovFitTimeShape::Shared &&
       c.time_shape != AmericanMarkovFitTimeShape::PerVarianceInterval &&
       c.time_shape != AmericanMarkovFitTimeShape::VarianceTimeKnots)) {
    return Err(ErrorCode::InvalidArgument, "Markov policy: invalid fit resource configuration");
  }
  if (!positive(c.min_sigma) || !positive(c.max_sigma) || c.min_sigma >= c.max_sigma ||
      c.min_sigma < 1.0e-6 || c.max_sigma > 100.0 || !positive(c.initial_sigma) ||
      c.initial_sigma < c.min_sigma || c.initial_sigma > c.max_sigma ||
      !positive(c.price_scale_floor) || c.price_scale_floor < 1.0e-12 ||
      !positive(c.initial_log_step) || c.initial_log_step > 10.0 ||
      !positive(c.min_log_step) || c.min_log_step > c.initial_log_step) {
    return Err(ErrorCode::InvalidArgument, "Markov policy: invalid sigma, scale or search bounds");
  }
  for (const double weight : {c.midpoint_weight, c.prior_weight, c.strike_roughness_weight,
                              c.time_roughness_weight}) {
    if (!nonnegative(weight) || weight > 1.0e6) {
      return Err(ErrorCode::InvalidArgument, "Markov policy: invalid objective weight");
    }
  }
  if (c.log_state_knots.size() > c.max_parameters ||
      c.log_state_knots.size() > kAmericanMarkovMaxLocalVolNodes ||
      c.initial_sigmas.size() > c.max_parameters || c.prior_sigmas.size() > c.max_parameters) {
    return Err(ErrorCode::OutOfRange, "Markov policy: fit vector exceeds parameter capacity");
  }
  double previous = -std::numeric_limits<double>::infinity();
  for (const double knot : c.log_state_knots) {
    if (!std::isfinite(knot) || std::abs(knot) > kAmericanMarkovMaxLocalVolLogState ||
        !(knot > previous)) {
      return Err(ErrorCode::InvalidArgument, "Markov policy: invalid local volatility knots");
    }
    previous = knot;
  }
  for (const auto values : {c.initial_sigmas, c.prior_sigmas}) {
    for (const double sigma : values) {
      if (!positive(sigma) || sigma < c.min_sigma || sigma > c.max_sigma) {
        return Err(ErrorCode::InvalidArgument, "Markov policy: invalid initial or prior sigmas");
      }
    }
  }
  ATX_TRY_VOID(as_policy_refusal(detail::validate_markov_fit_search(c)));
  ATX_TRY_VOID(as_policy_refusal(detail::validate_markov_time_knots(c)));
  const auto knots = std::max(std::size_t{1}, c.log_state_knots.size());
  const auto fixed_groups = c.time_shape == AmericanMarkovFitTimeShape::Shared ? 1U :
      c.time_shape == AmericanMarkovFitTimeShape::VarianceTimeKnots
          ? c.variance_time_knots.size() : 0U;
  const auto fixed_count = fixed_groups * knots;
  if ((fixed_groups != 0 &&
       ((!c.initial_sigmas.empty() && c.initial_sigmas.size() != fixed_count) ||
        (!c.prior_sigmas.empty() && c.prior_sigmas.size() != fixed_count))) ||
      (!c.initial_sigmas.empty() && c.initial_sigmas.size() % knots != 0) ||
      (!c.prior_sigmas.empty() && c.prior_sigmas.size() % knots != 0) ||
      (!c.initial_sigmas.empty() && !c.prior_sigmas.empty() &&
       c.initial_sigmas.size() != c.prior_sigmas.size())) {
    return Err(ErrorCode::InvalidArgument, "Markov policy: inconsistent sigma vector layout");
  }
  const auto states = profile.grid_config.grid_nodes;
  const auto batch = std::min(c.batch_options, c.max_quotes);
  const auto minimum_action_work = states *
      (profile.model_config.diffusion_method == AmericanMarkovDiffusionMethod::VarianceResolvent
           ? 2U : 1U);
  if (batch > c.max_batch_values / states || c.max_rollback_work < minimum_action_work ||
      knots > profile.model_config.max_stored_volatility_values - states) {
    return Err(ErrorCode::OutOfRange, "Markov policy: inconsistent grid and fitting capacities");
  }
  return Ok();
}

struct OwnedProfile {
  std::vector<double> knots;
  std::vector<double> time_knots;
  std::vector<double> initial;
  std::vector<double> prior;
  AmericanMarkovPolicyProfile profile;
};

Result<std::size_t>
active_variance_intervals(std::span<const AmericanMarkovFitInterval> intervals) {
  if (intervals.empty() || intervals.size() > kAmericanMarkovMaxTransitions) {
    return Err(ErrorCode::InvalidArgument, "Markov policy: invalid interval count");
  }
  double calendar = 0.0;
  double variance = 0.0;
  std::size_t active = 0;
  for (const auto &interval : intervals) {
    if (!positive(interval.calendar_end) || interval.calendar_end <= calendar ||
        !nonnegative(interval.variance_end) || interval.variance_end < variance ||
        !std::isfinite(interval.rate) || !std::isfinite(interval.continuous_yield) ||
        !nonnegative(interval.cash_dividend_at_end)) {
      return Err(ErrorCode::InvalidArgument, "Markov policy: invalid common interval schedule");
    }
    active += interval.variance_end > variance ? 1U : 0U;
    calendar = interval.calendar_end;
    variance = interval.variance_end;
  }
  if (active == 0) {
    return Err(ErrorCode::OutOfRange, "Markov policy: all variance intervals are unidentifiable");
  }
  return Ok(active);
}

Status validate_snapshots(std::span<const QuoteFitExpirySnapshot> snapshots,
                           std::span<const AmericanMarkovFitInterval> intervals) {
  if (snapshots.empty() || snapshots.size() > kAmericanMarkovPolicyMaxSnapshots) {
    return Err(ErrorCode::OutOfRange, "Markov policy: raw snapshot count outside policy capacity");
  }
  const double spot = snapshots.front().market.spot;
  std::size_t rows = 0;
  for (const auto &snapshot : snapshots) {
    if (snapshot.market.spot != spot || !positive(spot)) {
      return Err(ErrorCode::InvalidArgument, "Markov policy: inconsistent common spot");
    }
    if (snapshot.quotes.size() > kAmericanMarkovPolicyMaxRawQuotes - rows) {
      return Err(ErrorCode::OutOfRange, "Markov policy: raw quote count exceeds policy capacity");
    }
    rows += snapshot.quotes.size();
    const auto expiry = std::lower_bound(
        intervals.begin(), intervals.end(), snapshot.market.calendar_years,
        [](const auto &interval, double time) { return interval.calendar_end < time; });
    if (expiry == intervals.end() || expiry->calendar_end != snapshot.market.calendar_years ||
        expiry->variance_end != snapshot.market.variance_years) {
      return Err(ErrorCode::InvalidArgument,
                 "Markov policy: raw expiry does not match common clocks");
    }
  }
  return Ok();
}
} // namespace

std::vector<AmericanMarkovPolicyProfile> default_american_markov_policy_profiles() {
  static constexpr std::array ordinary_knots{-0.25, 0.0, 0.25};
  static constexpr std::array dense_knots{-0.5, -0.2, 0.0, 0.2, 0.5};
  AmericanMarkovPolicyProfile sparse;
  sparse.name = "sparse";
  sparse.grid_config = {65, 4.0, 0.20, 1025};
  sparse.fit_config.prior_weight = 0.10;
  sparse.fit_config.strike_roughness_weight = 0.10;
  sparse.fit_config.time_roughness_weight = 0.10;
  sparse.fit_config.max_parameters = 8;
  sparse.fit_config.max_evaluations = 128;
  sparse.fit_config.max_sweeps = 24;

  AmericanMarkovPolicyProfile ordinary;
  ordinary.name = "ordinary";
  ordinary.grid_config = {129, 4.0, 0.10, 1025};
  ordinary.fit_config.log_state_knots = ordinary_knots;
  ordinary.fit_config.prior_weight = 0.01;
  ordinary.fit_config.strike_roughness_weight = 0.05;
  ordinary.fit_config.time_roughness_weight = 0.05;

  AmericanMarkovPolicyProfile dense;
  dense.name = "dense";
  dense.grid_config = {129, 4.0, 0.10, 1025};
  dense.fit_config.log_state_knots = dense_knots;
  dense.fit_config.time_shape = AmericanMarkovFitTimeShape::PerVarianceInterval;
  dense.fit_config.prior_weight = 0.001;
  dense.fit_config.strike_roughness_weight = 0.01;
  dense.fit_config.time_roughness_weight = 0.02;
  dense.fit_config.max_parameters = 64;
  dense.fit_config.max_quotes = 8192;
  dense.fit_config.max_evaluations = 512;
  dense.fit_config.max_sweeps = 48;
  dense.fit_config.max_rollback_work = 100000000;
  return {std::move(dense), std::move(ordinary), std::move(sparse)};
}

Result<AmericanMarkovPolicyCatalog>
AmericanMarkovPolicyCatalog::create(std::span<const AmericanMarkovPolicyProfile> profiles,
                                     std::span<const QuoteFitSymbolOverride> overrides,
                                     const QuoteFitRegimeConfig &regimes) {
  if (profiles.empty() || profiles.size() > kAmericanMarkovPolicyMaxProfiles ||
      overrides.size() > kAmericanMarkovPolicyMaxOverrides) {
    return Err(ErrorCode::OutOfRange, "Markov policy: catalog count outside policy capacity");
  }
  if (!valid_name(regimes.dense_profile) || !valid_name(regimes.ordinary_profile) ||
      !valid_name(regimes.sparse_profile)) {
    return Err(ErrorCode::InvalidArgument, "Markov policy: invalid regime profile name");
  }
  ATX_TRY(const auto unused_regime, classify_quote_fit_liquidity({}, regimes));
  static_cast<void>(unused_regime);
  AmericanMarkovPolicyCatalog out;
  out.regimes_ = regimes;
  out.profiles_.reserve(profiles.size());
  for (const auto &profile : profiles) {
    if (!valid_name(profile.name)) {
      return Err(ErrorCode::InvalidArgument, "Markov policy: invalid profile name");
    }
    ATX_TRY_VOID(validate_model(profile));
    ATX_TRY_VOID(validate_fit(profile));
    auto owned = std::make_shared<OwnedProfile>();
    const auto &config = profile.fit_config;
    owned->knots.assign(config.log_state_knots.begin(), config.log_state_knots.end());
    owned->time_knots.assign(config.variance_time_knots.begin(), config.variance_time_knots.end());
    owned->initial.assign(config.initial_sigmas.begin(), config.initial_sigmas.end());
    owned->prior.assign(config.prior_sigmas.begin(), config.prior_sigmas.end());
    owned->profile = profile;
    owned->profile.fit_config.log_state_knots = owned->knots;
    owned->profile.fit_config.variance_time_knots = owned->time_knots;
    owned->profile.fit_config.initial_sigmas = owned->initial;
    owned->profile.fit_config.prior_sigmas = owned->prior;
    out.profiles_.emplace_back(owned, &owned->profile);
  }
  std::sort(out.profiles_.begin(), out.profiles_.end(),
            [](const auto &a, const auto &b) { return a->name < b->name; });
  for (std::size_t i = 1; i < out.profiles_.size(); ++i) {
    if (out.profiles_[i - 1]->name == out.profiles_[i]->name) {
      return Err(ErrorCode::InvalidArgument, "Markov policy: duplicate profile name");
    }
  }
  const auto find = [&out](std::string_view name) -> Result<std::size_t> {
    const auto it = std::lower_bound(out.profiles_.begin(), out.profiles_.end(), name,
                                     [](const auto &p, std::string_view key) {
                                       return p->name < key;
                                     });
    if (it == out.profiles_.end() || (*it)->name != name) {
      return Err(ErrorCode::NotFound, "Markov policy: unknown referenced profile");
    }
    return Ok(static_cast<std::size_t>(it - out.profiles_.begin()));
  };
  ATX_TRY(out.dense_, find(regimes.dense_profile));
  ATX_TRY(out.ordinary_, find(regimes.ordinary_profile));
  ATX_TRY(out.sparse_, find(regimes.sparse_profile));
  out.overrides_.reserve(overrides.size());
  for (const auto &entry : overrides) {
    if (!valid_name(entry.symbol) || !valid_name(entry.profile)) {
      return Err(ErrorCode::InvalidArgument, "Markov policy: invalid override symbol or profile");
    }
    ATX_TRY(const auto index, find(entry.profile));
    out.overrides_.push_back({entry.symbol, index});
  }
  std::sort(out.overrides_.begin(), out.overrides_.end(),
            [](const auto &a, const auto &b) { return a.symbol < b.symbol; });
  for (std::size_t i = 1; i < out.overrides_.size(); ++i) {
    if (out.overrides_[i - 1].symbol == out.overrides_[i].symbol) {
      return Err(ErrorCode::InvalidArgument, "Markov policy: duplicate exact symbol override");
    }
  }
  return Ok(std::move(out));
}

Result<ResolvedAmericanMarkovPolicy>
AmericanMarkovPolicyCatalog::resolve(std::string_view symbol,
                                      std::span<const QuoteFitExpirySnapshot> snapshots,
                                      std::span<const AmericanMarkovFitInterval> intervals) const {
  if (profiles_.empty() || !valid_name(symbol)) {
    return Err(ErrorCode::InvalidArgument, "Markov policy: moved-from catalog or invalid symbol");
  }
  ATX_TRY(const auto active, active_variance_intervals(intervals));
  ATX_TRY_VOID(validate_snapshots(snapshots, intervals));
  ATX_TRY(auto features, quote_fit_liquidity_features(snapshots, regimes_.near_money_log_band));
  ATX_TRY(const auto regime, classify_quote_fit_liquidity(features, regimes_));
  std::size_t selected = ordinary_;
  switch (regime) {
  case QuoteFitLiquidityRegime::Dense:
    selected = dense_;
    break;
  case QuoteFitLiquidityRegime::Sparse:
    selected = sparse_;
    break;
  case QuoteFitLiquidityRegime::Ordinary:
    break;
  }
  QuoteFitPolicySource source = QuoteFitPolicySource::RawLiquidity;
  const auto override = std::lower_bound(overrides_.begin(), overrides_.end(), symbol,
                                         [](const auto &entry, std::string_view key) {
                                           return entry.symbol < key;
                                         });
  if (override != overrides_.end() && override->symbol == symbol) {
    selected = override->profile;
    source = QuoteFitPolicySource::ExactSymbolOverride;
  }
  const auto &profile = *profiles_[selected];
  const auto &fit = profile.fit_config;
  const std::size_t groups = fit.time_shape == AmericanMarkovFitTimeShape::Shared ? 1 :
      fit.time_shape == AmericanMarkovFitTimeShape::VarianceTimeKnots
          ? fit.variance_time_knots.size() : active;
  const auto knots = std::max(std::size_t{1}, fit.log_state_knots.size());
  if (groups > fit.max_parameters / knots ||
      intervals.size() > profile.model_config.max_transitions ||
      (fit.time_shape == AmericanMarkovFitTimeShape::VarianceTimeKnots &&
       intervals.size() > profile.model_config.max_stored_volatility_values / knots)) {
    return Err(ErrorCode::OutOfRange, "Markov policy: schedule exceeds profile parameter capacity");
  }
  const auto count = groups * knots;
  if ((!fit.initial_sigmas.empty() && fit.initial_sigmas.size() != count) ||
      (!fit.prior_sigmas.empty() && fit.prior_sigmas.size() != count)) {
    return Err(ErrorCode::InvalidArgument, "Markov policy: sigma vector does not match schedule");
  }
  return Ok(ResolvedAmericanMarkovPolicy{profiles_[selected], std::move(features), source,
                                         groups, count});
}

} // namespace atx::vol
