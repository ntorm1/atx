// Diagnostic-only fixed experiment; no timing or production route selection.
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "pricing/american_discrete_div_reference.hpp"
#include "support/ddiv_matrix_cases.hpp"

namespace {
namespace ref = atx::vol::ddiv_ref;
namespace fixture = atx::vol::test::ddiv_matrix;
using Json = nlohmann::json;

Json result_json(const ref::PdeResult &r) {
  Json bits;
  for (const auto &[name, value] : std::array<std::pair<const char *, double>, 5>{{
           {"price", r.price}, {"delta", r.delta}, {"gamma", r.gamma},
           {"max_policy_time", r.max_policy_time}, {"max_lcp_violation", r.max_lcp_violation}}}) {
    if (!std::isfinite(value)) throw std::runtime_error("nonfinite published result");
    bits[name] = std::to_string(std::bit_cast<std::uint64_t>(value));
  }
  return {{"price", r.price}, {"delta", r.delta}, {"gamma", r.gamma},
      {"space_intervals", r.space_intervals}, {"time_steps", r.time_steps},
      {"max_policy_iterations", r.max_policy_iterations}, {"max_policy_time", r.max_policy_time},
      {"max_lcp_violation", r.max_lcp_violation}, {"lcp_rounding_ties", r.lcp_rounding_ties},
      {"binary64", std::move(bits)}};
}

Json run() {
  const auto cases = fixture::matrix();
  if (cases.size() != 50 || fixture::input_fingerprint(cases) != 12110935955959749584ULL) {
    throw std::runtime_error("canonical input manifest changed");
  }
  Json manifest = Json::array();
  Json rows = Json::array();
  constexpr std::array<std::array<int, 2>, 6> levels{{
      {0, 0}, {1, 0}, {0, 1}, {1, 1}, {2, 0}, {0, 2}}};
  for (const auto &c : cases) {
    const bool selected = c.id == "DSPYp" || c.id == "DSPYc";
    Json events = Json::array();
    for (const auto &d : c.divs) events.push_back({{"tau", d.tau}, {"cash", d.amount}});
    manifest.push_back({{"id", c.id}, {"selected", selected}, {"input", {
        {"spot", c.S}, {"strike", c.K}, {"maturity", c.T}, {"sigma", c.sigma},
        {"rate", c.r}, {"yield", c.q},
        {"side", c.side == atx::vol::Side::Call ? "call" : "put"},
        {"cash_process", ref::to_string(c.process)}, {"exercise", "american"},
        {"events", std::move(events)}}}});
    if (!selected) continue;
    const ref::Problem p{.S = c.S, .K = c.K, .T = c.T, .sigma = c.sigma,
        .r = c.r, .q = c.q, .side = c.side, .exercise = atx::vol::ExerciseStyle::American,
        .process = c.process, .dividends = c.divs};
    for (const auto &level : levels) {
      const ref::PdeGrid grid{.space_base = 1600, .space_level = level[0],
          .time_base = 800, .time_level = level[1]};
      ref::PdeDiagnostics diagnostics;
      const auto result = ref::pde_price(p, grid, &diagnostics);
      rows.push_back({{"id", c.id}, {"space_level", level[0]}, {"time_level", level[1]},
          {"status", result ? "success" : "refused"},
          {"error", result ? "" : result.error().to_string()},
          {"result", result ? result_json(*result) : Json(nullptr)},
          {"work", {{"thomas_sweeps", diagnostics.thomas_sweeps},
              {"fresh_seed_sweeps", diagnostics.fresh_seed_sweeps},
              {"forward_rows", diagnostics.forward_rows},
              {"continuation_rows", diagnostics.continuation_rows},
              {"identity_rows", diagnostics.identity_rows},
              {"continuation_runs", diagnostics.continuation_runs}}}});
    }
  }
  return {{"schema", "cash-dspy-axis-v1"}, {"timed", false},
      {"declaration_commit", "be0551a871315d4ca8c5f1f6d24e9b5f19c40c0f"},
      {"input_fingerprint", "12110935955959749584"},
      {"grid_base", {{"space_base", 1600}, {"time_base", 800},
          {"width_sd", 8.}, {"rannacher_steps", 2}, {"domain_sigma", 0.},
          {"time_grading", 2.}}},
      {"manifest", std::move(manifest)}, {"rows", std::move(rows)}};
}
} // namespace

int main(int argc, char **) {
  try {
    if (argc != 1) throw std::runtime_error("fixed predeclared experiment accepts no arguments");
    std::cout << run().dump(2) << '\n';
    return 0; // Typed per-request refusals are retained in the report, never omitted.
  } catch (const std::exception &error) {
    std::cerr << "cash-dspy-axis: " << error.what() << '\n';
    return 2;
  }
}
