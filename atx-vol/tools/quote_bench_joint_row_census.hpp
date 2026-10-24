#pragma once

#include "fitting/joint_row_census.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace quote_bench_joint_row_census {
using Json = nlohmann::json;
using namespace atx::vol;

inline constexpr std::array<std::size_t, 5> limits{64, 7000, 14000, 14000, 60000};

// Explicit tool owner: construct before fitting; destroy after scope/export.
// A failed setup still supplies an invalid recorder so numerical work proceeds.
struct Owner {
  std::vector<research::JointCapturedSlice> slices;
  std::vector<research::JointCapturedNode> nodes;
  std::vector<research::JointCapturedCoordinate> coordinates;
  std::vector<research::JointCapturedVariable> variables;
  std::vector<research::JointCapturedRow> rows;
  std::optional<research::JointRowRecorder> recorder;
  Owner() noexcept {
    bool failed = false;
    try {
      slices.resize(limits[0]); nodes.resize(limits[1]); coordinates.resize(limits[2]);
      variables.resize(limits[3]); rows.resize(limits[4]);
    } catch (const std::bad_alloc &) { failed = true; }
    recorder.emplace(research::JointRowStorage{slices, nodes, coordinates, variables, rows});
    if (failed) recorder->observer().invalidate(research::JointRowCaptureError::SetupAllocation);
  }
};

// Every numerical value is exact binary64 bits, independent of JSON float
// spelling, signed zero or nonfinite-number conventions. Reader decodes bits.
inline std::string bits(double value) {
  constexpr char digits[] = "0123456789abcdef";
  const auto word = std::bit_cast<std::uint64_t>(value);
  std::array<char, 16> chars{};
  for (std::size_t i = 0; i < chars.size(); ++i)
    chars[i] = digits[(word >> (4 * (15 - i))) & 15u];
  return {chars.data(), chars.size()};
}
inline Json capacities(research::JointRowStorage s) {
  return {{"slices", s.slices.size()}, {"nodes", s.nodes.size()},
      {"coordinates", s.coordinates.size()}, {"variables", s.variables.size()},
      {"rows", s.rows.size()}};
}
inline Json report(const research::JointRowRecorder &recorder) {
  const auto data = recorder.data();
  const auto &observer = recorder.observer();
  const auto &diagnostics = recorder.diagnostics();
  Json slices = Json::array(), nodes = Json::array(), coordinates = Json::array();
  Json variables = Json::array(), rows = Json::array();
  for (const auto &s : data.slices) {
    slices.push_back({{"node_offset", s.node_offset}, {"nodes", s.nodes},
        {"coordinate_offset", s.coordinate_offset}, {"bounds", {
            bits(s.bounds.spot), bits(s.bounds.discount_min), bits(s.bounds.discount_max),
            bits(s.bounds.prepaid_min), bits(s.bounds.prepaid_max)}},
        {"terminal_discount", s.has_discount ? Json(bits(s.discount)) : Json(nullptr)},
        {"has_terminal_prepaid", s.has_prepaid}});
  }
  for (const auto &n : data.nodes)
    nodes.push_back({{"strike", bits(n.strike)},
        {"selected", {bits(n.selected_call), bits(n.selected_put)}},
        {"raw", {bits(n.raw_call), bits(n.raw_put)}},
        {"published", {bits(n.published_call), bits(n.published_put)}},
        {"terminal_prepaid", bits(n.prepaid)},
        {"box", {bits(n.box.call_lo), bits(n.box.call_hi), bits(n.box.put_lo), bits(n.box.put_hi)}}});
  for (const auto &c : data.coordinates)
    coordinates.push_back({{"variable", c.variable}, {"fixed", bits(c.fixed)}});
  for (const auto &v : data.variables)
    variables.push_back({{"lower", bits(v.lower)}, {"upper", bits(v.upper)},
        {"target", bits(v.target)}, {"initial", bits(v.initial)},
        {"iterate", bits(v.iterate)}, {"published", bits(v.published)}});
  for (const auto &r : data.rows) {
    Json coefficients = Json::array();
    for (const auto a : r.coefficient) coefficients.push_back(bits(a));
    rows.push_back({{"count", r.count}, {"indices", r.index},
        {"coefficients", coefficients}, {"rhs", bits(r.rhs)},
        {"initial_slack", bits(r.initial_slack)}});
  }
  Json profile = nullptr;
  if (recorder.has_profile()) {
    const auto &p = recorder.profile();
    profile = {{"row_arities", p.row_arities}, {"bound_rows", p.bound_rows},
        {"row_storage_bytes", p.row_storage_bytes}, {"matrix_nonzeros", p.matrix_nonzeros},
        {"factor_nonzeros", p.factor_nonzeros}, {"factorizations", p.factorizations},
        {"directions", p.directions}, {"centrality_enabled", p.centrality_enabled}};
  }
  return {{"schema", "atx-joint-row-census-v1"}, {"valid", observer.valid()},
      {"error", static_cast<unsigned>(observer.error())},
      {"attempts", observer.attempts()}, {"built_events", observer.built_events()},
      {"published_events", observer.published_events()}, {"timing_qualifying", false},
      {"row_arithmetic", "ordered_fma_binary64_nearest_v1"},
      {"arithmetic_evidence_required", true},
      {"capacity", capacities(recorder.capacity())}, {"counts", capacities(data)},
      {"iteration_budget", recorder.iteration_budget()}, {"sweeps", recorder.sweeps()},
      {"tolerance", bits(recorder.tolerance())}, {"diagnostics", {
          {"variables", diagnostics.variables}, {"constraints", diagnostics.constraints},
          {"certificate_passed", diagnostics.certificate_passed},
          {"primal_residual", bits(diagnostics.primal_residual)},
          {"dual_residual", bits(diagnostics.dual_residual)},
          {"duality_gap", bits(diagnostics.duality_gap)}}},
      {"profile", profile}, {"slices", slices}, {"nodes", nodes},
      {"coordinates", coordinates}, {"variables", variables}, {"rows", rows}};
}
inline void write(const std::filesystem::path &path, const Json &value) {
  if (std::filesystem::exists(path)) throw std::runtime_error("Census output already exists");
  std::ofstream output(path, std::ios::binary);
  if (!output) throw std::runtime_error("Cannot create census output");
  output << value.dump(2) << '\n';
  output.close();
  if (!output) throw std::runtime_error("Census output write failed");
}
} // namespace quote_bench_joint_row_census
