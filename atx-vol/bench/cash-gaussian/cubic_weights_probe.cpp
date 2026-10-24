#include "cubic_weights.hpp"

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

#include <boost/version.hpp>
#include <nlohmann/json.hpp>

namespace {
namespace cg = atx::vol::cash_gaussian;
using Json = nlohmann::json;

Json run() {
  const auto next = [](double x) {
    return std::nextafter(x, std::numeric_limits<double>::infinity());
  };
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
      cg::WeightWork work;
      const auto result = cg::cubic_weight_control(request, precision, {}, work);
      Json row{{"id", id}, {"input", input}, {"input_bits", input_bits},
          {"requested_digits", precision == cg::WeightPrecision::Decimal100 ? 100 : 200},
          {"status", result ? "success" : "refused"},
          {"error", result ? "" : result.error().to_string()},
          {"error_code", result ? 0 : static_cast<int>(result.error().code())},
          {"work", {{"moment_attempts", work.moment_attempts},
              {"normal_functions", work.normal_functions},
              {"polynomial_terms", work.polynomial_terms},
              {"decimal_digits", work.decimal_digits}}}};
      row["weights"] = result ? Json{{"value", result->value},
          {"derivative", result->derivative}, {"scaled_moments", result->scaled_moments},
          {"mass", result->mass}, {"discounted_stock_ratio", result->discounted_stock_ratio},
          {"maximum_condition", result->maximum_condition}} : Json(nullptr);
      rows.push_back(std::move(row));
    }
  }
  return {{"schema", "cash-cubic-weight-control-v1"}, {"declaration", "73dd008e"},
      {"boost_version", BOOST_LIB_VERSION}, {"timed", false},
      {"full_price_error_bound", false}, {"semigroup_run", false},
      {"limits", {{"max_moments", 4}, {"max_normal_functions", 8},
          {"max_abs_standardized_endpoint", 256}, {"max_abs_log_prefactor", 32768},
          {"max_series_iterations", 4096}, {"max_root_iterations", 256}}},
      {"rows", std::move(rows)}};
}
} // namespace

int main(int argc, char **) {
  try {
    if (argc != 1) throw std::runtime_error("fixed weight control accepts no arguments");
    std::cout << run().dump(2) << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "cash-cubic-weight-control: " << error.what() << '\n';
    return 2;
  }
}
