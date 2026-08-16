#include "oracle_convention_sweep.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>

#include "atx/vol/api/pricing/american.hpp"

namespace atx::vol::oracle {

using atx::core::Ok;

namespace {

constexpr std::array<InputModel, 8> kInputModels = {
    InputModel::CurrentSpotSdivYield,      InputModel::DiscreteDividendPvSdivYield,
    InputModel::DiscreteForwardNetCarry,   InputModel::DiscreteForwardRateSdivYield,
    InputModel::DiscreteForwardNetRate,    InputModel::DiscreteForwardZeroRates,
    InputModel::DiscreteDividendPvNetRate, InputModel::DiscreteDividendPvRatePlusSdiv,
};

constexpr std::array<double, 6> kUnitScales = {0.01, -0.01, 1.0, -1.0, 100.0, -100.0};
constexpr std::array<double, 6> kPointScales = {0.0001, -0.0001, 0.01, -0.01, 1.0, -1.0};
constexpr std::array<double, 10> kTimeScales = {
    1.0 / 365.0,  -1.0 / 365.0, 1.0 / 365.25, -1.0 / 365.25, 1.0 / 360.0,
    -1.0 / 360.0, 1.0 / 252.0,  -1.0 / 252.0, 1.0,           -1.0};

struct Accumulator {
  double sum = 0.0;
  std::int64_t count = 0;

  void absolute(double model, double oracle) noexcept {
    if (std::isfinite(model) && std::isfinite(oracle)) {
      sum += std::abs(model - oracle);
      ++count;
    }
  }

  void relative(double model, double oracle) noexcept {
    if (std::isfinite(model) && std::isfinite(oracle)) {
      sum += std::abs(model - oracle) / std::max(std::abs(oracle), 1.0e-4);
      ++count;
    }
  }

  [[nodiscard]] double mean() const noexcept {
    return count > 0 ? sum / static_cast<double>(count) : std::numeric_limits<double>::infinity();
  }
};

struct PriceCandidate {
  InputModel model{};
  Accumulator smoke;
  Accumulator tune;
};

struct ScaleCandidate {
  GreekSource source{};
  double scale = 1.0;
  Accumulator error;
};

[[nodiscard]] bool less_price(const PriceCandidate &left, const PriceCandidate &right,
                              bool include_tune) {
  const double left_sum = left.smoke.sum + (include_tune ? left.tune.sum : 0.0);
  const double right_sum = right.smoke.sum + (include_tune ? right.tune.sum : 0.0);
  const auto left_n = left.smoke.count + (include_tune ? left.tune.count : 0);
  const auto right_n = right.smoke.count + (include_tune ? right.tune.count : 0);
  const double left_mean =
      left_n > 0 ? left_sum / static_cast<double>(left_n) : std::numeric_limits<double>::infinity();
  const double right_mean = right_n > 0 ? right_sum / static_cast<double>(right_n)
                                        : std::numeric_limits<double>::infinity();
  if (left_mean != right_mean) {
    return left_mean < right_mean;
  }
  return input_model_id(left.model) < input_model_id(right.model);
}

void evaluate_price_rows(std::span<const OracleRow> rows, std::size_t stride,
                         PriceCandidate &candidate, bool tune) {
  ConventionMap map = baseline_convention();
  map.input_model = candidate.model;
  Accumulator &acc = tune ? candidate.tune : candidate.smoke;
  for (std::size_t index = 0; index < rows.size(); index += stride) {
    const OracleRow &row = rows[index];
    const EnginePricingInputs in = mode_a_inputs(row, map);
    const auto price = andersen_lake(in.spot, in.strike, in.years, in.sigma, in.rate, in.carry,
                                     in.side, al_fast_opts());
    if (price.has_value()) {
      acc.absolute(*price, row.sr_prc);
    }
  }
}

[[nodiscard]] std::vector<ScaleCandidate> scales_for(GreekSource source,
                                                     std::span<const double> scales) {
  std::vector<ScaleCandidate> out;
  out.reserve(scales.size());
  for (const double scale : scales) {
    out.push_back(ScaleCandidate{source, scale, {}});
  }
  return out;
}

[[nodiscard]] std::vector<ScaleCandidate> source_scales(std::span<const GreekSource> sources,
                                                        std::span<const double> scales) {
  std::vector<ScaleCandidate> out;
  out.reserve(sources.size() * scales.size());
  for (const GreekSource source : sources) {
    for (const double scale : scales) {
      out.push_back(ScaleCandidate{source, scale, {}});
    }
  }
  return out;
}

[[nodiscard]] double source_value(const AmericanGreeks &g, double dp_dq,
                                  GreekSource source) noexcept {
  switch (source) {
  case GreekSource::Delta:
    return g.delta;
  case GreekSource::Gamma:
    return g.gamma;
  case GreekSource::Theta:
    return g.theta;
  case GreekSource::Vega:
    return g.vega;
  case GreekSource::Rho:
    return g.rho;
  case GreekSource::CarryRho:
    return dp_dq;
  case GreekSource::Volga:
    return g.volga;
  case GreekSource::Vanna:
    return g.vanna;
  case GreekSource::Charm:
    return g.charm;
  }
  return std::numeric_limits<double>::quiet_NaN();
}

void observe_scales(std::vector<ScaleCandidate> &candidates, const AmericanGreeks &g, double dp_dq,
                    double oracle) noexcept {
  for (ScaleCandidate &candidate : candidates) {
    candidate.error.relative(source_value(g, dp_dq, candidate.source) * candidate.scale, oracle);
  }
}

[[nodiscard]] std::size_t best_scale(const std::vector<ScaleCandidate> &candidates) noexcept {
  std::size_t best = 0;
  for (std::size_t index = 1; index < candidates.size(); ++index) {
    const double value = candidates[index].error.mean();
    const double best_value = candidates[best].error.mean();
    const std::string id = std::string(greek_source_id(candidates[index].source)) + ":" +
                           std::to_string(candidates[index].scale);
    const std::string best_id = std::string(greek_source_id(candidates[best].source)) + ":" +
                                std::to_string(candidates[best].scale);
    if (value < best_value || (value == best_value && id < best_id)) {
      best = index;
    }
  }
  return best;
}

[[nodiscard]] FloorMetric floor_metric(std::string id, const Accumulator &acc, std::string unit,
                                       double multiplier = 1.0) {
  return FloorMetric{std::move(id), acc.mean() * multiplier, acc.count, std::move(unit)};
}

[[nodiscard]] std::string_view sign_id(double scale) noexcept {
  return std::signbit(scale) ? "negative" : "positive";
}

[[nodiscard]] std::string_view scale_id(double scale) noexcept {
  const double value = std::abs(scale);
  if (value == 100.0) {
    return "per_contract_100";
  }
  if (value == 1.0) {
    return "per_unit";
  }
  if (value == 0.01) {
    return "per_point";
  }
  if (value == 0.0001) {
    return "per_point_squared";
  }
  return "per_day";
}

[[nodiscard]] std::string_view price_scale_id(double scale) noexcept {
  const double value = std::abs(scale);
  if (value == 100.0) {
    return "per_contract_100";
  }
  if (value == 0.01) {
    return "per_share_from_contract";
  }
  return "per_share";
}

[[nodiscard]] std::string_view day_count_id(double days) noexcept {
  if (days == 365.0) {
    return "ACT_365F";
  }
  if (days == 365.25) {
    return "ACT_365_25";
  }
  if (days == 360.0) {
    return "ACT_360";
  }
  if (days == 252.0) {
    return "BUS_252";
  }
  return "PER_YEAR";
}

[[nodiscard]] double days_from_time_scale(double scale) noexcept {
  const double value = std::abs(scale);
  return value == 1.0 ? 365.0 : 1.0 / value;
}

[[nodiscard]] std::string_view time_basis_id(double scale) noexcept {
  return std::abs(scale) == 1.0 ? "per_year" : "per_day";
}

void append_json_string(std::string &out, std::string_view value) {
  out.push_back('"');
  for (const char ch : value) {
    if (ch == '"') {
      out.append("\\\"");
    } else if (ch == '\\') {
      out.append(2, '\\');
    } else {
      out.push_back(ch);
    }
  }
  out.push_back('"');
}

void append_double(std::string &out, double value) {
  char buffer[48];
  std::snprintf(buffer, sizeof buffer, "%.17g", value);
  out.append(buffer);
}

void append_int(std::string &out, std::int64_t value) {
  char buffer[32];
  std::snprintf(buffer, sizeof buffer, "%lld", static_cast<long long>(value));
  out.append(buffer);
}

void append_metric_array(std::string &out, std::span<const FloorMetric> metrics) {
  out.push_back('[');
  for (std::size_t index = 0; index < metrics.size(); ++index) {
    if (index != 0) {
      out.push_back(',');
    }
    const FloorMetric &metric = metrics[index];
    out.append("{\"metric_id\":");
    append_json_string(out, metric.metric_id);
    out.append(",\"value\":");
    append_double(out, metric.value);
    out.append(",\"count\":");
    append_int(out, metric.count);
    out.append(",\"unit\":");
    append_json_string(out, metric.unit);
    out.push_back('}');
  }
  out.push_back(']');
}

[[nodiscard]] std::string_view forward_formula(InputModel model) noexcept {
  return model == InputModel::CurrentSpotSdivYield ? "none" : "uprc_exp_rate_t_minus_ddiv";
}

[[nodiscard]] std::string_view rate_model(InputModel model) noexcept {
  switch (model) {
  case InputModel::DiscreteForwardZeroRates:
    return "zero";
  case InputModel::DiscreteDividendPvNetRate:
  case InputModel::DiscreteForwardNetRate:
    return "continuous_rate_minus_sdiv";
  case InputModel::DiscreteDividendPvRatePlusSdiv:
    return "continuous_rate_plus_sdiv";
  default:
    return "continuous_row_rate";
  }
}

[[nodiscard]] std::string_view carry_model(InputModel model) noexcept {
  switch (model) {
  case InputModel::CurrentSpotSdivYield:
  case InputModel::DiscreteDividendPvSdivYield:
  case InputModel::DiscreteForwardNetCarry:
  case InputModel::DiscreteForwardRateSdivYield:
    return "sdiv_as_yield";
  default:
    return "zero";
  }
}

} // namespace

double select_relative_scale(std::span<const ScaleObservation> observations,
                             std::span<const double> signed_scales) noexcept {
  if (signed_scales.empty()) {
    return 0.0;
  }
  std::size_t best = 0;
  double best_error = std::numeric_limits<double>::infinity();
  for (std::size_t index = 0; index < signed_scales.size(); ++index) {
    Accumulator acc;
    for (const ScaleObservation &observation : observations) {
      acc.relative(observation.raw * signed_scales[index], observation.oracle);
    }
    const double error = acc.mean();
    if (error < best_error || (error == best_error && signed_scales[index] < signed_scales[best])) {
      best = index;
      best_error = error;
    }
  }
  return signed_scales[best];
}

Result<ConventionSweepResult> run_convention_sweep(std::span<const OracleRow> smoke,
                                                   std::span<const OracleRow> tune) {
  if (smoke.empty() || tune.empty()) {
    return Err(ErrorCode::InvalidArgument, "convention sweep requires non-empty smoke+tune");
  }
  std::vector<PriceCandidate> prices;
  prices.reserve(kInputModels.size());
  for (const InputModel model : kInputModels) {
    prices.push_back(PriceCandidate{model, {}, {}});
    evaluate_price_rows(smoke, 1, prices.back(), false);
  }
  std::vector<std::size_t> finalists(prices.size());
  for (std::size_t index = 0; index < finalists.size(); ++index) {
    finalists[index] = index;
  }
  std::sort(finalists.begin(), finalists.end(), [&](std::size_t left, std::size_t right) {
    return less_price(prices[left], prices[right], false);
  });
  finalists.resize(2);
  const std::size_t tune_stride = std::max<std::size_t>(1, tune.size() / 32768);
  for (const std::size_t index : finalists) {
    evaluate_price_rows(tune, tune_stride, prices[index], true);
  }
  std::sort(finalists.begin(), finalists.end(), [&](std::size_t left, std::size_t right) {
    return less_price(prices[left], prices[right], true);
  });
  ConventionMap winner = baseline_convention();
  winner.input_model = prices[finalists.front()].model;

  std::vector<ScaleCandidate> price_scales = scales_for(GreekSource::Delta, kUnitScales);
  std::vector<ScaleCandidate> delta = scales_for(GreekSource::Delta, kUnitScales);
  std::vector<ScaleCandidate> gamma = scales_for(GreekSource::Gamma, kUnitScales);
  std::vector<ScaleCandidate> theta = scales_for(GreekSource::Theta, kTimeScales);
  std::vector<ScaleCandidate> vega = scales_for(GreekSource::Vega, kPointScales);
  std::vector<ScaleCandidate> rho = scales_for(GreekSource::Rho, kPointScales);
  std::vector<ScaleCandidate> phi = scales_for(GreekSource::CarryRho, kPointScales);
  constexpr std::array<GreekSource, 2> kSecondOrder = {GreekSource::Volga, GreekSource::Vanna};
  std::vector<ScaleCandidate> volga = source_scales(kSecondOrder, kPointScales);
  std::vector<ScaleCandidate> vanna = source_scales(kSecondOrder, kPointScales);
  std::vector<ScaleCandidate> decay = scales_for(GreekSource::Charm, kTimeScales);
  std::array<Accumulator, 11> baseline_acc{};
  Accumulator candidate_vol;
  Accumulator baseline_vol;
  ConventionSweepResult out;
  out.smoke_rows = static_cast<std::int64_t>(smoke.size());
  out.tune_rows = static_cast<std::int64_t>(tune.size());
  const auto start = std::chrono::steady_clock::now();

  auto evaluate_full = [&](std::span<const OracleRow> rows) {
    for (const OracleRow &row : rows) {
      const EnginePricingInputs win_in = mode_a_inputs(row, winner);
      const auto win_greeks =
          american_greeks_al(win_in.spot, win_in.strike, win_in.years, win_in.sigma, win_in.rate,
                             win_in.carry, win_in.side, al_fast_opts());
      if (!win_greeks.has_value()) {
        ++out.engine_errors;
        continue;
      }
      ++out.rows_priced;
      double win_dpdq = std::numeric_limits<double>::quiet_NaN();
      const auto win_carry =
          american_carry_greeks_al(win_in.spot, win_in.strike, win_in.years, win_in.sigma,
                                   win_in.rate, win_in.carry, win_in.side, al_fast_opts());
      if (win_carry.has_value()) {
        win_dpdq = win_carry->dP_dq;
      }
      for (ScaleCandidate &candidate : price_scales) {
        candidate.error.absolute(win_greeks->price * candidate.scale, row.sr_prc);
      }
      candidate_vol.absolute(row.sr_vol, row.sr_vol);
      observe_scales(delta, *win_greeks, win_dpdq, row.de);
      observe_scales(gamma, *win_greeks, win_dpdq, row.ga);
      observe_scales(theta, *win_greeks, win_dpdq, row.th);
      observe_scales(vega, *win_greeks, win_dpdq, row.ve);
      observe_scales(rho, *win_greeks, win_dpdq, row.rh);
      observe_scales(phi, *win_greeks, win_dpdq, row.ph);
      observe_scales(volga, *win_greeks, win_dpdq, row.vo);
      observe_scales(vanna, *win_greeks, win_dpdq, row.va);
      observe_scales(decay, *win_greeks, win_dpdq, row.de_decay);

      const EnginePricingInputs base_in = mode_a_inputs(row, baseline_convention());
      const AmericanGreeks *base_greeks = nullptr;
      AmericanGreeks distinct_base{};
      double base_dpdq = win_dpdq;
      if (winner.input_model == baseline_convention().input_model) {
        base_greeks = &*win_greeks;
      } else {
        const auto priced =
            american_greeks_al(base_in.spot, base_in.strike, base_in.years, base_in.sigma,
                               base_in.rate, base_in.carry, base_in.side, al_fast_opts());
        if (!priced.has_value()) {
          continue;
        }
        distinct_base = *priced;
        base_greeks = &distinct_base;
        const auto carry =
            american_carry_greeks_al(base_in.spot, base_in.strike, base_in.years, base_in.sigma,
                                     base_in.rate, base_in.carry, base_in.side, al_fast_opts());
        base_dpdq = carry.has_value() ? carry->dP_dq : std::numeric_limits<double>::quiet_NaN();
      }
      const OracleUnitGreeks base_units =
          to_oracle_units(*base_greeks, base_dpdq, baseline_convention());
      baseline_acc[0].absolute(base_greeks->price, row.sr_prc);
      baseline_vol.absolute(row.sr_vol, row.sr_vol);
      baseline_acc[2].relative(base_units.de, row.de);
      baseline_acc[3].relative(base_units.ga, row.ga);
      baseline_acc[4].relative(base_units.th, row.th);
      baseline_acc[5].relative(base_units.ve, row.ve);
      baseline_acc[6].relative(base_units.rh, row.rh);
      baseline_acc[7].relative(base_units.ph, row.ph);
      baseline_acc[8].relative(base_units.vo, row.vo);
      baseline_acc[9].relative(base_units.va, row.va);
      baseline_acc[10].relative(base_units.de_decay, row.de_decay);
    }
  };
  evaluate_full(smoke);
  evaluate_full(tune);

  const std::size_t price_index = best_scale(price_scales);
  const std::size_t delta_index = best_scale(delta);
  const std::size_t gamma_index = best_scale(gamma);
  const std::size_t theta_index = best_scale(theta);
  const std::size_t vega_index = best_scale(vega);
  const std::size_t rho_index = best_scale(rho);
  const std::size_t phi_index = best_scale(phi);
  const std::size_t volga_index = best_scale(volga);
  const std::size_t vanna_index = best_scale(vanna);
  const std::size_t decay_index = best_scale(decay);
  winner.price_scale = price_scales[price_index].scale;
  winner.delta_scale = delta[delta_index].scale;
  winner.gamma_scale = gamma[gamma_index].scale;
  winner.theta_scale = theta[theta_index].scale;
  winner.days_per_year = days_from_time_scale(winner.theta_scale);
  winner.vega_scale = vega[vega_index].scale;
  winner.rho_scale = rho[rho_index].scale;
  winner.phi_scale = phi[phi_index].scale;
  winner.volga_source = volga[volga_index].source;
  winner.volga_scale = volga[volga_index].scale;
  winner.vanna_source = vanna[vanna_index].source;
  winner.vanna_scale = vanna[vanna_index].scale;
  winner.delta_decay_scale = decay[decay_index].scale;
  out.winner = winner;
  out.metrics = {
      floor_metric("mode_a_price_mae", price_scales[price_index].error, "ticks", 100.0),
      floor_metric("mode_a_vol_mae", candidate_vol, "bp", 10000.0),
      floor_metric("mode_a_delta_rel", delta[delta_index].error, "relative"),
      floor_metric("mode_a_gamma_rel", gamma[gamma_index].error, "relative"),
      floor_metric("mode_a_theta_rel", theta[theta_index].error, "relative"),
      floor_metric("mode_a_vega_rel", vega[vega_index].error, "relative"),
      floor_metric("mode_a_rho_rel", rho[rho_index].error, "relative"),
      floor_metric("mode_a_phi_rel", phi[phi_index].error, "relative"),
      floor_metric("mode_a_volga_rel", volga[volga_index].error, "relative"),
      floor_metric("mode_a_vanna_rel", vanna[vanna_index].error, "relative"),
      floor_metric("mode_a_delta_decay_rel", decay[decay_index].error, "relative"),
  };
  baseline_acc[1] = baseline_vol;
  out.baseline_metrics = {
      floor_metric("mode_a_price_mae", baseline_acc[0], "ticks", 100.0),
      floor_metric("mode_a_vol_mae", baseline_acc[1], "bp", 10000.0),
      floor_metric("mode_a_delta_rel", baseline_acc[2], "relative"),
      floor_metric("mode_a_gamma_rel", baseline_acc[3], "relative"),
      floor_metric("mode_a_theta_rel", baseline_acc[4], "relative"),
      floor_metric("mode_a_vega_rel", baseline_acc[5], "relative"),
      floor_metric("mode_a_rho_rel", baseline_acc[6], "relative"),
      floor_metric("mode_a_phi_rel", baseline_acc[7], "relative"),
      floor_metric("mode_a_volga_rel", baseline_acc[8], "relative"),
      floor_metric("mode_a_vanna_rel", baseline_acc[9], "relative"),
      floor_metric("mode_a_delta_decay_rel", baseline_acc[10], "relative"),
  };
  for (const PriceCandidate &candidate : prices) {
    out.candidate_prices.push_back(CandidatePriceMetric{
        std::string(input_model_id(candidate.model)), candidate.smoke.mean() * 100.0,
        candidate.smoke.count, candidate.tune.count > 0 ? candidate.tune.mean() * 100.0 : 0.0,
        candidate.tune.count});
  }
  std::sort(out.candidate_prices.begin(), out.candidate_prices.end(),
            [](const CandidatePriceMetric &left, const CandidatePriceMetric &right) {
              return left.candidate_id < right.candidate_id;
            });
  out.diagnostic_wall_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  out.diagnostic_rows_per_second =
      out.diagnostic_wall_seconds > 0.0
          ? static_cast<double>(out.rows_priced) / out.diagnostic_wall_seconds
          : 0.0;
  return Ok(std::move(out));
}

std::string convention_map_json(const ConventionMap &map) {
  std::string out = "{";
  auto field = [&](std::string_view name, std::string_view value, bool first = false) {
    if (!first) {
      out.push_back(',');
    }
    append_json_string(out, name);
    out.push_back(':');
    append_json_string(out, value);
  };
  field("input_model", input_model_id(map.input_model), true);
  field("forward_formula", forward_formula(map.input_model));
  field("rate_model", rate_model(map.input_model));
  field("carry_model", carry_model(map.input_model));
  field("dividend_model", map.input_model == InputModel::CurrentSpotSdivYield
                              ? "continuous_yield_only"
                              : "discrete_cash_forward");
  field("day_count", day_count_id(map.days_per_year));
  field("price_scale", price_scale_id(map.price_scale));
  field("price_sign", sign_id(map.price_scale));
  field("vol_scale", "decimal_identity");
  field("delta_scale", scale_id(map.delta_scale));
  field("delta_sign", sign_id(map.delta_scale));
  field("gamma_scale", scale_id(map.gamma_scale));
  field("gamma_sign", sign_id(map.gamma_scale));
  field("theta_basis", time_basis_id(map.theta_scale));
  field("theta_sign", sign_id(map.theta_scale));
  field("vega_scale", scale_id(map.vega_scale));
  field("vega_sign", sign_id(map.vega_scale));
  field("rho_scale", scale_id(map.rho_scale));
  field("rho_sign", sign_id(map.rho_scale));
  field("phi_scale", scale_id(map.phi_scale));
  field("phi_sign", sign_id(map.phi_scale));
  field("volga_source", greek_source_id(map.volga_source));
  field("volga_scale", scale_id(map.volga_scale));
  field("volga_sign", sign_id(map.volga_scale));
  field("vanna_source", greek_source_id(map.vanna_source));
  field("vanna_scale", scale_id(map.vanna_scale));
  field("vanna_sign", sign_id(map.vanna_scale));
  field("delta_decay_basis", time_basis_id(map.delta_decay_scale));
  field("delta_decay_day_count", day_count_id(days_from_time_scale(map.delta_decay_scale)));
  field("delta_decay_sign", sign_id(map.delta_decay_scale));
  out.push_back('}');
  return out;
}

std::string convention_sweep_json(const ConventionSweepResult &result, std::string_view git_sha) {
  std::string out = "{\"schema_version\":2,\"kind\":\"convention_sweep\",\"git_sha\":";
  append_json_string(out, git_sha);
  out.append(",\"cohorts\":[\"smoke\",\"tune\"],\"selection_strategy\":"
             "\"all_smoke_then_top2_deterministic_tune_sample_then_full_attribution\","
             "\"smoke_rows\":");
  append_int(out, result.smoke_rows);
  out.append(",\"tune_rows\":");
  append_int(out, result.tune_rows);
  out.append(",\"rows_priced\":");
  append_int(out, result.rows_priced);
  out.append(",\"engine_errors\":");
  append_int(out, result.engine_errors);
  out.append(",\"baseline_conventions\":");
  out.append(convention_map_json(baseline_convention()));
  out.append(",\"conventions\":");
  out.append(convention_map_json(result.winner));
  out.append(",\"metrics\":");
  append_metric_array(out, result.metrics);
  out.append(",\"baseline_metrics\":");
  append_metric_array(out, result.baseline_metrics);
  out.append(",\"metric_deltas\":[");
  for (std::size_t index = 0; index < result.metrics.size(); ++index) {
    if (index != 0) {
      out.push_back(',');
    }
    out.append("{\"metric_id\":");
    append_json_string(out, result.metrics[index].metric_id);
    out.append(",\"candidate\":");
    append_double(out, result.metrics[index].value);
    out.append(",\"baseline\":");
    append_double(out, result.baseline_metrics[index].value);
    out.append(",\"delta\":");
    append_double(out, result.metrics[index].value - result.baseline_metrics[index].value);
    out.append(",\"count\":");
    append_int(out, result.metrics[index].count);
    out.append(",\"unit\":");
    append_json_string(out, result.metrics[index].unit);
    out.push_back('}');
  }
  out.append("],\"candidate_prices\":[");
  for (std::size_t index = 0; index < result.candidate_prices.size(); ++index) {
    if (index != 0) {
      out.push_back(',');
    }
    const CandidatePriceMetric &candidate = result.candidate_prices[index];
    out.append("{\"candidate_id\":");
    append_json_string(out, candidate.candidate_id);
    out.append(",\"smoke_price_mae_ticks\":");
    append_double(out, candidate.smoke_price_mae_ticks);
    out.append(",\"smoke_count\":");
    append_int(out, candidate.smoke_count);
    out.append(",\"tune_sample_price_mae_ticks\":");
    append_double(out, candidate.tune_sample_price_mae_ticks);
    out.append(",\"tune_sample_count\":");
    append_int(out, candidate.tune_sample_count);
    out.push_back('}');
  }
  out.append("],\"oracle_suspect_candidates\":[],"
             "\"market_evidence_status\":\"not_evaluated_no_nbbo_gate\","
             "\"diagnostic_speed\":{\"preset\":\"dev\",\"citable\":false,"
             "\"wall_seconds\":");
  append_double(out, result.diagnostic_wall_seconds);
  out.append(",\"rows_per_second\":");
  append_double(out, result.diagnostic_rows_per_second);
  out.append("}}\n");
  return out;
}

} // namespace atx::vol::oracle
