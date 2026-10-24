#include "batch.hpp"

#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/vol/api/simd/cpu.hpp"
#include "bench_clocks.hpp"
#include "support/ddiv_matrix_cases.hpp"

namespace {
namespace ref = atx::vol::ddiv_ref;
namespace research = ref::research;
namespace bench = atx::vol::bench;
namespace fixture = atx::vol::test::ddiv_matrix;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

struct Args {
  std::string mode;
  int grid = 0;
  std::optional<unsigned> affinity;
  bool require_pcore = false;
  bool untimed = false;
};

unsigned number(std::string_view text) {
  unsigned out = 0;
  const auto converted = std::from_chars(text.data(), text.data() + text.size(), out);
  if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size()) {
    throw std::runtime_error("expected a nonnegative decimal integer");
  }
  return out;
}

Args parse(int argc, char **argv) {
  Args out;
  std::unordered_set<std::string> seen;
  for (int i = 1; i < argc; ++i) {
    const std::string key(argv[i]);
    if (!seen.insert(key).second) throw std::runtime_error("duplicate argument: " + key);
    if (key == "--untimed") { out.untimed = true; continue; }
    if (key == "--require-pcore") { out.require_pcore = true; continue; }
    if (i + 1 == argc) throw std::runtime_error("missing value: " + key);
    const std::string value(argv[++i]);
    if (key == "--mode") out.mode = value;
    else if (key == "--grid") {
      const auto grid = number(value);
      if (grid != 400 && grid != 800 && grid != 1600) {
        throw std::runtime_error("--grid must be 400, 800 or 1600");
      }
      out.grid = static_cast<int>(grid);
    } else if (key == "--affinity") out.affinity = number(value);
    else throw std::runtime_error("unknown argument: " + key);
  }
  if (out.mode != "original" && out.mode != "batch-scalar" && out.mode != "batch-avx2") {
    throw std::runtime_error("--mode must be original, batch-scalar or batch-avx2");
  }
  if (out.grid == 0) throw std::runtime_error("--grid is required");
  if (out.require_pcore && !out.affinity) {
    throw std::runtime_error("--require-pcore requires --affinity");
  }
  return out;
}

struct Published {
  std::optional<ref::PdeResult> value;
  std::string error;
};

// The measured scope owns request adapters, every PDE workspace, temporary
// outputs. These are destroyed before it returns. Published result copies and
// refusal text survive for untimed encoding; qualification requires no refusals.
void execute(const Args &args, const std::vector<fixture::InputCase> &cases,
             std::array<Published, 50> &published) {
  std::vector<ref::Problem> problems;
  problems.reserve(cases.size());
  for (const auto &c : cases) {
    problems.push_back({.S = c.S, .K = c.K, .T = c.T, .sigma = c.sigma,
        .r = c.r, .q = c.q, .side = c.side, .exercise = atx::vol::ExerciseStyle::American,
        .process = c.process, .dividends = c.divs});
  }
  const ref::PdeGrid grid{.space_base = args.grid, .time_base = args.grid / 2};
  std::vector<atx::vol::Result<ref::PdeResult>> results;
  if (args.mode == "original") {
    results.reserve(problems.size());
    for (const auto &p : problems) results.push_back(ref::pde_price(p, grid));
  } else {
    const auto sweep = args.mode == "batch-avx2" ? &research::solve_avx2 : &research::solve_scalar;
    results = research::pde_price_batch(problems, grid, sweep);
  }
  if (results.size() != published.size()) throw std::runtime_error("lost output positions");
  for (std::size_t i = 0; i < results.size(); ++i) {
    if (results[i]) published[i].value = *results[i];
    else published[i].error = results[i].error().to_string();
  }
}

Json model_json(const fixture::InputCase &c) {
  Json events = Json::array();
  for (const auto &d : c.divs) events.push_back({{"tau", d.tau}, {"cash", d.amount}});
  return {{"spot", c.S}, {"strike", c.K}, {"maturity", c.T}, {"sigma", c.sigma},
          {"rate", c.r}, {"continuous_yield", c.q},
          {"side", c.side == atx::vol::Side::Call ? "call" : "put"},
          {"cash_process", ref::to_string(c.process)}, {"exercise", "american"},
          {"events", std::move(events)}};
}

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
          {"max_policy_iterations", r.max_policy_iterations},
          {"max_policy_time", r.max_policy_time}, {"max_lcp_violation", r.max_lcp_violation},
          {"lcp_rounding_ties", r.lcp_rounding_ties}, {"binary64", std::move(bits)}};
}

Json run(const Args &args) {
  if (args.affinity) bench::pin_to_logical_cpu(*args.affinity);
  if (args.require_pcore && bench::is_performance_cpu(*args.affinity) != true) {
    throw std::runtime_error("requested CPU is not proven to be a performance core");
  }
  if (args.mode == "batch-avx2" && !atx::vol::simd::have_avx2()) {
    throw std::runtime_error("batch-avx2 requires AVX2 and FMA");
  }
  const auto cases = fixture::matrix();
  if (cases.size() != 50 || fixture::input_fingerprint(cases) != 12110935955959749584ULL) {
    throw std::runtime_error("canonical input manifest changed");
  }
  std::array<Published, 50> published;
  const auto before = args.untimed ? bench::CpuSample{} : bench::sample_cpus();
  const auto start = args.untimed ? Clock::time_point{} : Clock::now();
  execute(args, cases, published);
  const auto finish = args.untimed ? Clock::time_point{} : Clock::now();
  const auto after = args.untimed ? bench::CpuSample{} : bench::sample_cpus();
  Json rows = Json::array();
  std::size_t success = 0;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    Json row{{"id", cases[i].id}, {"input", model_json(cases[i])},
             {"status", published[i].value ? "success" : "refused"},
             {"error", published[i].error}};
    if (published[i].value) {
      row["result"] = result_json(*published[i].value);
      ++success;
    } else row["result"] = nullptr;
    rows.push_back(std::move(row));
  }
  return {{"schema", "cash-pde-batch-cold-v1"}, {"mode", args.mode},
      {"grid", {{"space_base", args.grid}, {"time_base", args.grid / 2},
                {"space_level", 0}, {"time_level", 0}, {"width_sd", 8.},
                {"rannacher_steps", 2}, {"domain_sigma", 0.}, {"time_grading", 2.}}},
      {"input_fingerprint", "12110935955959749584"}, {"requests", cases.size()},
      {"successes", success}, {"refusals", cases.size() - success},
      {"timed", !args.untimed},
      {"inclusive_seconds", args.untimed ? Json(nullptr) :
          Json(std::chrono::duration<double>(finish - start).count())},
      {"clock_scope", "request adaptation, validation, calendar/model/grid preparation, "
          "grouping, allocation/layout, all theta/Howard/Thomas work, jumps, result copies "
          "and all temporary/workspace destruction; excludes immutable raw fixture creation "
          "and JSON encoding; no warmup or workspace reuse"},
      {"qualification", "research-only; timing requires external quiet/affinity audit; "
          "all150 accuracy denominator retained; finest DSPYp uncertain"},
      {"process", bench::process_identity_json()}, {"build", bench::build_info_json()},
      {"ipo", ATX_CASH_BATCH_IPO != 0},
      {"affinity", bench::affinity_json(args.affinity, args.require_pcore, before, after)},
      {"rows", std::move(rows)}};
}
} // namespace

int main(int argc, char **argv) {
  try {
    const auto result = run(parse(argc, argv));
    std::cout << result.dump(2) << '\n';
    return result["refusals"].get<std::size_t>() == 0 ? 0 : 3;
  } catch (const std::exception &error) {
    std::cerr << "cash-pde-batch: " << error.what() << '\n';
    return 2;
  }
}
