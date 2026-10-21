#pragma once

// Offline policy parsing/provenance only. Numerical routing belongs to the
// reusable quote_fit_policy API; this file owns no fitting or market heuristics.
#include "atx/vol/api/fitting/quote_fit_policy.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace quote_bench_policy {
using namespace atx::vol;
using Json = nlohmann::json;

inline void keys(const Json &object, std::initializer_list<std::string_view> allowed) {
  if (!object.is_object()) throw std::runtime_error("Policy entry must be a JSON object");
  for (const auto &[key, value] : object.items()) {
    static_cast<void>(value);
    if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
      throw std::runtime_error("Unknown policy key: " + key);
    }
  }
}

inline std::string text(const Json &value, bool identifier = false) {
  if (!value.is_string()) throw std::runtime_error("Policy name must be a string");
  const auto result = value.get<std::string>();
  if (result.empty() || result.size() > 128U ||
      std::any_of(result.begin(), result.end(), [identifier](unsigned char c) {
        return c < 32U || c == 127U || (identifier &&
            !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'));
      })) throw std::runtime_error("Invalid policy name or symbol");
  return result;
}

template <class T>
inline void numeric(const Json &object, const char *key, T &target, double lower, double upper) {
  if (!object.contains(key)) return;
  const auto &value = object.at(key);
  if (!value.is_number() || (std::is_integral_v<T> && !value.is_number_integer())) {
    throw std::runtime_error("Policy field must be a number of the correct type: " +
                             std::string(key));
  }
  const double x = value.get<double>();
  if (!std::isfinite(x) || x < lower || x > upper) {
    throw std::runtime_error("Policy field outside supported bounds: " + std::string(key));
  }
  target = static_cast<T>(x);
}

inline CalibLossKind loss(const Json &value) {
  const auto name = text(value);
  if (name == "mid") return CalibLossKind::Mid;
  if (name == "interval") return CalibLossKind::Interval;
  throw std::runtime_error("Policy loss must be mid or interval");
}

inline const char *loss_name(CalibLossKind value) {
  return value == CalibLossKind::Mid ? "mid" : "interval";
}

// Opt-in per-expiry carry resolution (quote_slice_carry.hpp). Absent keys keep
// the historical supplied carry.
inline void parse_carry(const Json &entry, QuoteSliceCarryConfig &carry) {
  if (entry.contains("carry_mode")) {
    const auto mode = text(entry.at("carry_mode"));
    if (mode == "supplied") {
      carry.mode = QuoteSliceCarryMode::Supplied;
    } else if (mode == "implied-forward") {
      carry.mode = QuoteSliceCarryMode::ImpliedForward;
      carry.implied.method = ImpliedForwardMethod::AmericanAlignment;
    } else if (mode == "implied-forward-parity") {
      carry.mode = QuoteSliceCarryMode::ImpliedForward;
      carry.implied.method = ImpliedForwardMethod::Parity;
    } else {
      throw std::runtime_error(
          "Policy carry_mode must be supplied, implied-forward or implied-forward-parity");
    }
  }
  if (!entry.contains("implied_forward")) return;
  // Under the resolved Supplied carry the block would be parsed and silently
  // inert (and absent from the expanded definition): refuse it, like every
  // other ineffective policy input.
  if (carry.mode == QuoteSliceCarryMode::Supplied) {
    throw std::runtime_error("Policy implied_forward requires carry_mode implied-forward or "
                             "implied-forward-parity");
  }
  const auto &j = entry.at("implied_forward");
  keys(j, {"min_pairs", "max_pairs", "band_sd", "spread_floor", "max_weight_ratio", "huber_k",
           "max_dispersion_sd", "max_leave_one_out_sd", "max_exercise_shift_sd",
           "max_pair_exercise_shift_sd", "min_time_value", "min_time_value_fraction",
           "al_preset", "fixed_point_tolerance", "max_fixed_point_iterations"});
  auto &f = carry.implied;
  numeric(j, "min_pairs", f.min_pairs, 1, static_cast<double>(kImpliedForwardMaxSelected));
  numeric(j, "max_pairs", f.max_pairs, 1, static_cast<double>(kImpliedForwardMaxSelected));
  numeric(j, "band_sd", f.band_sd, 1.0e-6, 100);
  numeric(j, "spread_floor", f.spread_floor, 1.0e-12, 1.0e6);
  numeric(j, "max_weight_ratio", f.max_weight_ratio, 1, 1.0e6);
  numeric(j, "huber_k", f.huber_k, 1.0e-6, 1.0e6);
  numeric(j, "max_dispersion_sd", f.max_dispersion_sd, 1.0e-12, 100);
  numeric(j, "max_leave_one_out_sd", f.max_leave_one_out_sd, 1.0e-12, 100);
  numeric(j, "max_exercise_shift_sd", f.max_exercise_shift_sd, 1.0e-12, 100);
  numeric(j, "max_pair_exercise_shift_sd", f.max_pair_exercise_shift_sd, 1.0e-12, 100);
  numeric(j, "min_time_value", f.min_time_value, 0, 1.0e9);
  numeric(j, "min_time_value_fraction", f.min_time_value_fraction, 0, 100);
  numeric(j, "fixed_point_tolerance", f.fixed_point_tolerance, 1.0e-15, 1.0e-2);
  numeric(j, "max_fixed_point_iterations", f.max_fixed_point_iterations, 1, 255);
  if (j.contains("al_preset")) {
    const auto preset = text(j.at("al_preset"));
    if (preset == "fast") f.al_opts = al_fast_opts();
    else if (preset == "bulk") f.al_opts = al_bulk_opts();
    else if (preset == "accurate") f.al_opts = std::nullopt;
    else throw std::runtime_error("Policy implied_forward al_preset must be fast, bulk or "
                                  "accurate");
  }
  if (f.max_pairs < f.min_pairs) {
    throw std::runtime_error("Policy implied_forward max_pairs must be at least min_pairs");
  }
}

inline const char *al_preset_name(const std::optional<AlOpts> &opts) {
  if (!opts) return "accurate";
  const auto same = [&](const AlOpts &o) {
    return opts->n_collocation == o.n_collocation && opts->n_quadrature == o.n_quadrature &&
           opts->n_quad_price == o.n_quad_price && opts->max_newton_iter == o.max_newton_iter &&
           opts->tol == o.tol;
  };
  if (same(al_fast_opts())) return "fast";
  if (same(al_bulk_opts())) return "bulk";
  return "custom";
}

// Every estimator control that changes results, so the expanded definition is
// complete runtime provenance. A "custom" preset also records its AlOpts.
inline Json carry_json(const QuoteSliceCarryConfig &carry) {
  const auto &f = carry.implied;
  Json out{{"method", to_string(f.method)}, {"min_pairs", f.min_pairs},
           {"max_pairs", f.max_pairs}, {"band_sd", f.band_sd}, {"spread_floor", f.spread_floor},
           {"max_weight_ratio", f.max_weight_ratio}, {"huber_k", f.huber_k},
           {"max_dispersion_sd", f.max_dispersion_sd},
           {"max_leave_one_out_sd", f.max_leave_one_out_sd},
           {"max_outlier_fraction", f.max_outlier_fraction},
           {"max_exercise_shift_sd", f.max_exercise_shift_sd},
           {"max_pair_exercise_shift_sd", f.max_pair_exercise_shift_sd},
           {"min_time_value", f.min_time_value},
           {"min_time_value_fraction", f.min_time_value_fraction},
           {"al_preset", al_preset_name(f.al_opts)}, {"iv_tolerance", f.iv_tolerance},
           {"max_iv_iterations", f.max_iv_iterations},
           {"fixed_point_tolerance", f.fixed_point_tolerance},
           {"max_fixed_point_iterations", f.max_fixed_point_iterations},
           {"exercise", f.exercise == ImpliedForwardExercise::American ? "american" : "european"}};
  if (f.al_opts && std::string_view(al_preset_name(f.al_opts)) == "custom") {
    const auto &a = *f.al_opts;
    out["al_opts"] = {{"n_collocation", a.n_collocation}, {"n_quadrature", a.n_quadrature},
                      {"n_quad_price", a.n_quad_price}, {"max_newton_iter", a.max_newton_iter},
                      {"tol", a.tol}};
  }
  return out;
}

// Opt-in per-profile dividend routing (quote_bench_main.cpp). `aggregate` (the
// default, also when the key is absent) prices every expiry with the --dividends
// aggregate-PV model. `inferred-schedule` infers the chain's dated cash schedule
// from its (ddiv, ddiv_pv, rate) ladder (api/marketdata/dividend_schedule.hpp) and
// sends every expiry with an inferred ex-date inside its life through the
// discrete-dividend route (discrete_div_route -> DatedCash lattice, scheme per
// `dividend_lattice`; the escrowed schemes fit the escrowed stock's vol).
// `inferred-schedule-exact` does the same only where every event in the life is
// exactly dated (no split or non-integer event).
enum class DividendRouting : std::uint8_t { Aggregate, InferredSchedule, InferredScheduleExact };

inline const char *to_string(DividendRouting value) {
  switch (value) {
  case DividendRouting::Aggregate:
    return "aggregate";
  case DividendRouting::InferredSchedule:
    return "inferred-schedule";
  case DividendRouting::InferredScheduleExact:
    return "inferred-schedule-exact";
  }
  return "unknown";
}

inline DividendRouting parse_dividend_routing(const Json &entry) {
  if (!entry.contains("dividends")) return DividendRouting::Aggregate;
  const auto name = text(entry.at("dividends"));
  if (name == "aggregate") return DividendRouting::Aggregate;
  if (name == "inferred-schedule") return DividendRouting::InferredSchedule;
  if (name == "inferred-schedule-exact") return DividendRouting::InferredScheduleExact;
  throw std::runtime_error(
      "Policy dividends must be aggregate, inferred-schedule or inferred-schedule-exact");
}

// Opt-in lattice scheme of the dated-cash route (american.hpp, DiscreteDivScheme)
// and its step count. Both act only where a chain is routed to the DatedCash
// lattice (`dividends` above); the aggregate models ignore them. Absent keys
// keep the historical spliced 301-step lattice.
inline void parse_lattice_options(const Json &entry, QuoteSliceConfig &cfg) {
  if (entry.contains("dividend_lattice")) {
    const auto name = text(entry.at("dividend_lattice"));
    bool known = false;
    for (const auto scheme : {DiscreteDivScheme::SplicedCrr, DiscreteDivScheme::SplicedBbs,
                              DiscreteDivScheme::SplicedBbsr, DiscreteDivScheme::EscrowedBbs,
                              DiscreteDivScheme::EscrowedLeisenReimer}) {
      if (name == to_string(scheme)) {
        cfg.discrete_div_scheme = scheme;
        known = true;
      }
    }
    if (!known) {
      throw std::runtime_error("Policy dividend_lattice must be spliced, spliced-bbs, "
                               "spliced-bbsr, escrowed-bbs or escrowed-lr");
    }
  }
  numeric(entry, "dividend_lattice_steps", cfg.discrete_div_steps, 1, kDiscreteDivMaxSteps);
}

inline void parse_profile(const Json &entry, QuoteFitProfile &profile,
                          std::vector<double> &spline_grid) {
  keys(entry, {"name", "base", "curve", "min_quotes", "quality", "parametric", "convex",
               "spline", "side_policy", "atm_log_band", "spread_floor", "min_time_value",
               "iv_tolerance", "max_iv_iterations", "carry_mode", "implied_forward",
               "channel", "dividends", "dividend_lattice", "dividend_lattice_steps"});
  profile.name = text(entry.at("name"), true);
  auto &cfg = profile.config;
  if (entry.contains("curve")) {
    const auto name = text(entry.at("curve"));
    if (name == "knot-spline") {
      // Channel-constrained knot spline; served as a spline-vol curve.
      cfg.method = QuoteSliceFitMethod::ChannelSpline;
      cfg.curve.kind = VolCurveKind::SplineVol;
    } else {
      const auto kind = parse_curve_kind(name);
      if (!kind) throw std::runtime_error(kind.error().to_string());
      cfg.curve.kind = *kind;
      cfg.method = QuoteSliceFitMethod::PointCurve;
    }
  }
  if (entry.contains("channel")) {
    const auto &j = entry.at("channel");
    keys(j, {"channel_weight", "mid_weight", "itm_mid_weight", "default_shape_weight",
             "prior_scale", "roughness", "min_half_width_vol", "mult_floor", "mult_ceil",
             "max_iterations", "min_channel_quotes", "repair_half_spreads", "family",
             "family_iterations", "point_max_outside_fraction", "point_min_outside_quotes"});
    auto &c = cfg.channel;
    if (j.contains("family")) {
      // The family that solves the channel objective; knot-spline is the native solver.
      const auto name = text(j.at("family"));
      if (name == "knot-spline") {
        c.family.reset();
      } else {
        const auto kind = parse_curve_kind(name);
        if (!kind) throw std::runtime_error(kind.error().to_string());
        if (*kind == VolCurveKind::LinearVariance) {
          throw std::runtime_error("Policy channel family cannot be linear-variance");
        }
        c.family = *kind;
      }
    }
    numeric(j, "family_iterations", c.family_iterations, 1, 1000);
    numeric(j, "channel_weight", c.channel_weight, 0, 1.0e8);
    numeric(j, "mid_weight", c.mid_weight, 0, 1.0e8);
    numeric(j, "itm_mid_weight", c.itm_mid_weight, 0, 1.0e3);
    numeric(j, "default_shape_weight", c.default_shape_weight, 0, 1.0e6);
    numeric(j, "prior_scale", c.prior_scale, 1.0e-4, 10);
    numeric(j, "roughness", c.roughness, 0, 1.0e6);
    numeric(j, "min_half_width_vol", c.min_half_width_vol, 1.0e-6, 1);
    numeric(j, "mult_floor", c.mult_floor, 1.0e-4, 1);
    numeric(j, "mult_ceil", c.mult_ceil, 1, 100);
    numeric(j, "max_iterations", c.max_iterations, 1, 1000);
    numeric(j, "min_channel_quotes", c.min_channel_quotes, 1, 100000);
    numeric(j, "repair_half_spreads", c.repair_half_spreads, 0, 100);
    // Opt-in channel dominance of point fits under graded admission (see
    // QuoteSliceChannelConfig::point_max_outside_fraction).
    if (j.contains("point_max_outside_fraction")) {
      double fraction = 0.0;
      numeric(j, "point_max_outside_fraction", fraction, 0, 1);
      c.point_max_outside_fraction = fraction;
    }
    if (j.contains("point_min_outside_quotes") && !c.point_max_outside_fraction) {
      throw std::runtime_error(
          "Policy channel point_min_outside_quotes requires point_max_outside_fraction");
    }
    numeric(j, "point_min_outside_quotes", c.point_min_outside_quotes, 1, 100000);
  }
  numeric(entry, "min_quotes", cfg.min_quotes, 4, 100000);
  numeric(entry, "atm_log_band", cfg.atm_log_band, 0, 5);
  numeric(entry, "spread_floor", cfg.spread_floor, 1.0e-12, 1.0e6);
  numeric(entry, "min_time_value", cfg.min_time_value, 0, 1.0e9);
  numeric(entry, "iv_tolerance", cfg.iv_tolerance, 1.0e-12, 1.0e-3);
  numeric(entry, "max_iv_iterations", cfg.max_iv_iterations, 1, 512);
  if (entry.contains("side_policy")) {
    const auto side = text(entry.at("side_policy"));
    if (side == "all") cfg.side_policy = QuoteSliceSidePolicy::All;
    else if (side == "otm") cfg.side_policy = QuoteSliceSidePolicy::OutOfTheMoney;
    else throw std::runtime_error("Policy side_policy must be all or otm");
  }
  if (entry.contains("quality")) {
    const auto &j = entry.at("quality");
    keys(j, {"min_bid", "max_spread_to_mid", "max_spread_vol", "min_vega_weight", "max_weight"});
    auto &q = cfg.quality;
    numeric(j, "min_bid", q.min_bid, 0, 1.0e9);
    numeric(j, "max_spread_to_mid", q.max_spread_to_mid, 0, 2);
    numeric(j, "max_spread_vol", q.max_spread_vol, 0, 100);
    numeric(j, "min_vega_weight", q.min_vega_weight, 0, 1.0e12);
    numeric(j, "max_weight", q.max_weight, 1.0e-12, 1.0e12);
  }
  if (entry.contains("parametric")) {
    const auto &j = entry.at("parametric");
    keys(j, {"max_outer_iter", "max_inner_iter", "tol_param", "tol_residual", "huber_k",
             "min_obs_per_slice", "loss_kind", "optimization_level", "max_iter_quick_mark",
             "max_iter_trading", "max_iter_risk", "max_iter_reference", "max_iter_cold_fast"});
    auto &p = cfg.curve.parametric;
    numeric(j, "max_outer_iter", p.max_outer_iter, 1, 32);
    numeric(j, "max_inner_iter", p.max_inner_iter, 1, 256);
    numeric(j, "tol_param", p.tol_param, 1.0e-14, 1.0e-2);
    numeric(j, "tol_residual", p.tol_residual, 1.0e-14, 1.0e-2);
    numeric(j, "huber_k", p.huber_k, 1.0e-6, 1.0e6);
    numeric(j, "min_obs_per_slice", p.min_obs_per_slice, 4, 100000);
    numeric(j, "max_iter_quick_mark", p.max_iter_quick_mark, 1, 512);
    numeric(j, "max_iter_trading", p.max_iter_trading, 1, 512);
    numeric(j, "max_iter_risk", p.max_iter_risk, 1, 512);
    numeric(j, "max_iter_reference", p.max_iter_reference, 1, 512);
    numeric(j, "max_iter_cold_fast", p.max_iter_cold_fast, 1, 512);
    if (j.contains("loss_kind")) p.loss_kind = loss(j.at("loss_kind"));
    if (j.contains("optimization_level")) {
      const auto name = text(j.at("optimization_level"));
      if (name == "quick-mark") p.optimization_level = OptimizationLevel::QuickMark;
      else if (name == "trading") p.optimization_level = OptimizationLevel::Trading;
      else if (name == "risk") p.optimization_level = OptimizationLevel::Risk;
      else if (name == "reference") p.optimization_level = OptimizationLevel::Reference;
      else if (name == "cold-fast") p.optimization_level = OptimizationLevel::ColdFast;
      else throw std::runtime_error("Unknown policy optimization_level");
    }
  }
  if (entry.contains("convex")) {
    const auto &j = entry.at("convex");
    keys(j, {"lambda", "node_cap", "max_iter", "loss"});
    auto &c = cfg.curve.convex;
    numeric(j, "lambda", c.lambda, 0, 1.0e6);
    numeric(j, "node_cap", c.node_cap, 4, 128);
    numeric(j, "max_iter", c.max_iter, 1, 2048);
    if (j.contains("loss")) c.loss = loss(j.at("loss"));
  }
  if (entry.contains("spline")) {
    const auto &j = entry.at("spline");
    keys(j, {"lambda", "mult_floor", "mult_ceil", "min_obs", "grid"});
    auto &s = cfg.curve.spline;
    numeric(j, "lambda", s.lambda, 0, 1.0e6);
    numeric(j, "mult_floor", s.mult_floor, 1.0e-6, 100);
    numeric(j, "mult_ceil", s.mult_ceil, 1.0e-6, 100);
    numeric(j, "min_obs", s.min_obs, 4, 100000);
    if (s.mult_ceil <= s.mult_floor) throw std::runtime_error("Invalid spline multiple bounds");
    if (j.contains("grid")) {
      const auto &grid = j.at("grid");
      if (!grid.is_array() || grid.size() < 4U || grid.size() > 257U) {
        throw std::runtime_error("Policy spline grid must contain 4..257 knots");
      }
      for (const auto &value : grid) {
        if (!value.is_number()) throw std::runtime_error("Policy spline knot must be numeric");
        const double x = value.get<double>();
        if (!std::isfinite(x) || std::abs(x) > 100.0) {
          throw std::runtime_error("Invalid spline knot");
        }
        spline_grid.push_back(x);
      }
      s.grid = spline_grid;
    }
  }
  parse_carry(entry, cfg.carry);
  parse_lattice_options(entry, cfg);
  // These are mandatory controls, never user-adjustable escape hatches.
  cfg.curve.convex.bound_slope_below = true;
}

inline Json quality_json(const QuoteSliceQualityConfig &q) {
  return {{"min_bid", q.min_bid}, {"max_spread_to_mid", q.max_spread_to_mid},
          {"max_spread_vol", q.max_spread_vol}, {"min_vega_weight", q.min_vega_weight},
          {"max_weight", q.max_weight}};
}

inline Json parametric_json(const CalibOpts &p) {
  Json out = Json::object();
  // Full input configuration, including unchanged defaults, is provenance.
  // Some observation-builder options do not apply to quote_slice's raw adapter.
#define ATX_POLICY_FIELD(name) out[#name] = p.name
  ATX_POLICY_FIELD(max_outer_iter);
  ATX_POLICY_FIELD(max_inner_iter);
  ATX_POLICY_FIELD(tol_param);
  ATX_POLICY_FIELD(tol_residual);
  ATX_POLICY_FIELD(huber_k);
  ATX_POLICY_FIELD(min_vega_weight);
  ATX_POLICY_FIELD(max_spread_vol);
  ATX_POLICY_FIELD(max_weight);
  ATX_POLICY_FIELD(itm_leg_fallback);
  ATX_POLICY_FIELD(one_sided_bounds);
  ATX_POLICY_FIELD(price_tick);
  ATX_POLICY_FIELD(max_obs_per_slice);
  ATX_POLICY_FIELD(warm_start_deam_adjacent_strikes);
  ATX_POLICY_FIELD(use_shared_boundary_deam);
  ATX_POLICY_FIELD(max_deam_strikes_per_expiry);
  ATX_POLICY_FIELD(max_otm_shortcut_premium_spread_frac);
  ATX_POLICY_FIELD(max_inversion_residual_half_spreads);
  ATX_POLICY_FIELD(audit_accurate_inversions);
  ATX_POLICY_FIELD(min_otm_shortcut_T);
  ATX_POLICY_FIELD(min_otm_shortcut_vega);
  ATX_POLICY_FIELD(max_otm_shortcut_abs_k);
  ATX_POLICY_FIELD(deam_tier0_max_eep_vol_pts);
  ATX_POLICY_FIELD(deam_tier1_max_eep_vol_pts);
  ATX_POLICY_FIELD(deam_tier_escalate_margin_vol_pts);
  ATX_POLICY_FIELD(deam_tier_div_call_max_T);
  ATX_POLICY_FIELD(deam_tier_div_call_max_div_frac);
  ATX_POLICY_FIELD(max_certified_deam_drop_fraction);
  ATX_POLICY_FIELD(prior_strength);
  ATX_POLICY_FIELD(optimization_level);
  ATX_POLICY_FIELD(essvi_fallback_rmse_threshold);
  ATX_POLICY_FIELD(n_butterfly_grid);
  ATX_POLICY_FIELD(max_iter_quick_mark);
  ATX_POLICY_FIELD(max_iter_trading);
  ATX_POLICY_FIELD(max_iter_risk);
  ATX_POLICY_FIELD(max_iter_reference);
  ATX_POLICY_FIELD(max_iter_cold_fast);
  ATX_POLICY_FIELD(wing_floor_alpha);
  ATX_POLICY_FIELD(lee_bound_project);
  ATX_POLICY_FIELD(morozov_stop);
  ATX_POLICY_FIELD(morozov_tau);
  ATX_POLICY_FIELD(validate_no_arb);
  ATX_POLICY_FIELD(essvi_alt_driver_theta_project);
  ATX_POLICY_FIELD(residual_disable);
  ATX_POLICY_FIELD(residual_basis_kind);
  ATX_POLICY_FIELD(residual_n_basis_terms);
  ATX_POLICY_FIELD(residual_ridge_factor);
  ATX_POLICY_FIELD(loss_kind);
  ATX_POLICY_FIELD(anchor_kind);
  ATX_POLICY_FIELD(essvi_asymmetric_rho);
  ATX_POLICY_FIELD(essvi_anchored);
  ATX_POLICY_FIELD(essvi_anchored_interpolate_thin);
  ATX_POLICY_FIELD(min_rows_to_fit_independently);
  ATX_POLICY_FIELD(min_obs_per_slice);
  ATX_POLICY_FIELD(max_post_fit_sigma);
  ATX_POLICY_FIELD(max_spread_to_mid_pct);
  ATX_POLICY_FIELD(per_slice_linear_fallback);
  ATX_POLICY_FIELD(per_slice_uncovered_parametric);
#undef ATX_POLICY_FIELD
  return out;
}

inline Json profile_json(const QuoteFitProfile &profile) {
  const auto &c = profile.config;
  const auto &dense = c.curve.convex;
  const auto &spline = c.curve.spline;
  Json out{{"name", profile.name}, {"kind", profile.kind},
          {"curve", to_string(c.curve.kind)},
          {"dividend_model", to_string(c.dividend_model)}, {"min_quotes", c.min_quotes},
          {"quality", quality_json(c.quality)},
          {"parametric", parametric_json(c.curve.parametric)},
          {"convex", {{"lambda", dense.lambda}, {"node_cap", dense.node_cap},
                      {"max_iter", dense.max_iter}, {"loss", loss_name(dense.loss)},
                      {"bound_slope_below", dense.bound_slope_below}}},
          {"spline", {{"grid", std::vector<double>(spline.grid.begin(), spline.grid.end())},
                      {"lambda", spline.lambda}, {"min_obs", spline.min_obs},
                      {"mult_floor", spline.mult_floor}, {"mult_ceil", spline.mult_ceil}}},
          {"side_policy", c.side_policy == QuoteSliceSidePolicy::All ? "all" : "otm"},
          {"atm_log_band", c.atm_log_band}, {"spread_floor", c.spread_floor},
          {"min_time_value", c.min_time_value}, {"iv_tolerance", c.iv_tolerance},
          {"max_iv_iterations", c.max_iv_iterations},
          {"discrete_div_steps", c.discrete_div_steps},
          {"al_opts", nullptr}, {"convex_repair", nullptr},
          {"spline_candidate", c.curve.spline_candidate}};
  // Emitted only when opted in, so the supplied-carry provenance is unchanged.
  if (c.carry.mode != QuoteSliceCarryMode::Supplied) {
    out["carry_mode"] = to_string(c.carry.mode);
    out["implied_forward"] = carry_json(c.carry);
  }
  // Emitted only when opted in, so the historical lattice provenance is unchanged.
  if (c.discrete_div_scheme != DiscreteDivScheme::SplicedCrr) {
    out["dividend_lattice"] = to_string(c.discrete_div_scheme);
  }
  return out;
}

inline Json channel_json(const QuoteSliceChannelConfig &c) {
  Json out{{"channel_weight", c.channel_weight}, {"mid_weight", c.mid_weight},
           {"itm_mid_weight", c.itm_mid_weight}, {"default_shape_weight", c.default_shape_weight},
           {"prior_scale", c.prior_scale}, {"roughness", c.roughness},
           {"min_half_width_vol", c.min_half_width_vol}, {"mult_floor", c.mult_floor},
           {"mult_ceil", c.mult_ceil}, {"max_iterations", c.max_iterations},
           {"min_channel_quotes", c.min_channel_quotes},
           {"repair_half_spreads", c.repair_half_spreads}};
  // A family solver is provenance only when selected (native knot spline otherwise).
  if (c.family) {
    out["family"] = to_string(*c.family);
    out["family_iterations"] = c.family_iterations;
  }
  if (c.point_max_outside_fraction) {
    out["point_max_outside_fraction"] = *c.point_max_outside_fraction;
    out["point_min_outside_quotes"] = c.point_min_outside_quotes;
  }
  return out;
}

// Opt-in fields are provenance only when used, so the default runtime
// definition (and every default-mode report) stays unchanged.
inline Json profile_json_with_options(const QuoteFitProfile &profile, bool graded) {
  Json out = profile_json(profile);
  const auto &c = profile.config;
  if (graded || c.method != QuoteSliceFitMethod::PointCurve) {
    out["method"] = c.method == QuoteSliceFitMethod::ChannelSpline ? "knot-spline" : "point";
    out["channel"] = channel_json(c.channel);
  }
  return out;
}

// Vol-space channel controls (graded fit_space / deam_profile / deam_cache / deam_inversion /
// confirm / confirm_k / confirm_kernel / grid_kernel; quote_slice.hpp QuoteSliceFitSpace),
// applied to every profile like the repair. The member defaults are the library's historical
// path: a static (non-graded) policy keeps them; a graded policy starts from
// graded_default_fit_space() instead.
struct FitSpaceOptions {
  QuoteSliceFitSpace fit_space = QuoteSliceFitSpace::PriceChannel;
  QuoteSliceDeamProfile deam_profile = QuoteSliceDeamProfile::Accurate;
  bool deam_cache = false;
  QuoteSliceDeamInversion deam_inversion = QuoteSliceDeamInversion::Search; // L2e
  // Seed-tier confirm policy / kernel and certification-grid kernel (SPY1s P1).
  QuoteSliceDeamConfirm confirm = QuoteSliceDeamConfirm::Always;
  double confirm_k = 10.0;
  QuoteSliceDeamConfirmKernel confirm_kernel = QuoteSliceDeamConfirmKernel::Accurate;
  QuoteSliceGridKernel grid_kernel = QuoteSliceGridKernel::Accurate;
};

// The graded default (PM ruling R3, integrate-W2c-a 2026-10-21): SPY1s Phase 1 arm D - the
// vol-space channel on the per-thread de-Am IV cache (L2a), Premium inversion (L2e) seeded on
// AlProfile::Screen with bound-decided band edges, every accepted root confirmed by one
// AlProfile::Fast solve (confirm always), the Fast certification grid, and the per-expiry
// rung memo (L2c; kGradedDefaultRungMemo, QuoteSurfaceConfig::rung_memo). Each key still opts
// out on its own (parse_fit_space): fit_space price (every vol-space control then takes the
// library value), deam_inversion search (deam_profile then defaults to accurate), deam_profile
// fast (deam_inversion then defaults to search), confirm band / never, confirm_kernel
// reference, grid_kernel reference, rung_memo false. {fit_space price, grid_kernel reference,
// rung_memo false} is the pre-R3 graded path byte for byte (the kill switch).
[[nodiscard]] inline FitSpaceOptions graded_default_fit_space() {
  FitSpaceOptions out;
  out.fit_space = QuoteSliceFitSpace::VolChannel;
  out.deam_profile = QuoteSliceDeamProfile::Screen;
  out.deam_cache = true;
  out.deam_inversion = QuoteSliceDeamInversion::Premium;
  out.confirm = QuoteSliceDeamConfirm::Always;
  out.confirm_kernel = QuoteSliceDeamConfirmKernel::Fast;
  out.grid_kernel = QuoteSliceGridKernel::Fast;
  return out;
}
inline constexpr bool kGradedDefaultRungMemo = true;

// The graded keys over graded_default_fit_space(). Throws std::runtime_error on an unknown
// value, a vol-space key under fit_space price, or a combination the library refuses.
[[nodiscard]] inline FitSpaceOptions parse_fit_space(const Json &graded) {
  FitSpaceOptions out = graded_default_fit_space();
  const FitSpaceOptions library{};
  if (graded.contains("fit_space")) {
    const auto name = text(graded.at("fit_space"));
    if (name == "price") {
      out.fit_space = QuoteSliceFitSpace::PriceChannel;
    } else if (name != "vol") {
      throw std::runtime_error("Policy graded fit_space must be price or vol");
    }
  }
  // Under the price-space channel both keys would be silently inert: refuse them.
  if ((graded.contains("deam_profile") || graded.contains("deam_cache")) &&
      out.fit_space != QuoteSliceFitSpace::VolChannel) {
    throw std::runtime_error("Policy graded deam_profile and deam_cache require fit_space vol");
  }
  // The price channel ignores every vol-space control; carry the library values so that the
  // profile config (and the policy provenance) is the historical one exactly.
  if (out.fit_space != QuoteSliceFitSpace::VolChannel) {
    out.deam_profile = library.deam_profile;
    out.deam_cache = library.deam_cache;
    out.deam_inversion = library.deam_inversion;
  }
  if (graded.contains("deam_profile")) {
    const auto name = text(graded.at("deam_profile"));
    if (name == "accurate") {
      out.deam_profile = QuoteSliceDeamProfile::Accurate;
    } else if (name == "fast") {
      out.deam_profile = QuoteSliceDeamProfile::Fast;
    } else if (name == "screen") {
      out.deam_profile = QuoteSliceDeamProfile::Screen;
    } else {
      throw std::runtime_error("Policy graded deam_profile must be accurate, fast or screen");
    }
  }
  if (graded.contains("deam_cache")) {
    if (!graded.at("deam_cache").is_boolean()) {
      throw std::runtime_error("Policy graded deam_cache must be true or false");
    }
    out.deam_cache = graded.at("deam_cache").get<bool>();
  }
  // Premium inversion of the cache (quote_slice.hpp QuoteSliceDeamInversion): inert under
  // the price channel and undefined on the Fast kernel, so refused there.
  if (graded.contains("deam_inversion")) {
    if (out.fit_space != QuoteSliceFitSpace::VolChannel) {
      throw std::runtime_error("Policy graded deam_inversion requires fit_space vol");
    }
    const auto name = text(graded.at("deam_inversion"));
    if (name == "premium") {
      out.deam_inversion = QuoteSliceDeamInversion::Premium;
    } else if (name == "search") {
      out.deam_inversion = QuoteSliceDeamInversion::Search;
    } else {
      throw std::runtime_error("Policy graded deam_inversion must be search or premium");
    }
    if (out.deam_inversion == QuoteSliceDeamInversion::Premium &&
        out.deam_profile == QuoteSliceDeamProfile::Fast) {
      throw std::runtime_error("Policy graded deam_inversion premium requires deam_profile "
                               "accurate or screen");
    }
  }
  // An explicit choice resolves its partner's default: Premium is undefined on the Fast
  // kernel (deam_profile fast alone means Search), and the Screen seed tier exists only
  // inside Premium (deam_inversion search alone means the accurate kernel).
  if (!graded.contains("deam_inversion") && out.deam_profile == QuoteSliceDeamProfile::Fast) {
    out.deam_inversion = QuoteSliceDeamInversion::Search;
  }
  if (!graded.contains("deam_profile") && out.deam_inversion == QuoteSliceDeamInversion::Search) {
    out.deam_profile = QuoteSliceDeamProfile::Accurate;
  }
  // The Screen seed tier exists only inside the Premium inversion (quote_slice.hpp
  // QuoteSliceDeamProfile::Screen): a Search fit on Screen would serve seed-grade values.
  if (out.deam_profile == QuoteSliceDeamProfile::Screen &&
      out.deam_inversion != QuoteSliceDeamInversion::Premium) {
    throw std::runtime_error("Policy graded deam_profile screen requires deam_inversion premium");
  }
  // Confirm policy / kernel of a Screen-seeded root (quote_slice.hpp deam_confirm,
  // deam_confirm_k, deam_confirm_kernel): inert without a seed tier, so refused there; the
  // library refuses any non-library value off Screen, so those take the library values.
  if ((graded.contains("confirm") || graded.contains("confirm_k") ||
       graded.contains("confirm_kernel")) &&
      out.deam_profile != QuoteSliceDeamProfile::Screen) {
    throw std::runtime_error("Policy graded confirm keys require deam_profile screen");
  }
  if (out.deam_profile != QuoteSliceDeamProfile::Screen) {
    out.confirm = library.confirm;
    out.confirm_k = library.confirm_k;
    out.confirm_kernel = library.confirm_kernel;
  }
  if (graded.contains("confirm")) {
    const auto name = text(graded.at("confirm"));
    if (name == "always") {
      out.confirm = QuoteSliceDeamConfirm::Always;
    } else if (name == "band") {
      out.confirm = QuoteSliceDeamConfirm::Band;
    } else if (name == "never") {
      out.confirm = QuoteSliceDeamConfirm::Never;
    } else {
      throw std::runtime_error("Policy graded confirm must be always, band or never");
    }
  }
  if (graded.contains("confirm_k")) {
    if (!graded.at("confirm_k").is_number()) {
      throw std::runtime_error("Policy graded confirm_k must be a positive number");
    }
    out.confirm_k = graded.at("confirm_k").get<double>();
    if (!(out.confirm_k > 0.0) || !std::isfinite(out.confirm_k)) {
      throw std::runtime_error("Policy graded confirm_k must be a positive number");
    }
  }
  if (graded.contains("confirm_kernel")) {
    const auto name = text(graded.at("confirm_kernel"));
    if (name == "fast") {
      out.confirm_kernel = QuoteSliceDeamConfirmKernel::Fast;
    } else if (name == "reference") {
      out.confirm_kernel = QuoteSliceDeamConfirmKernel::Accurate;
    } else {
      throw std::runtime_error("Policy graded confirm_kernel must be reference or fast");
    }
  }
  // Certification-grid kernel (quote_slice.hpp QuoteSliceGridKernel): independent of the
  // cache keys (it acts on the served reprice of every graded profile).
  if (graded.contains("grid_kernel")) {
    const auto name = text(graded.at("grid_kernel"));
    if (name == "fast") {
      out.grid_kernel = QuoteSliceGridKernel::Fast;
    } else if (name == "reference") {
      out.grid_kernel = QuoteSliceGridKernel::Accurate;
    } else {
      throw std::runtime_error("Policy graded grid_kernel must be reference or fast");
    }
  }
  return out;
}

// Opt-in graded keys (channel repair, channel dominance) and every vol-space / grid / memo
// control that differs from the historical path are provenance only when set, so the
// kill-switch definition {fit_space price, grid_kernel reference, rung_memo false} prints
// the pre-R3 graded definition unchanged.
inline Json surface_json(const QuoteSurfaceConfig &s, const QuoteSliceRepairConfig &repair,
                         const FitSpaceOptions &space = {}) {
  Json out{{"term_prior_weight", s.term_prior_weight},
           {"prior_decay_log_time", s.prior_decay_log_time},
           {"prior_quote_halflife", s.prior_quote_halflife},
           {"channel_node_weight", s.channel_node_weight},
           {"calendar_retry_factor", s.calendar_retry_factor},
           {"grade_b_min_mid_strikes", s.grade_b_min_mid_strikes},
           {"calendar_repair_half_spreads", s.calendar_repair_half_spreads}};
  if (repair.mode == QuoteSliceRepairMode::Channel) {
    out["repair"] = "channel";
    out["repair_max_nodes"] = repair.max_nodes;
    out["repair_max_vol"] = repair.max_vol_move;
    out["repair_immaterial_price"] = repair.immaterial_price_move;
  }
  if (s.ladder == QuoteSurfaceLadder::KnotFirst) {
    out["ladder"] = "knot-first";
    out["ladder_max_outside_fraction"] = s.ladder_max_outside_fraction;
    out["ladder_channel_max_outside_fraction"] = s.ladder_channel_max_outside_fraction;
    out["ladder_min_outside_quotes"] = s.ladder_min_outside_quotes;
  }
  if (space.fit_space == QuoteSliceFitSpace::VolChannel) {
    out["fit_space"] = "vol";
    out["deam_profile"] = space.deam_profile == QuoteSliceDeamProfile::Fast     ? "fast"
                          : space.deam_profile == QuoteSliceDeamProfile::Screen ? "screen"
                                                                                : "accurate";
    out["deam_cache"] = space.deam_cache;
    if (space.deam_inversion == QuoteSliceDeamInversion::Premium) {
      out["deam_inversion"] = "premium"; // provenance only when opted in
    }
    if (space.deam_profile == QuoteSliceDeamProfile::Screen) {
      out["confirm"] = space.confirm == QuoteSliceDeamConfirm::Band    ? "band"
                       : space.confirm == QuoteSliceDeamConfirm::Never ? "never"
                                                                       : "always";
      out["confirm_k"] = space.confirm_k;
      out["confirm_kernel"] =
          space.confirm_kernel == QuoteSliceDeamConfirmKernel::Fast ? "fast" : "reference";
    }
  }
  if (space.grid_kernel == QuoteSliceGridKernel::Fast) out["grid_kernel"] = "fast";
  if (s.rung_memo) out["rung_memo"] = true;
  return out;
}

inline Json regimes_json(const QuoteFitRegimeConfig &r) {
  return {{"dense_profile", r.dense_profile}, {"ordinary_profile", r.ordinary_profile},
          {"sparse_profile", r.sparse_profile}, {"near_money_log_band", r.near_money_log_band},
          {"dense_min_median_strikes", r.dense_min_median_strikes},
          {"dense_max_median_relative_spread", r.dense_max_median_relative_spread},
          {"dense_max_bidless_fraction", r.dense_max_bidless_fraction},
          {"sparse_min_median_strikes", r.sparse_min_median_strikes},
          {"sparse_min_median_relative_spread", r.sparse_min_median_relative_spread},
          {"sparse_min_bidless_fraction", r.sparse_min_bidless_fraction}};
}

inline Json read_document(const std::filesystem::path &path) {
  constexpr std::uintmax_t limit = 1024U * 1024U;
  const auto size = std::filesystem::file_size(path);
  if (size == 0U || size > limit) throw std::runtime_error("Policy JSON must be 1 byte..1 MiB");
  std::ifstream stream(path, std::ios::binary);
  std::string bytes(static_cast<std::size_t>(size), '\0');
  if (!stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) ||
      stream.peek() != std::char_traits<char>::eof()) {
    throw std::runtime_error("Policy JSON read failed or file changed while reading");
  }
  std::vector<std::set<std::string>> objects;
  return Json::parse(bytes, [&](int depth, Json::parse_event_t event, Json &value) {
    if (depth > 16) throw std::runtime_error("Policy JSON nesting exceeds 16 levels");
    if (event == Json::parse_event_t::object_start) objects.emplace_back();
    else if (event == Json::parse_event_t::object_end) objects.pop_back();
    else if (event == Json::parse_event_t::key &&
             !objects.back().insert(value.get<std::string>()).second) {
      throw std::runtime_error("Duplicate policy JSON key");
    }
    return true;
  });
}

struct LoadedPolicy {
  QuoteFitPolicyCatalog catalog;
  Json definition;
  // Profiles that opted out of the aggregate dividend model, by profile name
  // (the catalog refuses duplicate names). Empty on the historical path.
  std::map<std::string, DividendRouting, std::less<>> dividend_routing;
  bool graded{false};
  QuoteSurfaceConfig surface{};
  // Opt-in (top-level `price_only`, static and graded): a served row whose
  // certified price has no identifiable volatility (PriceOnly) is published as
  // status ok_price_only with that price and a blank volatility.
  bool price_only{false};
};

inline LoadedPolicy load(const std::string &selection, QuoteSliceDividendModel dividends) {
  const Json document = selection == "auto" ? Json{{"schema_version", 1}} :
                                             read_document(selection);
  keys(document, {"schema_version", "profiles", "symbol_overrides", "regimes", "admission",
                  "graded", "price_only"});
  bool price_only = false;
  if (document.contains("price_only")) {
    if (!document.at("price_only").is_boolean()) {
      throw std::runtime_error("Policy price_only must be true or false");
    }
    price_only = document.at("price_only").get<bool>();
  }
  if (!document.contains("schema_version") || !document.at("schema_version").is_number_integer() ||
      document.at("schema_version") != 1) {
    throw std::runtime_error("Policy schema_version must be 1");
  }
  const auto defaults = default_quote_fit_profiles();
  std::vector<QuoteFitProfile> profiles = defaults;
  std::vector<std::vector<double>> grids;
  std::map<std::string, DividendRouting, std::less<>> routing;
  if (document.contains("profiles")) {
    const auto &list = document.at("profiles");
    if (!list.is_array() || list.empty() || list.size() > 64U) {
      throw std::runtime_error("Policy profiles must contain 1..64 definitions");
    }
    profiles.clear();
    profiles.reserve(list.size());
    grids.resize(list.size());
    for (std::size_t i = 0; i < list.size(); ++i) {
      const auto base = text(list[i].at("base"), true);
      const auto found = std::find_if(defaults.begin(), defaults.end(),
                                      [&](const auto &profile) { return profile.name == base; });
      if (found == defaults.end()) {
        throw std::runtime_error("Unknown built-in policy base: " + base);
      }
      auto profile = *found;
      parse_profile(list[i], profile, grids[i]);
      const auto dividends = parse_dividend_routing(list[i]);
      // The lattice keys act only on routed chains; on an aggregate profile they
      // would be silently inert, so they are refused instead.
      if (dividends == DividendRouting::Aggregate &&
          (list[i].contains("dividend_lattice") || list[i].contains("dividend_lattice_steps"))) {
        throw std::runtime_error("Policy dividend_lattice and dividend_lattice_steps require "
                                 "dividends inferred-schedule or inferred-schedule-exact");
      }
      if (dividends != DividendRouting::Aggregate) routing[profile.name] = dividends;
      profiles.push_back(std::move(profile));
    }
  }
  std::vector<QuoteFitSymbolOverride> overrides;
  if (document.contains("symbol_overrides")) {
    const auto &list = document.at("symbol_overrides");
    if (!list.is_array() || list.size() > 4096U) {
      throw std::runtime_error("Policy permits at most 4096 overrides");
    }
    for (const auto &entry : list) {
      keys(entry, {"symbol", "profile"});
      overrides.push_back({text(entry.at("symbol")), text(entry.at("profile"), true)});
    }
  }
  QuoteFitRegimeConfig regimes;
  if (document.contains("regimes")) {
    const auto &j = document.at("regimes");
    keys(j, {"dense_profile", "ordinary_profile", "sparse_profile", "near_money_log_band",
             "dense_min_median_strikes", "dense_max_median_relative_spread",
             "dense_max_bidless_fraction", "sparse_min_median_strikes",
             "sparse_min_median_relative_spread", "sparse_min_bidless_fraction"});
    using ProfileField = std::pair<const char *, std::string QuoteFitRegimeConfig::*>;
    for (const auto &[key, member] : std::initializer_list<ProfileField>{
             {"dense_profile", &QuoteFitRegimeConfig::dense_profile},
             {"ordinary_profile", &QuoteFitRegimeConfig::ordinary_profile},
             {"sparse_profile", &QuoteFitRegimeConfig::sparse_profile}}) {
      if (j.contains(key)) regimes.*member = text(j.at(key), true);
    }
    numeric(j, "near_money_log_band", regimes.near_money_log_band, 1.0e-6, 5);
    numeric(j, "dense_min_median_strikes", regimes.dense_min_median_strikes, 4, 100000);
    numeric(j, "sparse_min_median_strikes", regimes.sparse_min_median_strikes, 4, 100000);
    numeric(j, "dense_max_median_relative_spread",
            regimes.dense_max_median_relative_spread, 1.0e-6, 2);
    numeric(j, "sparse_min_median_relative_spread",
            regimes.sparse_min_median_relative_spread, 1.0e-6, 2);
    numeric(j, "dense_max_bidless_fraction", regimes.dense_max_bidless_fraction, 0, 1);
    numeric(j, "sparse_min_bidless_fraction", regimes.sparse_min_bidless_fraction, 0, 1);
  }
  bool graded = false;
  if (document.contains("admission")) {
    const auto mode = text(document.at("admission"));
    if (mode == "graded") graded = true;
    else if (mode != "static") {
      throw std::runtime_error("Policy admission must be static or graded");
    }
  }
  QuoteSurfaceConfig surface;
  surface.rung_memo = graded && kGradedDefaultRungMemo; // R3 graded default
  QuoteSliceRepairConfig repair; // graded only; applied to every profile
  // Applied to every profile: the library values for a static policy, R3's for a graded one.
  FitSpaceOptions space = graded ? graded_default_fit_space() : FitSpaceOptions{};
  if (document.contains("graded")) {
    if (!graded) throw std::runtime_error("Policy graded options require admission graded");
    const auto &j = document.at("graded");
    keys(j, {"term_prior_weight", "prior_decay_log_time", "prior_quote_halflife",
             "channel_node_weight", "calendar_retry_factor", "grade_b_min_mid_strikes",
             "calendar_repair_half_spreads", "repair", "repair_max_nodes", "repair_max_vol",
             "repair_immaterial_price", "ladder", "ladder_max_outside_fraction",
             "ladder_channel_max_outside_fraction", "ladder_min_outside_quotes", "fit_space",
             "deam_profile", "deam_cache", "rung_memo", "deam_inversion", "confirm", "confirm_k",
             "confirm_kernel", "grid_kernel"});
    space = parse_fit_space(j);
    // Per-expiry rung memo (quote_slice.hpp QuoteSurfaceConfig::rung_memo): exact-keyed
    // reuse across an expiry's rungs, speed only; on by default under graded admission (R3).
    if (j.contains("rung_memo")) {
      if (!j.at("rung_memo").is_boolean()) {
        throw std::runtime_error("Policy graded rung_memo must be true or false");
      }
      surface.rung_memo = j.at("rung_memo").get<bool>();
    }
    // Opt-in knot-first ladder (quote_slice.hpp fit_quote_surface); "off" is the
    // historical point-first admission.
    if (j.contains("ladder")) {
      const auto mode = text(j.at("ladder"));
      if (mode == "knot-first") {
        surface.ladder = QuoteSurfaceLadder::KnotFirst;
      } else if (mode != "off") {
        throw std::runtime_error("Policy graded ladder must be off or knot-first");
      }
    }
    if ((j.contains("ladder_max_outside_fraction") ||
         j.contains("ladder_channel_max_outside_fraction") ||
         j.contains("ladder_min_outside_quotes")) &&
        surface.ladder != QuoteSurfaceLadder::KnotFirst) {
      throw std::runtime_error("Policy graded ladder containment keys require ladder knot-first");
    }
    numeric(j, "ladder_max_outside_fraction", surface.ladder_max_outside_fraction, 0, 1);
    numeric(j, "ladder_channel_max_outside_fraction", surface.ladder_channel_max_outside_fraction,
            0, 1);
    numeric(j, "ladder_min_outside_quotes", surface.ladder_min_outside_quotes, 1, 100000);
    numeric(j, "term_prior_weight", surface.term_prior_weight, 0, 1.0e6);
    numeric(j, "prior_decay_log_time", surface.prior_decay_log_time, 1.0e-3, 100);
    numeric(j, "prior_quote_halflife", surface.prior_quote_halflife, 1.0e-3, 1.0e6);
    numeric(j, "channel_node_weight", surface.channel_node_weight, 1.0e-6, 1.0e6);
    numeric(j, "calendar_retry_factor", surface.calendar_retry_factor, 1, 1.0e6);
    numeric(j, "grade_b_min_mid_strikes", surface.grade_b_min_mid_strikes, 1, 100000);
    numeric(j, "calendar_repair_half_spreads", surface.calendar_repair_half_spreads, 0, 100);
    // Opt-in channel repair (quote_slice.hpp QuoteSliceRepairConfig): a fallback
    // rung under graded admission; "off" is the historical behaviour.
    if (j.contains("repair")) {
      const auto mode = text(j.at("repair"));
      if (mode == "channel") {
        repair.mode = QuoteSliceRepairMode::Channel;
      } else if (mode != "off") {
        throw std::runtime_error("Policy graded repair must be off or channel");
      }
    }
    if ((j.contains("repair_max_nodes") || j.contains("repair_max_vol") ||
         j.contains("repair_immaterial_price")) &&
        repair.mode != QuoteSliceRepairMode::Channel) {
      throw std::runtime_error("Policy graded repair budgets require repair channel");
    }
    numeric(j, "repair_max_nodes", repair.max_nodes, 1, 100000);
    numeric(j, "repair_max_vol", repair.max_vol_move, 1.0e-6, 10);
    numeric(j, "repair_immaterial_price", repair.immaterial_price_move, 0, 1.0e3);
  }
  Json definition{{"schema_version", 1}, {"profiles", Json::array()},
                  {"symbol_overrides", Json::array()}, {"regimes", regimes_json(regimes)}};
  if (graded) {
    definition["admission"] = "graded";
    definition["graded"] = surface_json(surface, repair, space);
  }
  if (price_only) definition["price_only"] = true; // provenance only when opted in
  for (auto &profile : profiles) {
    profile.config.dividend_model = dividends;
    profile.config.repair = repair;
    profile.config.channel.fit_space = space.fit_space;
    profile.config.channel.deam_profile = space.deam_profile;
    profile.config.channel.deam_cache = space.deam_cache;
    profile.config.channel.deam_inversion = space.deam_inversion;
    profile.config.channel.deam_confirm = space.confirm;
    profile.config.channel.deam_confirm_k = space.confirm_k;
    profile.config.channel.deam_confirm_kernel = space.confirm_kernel;
    profile.config.grid_kernel = space.grid_kernel;
    // The ladder brings its own knot rung and containment rule: a knot-spline
    // profile (already knot-first) or the dominance gate would be ambiguous.
    if (surface.ladder == QuoteSurfaceLadder::KnotFirst &&
        profile.config.method == QuoteSliceFitMethod::ChannelSpline) {
      throw std::runtime_error("Policy graded ladder knot-first requires point profiles; profile " +
                               profile.name + " is already curve knot-spline");
    }
    if (surface.ladder == QuoteSurfaceLadder::KnotFirst &&
        profile.config.channel.point_max_outside_fraction) {
      throw std::runtime_error("Policy graded ladder knot-first replaces the dominance gate; "
                               "drop channel point_max_outside_fraction");
    }
    // Policy-key rejection matrix for graded admission: it requires supplied
    // carry (no carry_mode) and the aggregate dividend model. A lattice-routed
    // expiry needs its own dated-cash config, schedule and aggregate fallback,
    // which fit_quote_surface (one config per snapshot) does not take yet.
    const auto routed = routing.find(profile.name);
    if (!graded && profile.config.channel.point_max_outside_fraction) {
      throw std::runtime_error(
          "Policy channel point_max_outside_fraction requires admission graded");
    }
    if (graded && profile.config.carry.mode != QuoteSliceCarryMode::Supplied) {
      throw std::runtime_error("Policy admission graded requires supplied carry");
    }
    if (graded && routed != routing.end()) {
      throw std::runtime_error(
          "Policy admission graded requires dividends aggregate (inferred-schedule routing "
          "is not supported under graded admission)");
    }
    auto json = profile_json_with_options(profile, graded);
    // Emitted only when opted in, so the aggregate-path provenance is unchanged.
    if (routed != routing.end()) json["dividends"] = to_string(routed->second);
    definition["profiles"].push_back(std::move(json));
  }
  for (const auto &entry : overrides) {
    definition["symbol_overrides"].push_back(
        {{"symbol", entry.symbol}, {"profile", entry.profile}});
  }
  auto catalog = QuoteFitPolicyCatalog::create(profiles, overrides, regimes);
  if (!catalog) throw std::runtime_error(catalog.error().to_string());
  return {std::move(*catalog), std::move(definition), std::move(routing), graded, surface,
          price_only};
}

inline Json features_json(const QuoteFitLiquidityFeatures &f) {
  const auto &in = f.classifier_inputs;
  return {{"rows_seen", f.rows_seen}, {"invalid_rows", f.invalid_rows},
          {"unique_contracts", f.unique_contracts}, {"two_sided_contracts", f.two_sided_contracts},
          {"bidless_contracts", f.bidless_contracts},
          {"near_money_contracts", f.near_money_contracts},
          {"near_money_two_sided_contracts", f.near_money_two_sided_contracts},
          {"near_money_bidless_contracts", f.near_money_bidless_contracts},
          {"bidless_fraction", f.bidless_fraction},
          {"near_money_bidless_fraction", f.near_money_bidless_fraction},
          {"median_relative_spread", f.median_relative_spread ?
              Json(*f.median_relative_spread) : Json(nullptr)},
          {"median_near_money_strikes", f.median_near_money_strikes},
          {"max_near_money_strikes", f.max_near_money_strikes},
          {"classifier", {{"kind", f.classifier.kind}, {"confidence", f.classifier.confidence}}},
          {"classifier_inputs", {{"n_live_quotes", in.n_live_quotes},
             {"n_atm_quotes", in.n_atm_quotes},
             {"n_live_expiries", in.n_live_expiries}, {"n_quoted_expiries", in.n_quoted_expiries},
             {"n_identifiable_expiries", in.n_identifiable_expiries},
             {"max_near_money_strikes", in.max_near_money_strikes},
             {"n_front_expiries", in.n_front_expiries},
             {"median_spread_pct", std::isfinite(in.median_spread_pct) ?
                 Json(in.median_spread_pct) : Json(nullptr)},
             {"has_weeklies", in.has_weeklies}, {"htb_flag", in.htb_flag},
             {"vol_product", in.vol_product},
             {"n_dividends", in.n_dividends}, {"event_distance_days", in.event_distance_days},
             {"forward_dispersion_bp", in.forward_dispersion_bp},
             {"median_q_eff", in.median_q_eff}}}};
}
} // namespace quote_bench_policy
