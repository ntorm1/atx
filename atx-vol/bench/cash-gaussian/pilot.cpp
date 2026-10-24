// Fixed diagnostic experiment: original inputs only, no reference prices or clocks.
#include "event_owner.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/ddiv_matrix_cases.hpp"

namespace {
namespace cg = atx::vol::cash_gaussian;
namespace fixture = atx::vol::test::ddiv_matrix;
using Json = nlohmann::json;
constexpr std::array<std::string_view, 8> ids{
    "A1p", "A1c", "E64p", "E64c", "ILp", "ILc", "DSPYp", "DSPYc"};

const char *cash_name(atx::vol::ddiv_ref::CashProcess process) {
  using Process = atx::vol::ddiv_ref::CashProcess;
  switch (process) {
    case Process::SpotLiquidator: return "spot-liquidator";
    case Process::Escrowed: return "escrowed";
    case Process::SpotSurvivor: return "spot-survivor";
  }
  throw std::runtime_error("unknown raw cash model");
}

Json model_json(const fixture::InputCase &c) {
  Json events = Json::array();
  for (const auto &d : c.divs) events.push_back({{"tau", d.tau}, {"cash", d.amount}});
  return {{"spot", c.S}, {"strike", c.K}, {"maturity", c.T}, {"sigma", c.sigma},
      {"rate", c.r}, {"yield", c.q},
      {"side", c.side == atx::vol::Side::Call ? "call" : "put"},
      {"cash_process", cash_name(c.process)}, {"exercise", "american"},
      {"events", std::move(events)}};
}

Json work_json(const cg::Work &w) {
  return {{"created_knots", w.integral.created_knots},
      {"interval_evaluations", w.integral.interval_evaluations},
      {"expectations", w.integral.expectations},
      {"deterministic_evaluations", w.integral.deterministic_evaluations},
      {"zero_state_evaluations", w.integral.zero_state_evaluations},
      {"planned_steps", w.planned_steps}, {"completed_steps", w.completed_steps},
      {"grid_nodes", w.grid_nodes}, {"jumps", w.jumps},
      {"exercise_projections", w.exercise_projections}, {"intersections", w.intersections},
      {"rounded_intersections", w.rounded_intersections},
      {"deterministic_rollbacks", w.deterministic_rollbacks}};
}

Json schedule_json(const cg::EventOwner &owner) {
  Json events = Json::array(), steps = Json::array(), grid = Json::array();
  for (const auto &e : owner.events()) events.push_back({{"time", e.time}, {"cash", e.cash}});
  for (const auto &s : owner.steps()) steps.push_back({{"from", s.from}, {"to", s.to},
                                                     {"cash_at_from", s.cash_at_from}});
  for (double x : owner.stock_grid()) grid.push_back(x);
  return {{"events", std::move(events)}, {"steps", std::move(steps)},
      {"stock_grid", std::move(grid)}};
}

Json request(const fixture::InputCase &c, std::size_t n, unsigned level) {
  cg::Options options;
  options.positive_intervals = n; options.time_level = level;
  cg::Model model;
  model.spot = c.S; model.strike = c.K; model.maturity = c.T; model.sigma = c.sigma;
  model.rate = c.r; model.yield = c.q; model.side = c.side;
  if (c.process != atx::vol::ddiv_ref::CashProcess::SpotLiquidator)
    throw std::runtime_error("declared pilot includes unsupported raw cash model");
  std::vector<cg::Event> raw;
  raw.reserve(c.divs.size());
  for (const auto &d : c.divs) raw.push_back({d.tau, d.amount});
  cg::Work work;
  const auto owner = cg::EventOwner::prepare(model, raw, options);
  const auto price = owner ? owner->price(work) : atx::core::Err(owner.error());
  if (price && !std::isfinite(*price)) throw std::runtime_error("nonfinite published price");
  std::cerr << c.id << ' ' << n << ' ' << level << ' '
            << (price ? "success" : "refused") << '\n';
  return {{"id", c.id}, {"positive_intervals", n}, {"time_level", level},
      {"status", price ? "success" : "refused"},
      {"phase", owner ? "price" : "prepare"},
      {"error", price ? "" : price.error().to_string()},
      {"error_code", price ? 0 : static_cast<int>(price.error().code())},
      {"price", price ? Json(*price) : Json(nullptr)},
      {"price_binary64", price ? Json(std::to_string(std::bit_cast<std::uint64_t>(*price)))
                                : Json(nullptr)},
      {"schedule", owner ? schedule_json(*owner) : Json(nullptr)}, {"work", work_json(work)}};
}

Json run() {
  const auto cases = fixture::matrix();
  if (cases.size() != 50 || fixture::input_fingerprint(cases) != 12110935955959749584ULL)
    throw std::runtime_error("original 50-case input manifest changed");
  Json manifest = Json::array(), rows = Json::array();
  for (const auto &c : cases) {
    const bool selected = std::find(ids.begin(), ids.end(), c.id) != ids.end();
    manifest.push_back({{"id", c.id}, {"selected", selected},
        {"status", selected ? "requested" : "uncomputed"}, {"input", model_json(c)}});
  }
  for (const auto id : ids) {
    const auto it = std::find_if(cases.begin(), cases.end(),
        [id](const auto &c) { return c.id == id; });
    if (it == cases.end()) throw std::runtime_error("missing predeclared case");
    for (const auto n : {256u, 512u, 1024u}) rows.push_back(request(*it, n, 0));
    if (it->side == atx::vol::Side::Put)
      for (const auto level : {1u, 2u}) rows.push_back(request(*it, 256, level));
  }
  if (rows.size() != 32) throw std::runtime_error("declared outcome count changed");
  return {{"schema", "cash-gaussian-pilot-v1"}, {"timed", false},
      {"declaration_commit", "5023cdf4faf31ffa3fc78434ad2a216a200d8c87"},
      {"primitive_commit", "80ae1ca27a56b2739627f1c7638c4529e8e7ee10"},
      {"owner_commit", "06d83e2fb7d8eeb99dc18894a2ee4501dcc86a38"},
      {"input_fingerprint", "12110935955959749584"},
      {"options", {{"time_base", 128}, {"width_sd", 8.},
          {"max_knots", 8192}, {"max_created_knots", 1U << 24},
          {"max_expectations", 1U << 20}, {"max_interval_evaluations", 1U << 28},
          {"max_events", 64}, {"max_steps", 4096}, {"probability_cutoff", nullptr},
          {"infinite_tail", "linear"}, {"full_interval_integral", true}}},
      {"manifest", std::move(manifest)}, {"rows", std::move(rows)}};
}
} // namespace

int main(int argc, char **) {
  try {
    if (argc != 1) throw std::runtime_error("fixed pilot accepts no arguments");
    std::cout << run().dump(2) << '\n';
    return 0; // Typed per-request refusals remain outcomes, never silently dropped.
  } catch (const std::exception &error) {
    std::cerr << "cash-gaussian-pilot: " << error.what() << '\n';
    return 2;
  }
}
