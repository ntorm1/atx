// Fixed European semigroup and exact failed-crossing diagnostics. No timing.
#include "event_owner.hpp"
#include "semigroup.hpp"

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

std::string bits(double x) { return std::to_string(std::bit_cast<std::uint64_t>(x)); }

const char *process_name(atx::vol::ddiv_ref::CashProcess process) {
  using P = atx::vol::ddiv_ref::CashProcess;
  switch (process) {
    case P::SpotLiquidator: return "spot-liquidator";
    case P::SpotSurvivor: return "spot-survivor";
    case P::Escrowed: return "escrowed";
  }
  throw std::runtime_error("unknown cash process");
}

Json input_json(const fixture::InputCase &c) {
  Json events = Json::array();
  for (const auto &e : c.divs) events.push_back({{"tau", e.tau}, {"cash", e.amount}});
  return {{"spot", c.S}, {"strike", c.K}, {"maturity", c.T}, {"sigma", c.sigma},
      {"rate", c.r}, {"yield", c.q}, {"side", c.side == atx::vol::Side::Call ? "call" : "put"},
      {"exercise", "american"}, {"cash_process", process_name(c.process)},
      {"events", std::move(events)}};
}

cg::Model model(const fixture::InputCase &c) {
  cg::Model m;
  m.spot = c.S; m.strike = c.K; m.maturity = c.T; m.sigma = c.sigma;
  m.rate = c.r; m.yield = c.q; m.side = c.side;
  if (c.process != atx::vol::ddiv_ref::CashProcess::SpotLiquidator)
    throw std::runtime_error("undeclared cash model");
  return m;
}

Json budget_json(const cg::Budget &b) {
  return {{"created_knots", b.created_knots}, {"expectations", b.expectations},
      {"interval_evaluations", b.interval_evaluations},
      {"deterministic_evaluations", b.deterministic_evaluations},
      {"zero_state_evaluations", b.zero_state_evaluations}};
}

Json work_json(const cg::Work &w) {
  auto out = budget_json(w.integral);
  out.update({{"planned_steps", w.planned_steps}, {"completed_steps", w.completed_steps},
      {"grid_nodes", w.grid_nodes}, {"jumps", w.jumps},
      {"exercise_projections", w.exercise_projections}, {"intersections", w.intersections},
      {"rounded_intersections", w.rounded_intersections},
      {"deterministic_rollbacks", w.deterministic_rollbacks}});
  return out;
}

Json semigroup(const fixture::InputCase &c, std::size_t n, std::size_t count) {
  auto m = model(c);
  m.exercise = atx::vol::ExerciseStyle::European;
  cg::Options options;
  options.positive_intervals = n;
  const auto owner = cg::EventOwner::prepare(m, {}, options);
  cg::Budget budget;
  budget.limits = options.limits;
  cg::SemigroupWork work;
  const double dt = m.maturity/static_cast<double>(count);
  std::string phase = "prepare";
  auto price = [&]() -> atx::vol::Result<double> {
    if (!owner) return atx::core::Err(owner.error());
    phase = "payoff";
    const bool call = m.side == atx::vol::Side::Call;
    const std::array<double, 2> x{0., m.strike}, y{call ? 0. : m.strike, 0.};
    auto initial = cg::StockPwl::create(x, y, call ? 1. : 0., budget);
    if (!initial) return atx::core::Err(initial.error());
    phase = "propagate";
    return cg::repeat_expectation(std::move(*initial), owner->stock_grid(), m.spot, dt,
        count, m.sigma, m.rate, m.yield, budget, work);
  }();
  if (price && !std::isfinite(*price)) throw std::runtime_error("nonfinite semigroup price");
  Json grid = Json::array();
  if (owner) for (double x : owner->stock_grid()) grid.push_back(x);
  auto counters = budget_json(budget);
  counters.update({{"completed_steps", work.completed_steps}, {"grid_nodes", work.grid_nodes}});
  auto control = input_json(c);
  control["exercise"] = "european";
  control["events"] = Json::array();
  std::cerr << "semigroup " << c.id << ' ' << n << ' ' << count << ' '
            << (price ? "success" : "refused") << '\n';
  return {{"id", c.id}, {"positive_intervals", n}, {"steps", count},
      {"dt", dt}, {"dt_binary64", bits(dt)}, {"control_model", std::move(control)},
      {"status", price ? "success" : "refused"}, {"phase", phase},
      {"error", price ? "" : price.error().to_string()},
      {"error_code", price ? 0 : static_cast<int>(price.error().code())},
      {"price", price ? Json(*price) : Json(nullptr)},
      {"price_binary64", price ? Json(bits(*price)) : Json(nullptr)},
      {"stock_grid", std::move(grid)}, {"work", std::move(counters)}};
}

struct Snapshot {
  std::vector<double> stock, value;
  cg::RoundedCrossing fields;
};
struct Capture {
  std::vector<Snapshot> records;
  std::size_t attempts = 0;
  bool failed = false;
  static void observe(const cg::RoundedCrossing &v, void *context) noexcept {
    auto &self = *static_cast<Capture *>(context);
    ++self.attempts;
    if (self.attempts > 2) { self.failed = true; return; }
    try {
      Snapshot s;
      s.stock.assign(v.stock.begin(), v.stock.end());
      s.value.assign(v.value.begin(), v.value.end());
      s.fields = v;
      s.fields.stock = {}; s.fields.value = {}; // No borrowed storage escapes.
      self.records.push_back(std::move(s));
    } catch (...) { self.failed = true; }
  }
};

Json snapshot_json(const Snapshot &s) {
  const auto &f = s.fields;
  Json x = Json::array(), y = Json::array(), xb = Json::array(), yb = Json::array();
  for (double v : s.stock) { x.push_back(v); xb.push_back(bits(v)); }
  for (double v : s.value) { y.push_back(v); yb.push_back(bits(v)); }
  Json numbers, number_bits;
  for (const auto &[name, v] : std::array<std::pair<const char *, double>, 8>{{
      {"right_slope", f.right_slope}, {"strike", f.strike}, {"stock_lo", f.stock_lo},
      {"stock_hi", f.stock_hi}, {"difference_lo", f.difference_lo},
      {"difference_hi", f.difference_hi}, {"slope_difference", f.slope_difference},
      {"computed_stock", f.computed_stock}}}) {
    numbers[name] = std::isfinite(v) ? Json(v) : Json(nullptr);
    number_bits[name] = bits(v);
  }
  return {{"stock", std::move(x)}, {"value", std::move(y)},
      {"stock_bits", std::move(xb)}, {"value_bits", std::move(yb)},
      {"numbers", std::move(numbers)}, {"number_bits", std::move(number_bits)},
      {"side", f.side == atx::vol::Side::Call ? "call" : "put"}, {"tail", f.tail}};
}

Json crossing(const fixture::InputCase &c, std::size_t n, unsigned level) {
  cg::Options options;
  options.positive_intervals = n; options.time_level = level;
  std::vector<cg::Event> events;
  for (const auto &e : c.divs) events.push_back({e.tau, e.amount});
  const auto owner = cg::EventOwner::prepare(model(c), events, options);
  cg::Work work;
  Capture capture;
  capture.records.reserve(2);
  const cg::CrossingObserver observer{Capture::observe, &capture};
  const auto price = owner ? owner->price(work, &observer) : atx::core::Err(owner.error());
  Json records = Json::array();
  for (const auto &record : capture.records) records.push_back(snapshot_json(record));
  std::cerr << "crossing " << c.id << ' ' << n << ' ' << level << ' '
            << (price ? "success" : "refused") << '\n';
  return {{"id", c.id}, {"positive_intervals", n}, {"time_level", level},
      {"status", price ? "success" : "refused"},
      {"error", price ? "" : price.error().to_string()},
      {"error_code", price ? 0 : static_cast<int>(price.error().code())},
      {"price", price ? Json(*price) : Json(nullptr)}, {"work", work_json(work)},
      {"capture_failed", capture.failed}, {"capture_attempts", capture.attempts},
      {"records", std::move(records)}};
}

Json run() {
  const auto cases = fixture::matrix();
  if (cases.size() != 50 || fixture::input_fingerprint(cases) != 12110935955959749584ULL)
    throw std::runtime_error("original manifest changed");
  const auto find = [&cases](std::string_view id) -> const fixture::InputCase & {
    const auto it = std::find_if(cases.begin(), cases.end(),
        [id](const auto &c) { return c.id == id; });
    if (it == cases.end()) throw std::runtime_error("missing fixed case");
    return *it;
  };
  Json manifest = Json::array(), european = Json::array(), crossings = Json::array();
  for (const auto &c : cases) manifest.push_back({{"id", c.id}, {"input", input_json(c)}});
  for (const auto id : {"DSPYp", "DSPYc"})
    for (const auto n : {256u, 512u, 1024u})
      for (const auto count : {1u, 32u, 128u}) european.push_back(semigroup(find(id), n, count));
  for (const auto level : {1u, 2u}) crossings.push_back(crossing(find("A1p"), 256, level));
  for (const auto n : {256u, 512u, 1024u}) crossings.push_back(crossing(find("E64c"), n, 0));
  return {{"schema", "cash-gaussian-followup-v1"}, {"timed", false},
      {"declaration_commit", "ed4139038bca27b3a8a175193f3b290031ad9bae"},
      {"input_fingerprint", "12110935955959749584"}, {"full_interval_integral", true},
      {"probability_cutoff", nullptr}, {"infinite_tail", "linear"},
      {"limits", {{"max_knots", 8192}, {"max_created_knots", 1U << 24},
          {"max_expectations", 1U << 20}, {"max_interval_evaluations", 1U << 28}}},
      {"manifest", std::move(manifest)}, {"semigroup", std::move(european)},
      {"crossings", std::move(crossings)}};
}
} // namespace

int main(int argc, char **) {
  try {
    if (argc != 1) throw std::runtime_error("fixed follow-up accepts no arguments");
    std::cout << run().dump(2) << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "cash-gaussian-followup: " << error.what() << '\n';
    return 2;
  }
}
