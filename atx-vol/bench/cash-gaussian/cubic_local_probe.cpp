#include "cubic_local_weights.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include <boost/multiprecision/cpp_bin_float.hpp>
#include <boost/version.hpp>
#include <nlohmann/json.hpp>

namespace {
namespace cg = atx::vol::cash_gaussian;
using Json = nlohmann::json;

const char *route_name(cg::LocalWeightRoute route) {
  switch (route) {
    case cg::LocalWeightRoute::NotSelected: return "not_selected";
    case cg::LocalWeightRoute::OriginalMoments: return "original_moments";
    case cg::LocalWeightRoute::LocalSeries: return "local_series";
  }
  throw std::runtime_error("unknown local weight route");
}

const char *reason_name(cg::LocalWeightReason reason) {
  switch (reason) {
    case cg::LocalWeightReason::None: return "none";
    case cg::LocalWeightReason::ZeroLower: return "zero_lower";
    case cg::LocalWeightReason::WideCell: return "wide_cell";
    case cg::LocalWeightReason::DensityVariation: return "density_variation";
    case cg::LocalWeightReason::OrderCap: return "order_cap";
  }
  throw std::runtime_error("unknown local weight reason");
}

Json run() {
  const auto next = [](double x) {
    return std::nextafter(x, std::numeric_limits<double>::infinity());
  };
  // Exact original twelve inputs, in the original order; no reference inputs.
  const std::array<std::pair<const char *, cg::WeightRequest>, 12> cases{{
      {"central", {80., 120., 100., .7, .35, .03, .01}},
      {"zero-lower", {0., 100., 80., .5, .2, -.03, .02}},
      {"adjacent-high", {1e12, next(1e12), 1e12, 1., .2, 0., 0.}},
      {"adjacent-low", {1e-12, next(1e-12), 1e-12, 1., .2, 0., 0.}},
      {"adjacent-unit", {1., next(1.), 1., 1., .2, 0., 0.}},
      {"lower-tail", {1., 2., 100., 1., .25, .01, .03}},
      {"upper-tail", {400., 450., 100., 1., .25, .01, .03}},
      {"positive-near-zero-time", {100., next(100.), 100., 1e-16, .2, 0., 0.}},
      {"negative-rate", {40., 180., 100., 3., .4, -.1, .07}},
      {"negative-yield", {70., 140., 100., 2., .5, .02, -.05}},
      {"small-variance", {99.99, 100.01, 100., .01, .001, 0., 0.}},
      {"wide", {1., 1000., 100., 5., 1., 0., 0.}},
  }};
  Json rows = Json::array();
  for (const auto &[id, request] : cases) {
    Json input, input_bits;
    for (const auto &[name, value] : std::array<std::pair<const char *, double>, 7>{{
        {"lower", request.lower}, {"upper", request.upper}, {"stock", request.stock},
        {"dt", request.dt}, {"sigma", request.sigma}, {"rate", request.rate},
        {"yield", request.yield}}}) {
      input[name] = value;
      input_bits[name] = std::to_string(std::bit_cast<std::uint64_t>(value));
    }
    for (const auto precision : {cg::WeightPrecision::Decimal100,
                                cg::WeightPrecision::Decimal200}) {
      cg::LocalWeightWork work;
      const auto result = cg::cubic_local_weight_control(request, precision, {}, work);
      Json row{{"id", id}, {"input", input}, {"input_bits", input_bits},
          {"requested_digits", precision == cg::WeightPrecision::Decimal100 ? 100 : 200},
          {"status", result ? "success" : "refused"},
          {"error", result ? "" : result.error().to_string()},
          {"error_code", result ? 0 : static_cast<int>(result.error().code())},
          {"work", {{"route", route_name(work.route)}, {"reason", reason_name(work.reason)},
              {"selection_checks", work.selection_checks},
              {"coefficient_products", work.coefficient_products},
              {"local_functions", work.local_functions}, {"workspace_bytes", work.workspace_bytes},
              {"log_order", work.log_order}, {"exponential_order", work.exponential_order},
              {"used_log1p", work.used_log1p},
              {"moments", {{"moment_attempts", work.moments.moment_attempts},
                  {"normal_functions", work.moments.normal_functions},
                  {"polynomial_terms", work.moments.polynomial_terms},
                  {"decimal_digits", work.moments.decimal_digits}}}}}};
      row["weights"] = nullptr;
      row["local"] = nullptr;
      if (result) {
        const auto &value = result->weights;
        row["weights"] = {{"value", value.value}, {"derivative", value.derivative},
            {"scaled_moments", value.scaled_moments}, {"mass", value.mass},
            {"discounted_stock_ratio", value.discounted_stock_ratio},
            {"maximum_condition", value.maximum_condition}};
        row["local"] = {{"value_conditions", result->value_conditions},
            {"derivative_conditions", result->derivative_conditions},
            {"value_remainders", result->value_remainders},
            {"derivative_remainders", result->derivative_remainders},
            {"density_remainder", result->density_remainder},
            {"relative_value_remainder", result->relative_value_remainder},
            {"relative_derivative_remainder", result->relative_derivative_remainder}};
      }
      rows.push_back(std::move(row));
    }
  }
  using R100 = boost::multiprecision::number<boost::multiprecision::cpp_bin_float<100>>;
  using R200 = boost::multiprecision::number<boost::multiprecision::cpp_bin_float<200>>;
  return {{"schema", "cash-cubic-local-weight-control-v1"},
      {"declaration", "2a078f20cd743367dfba0793c4632e7e30ca19fc"},
      {"primitive_source", "8436a3b978a1dae98cb4c5ec988b07001bfffa5c"},
      {"boost_version", BOOST_LIB_VERSION}, {"timed", false},
      {"full_price_error_bound", false}, {"semigroup_run", false},
      {"scalar_object_bytes", {{"100", sizeof(R100)}, {"200", sizeof(R200)}}},
      {"limits", {{"max_moments", 4}, {"max_normal_functions", 8},
          {"max_abs_standardized_endpoint", 256}, {"max_abs_log_prefactor", 32768},
          {"max_series_iterations", 4096}, {"max_root_iterations", 256},
          {"max_selection_checks", 256}, {"max_coefficient_products", 262144},
          {"max_local_functions", 64}, {"max_workspace_bytes", 1048576},
          {"max_log_order", 16}, {"max_exponential_order", 16},
          {"coefficient_buffer_entries", 514}}}, {"rows", std::move(rows)}};
}
} // namespace

int main(int argc, char **) {
  try {
    if (argc != 1) throw std::runtime_error("fixed local weight control accepts no arguments");
    std::cout << run().dump(2) << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "cash-cubic-local-weight-control: " << error.what() << '\n';
    return 2;
  }
}
